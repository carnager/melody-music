package com.melody.app

import android.content.Context
import android.net.ConnectivityManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.TimeUnit

/**
 * Owns offline album downloads end to end: the persistent download queue, the
 * sequential download processor, retries with backoff, and the on-disk cache.
 *
 * The pipeline lives here (application scope) rather than in a ViewModel so
 * queued downloads survive the UI being destroyed. The queue and retry set are
 * persisted to prefs so they also survive process death.
 */
class OfflineManager(private val context: Context) {
    private val offlineDir get() = File(context.filesDir, "offline").also { it.mkdirs() }
    private val prefs get() = context.getSharedPreferences("melody_offline", Context.MODE_PRIVATE)

    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    private val client = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(60, TimeUnit.SECONDS)
        .build()

    // --- Pipeline state (all mutations synchronized on `pipelineLock`) ---

    private val pipelineLock = Any()
    private val downloadQueue = Channel<Album>(Channel.UNLIMITED)
    private val queuedAlbums = linkedMapOf<String, Album>()
    private val pendingRetries = linkedMapOf<String, Album>()
    private val retryAttempts = mutableMapOf<String, Int>()
    private val cancelledAlbumIds = mutableSetOf<String>()
    @Volatile private var currentDownloadAlbumId: String? = null
    private var retryJob: Job? = null

    // Callbacks for UI observation (may be invoked from IO threads)
    var onProgress: ((DownloadProgress?) -> Unit)? = null
    var onDownloadedAlbumsChanged: ((Set<String>) -> Unit)? = null
    var onDownloadError: ((String) -> Unit)? = null

    init {
        scope.launch { processQueue() }
        scope.launch {
            reconcileDownloadedAlbums()
            collectGarbage()
            restorePersistedQueue()
        }
    }

    // --- Query ---

    fun isAlbumDownloaded(albumId: String): Boolean {
        if (prefs.getStringSet("downloaded_albums", emptySet())?.contains(albumId) != true) return false
        val meta = loadAlbumMeta(albumId) ?: return false
        return meta.tracks.isNotEmpty() && meta.tracks.all { isSongDownloaded(it.songId) }
    }

    fun isSongDownloaded(songId: String): Boolean {
        return isCompleteAudioFile(songId)
    }

    fun getLocalPath(songId: String): String? {
        val file = audioFile(songId)
        return if (isCompleteAudioFile(songId)) file.absolutePath else null
    }

    fun getDownloadedAlbumIds(): Set<String> {
        return prefs.getStringSet("downloaded_albums", emptySet()) ?: emptySet()
    }

    /** Returns all downloaded albums with metadata (artist, album name, etc). */
    fun getDownloadedAlbums(): List<DownloadedAlbumInfo> {
        val albumIds = getDownloadedAlbumIds()
        return albumIds.mapNotNull { albumId ->
            val meta = loadAlbumMeta(albumId)
            if (meta == null || meta.tracks.isEmpty()) return@mapNotNull null
            meta
        }
    }

    data class DownloadedAlbumInfo(
        val albumId: String,
        val albumArtist: String,
        val album: String,
        val date: String,
        val tracks: List<Track>
    )

    // --- Download pipeline ---

    data class DownloadProgress(val albumId: String, val current: Int, val total: Int, val trackTitle: String)

    fun enqueueAlbum(album: Album) {
        synchronized(pipelineLock) {
            cancelledAlbumIds.remove(album.id)
            pendingRetries.remove(album.id)
            retryAttempts.remove(album.id)
            if (queuedAlbums.containsKey(album.id)) return
            queuedAlbums[album.id] = album
            if (downloadQueue.trySend(album).isFailure) {
                queuedAlbums.remove(album.id)
                return
            }
        }
        persistQueue()
    }

