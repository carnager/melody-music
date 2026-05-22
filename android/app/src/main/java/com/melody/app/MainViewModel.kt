package com.melody.app

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

enum class LibView { Artists, Albums, Tracks }

class MainViewModel : ViewModel() {
    private val mpd get() = MelodyApp.instance.mpd
    private val offline = MelodyApp.instance.offlineManager

    // Playback status
    var status by mutableStateOf<PlaybackStatus?>(null); private set
    var queue by mutableStateOf<List<QueueItem>>(emptyList()); private set
    var currentTrackOffline by mutableStateOf(false); private set

    // Library
    var libView by mutableStateOf(LibView.Artists); private set
    var artists by mutableStateOf<List<String>>(emptyList()); private set
    var albums by mutableStateOf<List<Album>>(emptyList()); private set
    var tracks by mutableStateOf<List<Track>>(emptyList()); private set
    var curArtist by mutableStateOf(""); private set
    var curAlbum by mutableStateOf<Album?>(null); private set
    var albumRating by mutableStateOf(0); private set
    var albumComputedRating by mutableStateOf(0.0); private set
    // Saved scroll positions for back navigation
    var savedArtistScrollIndex by mutableStateOf(0); private set
    var savedArtistScrollOffset by mutableStateOf(0); private set
    var savedAlbumScrollIndex by mutableStateOf(0); private set
    var savedAlbumScrollOffset by mutableStateOf(0); private set

    // Search
    var searchQuery by mutableStateOf("")
    var searchResult by mutableStateOf(SearchResult(emptyList(), emptyList())); private set
    private var searchJob: Job? = null

    // Devices
    var devices by mutableStateOf<List<DeviceInfo>>(emptyList()); private set

    // Playlists
    var playlists by mutableStateOf<List<PlaylistInfo>>(emptyList()); private set
    var playlistTracks by mutableStateOf<List<Track>>(emptyList()); private set
    var curPlaylist by mutableStateOf<PlaylistInfo?>(null); private set
    var playlistView by mutableStateOf(false); private set

    // Action menu
    var showActionMenu by mutableStateOf(false)
    var actionTarget by mutableStateOf<ActionTarget?>(null); private set

    // Offline downloads
    var downloadProgress by mutableStateOf<OfflineManager.DownloadProgress?>(null); private set
    var downloadedAlbums by mutableStateOf<Set<String>>(emptySet()); private set

    private var pollJob: Job? = null
    private var downloadJob: Job? = null

    init {
        downloadedAlbums = offline.getDownloadedAlbumIds()
        startPolling()
        loadArtists()
    }

    private fun startPolling() {
        pollJob?.cancel()
        pollJob = viewModelScope.launch {
            while (true) {
                refresh()
                delay(800)
            }
        }
    }

    private suspend fun refresh() {
        try {
            status = mpd.getStatus()
            val curPos = status?.currentSongPos ?: -1
            queue = mpd.getQueue().map { it.copy(current = it.position == curPos) }
        } catch (_: Exception) {}
        currentTrackOffline = PlaybackService.instance?.isCurrentTrackOffline ?: false
        // Retry loading artists if connection recovered
        if (artists.isEmpty() && mpd.connected) {
            try { artists = mpd.getArtists() } catch (_: Exception) {}
        }
    }

    // --- Library ---

    fun loadArtists() {
        viewModelScope.launch {
            try { artists = mpd.getArtists() } catch (_: Exception) {}
            libView = LibView.Artists
        }
    }

    fun saveArtistScroll(index: Int, offset: Int) {
        savedArtistScrollIndex = index
        savedArtistScrollOffset = offset
    }

    fun saveAlbumScroll(index: Int, offset: Int) {
        savedAlbumScrollIndex = index
        savedAlbumScrollOffset = offset
    }

    fun loadAlbums(artist: String) {
        viewModelScope.launch {
            curArtist = artist
            try { albums = mpd.getAlbums(artist) } catch (_: Exception) {}
            libView = LibView.Albums
        }
    }

    fun loadTracks(album: Album) {
        viewModelScope.launch {
            curAlbum = album
            try { tracks = mpd.getTracks(album.albumArtist, album.album) } catch (_: Exception) {}
            try {
                val r = mpd.getAlbumRating(album.albumArtist, album.album, album.date)
                albumRating = r.rating
                albumComputedRating = r.computed
            } catch (_: Exception) {
                albumRating = 0
                albumComputedRating = 0.0
            }
            libView = LibView.Tracks
        }
    }

