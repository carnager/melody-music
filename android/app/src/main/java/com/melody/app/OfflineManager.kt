package com.melody.app

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
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
        return prefs.getStringSet("downloaded_albums", emptySet())?.contains(albumId) == true
    }

    fun isSongDownloaded(songId: String): Boolean {
        return audioFile(songId).exists()
    }

    fun getLocalPath(songId: String): String? {
        val file = audioFile(songId)
        return if (file.exists()) file.absolutePath else null
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
            if (audioFile(track.songId).exists()) {
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
                val req = Request.Builder().url(url).build()
                client.newCall(req).execute().use { resp ->
                    if (!resp.isSuccessful) {
                        success = false
                        return@use
                    }
                    val body = resp.body ?: return@use
                    audioFile(track.songId).outputStream().use { out ->
                        body.byteStream().copyTo(out)
                    }
                }
            } catch (e: Exception) {
                android.util.Log.e("OfflineManager", "Download failed: ${track.title}: ${e.message}")
                success = false
            }
        }

        // Save track metadata for this album
        saveAlbumMeta(albumId, albumArtist, albumName, date, tracks)
        markAlbumDownloaded(albumId)
        success
    }

    // --- Remove ---

    fun removeAlbum(albumId: String) {
        val meta = loadAlbumMeta(albumId)
        meta?.tracks?.forEach { audioFile(it.songId).delete() }
        removeAlbumMeta(albumId)
        val albums = prefs.getStringSet("downloaded_albums", emptySet())?.toMutableSet() ?: mutableSetOf()
        albums.remove(albumId)
        prefs.edit().putStringSet("downloaded_albums", albums).apply()
    }

    // --- Internal ---

    private fun audioFile(songId: String): File {
        return File(offlineDir, "$songId.audio")
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