    /** Cancels a queued or in-flight download and removes the album from disk. */
    fun cancelAndRemoveAlbum(albumId: String) {
        synchronized(pipelineLock) {
            cancelledAlbumIds.add(albumId)
            queuedAlbums.remove(albumId)
            pendingRetries.remove(albumId)
            retryAttempts.remove(albumId)
        }
        persistQueue()
        scope.launch {
            // If this album is downloading right now, wait for the processor to
            // notice the cancellation (it checks between tracks) before deleting
            // files — otherwise the in-flight track re-creates them.
            while (currentDownloadAlbumId == albumId) delay(200)
            removeAlbumFiles(albumId)
            onDownloadedAlbumsChanged?.invoke(getDownloadedAlbumIds())
        }
    }

    fun retryPending() {
        val albums = synchronized(pipelineLock) { pendingRetries.values.toList() }
        for (album in albums) {
            synchronized(pipelineLock) {
                if (!queuedAlbums.containsKey(album.id)) {
                    queuedAlbums[album.id] = album
                    if (downloadQueue.trySend(album).isFailure) queuedAlbums.remove(album.id)
                }
            }
        }
    }

    private suspend fun processQueue() {
        for (album in downloadQueue) {
            val skip = synchronized(pipelineLock) {
                queuedAlbums.remove(album.id)
                cancelledAlbumIds.remove(album.id)
            }
            persistQueue()
            if (skip) continue

            if (!downloadsAllowedNow()) {
                // Wrong network (metered and Wi-Fi-only is set) — park it for
                // retry; MelodyApp pings retryPending() on network changes.
                rememberRetry(album, scheduleTimer = false)
                continue
            }

            var completed = false
            currentDownloadAlbumId = album.id
            try {
                val mpd = MelodyApp.instance.mpd
                val albumTracks = mpd.getTracks(album.albumArtist, album.album)
                if (albumTracks.isEmpty()) {
                    rememberRetry(album)
                    continue
                }
                val appPrefs = context.getSharedPreferences("melody", Context.MODE_PRIVATE)
                val format = appPrefs.getString("audio_format", "")?.ifBlank { null }
                val bitrate = appPrefs.getInt("audio_bitrate", 0)
                completed = downloadAlbum(album.id, album.albumArtist, album.album, album.date, albumTracks, mpd, format, bitrate) { progress ->
                    onProgress?.invoke(progress)
                }
            } catch (e: Exception) {
                android.util.Log.e("OfflineManager", "download failed for ${album.albumArtist} - ${album.album}: ${e.message}")
            } finally {
                currentDownloadAlbumId = null
                onProgress?.invoke(null)
                onDownloadedAlbumsChanged?.invoke(getDownloadedAlbumIds())
                val wasCancelled = synchronized(pipelineLock) { album.id in cancelledAlbumIds }
                if (completed || wasCancelled) {
                    synchronized(pipelineLock) {
                        pendingRetries.remove(album.id)
                        retryAttempts.remove(album.id)
                    }
                    persistQueue()
                } else {
                    rememberRetry(album)
                }
            }
        }
    }

    private fun rememberRetry(album: Album, scheduleTimer: Boolean = true) {
        val attempts: Int
        synchronized(pipelineLock) {
            attempts = (retryAttempts[album.id] ?: 0) + 1
            if (attempts > MAX_RETRY_ATTEMPTS) {
                pendingRetries.remove(album.id)
                retryAttempts.remove(album.id)
            } else {
                retryAttempts[album.id] = attempts
                pendingRetries[album.id] = album
            }
        }
        persistQueue()
        if (attempts > MAX_RETRY_ATTEMPTS) {
            onDownloadError?.invoke("Download of \"${album.album}\" failed after $MAX_RETRY_ATTEMPTS attempts")
            return
        }
        if (scheduleTimer) scheduleRetry(attempts)
    }

    private fun scheduleRetry(attempts: Int) {
        synchronized(pipelineLock) {
            if (retryJob?.isActive == true) return
            // Exponential backoff: 5s, 10s, 20s, ... capped at 10 minutes.
            val delayMs = (5000L shl (attempts - 1).coerceAtMost(7)).coerceAtMost(600_000L)
            retryJob = scope.launch {
                delay(delayMs)
                retryPending()
            }
        }
    }

