package com.melody.app

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext

enum class LibView { Artists, Albums, Tracks }

class MainViewModel : ViewModel() {
    private val mpd get() = MelodyApp.instance.mpd
    private val offline = MelodyApp.instance.offlineManager

    // Transient user-visible message (shown as a snackbar); null when nothing to show
    var toast by mutableStateOf<String?>(null); private set
    fun clearToast() { toast = null }

    // Runs a user action, surfacing failures instead of swallowing them.
    // CancellationException must propagate or cancelled jobs keep running.
    private fun runAction(label: String, block: suspend () -> Unit): Job =
        viewModelScope.launch {
            try {
                block()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                android.util.Log.e("MainViewModel", "$label failed", e)
                toast = "$label failed"
            }
        }

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
    private val devicesLoadMutex = Mutex()
    private var callbacksClient: MpdClient? = null

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
    // Serializes concurrent refresh() calls (poll, idle, reconnect, foreground)
    // so an older status/queue snapshot can't overwrite a newer one.
    // MUST be declared before the init block below: startPolling() can enter
    // refresh() synchronously (Main.immediate) during construction.
    private val refreshMutex = Mutex()
    private val mpdClientChangedHandler: (MpdClient) -> Unit = {
        attachMpdCallbacks()
        viewModelScope.launch {
            refresh(forceQueue = true)
            loadDevicesNow()
            retryPendingDownloads()
        }
    }

    init {
        downloadedAlbums = offline.getDownloadedAlbumIds()
        MelodyApp.instance.onMpdClientChanged = mpdClientChangedHandler
        startPolling()
        attachMpdCallbacks()
        loadArtists()
        loadDevices()
    }