    fun libBack() {
        when (libView) {
            LibView.Tracks -> {
                libView = LibView.Albums
                tracks = emptyList()
                albumRating = 0
                albumComputedRating = 0.0
            }
            LibView.Albums -> {
                libView = LibView.Artists
                albums = emptyList()
            }
            LibView.Artists -> {}
        }
    }

    // --- Action menu ---

    sealed class ActionTarget {
        data class ArtistTarget(val name: String) : ActionTarget()
        data class AlbumTarget(val album: Album) : ActionTarget()
        data class TrackTarget(val track: Track) : ActionTarget()
        data class SearchAlbumTarget(val album: Album) : ActionTarget()
        data class SearchTrackTarget(val track: Track) : ActionTarget()
        data class QueueItemTarget(val item: QueueItem) : ActionTarget()
        data class PlaylistTarget(val playlist: PlaylistInfo) : ActionTarget()
    }

    fun showAction(target: ActionTarget) {
        actionTarget = target
        showActionMenu = true
    }

    fun dismissAction() {
        showActionMenu = false
        actionTarget = null
    }

    fun executeAction(mode: String) {
        val t = actionTarget ?: return
        viewModelScope.launch {
            try {
                when (t) {
                    is ActionTarget.ArtistTarget -> mpd.addAllArtistAlbums(t.name, mode)
                    is ActionTarget.AlbumTarget -> mpd.addAlbum(t.album.albumArtist, t.album.album, mode)
                    is ActionTarget.TrackTarget -> mpd.addTrack(t.track.uri, mode)
                    is ActionTarget.SearchAlbumTarget -> mpd.addAlbum(t.album.albumArtist, t.album.album, mode)
                    is ActionTarget.SearchTrackTarget -> mpd.addTrack(t.track.uri, mode)
                    is ActionTarget.QueueItemTarget -> {}
                    is ActionTarget.PlaylistTarget -> mpd.loadPlaylist(t.playlist.name, mode)
                }
            } catch (_: Exception) {}
        }
        dismissAction()
    }

    fun browseIntoAction() {
        val t = actionTarget ?: return
        when (t) {
            is ActionTarget.ArtistTarget -> loadAlbums(t.name)
            is ActionTarget.AlbumTarget -> loadTracks(t.album)
            is ActionTarget.SearchAlbumTarget -> loadTracks(t.album)
            is ActionTarget.QueueItemTarget -> {
                if (t.item.albumId.isNotBlank()) {
                    loadTracks(Album(t.item.albumId, t.item.artist, t.item.album, ""))
                }
            }
            else -> {}
        }
        dismissAction()
    }

    fun goToArtistFromQueue(item: QueueItem) {
        loadAlbums(item.artist)
    }

    // --- Ratings ---

    fun rateCurrentTrack(rating: Int) {
        val songId = status?.songId ?: return
        if (songId.isBlank()) return
        viewModelScope.launch {
            try {
                mpd.rateTrack(songId, rating)
                refresh()
            } catch (_: Exception) {}
        }
    }

    fun rateAlbum(rating: Int) {
        val album = curAlbum ?: return
        viewModelScope.launch {
            try {
                mpd.rateAlbum(album.albumArtist, album.album, album.date, rating)
                val r = mpd.getAlbumRating(album.albumArtist, album.album, album.date)
                albumRating = r.rating
                albumComputedRating = r.computed
            } catch (_: Exception) {}
        }
    }

    fun rateAlbumDirect(album: Album, rating: Int) {
        viewModelScope.launch {
            try { mpd.rateAlbum(album.albumArtist, album.album, album.date, rating) } catch (_: Exception) {}
        }
    }

    fun rateQueueTrack(songId: String, rating: Int) {
        viewModelScope.launch {
            try { mpd.rateTrack(songId, rating) } catch (_: Exception) {}
        }
    }

    // --- Search ---

    fun updateSearch(query: String) {
        searchQuery = query
        searchJob?.cancel()
        if (query.isBlank()) {
            searchResult = SearchResult(emptyList(), emptyList())
            return
        }
        searchJob = viewModelScope.launch {
            delay(200)
            try { searchResult = mpd.search(query) } catch (_: Exception) {}
        }
    }

    // --- Playback ---

    fun togglePlay() {
        viewModelScope.launch {
            try {
                if (status?.state == "playing") mpd.pause() else mpd.resume()
            } catch (_: Exception) {}
        }
    }