    /**
     * Downloads are allowed on unmetered networks always; on metered networks
     * only when the user disabled the Wi-Fi-only preference (default on).
     */
    private fun downloadsAllowedNow(): Boolean {
        val wifiOnly = context.getSharedPreferences("melody", Context.MODE_PRIVATE)
            .getBoolean("download_wifi_only", true)
        if (!wifiOnly) return true
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        return !cm.isActiveNetworkMetered
    }

    // --- Persistence: queued + pending-retry albums survive process death ---

    private fun persistQueue() {
        val albums = synchronized(pipelineLock) {
            // Note: the album currently being downloaded is re-persisted via
            // rememberRetry if it fails; a completed one is dropped.
            (queuedAlbums.values + pendingRetries.values).distinctBy { it.id }
        }
        val arr = JSONArray()
        albums.forEach { a ->
            arr.put(JSONObject().apply {
                put("id", a.id)
                put("album_artist", a.albumArtist)
                put("album", a.album)
                put("date", a.date)
            })
        }
        prefs.edit().putString("pending_download_queue", arr.toString()).apply()
    }

    private fun restorePersistedQueue() {
        val raw = prefs.getString("pending_download_queue", null) ?: return
        try {
            val arr = JSONArray(raw)
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val album = Album(
                    id = o.optString("id"),
                    albumArtist = o.optString("album_artist"),
                    album = o.optString("album"),
                    date = o.optString("date")
                )
                if (album.id.isNotBlank()) enqueueAlbum(album)
            }
        } catch (_: Exception) {}
    }

    // --- Startup reconciliation & garbage collection ---

    /**
     * Drops "downloaded" badges whose files are gone or incomplete, so the
     * prefs set (which the UI trusts) matches on-disk truth.
     */
    private fun reconcileDownloadedAlbums() {
        val ids = getDownloadedAlbumIds()
        val valid = ids.filter { albumId ->
            val meta = loadAlbumMeta(albumId)
            meta != null && meta.tracks.isNotEmpty() && meta.tracks.all { isCompleteAudioFile(it.songId) }
        }.toSet()
        if (valid != ids) {
            prefs.edit().putStringSet("downloaded_albums", valid).apply()
            onDownloadedAlbumsChanged?.invoke(valid)
        }
    }

    /** Deletes audio files not referenced by any album meta (orphans). */
    private fun collectGarbage() {
        val referenced = mutableSetOf<String>()
        offlineDir.listFiles { f -> f.name.startsWith("album_") && f.name.endsWith(".json") }?.forEach { f ->
            val albumId = f.name.removePrefix("album_").removeSuffix(".json")
            loadAlbumMeta(albumId)?.tracks?.forEach { referenced.add(it.songId) }
        }
        offlineDir.listFiles()?.forEach { f ->
            val name = f.name
            val songId = when {
                name.endsWith(".audio") -> name.removeSuffix(".audio")
                name.endsWith(".audio.part") -> name.removeSuffix(".audio.part")
                name.endsWith(".audio.ok") -> name.removeSuffix(".audio.ok")
                else -> return@forEach
            }
            if (songId !in referenced) {
                android.util.Log.d("OfflineManager", "GC: deleting orphaned $name")
                f.delete()
            }
        }
    }

    // --- Download internals ---

    private suspend fun downloadAlbum(
        albumId: String,
        albumArtist: String,
        albumName: String,
        date: String,
        tracks: List<Track>,
        mpd: MpdClient,
        format: String? = null,
        maxBitrate: Int = 0,
        onProgress: (DownloadProgress) -> Unit
    ): Boolean = withContext(Dispatchers.IO) {
        var success = true
        for ((i, track) in tracks.withIndex()) {
            // React to user cancellation between tracks
            if (synchronized(pipelineLock) { albumId in cancelledAlbumIds }) return@withContext false
            if (isCompleteAudioFile(track.songId, format, maxBitrate)) {
                onProgress(DownloadProgress(albumId, i + 1, tracks.size, track.title))
                continue
            }
            if (track.songId.isBlank()) {
                success = false
                continue
            }
            val url = mpd.streamUrl(track.songId, format, maxBitrate)
            onProgress(DownloadProgress(albumId, i + 1, tracks.size, track.title))
            try {
                downloadTrack(track.songId, url, format, maxBitrate)
            } catch (e: Exception) {
                android.util.Log.e("OfflineManager", "Download failed: ${track.title}: ${e.message}")
                success = false
            }
        }

        if (synchronized(pipelineLock) { albumId in cancelledAlbumIds }) return@withContext false
        if (success && tracks.all { isCompleteAudioFile(it.songId) }) {
            saveAlbumMeta(albumId, albumArtist, albumName, date, tracks)
            markAlbumDownloaded(albumId)
        }
        success
    }

    // --- Remove ---

    fun removeAlbumFiles(albumId: String) {
        val meta = loadAlbumMeta(albumId)
        meta?.tracks?.forEach {
            audioFile(it.songId).delete()
            partFile(it.songId).delete()
            completeMarkerFile(it.songId).delete()
        }
        removeAlbumMeta(albumId)
        val albums = prefs.getStringSet("downloaded_albums", emptySet())?.toMutableSet() ?: mutableSetOf()
        albums.remove(albumId)
        prefs.edit().putStringSet("downloaded_albums", albums).apply()
    }

    // --- Internal ---

    private fun audioFile(songId: String): File {
        return File(offlineDir, "$songId.audio")
    }

    private fun partFile(songId: String): File {
        return File(offlineDir, "$songId.audio.part")
    }

    private fun completeMarkerFile(songId: String): File {
        return File(offlineDir, "$songId.audio.ok")
    }

    /**
     * Checks that a completed download exists. When [format]/[maxBitrate] are
     * given, the file must also have been downloaded with those settings —
     * a quality change triggers a re-download instead of silently keeping the
     * old encoding.
     */
    private fun isCompleteAudioFile(songId: String, format: String? = null, maxBitrate: Int = -1): Boolean {
        if (songId.isBlank()) return false
        val file = audioFile(songId)
        if (!file.exists() || file.length() <= 0) return false
        val marker = completeMarkerFile(songId)
        if (!marker.exists()) return false
        return try {
            val obj = JSONObject(marker.readText())
            if (obj.optLong("bytes", -1L) != file.length()) return false
            if (format != null || maxBitrate >= 0) {
                val markerFormat = obj.optString("format", "")
                val markerBitrate = obj.optInt("max_bitrate", 0)
                if (markerFormat != (format ?: "")) return false
                if (maxBitrate >= 0 && markerBitrate != maxBitrate) return false
            }
            true
        } catch (_: Exception) {
            false
        }
    }

    private fun downloadTrack(songId: String, url: String, format: String?, maxBitrate: Int) {
        val target = audioFile(songId)
        val part = partFile(songId)
        val marker = completeMarkerFile(songId)
        part.delete()

        try {
            val req = Request.Builder().url(url).build()
            var bytesCopied = 0L
            client.newCall(req).execute().use { resp ->
                if (!resp.isSuccessful) throw IllegalStateException("HTTP ${resp.code}")
                val body = resp.body ?: throw IllegalStateException("empty response body")
                val expectedBytes = body.contentLength()
                body.byteStream().use { input ->
                    FileOutputStream(part).use { out ->
                        val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                        while (true) {
                            val read = input.read(buffer)
                            if (read == -1) break
                            out.write(buffer, 0, read)
                            bytesCopied += read.toLong()
                        }
                        out.flush()
                        out.fd.sync()
                    }
                }
                if (expectedBytes >= 0 && bytesCopied != expectedBytes) {
                    throw IllegalStateException("incomplete download: got $bytesCopied of $expectedBytes bytes")
                }
                if (bytesCopied <= 0) {
                    throw IllegalStateException("empty download")
                }
            }

            marker.delete()
            target.delete()
            if (!part.renameTo(target)) {
                throw IllegalStateException("could not finalize download")
            }
            val obj = JSONObject().apply {
                put("bytes", bytesCopied)
                put("format", format ?: "")
                put("max_bitrate", maxBitrate)
            }
            marker.writeText(obj.toString())
        } catch (e: Exception) {
            part.delete()
            throw e
        }
    }

    private fun markAlbumDownloaded(albumId: String) {
        val albums = prefs.getStringSet("downloaded_albums", emptySet())?.toMutableSet() ?: mutableSetOf()
        albums.add(albumId)
        prefs.edit().putStringSet("downloaded_albums", albums).apply()
    }

    private fun albumMetaFile(albumId: String): File {
        return File(offlineDir, "album_$albumId.json")
    }

    private fun saveAlbumMeta(albumId: String, albumArtist: String, albumName: String, date: String, tracks: List<Track>) {
        val obj = JSONObject().apply {
            put("album_artist", albumArtist)
            put("album", albumName)
            put("date", date)
            val arr = JSONArray()
            tracks.forEach { t ->
                arr.put(JSONObject().apply {
                    put("id", t.id)
                    put("song_id", t.songId)
                    put("title", t.title)
                    put("artist", t.artist)
                    put("album", t.album)
                    put("tracknumber", t.trackNumber)
                    put("disc", t.disc)
                    put("album_id", t.albumId)
                    put("duration", t.duration)
                    put("uri", t.uri)
                    put("rating", t.rating)
                })
            }
            put("tracks", arr)
        }
        albumMetaFile(albumId).writeText(obj.toString())
    }

    private fun loadAlbumMeta(albumId: String): DownloadedAlbumInfo? {
        val file = albumMetaFile(albumId)
        if (!file.exists()) return null
        return try {
            val raw = file.readText()
            // Support both old format (JSONArray) and new format (JSONObject with album info)
            if (raw.trimStart().startsWith("[")) {
                // Legacy format: plain array of tracks
                val arr = JSONArray(raw)
                val tracks = (0 until arr.length()).map { i ->
                    val o = arr.getJSONObject(i)
                    Track(
                        id = o.optString("id"),
                        songId = o.optString("song_id", o.optString("id")),
                        title = o.optString("title"),
                        artist = o.optString("artist"),
                        album = o.optString("album"),
                        trackNumber = o.optInt("tracknumber", 0),
                        albumId = o.optString("album_id", albumId),
                        duration = o.optDouble("duration", 0.0),
                        uri = o.optString("uri", o.optString("id")),
                        rating = o.optInt("rating", 0),
                        disc = o.optInt("disc", 1)
                    )
                }
                if (tracks.isEmpty()) return null
                val first = tracks.first()
                DownloadedAlbumInfo(albumId, first.artist, first.album, "", tracks)
            } else {
                val obj = JSONObject(raw)
                val arr = obj.getJSONArray("tracks")
                val tracks = (0 until arr.length()).map { i ->
                    val o = arr.getJSONObject(i)
                    Track(
                        id = o.optString("id"),
                        songId = o.optString("song_id", o.optString("id")),
                        title = o.optString("title"),
                        artist = o.optString("artist"),
                        album = o.optString("album"),
                        trackNumber = o.optInt("tracknumber", 0),
                        albumId = o.optString("album_id", albumId),
                        duration = o.optDouble("duration", 0.0),
                        uri = o.optString("uri", o.optString("id")),
                        rating = o.optInt("rating", 0),
                        disc = o.optInt("disc", 1)
                    )
                }
                DownloadedAlbumInfo(
                    albumId = albumId,
                    albumArtist = obj.optString("album_artist", ""),
                    album = obj.optString("album", ""),
                    date = obj.optString("date", ""),
                    tracks = tracks
                )
            }
        } catch (e: Exception) { null }
    }

    private fun removeAlbumMeta(albumId: String) {
        albumMetaFile(albumId).delete()
    }

    companion object {
        private const val MAX_RETRY_ATTEMPTS = 6
    }
}
