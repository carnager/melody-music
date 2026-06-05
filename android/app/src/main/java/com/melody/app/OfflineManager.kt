package com.melody.app

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.TimeUnit

class OfflineManager(private val context: Context) {
    private val offlineDir get() = File(context.filesDir, "offline").also { it.mkdirs() }
    private val metaFile get() = File(offlineDir, "meta.json")
    private val prefs get() = context.getSharedPreferences("melody_offline", Context.MODE_PRIVATE)

    private val client = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(60, TimeUnit.SECONDS)
        .build()

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

    // --- Download ---

    data class DownloadProgress(val albumId: String, val current: Int, val total: Int, val trackTitle: String)

    suspend fun downloadAlbum(
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
            if (isCompleteAudioFile(track.songId)) {
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
                downloadTrack(track.songId, url)
            } catch (e: Exception) {
                android.util.Log.e("OfflineManager", "Download failed: ${track.title}: ${e.message}")
                success = false
            }
        }

        if (success && tracks.all { isCompleteAudioFile(it.songId) }) {
            saveAlbumMeta(albumId, albumArtist, albumName, date, tracks)
            markAlbumDownloaded(albumId)
        }
        success
    }

    // --- Remove ---

    fun removeAlbum(albumId: String) {
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

    private fun isCompleteAudioFile(songId: String): Boolean {
        if (songId.isBlank()) return false
        val file = audioFile(songId)
        if (!file.exists() || file.length() <= 0) return false
        val marker = completeMarkerFile(songId)
        if (!marker.exists()) return false
        return try {
            val obj = JSONObject(marker.readText())
            obj.optLong("bytes", -1L) == file.length()
        } catch (_: Exception) {
            false
        }
    }

    private fun downloadTrack(songId: String, url: String) {
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
}