    fun playNext() { viewModelScope.launch { try { mpd.next() } catch (_: Exception) {} } }
    fun playPrev() { viewModelScope.launch { try { mpd.prev() } catch (_: Exception) {} } }
    fun stopPlayback() { viewModelScope.launch { try { mpd.stop() } catch (_: Exception) {} } }
    fun seek(pos: Double) { viewModelScope.launch { try { mpd.seek(pos) } catch (_: Exception) {} } }

    fun randomAlbum() {
        viewModelScope.launch {
            try {
                val allAlbums = mpd.getAllAlbums()
                if (allAlbums.isNotEmpty()) {
                    val album = allAlbums.random()
                    mpd.addAlbum(album.albumArtist, album.album, "replace")
                }
            } catch (_: Exception) {}
        }
    }

    // --- Queue ---

    fun queuePlay(position: Int) { viewModelScope.launch { try { mpd.queuePlay(position) } catch (_: Exception) {} } }
    fun queueRemove(position: Int) { viewModelScope.launch { try { mpd.queueRemove(position) } catch (_: Exception) {} } }
    fun queueMove(from: Int, to: Int) { viewModelScope.launch { try { mpd.queueMove(from, to) } catch (_: Exception) {} } }
    fun queueClear() { viewModelScope.launch { try { mpd.queueClear() } catch (_: Exception) {} } }

    // --- Offline downloads ---

    fun downloadAlbum(album: Album) {
        if (downloadJob?.isActive == true) return
        downloadJob = viewModelScope.launch {
            try {
                val albumTracks = mpd.getTracks(album.albumArtist, album.album)
                if (albumTracks.isEmpty()) return@launch
                val prefs = MelodyApp.instance.getSharedPreferences("melody", android.content.Context.MODE_PRIVATE)
                val format = prefs.getString("audio_format", "")?.ifBlank { null }
                val bitrate = prefs.getInt("audio_bitrate", 0)
                offline.downloadAlbum(album.id, albumTracks, mpd, format, bitrate) { progress ->
                    downloadProgress = progress
                }
                downloadProgress = null
                downloadedAlbums = offline.getDownloadedAlbumIds()
            } catch (_: Exception) {}
        }
    }

    fun removeOfflineAlbum(albumId: String) {
        offline.removeAlbum(albumId)
        downloadedAlbums = offline.getDownloadedAlbumIds()
    }

    fun isAlbumDownloaded(albumId: String): Boolean = offline.isAlbumDownloaded(albumId)

    // --- Add to playlist ---

    var showPlaylistPicker by mutableStateOf(false); private set
    var playlistPickerUri by mutableStateOf(""); private set

    fun showAddToPlaylist(uri: String) {
        playlistPickerUri = uri
        viewModelScope.launch {
            try { playlists = mpd.getPlaylists() } catch (_: Exception) {}
            showPlaylistPicker = true
        }
    }

    fun addToPlaylist(playlistName: String) {
        val uri = playlistPickerUri
        if (uri.isBlank()) return
        viewModelScope.launch {
            try { mpd.addToPlaylist(playlistName, uri) } catch (_: Exception) {}
        }
        showPlaylistPicker = false
        playlistPickerUri = ""
    }

    fun dismissPlaylistPicker() {
        showPlaylistPicker = false
        playlistPickerUri = ""
    }

    // --- Playlists ---

    fun loadPlaylists() {
        viewModelScope.launch {
            try { playlists = mpd.getPlaylists() } catch (_: Exception) {}
            playlistView = false
        }
    }

    fun loadPlaylistTracks(playlist: PlaylistInfo) {
        viewModelScope.launch {
            curPlaylist = playlist
            try { playlistTracks = mpd.getPlaylistTracks(playlist.name) } catch (_: Exception) {}
            playlistView = true
        }
    }

    fun playlistBack() {
        playlistView = false
        playlistTracks = emptyList()
        curPlaylist = null
    }

    // --- Devices ---

    fun loadDevices() {
        viewModelScope.launch {
            try {
                devices = mpd.getOutputs()
                android.util.Log.d("MainViewModel", "loadDevices: ${devices.size} devices: ${devices.map { "${it.name}(${it.type})" }}")
            } catch (e: Exception) {
                android.util.Log.e("MainViewModel", "loadDevices failed: ${e.message}")
            }
        }
    }

    fun setActiveDevice(id: String) {
        viewModelScope.launch {
            try {
                mpd.enableOutput(id)
                delay(300)
                devices = mpd.getOutputs()
            } catch (_: Exception) {}
        }
    }
}
