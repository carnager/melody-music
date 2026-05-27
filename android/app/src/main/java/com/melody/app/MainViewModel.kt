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
    var lastPlayingStatus by mutableStateOf<PlaybackStatus?>(null); private set
    var queue by mutableStateOf<List<QueueItem>>(emptyList()); private set
    var currentTrackOffline by mutableStateOf(false); private set
    var codecInfo by mutableStateOf(""); private set
    var isConnected by mutableStateOf(true); private set
    var lyrics by mutableStateOf<MpdClient.LyricsResult?>(null); private set
    private var lyricsForUri by mutableStateOf("")

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
    var libSortLatest by mutableStateOf(false); private set

    // Search
    var searchQuery by mutableStateOf("")
    var searchResult by mutableStateOf(SearchResult(emptyList(), emptyList())); private set
    private var searchJob: Job? = null

    // Rating filter (structured)
    var searchRatingType by mutableStateOf("rating"); private set       // "rating" or "albumrating"
    var searchRatingOp by mutableStateOf(">="); private set
    var searchRatingValue by mutableStateOf<Int?>(null); private set    // null = no filter

    // Multi-select
    var searchSelectionMode by mutableStateOf(false); private set
    var selectedSearchAlbums by mutableStateOf<Set<Album>>(emptySet()); private set
    var selectedSearchTracks by mutableStateOf<Set<String>>(emptySet()); private set // keyed by URI

    // Devices
    var devices by mutableStateOf<List<DeviceInfo>>(emptyList()); private set

    // "Play on phone?" prompt — shown when on mobile data and phone isn't the active device
    var showPhonePrompt by mutableStateOf(false); private set
    private var pendingPhoneAction: (suspend () -> Unit)? = null

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
    var showCachedOnly by mutableStateOf(false); private set

    private var pollJob: Job? = null
    private var playbackPollJob: Job? = null
    private var lastPlaylistVersion = 0

    init {
        downloadedAlbums = offline.getDownloadedAlbumIds()
        startPolling()
        startIdle()
        loadArtists()
        loadDevices()
    }

    private fun startPolling() {
        pollJob?.cancel()
        pollJob = viewModelScope.launch {
            while (true) {
                refresh()
                delay(5000) // Slow poll as fallback; idle handles instant updates
            }
        }
    }

    private var idleRefreshJob: Job? = null

    private fun startIdle() {
        mpd.onIdleNotification = { changed ->
            if (idleRefreshJob?.isActive != true) {
                idleRefreshJob = viewModelScope.launch { refresh(forceQueue = "rating" in changed) }
            }
        }
        mpd.onReconnected = {
            isConnected = true
            viewModelScope.launch { refresh(forceQueue = true) }
        }
        mpd.startIdle()
    }

    fun onForeground() {
        viewModelScope.launch {
            if (!mpd.connected) {
                mpd.reconnectNow()
            }
            refresh(forceQueue = true)
        }
    }

    private suspend fun refresh(forceQueue: Boolean = false) {
        try {
            val newStatus = mpd.getStatus() ?: run {
                isConnected = mpd.connected
                return
            }
            isConnected = true
            status = newStatus
            if (newStatus.title.isNotBlank() || newStatus.artist.isNotBlank()) {
                lastPlayingStatus = newStatus
            }
            val curPos = newStatus.currentSongPos
            val plVersion = newStatus.playlistVersion
            if (plVersion != lastPlaylistVersion || queue.isEmpty() || forceQueue) {
                val newQueue = mpd.getQueue()
                // Don't replace a valid queue with empty on transient failure
                if (newQueue.isNotEmpty() || forceQueue) {
                    queue = newQueue.map { it.copy(current = it.position == curPos) }
                    lastPlaylistVersion = plVersion
                }
            } else {
                queue = queue.map { it.copy(current = it.position == curPos) }
            }
        } catch (_: Exception) {}
        currentTrackOffline = PlaybackService.instance?.isCurrentTrackOffline ?: false
        codecInfo = PlaybackService.instance?.codecInfo ?: ""
        // Fetch lyrics when track changes
        val curUri = queue.firstOrNull { it.current }?.uri ?: ""
        if (curUri.isNotBlank() && curUri != lyricsForUri) {
            lyricsForUri = curUri
            lyrics = null
            viewModelScope.launch {
                lyrics = mpd.getLyrics(curUri)
            }
        }
        // Cancel existing poll so it restarts immediately with fresh status
        playbackPollJob?.cancel()
        updatePlaybackPolling()
        // Retry loading artists if connection recovered (but not when filtering to cached-only)
        if (artists.isEmpty() && mpd.connected && !showCachedOnly) {
            try { artists = mpd.getArtists() } catch (_: Exception) {}
        }
    }

    private fun updatePlaybackPolling() {
        if (status?.state == "playing") {
            if (playbackPollJob?.isActive != true) {
                playbackPollJob = viewModelScope.launch {
                    while (true) {
                        delay(1000)
                        try {
                            val s = mpd.getStatus()
                            status = s
                            if (s != null && (s.title.isNotBlank() || s.artist.isNotBlank())) {
                                lastPlayingStatus = s
                            }
                            codecInfo = PlaybackService.instance?.codecInfo ?: ""
                        } catch (_: Exception) {}
                        if (status?.state != "playing") break
                    }
                }
            }
        } else {
            playbackPollJob?.cancel()
        }
    }

    // --- Library ---

    fun loadArtists() {
        viewModelScope.launch {
            try { artists = mpd.getArtists() } catch (_: Exception) {}
            libView = LibView.Artists
        }
    }

    fun toggleLibSortLatest() {
        libSortLatest = !libSortLatest
        if (libSortLatest) {
            loadAllAlbumsLatest()
        } else {
            loadArtists()
        }
    }

    fun loadAllAlbumsLatest() {
        viewModelScope.launch {
            try { albums = mpd.getAllAlbumsLatest() } catch (_: Exception) {}
            libView = LibView.Albums
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
        if (showCachedOnly) { loadCachedAlbums(artist); return }
        viewModelScope.launch {
            curArtist = artist
            try { albums = mpd.getAlbums(artist) } catch (_: Exception) {}
            libView = LibView.Albums
        }
    }

    fun loadTracks(album: Album) {
        if (showCachedOnly) { loadCachedTracks(album); return }
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
                if (libSortLatest) {
                    // Go back to all-albums-latest view
                    tracks = emptyList()
                    albumRating = 0
                    albumComputedRating = 0.0
                    loadAllAlbumsLatest()
                } else {
                    libView = LibView.Albums
                    tracks = emptyList()
                    albumRating = 0
                    albumComputedRating = 0.0
                }
            }
            LibView.Albums -> {
                if (libSortLatest) {
                    libSortLatest = false
                    loadArtists()
                } else {
                    libView = LibView.Artists
                    albums = emptyList()
                }
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
        dismissAction()
        val doIt: suspend () -> Unit = {
            when (t) {
                is ActionTarget.ArtistTarget -> mpd.addAllArtistAlbums(t.name, mode)
                is ActionTarget.AlbumTarget -> mpd.addAlbum(t.album.albumArtist, t.album.album, mode)
                is ActionTarget.TrackTarget -> mpd.addTrack(t.track.uri, mode)
                is ActionTarget.SearchAlbumTarget -> mpd.addAlbum(t.album.albumArtist, t.album.album, mode)
                is ActionTarget.SearchTrackTarget -> mpd.addTrack(t.track.uri, mode)
                is ActionTarget.QueueItemTarget -> {}
                is ActionTarget.PlaylistTarget -> mpd.loadPlaylist(t.playlist.name, mode)
            }
        }
        if (mode == "replace" && shouldPromptPhone(doIt)) return
        viewModelScope.launch { try { doIt() } catch (_: Exception) {} }
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
                // Update queue item locally for immediate UI feedback
                queue = queue.map { if (it.songId == songId) it.copy(rating = rating) else it }
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
            try {
                mpd.rateTrack(songId, rating)
                // Update queue item locally for immediate UI feedback
                queue = queue.map { if (it.songId == songId) it.copy(rating = rating) else it }
            } catch (_: Exception) {}
        }
    }

    // --- Search ---

    private fun buildCompositeQuery(): String {
        var q = searchQuery.trim()
        val rv = searchRatingValue
        if (rv != null) {
            q = "$q ${searchRatingType}${searchRatingOp}$rv"
        }
        return q
    }

    fun updateSearch(query: String) {
        searchQuery = query
        triggerSearch()
    }

    fun setRatingFilter(type: String, op: String, value: Int?) {
        searchRatingType = type
        searchRatingOp = op
        searchRatingValue = value
        triggerSearch()
    }

    fun clearRatingFilter() {
        searchRatingValue = null
        triggerSearch()
    }

    private fun triggerSearch() {
        searchJob?.cancel()
        val composite = buildCompositeQuery()
        if (composite.isBlank()) {
            searchResult = SearchResult(emptyList(), emptyList())
            return
        }
        searchJob = viewModelScope.launch {
            delay(300)
            try {
                val result = mpd.search(composite)
                if (buildCompositeQuery() == composite) {
                    searchResult = result
                }
            } catch (_: Exception) {
                if (buildCompositeQuery() == composite) {
                    searchResult = SearchResult(emptyList(), emptyList())
                }
            }
        }
    }

    // --- Multi-select ---

    fun enterSearchSelectionMode(album: Album? = null, trackUri: String? = null) {
        searchSelectionMode = true
        if (album != null) selectedSearchAlbums = setOf(album)
        if (trackUri != null) selectedSearchTracks = setOf(trackUri)
    }

    fun exitSearchSelectionMode() {
        searchSelectionMode = false
        selectedSearchAlbums = emptySet()
        selectedSearchTracks = emptySet()
    }

    fun toggleSearchAlbum(album: Album) {
        selectedSearchAlbums = if (album in selectedSearchAlbums)
            selectedSearchAlbums - album else selectedSearchAlbums + album
    }

    fun toggleSearchTrack(uri: String) {
        selectedSearchTracks = if (uri in selectedSearchTracks)
            selectedSearchTracks - uri else selectedSearchTracks + uri
    }

    fun selectAllSearchAlbums() {
        selectedSearchAlbums = searchResult.albums.toSet()
    }

    fun selectAllSearchTracks() {
        selectedSearchTracks = searchResult.tracks.map { it.uri }.toSet()
    }

    fun deselectAllSearchAlbums() { selectedSearchAlbums = emptySet() }
    fun deselectAllSearchTracks() { selectedSearchTracks = emptySet() }

    val searchSelectionCount: Int
        get() = selectedSearchAlbums.size + selectedSearchTracks.size

    fun executeBatchAction(mode: String) {
        val albums = selectedSearchAlbums.toList()
        val trackUris = selectedSearchTracks.toList()
        exitSearchSelectionMode()
        val doIt: suspend () -> Unit = {
            if (mode == "replace") mpd.queueClear()
            val addMode = if (mode == "replace") "add" else mode
            for (album in albums) {
                mpd.addAlbum(album.albumArtist, album.album, addMode)
            }
            for (uri in trackUris) {
                mpd.addTrack(uri, addMode)
            }
            if (mode == "replace") mpd.resume()
        }
        if (mode == "replace" && shouldPromptPhone(doIt)) return
        viewModelScope.launch { try { doIt() } catch (_: Exception) {} }
    }

    // --- Playback ---

    fun togglePlay() {
        viewModelScope.launch {
            try {
                if (status?.state == "playing") mpd.pause() else mpd.resume()
                refresh()
            } catch (_: Exception) {}
        }
    }

    fun playNext() { viewModelScope.launch { try { mpd.next(); refresh() } catch (e: Exception) { android.util.Log.e("VM", "next: ${e.message}") } } }
    fun playPrev() { viewModelScope.launch { try { mpd.prev(); refresh() } catch (e: Exception) { android.util.Log.e("VM", "prev: ${e.message}") } } }
    fun stopPlayback() { viewModelScope.launch { try { mpd.stop() } catch (_: Exception) {} } }
    fun seek(pos: Double) { viewModelScope.launch { try { mpd.seek(pos) } catch (_: Exception) {} } }

    fun toggleRepeat() { viewModelScope.launch { try { mpd.cmd("repeat ${if (status?.repeat == true) "0" else "1"}") } catch (_: Exception) {} } }
    fun toggleRandom() { viewModelScope.launch { try { mpd.cmd("random ${if (status?.random == true) "0" else "1"}") } catch (_: Exception) {} } }
    fun toggleSingle() { viewModelScope.launch { try { mpd.cmd("single ${if (status?.single == true) "0" else "1"}") } catch (_: Exception) {} } }
    fun toggleConsume() { viewModelScope.launch { try { mpd.cmd("consume ${if (status?.consume == true) "0" else "1"}") } catch (_: Exception) {} } }
    fun cycleReplayGain() {
        viewModelScope.launch {
            try {
                val next = when (status?.replayGainMode) {
                    "track" -> "album"
                    "album" -> "off"
                    else -> "track"
                }
                mpd.cmd("replay_gain_mode $next")
            } catch (_: Exception) {}
        }
    }

    fun randomAlbum() {
        val doIt: suspend () -> Unit = {
            val allAlbums = mpd.getAllAlbums()
            if (allAlbums.isNotEmpty()) {
                val album = allAlbums.random()
                mpd.addAlbum(album.albumArtist, album.album, "replace")
            }
        }
        if (shouldPromptPhone(doIt)) return
        viewModelScope.launch { try { doIt() } catch (_: Exception) {} }
    }

    // --- Queue ---

    fun queuePlay(position: Int) { viewModelScope.launch { try { mpd.queuePlay(position) } catch (_: Exception) {} } }
    fun queueRemove(position: Int) { viewModelScope.launch { try { mpd.queueRemove(position) } catch (_: Exception) {} } }
    fun queueMove(from: Int, to: Int) { viewModelScope.launch { try { mpd.queueMove(from, to) } catch (_: Exception) {} } }
    fun queueClear() { viewModelScope.launch { try { mpd.queueClear() } catch (_: Exception) {} } }

    // --- Offline downloads ---

    private val downloadQueue = kotlinx.coroutines.channels.Channel<Album>(kotlinx.coroutines.channels.Channel.UNLIMITED)

    init {
        // Process download queue sequentially
        viewModelScope.launch {
            for (album in downloadQueue) {
                try {
                    val albumTracks = mpd.getTracks(album.albumArtist, album.album)
                    if (albumTracks.isEmpty()) continue
                    val prefs = MelodyApp.instance.getSharedPreferences("melody", android.content.Context.MODE_PRIVATE)
                    val format = prefs.getString("audio_format", "")?.ifBlank { null }
                    val bitrate = prefs.getInt("audio_bitrate", 0)
                    offline.downloadAlbum(album.id, album.albumArtist, album.album, album.date, albumTracks, mpd, format, bitrate) { progress ->
                        downloadProgress = progress
                    }
                    downloadProgress = null
                    downloadedAlbums = offline.getDownloadedAlbumIds()
                } catch (_: Exception) {}
            }
        }
    }

    fun downloadAlbum(album: Album) {
        downloadQueue.trySend(album)
    }

    fun removeOfflineAlbum(albumId: String) {
        offline.removeAlbum(albumId)
        downloadedAlbums = offline.getDownloadedAlbumIds()
    }

    fun isAlbumDownloaded(albumId: String): Boolean = offline.isAlbumDownloaded(albumId)

    fun toggleCachedOnly() {
        showCachedOnly = !showCachedOnly
        if (showCachedOnly) {
            loadCachedLibrary()
        } else {
            loadArtists()
        }
    }

    private fun cachedAlbumsWithFiles(): List<OfflineManager.DownloadedAlbumInfo> {
        return offline.getDownloadedAlbums()
            .filter { it.albumArtist.isNotBlank() && it.album.isNotBlank() }
            .filter { info -> info.tracks.any { offline.isSongDownloaded(it.songId) } }
    }

    private fun loadCachedLibrary() {
        val cached = cachedAlbumsWithFiles()
        artists = cached.map { it.albumArtist }.distinct().sorted()
        libView = LibView.Artists
    }

    fun loadCachedAlbums(artist: String) {
        curArtist = artist
        val cached = cachedAlbumsWithFiles().filter { it.albumArtist == artist }
        albums = cached.map { Album(it.albumId, it.albumArtist, it.album, it.date) }
            .sortedBy { it.date + it.album }
        libView = LibView.Albums
    }

    fun loadCachedTracks(album: Album) {
        curAlbum = album
        val cached = cachedAlbumsWithFiles().find { it.albumId == album.id }
        tracks = cached?.tracks?.filter { offline.isSongDownloaded(it.songId) }
            ?.sortedWith(compareBy({ it.disc }, { it.trackNumber }))
            ?: emptyList()
        libView = LibView.Tracks
    }

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
                val newDevices = mpd.getOutputs()
                if (newDevices.isNotEmpty()) {
                    devices = newDevices
                }
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

    /**
     * Check if we should prompt to switch to phone. Returns true (and shows dialog)
     * if on mobile data and phone agent isn't the active device.
     */
    private fun isPhoneAgent(dev: DeviceInfo): Boolean {
        return dev.type == "agent" && dev.name.contains("android", ignoreCase = true)
    }

    private fun shouldPromptPhone(action: suspend () -> Unit): Boolean {
        if (!MelodyApp.instance.isOnMobileData()) return false
        val active = devices.firstOrNull { it.active }
        if (active != null && isPhoneAgent(active)) return false
        pendingPhoneAction = action
        showPhonePrompt = true
        return true
    }

    fun phonePromptConfirm() {
        val action = pendingPhoneAction
        showPhonePrompt = false
        pendingPhoneAction = null
        viewModelScope.launch {
            // Stop playback before switching so the old track doesn't briefly play on phone
            try { mpd.stop() } catch (_: Exception) {}
            // Switch to phone first, then run the play action
            try {
                devices = mpd.getOutputs()
                val phoneDev = devices.find { isPhoneAgent(it) }
                if (phoneDev != null && !phoneDev.active) {
                    mpd.enableOutput(phoneDev.id)
                    delay(500)
                    devices = mpd.getOutputs()
                }
            } catch (e: Exception) {
                android.util.Log.e("MainViewModel", "Phone switch failed: ${e.message}")
            }
            try {
                action?.invoke()
            } catch (e: Exception) {
                android.util.Log.e("MainViewModel", "Pending action failed: ${e.message}")
            }
        }
    }

    fun phonePromptDismiss() {
        val action = pendingPhoneAction
        showPhonePrompt = false
        pendingPhoneAction = null
        viewModelScope.launch {
            try { action?.invoke() } catch (_: Exception) {}
        }
    }
}