    override fun onCleared() {
        if (MelodyApp.instance.onMpdClientChanged === mpdClientChangedHandler) {
            MelodyApp.instance.onMpdClientChanged = null
        }
        // Detach download observers — the pipeline keeps running app-scoped,
        // but must not hold a reference to this dead ViewModel.
        offline.onProgress = null
        offline.onDownloadedAlbumsChanged = null
        offline.onDownloadError = null
        super.onCleared()
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
    // Idle subsystems that arrived while a refresh was already running —
    // coalesced into a follow-up refresh instead of being dropped.
    // Main-thread confined (viewModelScope).
    private val pendingIdleChanged = mutableSetOf<String>()

    private fun handleIdleChanged(changed: Set<String>) {
        pendingIdleChanged.addAll(changed)
        if ("output" in changed) loadDevices()
        if (idleRefreshJob?.isActive == true) return
        idleRefreshJob = viewModelScope.launch {
            while (pendingIdleChanged.isNotEmpty()) {
                val batch = pendingIdleChanged.toSet()
                pendingIdleChanged.clear()
                refresh(forceQueue = "rating" in batch)
            }
        }
    }

    private fun attachMpdCallbacks() {
        val client = mpd
        if (callbacksClient === client) return
        callbacksClient = client
        client.onIdleNotification = { changed ->
            viewModelScope.launch { handleIdleChanged(changed) }
        }
        client.onReconnected = {
            isConnected = true
            viewModelScope.launch {
                refresh(forceQueue = true)
                loadDevicesNow()
                retryPendingDownloads()
            }
        }
        client.startIdle()
    }

    fun onForeground() {
        viewModelScope.launch {
            MelodyApp.instance.applyServerForCurrentNetwork()
            attachMpdCallbacks()
            if (!mpd.connected) {
                mpd.reconnectNow()
            }
            refresh(forceQueue = true)
            loadDevicesNow()
            retryPendingDownloads()
        }
    }

    private suspend fun refresh(forceQueue: Boolean = false) = refreshMutex.withLock {
        try {
            val newStatus = mpd.getStatus() ?: run {
                isConnected = mpd.connected
                return@withLock
            }
            isConnected = true
            status = newStatus
            if (newStatus.title.isNotBlank() || newStatus.artist.isNotBlank()) {
                lastPlayingStatus = newStatus
            } else if (newStatus.playlistLength == 0 || newStatus.state == "stopped") {
                // Queue emptied / playback fully stopped — drop the stale
                // snapshot so the mini player doesn't show a dead track.
                lastPlayingStatus = null
            }
            val curPos = newStatus.currentSongPos
            val plVersion = newStatus.playlistVersion
            if (plVersion != lastPlaylistVersion || queue.isEmpty() || forceQueue) {
                val newQueue = mpd.getQueue()
                val expectedLen = newStatus.playlistLength
                // Reject truncated responses: if status says N tracks but we got
                // fewer, the response was likely corrupted — keep the old queue.
                val valid = when {
                    forceQueue -> newQueue.isNotEmpty() || expectedLen == 0
                    expectedLen > 0 -> newQueue.size >= expectedLen
                    else -> true
                }
                if (valid) {
                    queue = newQueue.map { it.copy(current = it.position == curPos) }
                    lastPlaylistVersion = plVersion
                }
            } else {
                queue = queue.map { it.copy(current = it.position == curPos) }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (_: Exception) {}
        currentTrackOffline = PlaybackService.instance?.isCurrentTrackOffline ?: false
        codecInfo = PlaybackService.instance?.codecInfo ?: ""
        // Fetch lyrics when track changes; clear them when nothing is current
        val curUri = queue.firstOrNull { it.current }?.uri ?: ""
        if (curUri.isBlank()) {
            if (lyricsForUri.isNotBlank()) {
                lyricsForUri = ""
                lyrics = null
            }
        } else if (curUri != lyricsForUri) {
            lyricsForUri = curUri
            lyrics = null
            viewModelScope.launch {
                val result = try { mpd.getLyrics(curUri) } catch (_: Exception) { null }
                // Guard against slow responses landing after another track change
                if (lyricsForUri == curUri) lyrics = result
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
                            // A transient failure returns null — keep the last
                            // status and keep polling instead of blanking the
                            // now-playing UI and killing the loop mid-song.
                            val s = mpd.getStatus()
                            if (s != null) {
                                status = s
                                if (s.title.isNotBlank() || s.artist.isNotBlank()) {
                                    lastPlayingStatus = s
                                }
                                if (s.state != "playing") break
                            }
                            codecInfo = PlaybackService.instance?.codecInfo ?: ""
                        } catch (e: CancellationException) {
                            throw e
                        } catch (_: Exception) {}
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
        savedAlbumScrollIndex = 0
        savedAlbumScrollOffset = 0
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
        // A different artist's album list must not restore the previous
        // artist's scroll position.
        if (artist != curArtist) {
            savedAlbumScrollIndex = 0
            savedAlbumScrollOffset = 0
        }
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
        runAction("Add to queue") { doIt() }
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
        runAction("Rating") {
            mpd.rateTrack(songId, rating)
            // Update queue item locally for immediate UI feedback
            queue = queue.map { if (it.songId == songId) it.copy(rating = rating) else it }
            refresh()
        }
    }

    fun rateAlbum(rating: Int) {
        val album = curAlbum ?: return
        runAction("Album rating") {
            mpd.rateAlbum(album.albumArtist, album.album, album.date, rating)
            val r = mpd.getAlbumRating(album.albumArtist, album.album, album.date)
            albumRating = r.rating
            albumComputedRating = r.computed
        }
    }

    fun rateAlbumDirect(album: Album, rating: Int) {
        runAction("Album rating") { mpd.rateAlbum(album.albumArtist, album.album, album.date, rating) }
    }

    fun rateQueueTrack(songId: String, rating: Int) {
        runAction("Rating") {
            mpd.rateTrack(songId, rating)
            // Update queue item locally for immediate UI feedback
            queue = queue.map { if (it.songId == songId) it.copy(rating = rating) else it }
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
        runAction("Add to queue") { doIt() }
    }

    // --- Playback ---

    fun togglePlay() {
        runAction("Play/pause") {
            if (status?.state == "playing") mpd.pause() else mpd.cmd("play")
            refresh()
        }
    }

    fun playNext() { runAction("Next") { mpd.next(); refresh() } }
    fun playPrev() { runAction("Previous") { mpd.prev(); refresh() } }
    fun stopPlayback() { runAction("Stop") { mpd.stop() } }
    fun seek(pos: Double) { runAction("Seek") { mpd.seek(pos) } }
    fun setVolume(volume: Int) { runAction("Volume") { mpd.setVolume(volume) } }

    fun toggleRepeat() { runAction("Repeat") { mpd.cmd("repeat ${if (status?.repeat == true) "0" else "1"}") } }
    fun toggleRandom() { runAction("Random") { mpd.cmd("random ${if (status?.random == true) "0" else "1"}") } }
    fun toggleSingle() { runAction("Single") { mpd.cmd("single ${if (status?.single == true) "0" else "1"}") } }
    fun toggleConsume() { runAction("Consume") { mpd.cmd("consume ${if (status?.consume == true) "0" else "1"}") } }
    fun setReplayGain(mode: String) { runAction("ReplayGain") { mpd.cmd("replay_gain_mode $mode"); refresh() } }
    fun cycleReplayGain() {
        val next = when (status?.replayGainMode) {
            "track" -> "album"
            "album" -> "off"
            else -> "track"
        }
        setReplayGain(next)
    }

    // Tap-to-play: replace the queue with the visible track list and start at
    // the tapped track — the standard music-app gesture.
    fun playTrackInContext(tracks: List<Track>, index: Int) {
        val uris = tracks.map { it.uri }.filter { it.isNotBlank() }
        if (uris.isEmpty()) return
        val doIt: suspend () -> Unit = { mpd.replaceQueueWithTracks(uris, index) }
        if (shouldPromptPhone(doIt)) return
        runAction("Play") { doIt() }
    }

    fun playAlbum(album: Album) {
        val doIt: suspend () -> Unit = { mpd.addAlbum(album.albumArtist, album.album, "replace") }
        if (shouldPromptPhone(doIt)) return
        runAction("Play album") { doIt() }
    }

    fun playAlbumShuffled(album: Album) {
        val doIt: suspend () -> Unit = { mpd.playAlbumShuffled(album.albumArtist, album.album) }
        if (shouldPromptPhone(doIt)) return
        runAction("Shuffle album") { doIt() }
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
        runAction("Random album") { doIt() }
    }

    // --- Queue ---

    fun queuePlay(position: Int) { runAction("Play") { mpd.queuePlay(position) } }
    fun queueRemove(position: Int) { runAction("Remove") { mpd.queueRemove(position) } }
    fun queueMove(from: Int, to: Int): Job = runAction("Move") { mpd.queueMove(from, to) }

    // Drag-reorder: apply the move to the local list immediately so the row
    // follows the finger; the server round-trip confirms it later. Without
    // this, `position` fields lag a full round-trip behind and the dragged
    // row's highlight jumps to the wrong item.
    fun queueMoveOptimistic(from: Int, to: Int) {
        val list = queue.toMutableList()
        val idx = list.indexOfFirst { it.position == from }
        if (idx >= 0) {
            val item = list.removeAt(idx)
            list.add(to.coerceIn(0, list.size), item)
            queue = list.mapIndexed { i, entry -> entry.copy(position = i) }
        }
        runAction("Move") { mpd.queueMove(from, to) }
    }
    fun queueClear() { runAction("Clear queue") { mpd.queueClear() } }
    fun queueShuffle() { runAction("Shuffle") { mpd.queueShuffle(); refresh(forceQueue = true) } }
    fun saveQueueAsPlaylist(name: String) {
        runAction("Save playlist") {
            mpd.saveQueueAsPlaylist(name)
            toast = "Saved as \"$name\""
        }
    }
    fun setQueuePriority(position: Int, priority: Int) {
        runAction("Priority") { mpd.setPriority(position, priority); refresh(forceQueue = true) }
    }
    fun updateLibrary() {
        runAction("Library update") {
            mpd.updateLibrary()
            toast = "Library update started"
        }
    }

    // --- Offline downloads ---
    // The pipeline itself lives in OfflineManager (application scope) so
    // downloads survive the UI being destroyed; the ViewModel only observes.

    init {
        offline.onProgress = { progress -> downloadProgress = progress }
        offline.onDownloadedAlbumsChanged = { ids -> downloadedAlbums = ids }
        offline.onDownloadError = { message -> toast = message }
    }

    fun downloadAlbum(album: Album) {
        offline.enqueueAlbum(album)
    }

    private fun retryPendingDownloads() {
        if (mpd.connected) offline.retryPending()
    }

    fun removeOfflineAlbum(albumId: String) {
        offline.cancelAndRemoveAlbum(albumId)
        downloadedAlbums = downloadedAlbums - albumId
    }

    // Uses the in-memory set (reconciled with disk at startup) — checking the
    // filesystem per call would run disk IO on the main thread for every row.
    fun isAlbumDownloaded(albumId: String): Boolean = albumId in downloadedAlbums

    fun toggleCachedOnly() {
        showCachedOnly = !showCachedOnly
        if (showCachedOnly) {
            loadCachedLibrary()
        } else {
            loadArtists()
        }
    }

    // Reads meta files and stats audio files — must run on Dispatchers.IO.
    private fun cachedAlbumsWithFiles(): List<OfflineManager.DownloadedAlbumInfo> {
        return offline.getDownloadedAlbums()
            .filter { it.albumArtist.isNotBlank() && it.album.isNotBlank() }
            .filter { info -> info.tracks.any { offline.isSongDownloaded(it.songId) } }
    }

    private fun loadCachedLibrary() {
        viewModelScope.launch {
            val cached = withContext(Dispatchers.IO) { cachedAlbumsWithFiles() }
            artists = cached.map { it.albumArtist }.distinct().sorted()
            libView = LibView.Artists
        }
    }

    fun loadCachedAlbums(artist: String) {
        viewModelScope.launch {
            curArtist = artist
            val cached = withContext(Dispatchers.IO) {
                cachedAlbumsWithFiles().filter { it.albumArtist == artist }
            }
            albums = cached.map { Album(it.albumId, it.albumArtist, it.album, it.date) }
                .sortedBy { it.date + it.album }
            libView = LibView.Albums
        }
    }

    fun loadCachedTracks(album: Album) {
        viewModelScope.launch {
            curAlbum = album
            tracks = withContext(Dispatchers.IO) {
                cachedAlbumsWithFiles().find { it.albumId == album.id }
                    ?.tracks?.filter { offline.isSongDownloaded(it.songId) }
                    ?.sortedWith(compareBy({ it.disc }, { it.trackNumber }))
                    ?: emptyList()
            }
            libView = LibView.Tracks
        }
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
        runAction("Add to playlist") { mpd.addToPlaylist(playlistName, uri) }
        showPlaylistPicker = false
        playlistPickerUri = ""
    }

    fun dismissPlaylistPicker() {
        showPlaylistPicker = false
        playlistPickerUri = ""
    }

    // --- Playlists ---

    fun loadPlaylists(resetView: Boolean = true) {
        viewModelScope.launch {
            try { playlists = mpd.getPlaylists() } catch (_: Exception) {}
            // Re-tapping the Playlists tab refreshes the list without kicking
            // the user out of an open playlist.
            if (resetView) playlistView = false
        }
    }

    fun deletePlaylist(playlist: PlaylistInfo) {
        runAction("Delete playlist") {
            mpd.deletePlaylist(playlist.name)
            if (curPlaylist?.name == playlist.name) playlistBack()
            playlists = mpd.getPlaylists()
            toast = "Deleted \"${playlist.name}\""
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
            loadDevicesNow()
        }
    }

    private suspend fun loadDevicesNow() {
        devicesLoadMutex.withLock {
            try {
                val newDevices = mpd.getOutputs()
                devices = newDevices
                isConnected = true
                android.util.Log.d("MainViewModel", "loadDevices: ${devices.size} devices: ${devices.map { "${it.name}(${it.type})" }}")
            } catch (e: Exception) {
                isConnected = mpd.connected
                android.util.Log.e("MainViewModel", "loadDevices failed: ${e.message}")
            }
        }
    }

    // Toggle a single output on/off without touching the others.
    fun toggleDevice(id: String) {
        runAction("Toggle output") {
            mpd.toggleOutput(id)
            delay(300)
            devices = mpd.getOutputs()
        }
    }

    /**
     * Check if we should prompt to switch to phone. Returns true (and shows dialog)
     * if on mobile data and the phone agent isn't among the enabled outputs.
     */
    private fun isPhoneAgent(dev: DeviceInfo): Boolean {
        return dev.type == "agent" && dev.name.contains("android", ignoreCase = true)
    }

    private fun shouldPromptPhone(action: suspend () -> Unit): Boolean {
        if (!MelodyApp.instance.isOnMobileData()) return false
        if (devices.any { it.active && isPhoneAgent(it) }) return false
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
            // Move playback to the phone with plain MPD toggles: enable the
            // phone output, then disable the others — leaving the house must
            // take playback over, not add the phone to the speakers at home.
            try {
                devices = mpd.getOutputs()
                val phoneDev = devices.find { isPhoneAgent(it) }
                if (phoneDev != null && !(phoneDev.active && devices.count { it.active } == 1)) {
                    if (!phoneDev.active) mpd.enableOutput(phoneDev.id)
                    devices.filter { it.active && it.id != phoneDev.id }
                        .forEach { mpd.disableOutput(it.id) }
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

    // Dismissing the prompt (tap outside / back) cancels the action entirely —
    // it must not silently start playback on the remote device.
    fun phonePromptDismiss() {
        showPhonePrompt = false
        pendingPhoneAction = null
    }

    // Explicit "keep current device" choice: run the action without switching.
    fun phonePromptPlayOnCurrent() {
        val action = pendingPhoneAction
        showPhonePrompt = false
        pendingPhoneAction = null
        runAction("Play") { action?.invoke() }
    }
}
