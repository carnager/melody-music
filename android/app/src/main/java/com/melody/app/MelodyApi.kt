package com.melody.app

// Data classes shared across the app

data class Album(val id: String, val albumArtist: String, val album: String, val date: String)
data class Track(
    val id: String,
    val songId: String,
    val title: String,
    val artist: String,
    val album: String,
    val trackNumber: Int,
    val albumId: String = "",
    val duration: Double = 0.0,
    val uri: String = "",
    val rating: Int = 0
)
data class QueueItem(val position: Int, val songId: String, val title: String, val artist: String, val album: String, val albumId: String, val duration: Double, val current: Boolean, val uri: String = "", val rating: Int = 0)
data class PlaybackStatus(val state: String, val title: String, val artist: String, val album: String, val date: String, val albumId: String, val timePos: Double, val duration: Double, val rating: Int = 0, val songId: String = "", val currentSongPos: Int = -1, val playlistVersion: Int = 0)
data class DeviceInfo(val id: String, val name: String, val isLocal: Boolean, val type: String, val online: Boolean, val format: String, val maxBitrate: Int, val active: Boolean)
data class SearchResult(val albums: List<Album>, val tracks: List<Track>)
data class PlaylistInfo(val id: String, val name: String, val songCount: Int, val duration: Int, val coverArt: String)
