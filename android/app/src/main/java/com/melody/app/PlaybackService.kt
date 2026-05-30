package com.melody.app

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import okhttp3.*
import java.util.concurrent.TimeUnit

/**
 * Queue item synced from server via playlistinfo.
 */
private data class QueueEntry(
    val position: Int,
    val file: String,       // relative path (e.g. "Artist/Album/Track.flac")
    val songId: String,     // X-SongId from DB
    val title: String,
    val artist: String,
    val album: String,
    val albumId: String,
    val duration: Double,
    val rgTrack: Double,
    val rgAlbum: Double
)

@androidx.annotation.OptIn(androidx.media3.common.util.UnstableApi::class)
class PlaybackService : Service() {
    private var player: ExoPlayer? = null
    private var mediaSession: MediaSession? = null
    private var agentWs: WebSocket? = null
    private var agentJob: Job? = null
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    // Queue state (synced from server)
    @Volatile private var queue: List<QueueEntry> = emptyList()
    @Volatile private var curPos: Int = -1
    @Volatile private var pendingNextPos: Int = -1

    // Track which playlist indices are playing from offline cache
    private val offlineIndexes = mutableSetOf<Int>()
    // Offset for transcoded streams where ExoPlayer position 0 = this offset in the real track
    @Volatile private var streamStartOffset: Double = 0.0

    @Volatile var codecInfo: String = ""
        private set
    @Volatile private var replaygainMode: String = "off"
    @Volatile private var rgTrackGain: Double = 0.0
    @Volatile private var rgAlbumGain: Double = 0.0
    @Volatile private var userVolume: Float = 1.0f

    // State reporter job
    private var stateReporterJob: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this

        val channelId = "melody_service"
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(channelId) == null) {
            nm.createNotificationChannel(
                NotificationChannel(channelId, "Melody", NotificationManager.IMPORTANCE_LOW).apply {
                    description = "Keeps Melody connected for device discovery"
                }
            )
        }
        val notification = NotificationCompat.Builder(this, channelId)
            .setContentTitle("Melody")
            .setContentText("Connected")
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK)
        } else {
            startForeground(1, notification)
        }

        player = ExoPlayer.Builder(applicationContext).build().also { p ->
            p.playWhenReady = false
            p.addListener(object : Player.Listener {
                override fun onPlaybackStateChanged(state: Int) {
                    if (state == Player.STATE_READY) updateCodecInfo()
                    if (state == Player.STATE_ENDED && p.mediaItemCount <= 1) {
                        // Only advance on STATE_ENDED when there's no next item to
                        // auto-transition to. With a preloaded next track,
                        // onMediaItemTransition(REASON_AUTO) handles it instead.
                        sendAgentAdvance()
                    }
                }
                override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                    if (reason == Player.MEDIA_ITEM_TRANSITION_REASON_AUTO) {
                        val oldPos = curPos
                        streamStartOffset = 0.0
                        // The preloaded next track is now playing — advance curPos.
                        // Server will send the definitive position via the next play command.
                        curPos = pendingNextPos
                        // Update replay gain for the new track
                        val q = queue
                        if (curPos in 0 until q.size) {
                            val entry = q.firstOrNull { it.position == curPos }
                            if (entry != null) {
                                rgTrackGain = entry.rgTrack
                                rgAlbumGain = entry.rgAlbum
                                applyReplayGain(p)
                            }
                        }
                        sendToServer("agent_advance $oldPos")
                    }
                    updateCodecInfo()
                }
                override fun onPlayerError(error: androidx.media3.common.PlaybackException) {
                    android.util.Log.e("PlaybackService", "ExoPlayer error: ${error.errorCodeName} — ${error.message}")
                }
            })
            mediaSession = MediaSession.Builder(this, p).setId("melody").build()
        }
        val prefs = getSharedPreferences("melody", MODE_PRIVATE)
        replaygainMode = prefs.getString("replaygain", "off") ?: "off"
        registerAgent()
    }

    private fun sendAgentAdvance() {
        val pos = curPos
        sendToServer("agent_advance $pos")
    }

    private fun sendToServer(msg: String) {
        val ws = agentWs ?: return
        ws.send("$msg\n")
    }

    private fun updateCodecInfo() {
        val p = player ?: return
        val format = p.audioFormat
        if (format == null) {
            codecInfo = ""
            return
        }
        val codec = format.sampleMimeType?.removePrefix("audio/") ?: ""
        val sr = format.sampleRate
        val srText = if (sr > 0) " ${sr / 1000}kHz" else ""
        codecInfo = "$codec$srText"
    }

    fun registerAgent() {
        val app = MelodyApp.instance
        if (!app.mpd.isConfigured) return

        agentWs?.close(1000, "reconnecting")
        agentJob?.cancel()
        stateReporterJob?.cancel()

        agentJob = scope.launch {
            val prefs = getSharedPreferences("melody", MODE_PRIVATE)
            val name = prefs.getString("device_name", null)
                ?: "android-${android.os.Build.MODEL}".replace(" ", "-").lowercase()
            val format = prefs.getString("audio_format", "") ?: ""
            val bitrate = prefs.getInt("audio_bitrate", 0)

            connectAgentLoop(name, format, bitrate)
        }
    }

    fun reconnect() {
        registerAgent()
    }

    private suspend fun connectAgentLoop(name: String, format: String, bitrate: Int) {
        while (true) {
            try {
                connectAgent(name, format, bitrate)
            } catch (e: Exception) {
                android.util.Log.e("PlaybackService", "Agent connection error: ${e.message}")
            } finally {
                stateReporterJob?.cancel()
                withContext(Dispatchers.Main) {
                    val p = player
                    if (p != null && !isCurrentTrackOffline) {
                        p.stop()
                        p.clearMediaItems()
                        offlineIndexes.clear()
                    }
                }
            }
            delay(5000)
        }
    }

    private suspend fun connectAgent(name: String, format: String, bitrate: Int) {
        val app = MelodyApp.instance
        val host = app.mpd.serverHost
        val port = app.mpd.serverPort
        val wsScheme = if (app.mpd.useSSL) "wss" else "ws"
        val wsUrl = "$wsScheme://$host:$port/mpd"
        android.util.Log.d("PlaybackService", "Connecting agent WebSocket to $wsUrl")

        val lineChannel = Channel<String>(Channel.UNLIMITED)
        val connected = CompletableDeferred<Boolean>()

        val client = OkHttpClient.Builder()
            .readTimeout(0, TimeUnit.SECONDS)
            .pingInterval(30, TimeUnit.SECONDS)
            .build()
        val request = Request.Builder().url(wsUrl).build()

        val ws = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {}

            override fun onMessage(webSocket: WebSocket, text: String) {
                text.lines().filter { it.isNotEmpty() }.forEach { line ->
                    lineChannel.trySend(line)
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                val code = response?.code?.let { " response=$it" } ?: ""
                android.util.Log.e("PlaybackService", "Agent WebSocket error for $wsUrl:$code ${t.message}")
                connected.complete(false)
                lineChannel.close()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                lineChannel.close()
            }
        })

        agentWs = ws

        val greeting = lineChannel.receive()
        if (!greeting.startsWith("OK MPD")) {
            ws.close(1000, "bad greeting")
            return
        }

        // Register as v2 autonomous agent
        val regCmd = buildString {
            append("agent_register ${mpdQuote(name)} v2")
            if (format.isNotBlank()) append(" format=${mpdQuote(format)}")
            if (bitrate > 0) append(" max_bitrate=$bitrate")
        }
        ws.send("$regCmd\n")

        val regResp = lineChannel.receive()
        if (regResp != "OK") {
            android.util.Log.e("PlaybackService", "Agent register failed: $regResp")
            ws.close(1000, "register failed")
            return
        }

        // Start periodic state reporter
        stateReporterJob = scope.launch {
            while (true) {
                delay(2000)
                reportState()
            }
        }

        // If resume-on-connect is enabled, tell server to unpause via the MpdClient
        val prefs = getSharedPreferences("melody", MODE_PRIVATE)
        if (prefs.getBoolean("resume_on_connect", false)) {
            scope.launch {
                try {
                    MelodyApp.instance.mpd.resume()
                } catch (_: Exception) {}
            }
        }

        // Command loop
        for (line in lineChannel) {
            if (line.isBlank()) continue
            val response = handleAgentCommand(line)
            ws.send("$response\n")
        }
    }

    // ---- v2 command handling ----

    private suspend fun handleAgentCommand(line: String): String {
        val parts = parseCommand(line)
        val cmd = parts.first
        val args = parts.second

        if (cmd == "ping") return "OK"

        return when (cmd) {
            "play" -> handlePlay(args)
            "preload" -> handlePreload(args)
            "pause" -> {
                withContext(Dispatchers.Main) { player?.pause() }
                "OK"
            }
            "resume" -> {
                withContext(Dispatchers.Main) {
                    val p = player ?: return@withContext
                    if (p.playbackState == Player.STATE_IDLE && p.mediaItemCount > 0) p.prepare()
                    p.play()
                }
                "OK"
            }
            "stop" -> {
                withContext(Dispatchers.Main) {
                    player?.stop()
                    player?.clearMediaItems()
                }
                curPos = -1
                "OK"
            }
            "seek" -> {
                if (args.isEmpty()) return "ACK [2@0] {seek} missing seconds"
                val secs = args[0].toDoubleOrNull() ?: 0.0
                withContext(Dispatchers.Main) {
                    val p = player ?: return@withContext
                    val currentUri = p.currentMediaItem?.localConfiguration?.uri?.toString() ?: ""
                    if (isTranscodedUrl(currentUri)) {
                        streamStartOffset = secs
                        reloadWithOffset(p, currentUri, secs)
                    } else {
                        streamStartOffset = 0.0
                        p.seekTo((secs * 1000).toLong())
                    }
                }
                "OK"
            }
            "volume" -> {
                if (args.isEmpty()) return "ACK [2@0] {volume} missing level"
                val vol = args[0].toDoubleOrNull() ?: 100.0
                userVolume = (vol / 100.0).toFloat().coerceIn(0f, 1f)
                withContext(Dispatchers.Main) { player?.let { applyReplayGain(it) } }
                "OK"
            }
            "replaygain" -> {
                if (args.isEmpty()) return "ACK [2@0] {replaygain} missing mode"
                replaygainMode = args[0]
                withContext(Dispatchers.Main) { player?.let { applyReplayGain(it) } }
                "OK"
            }
            "queue_changed" -> {
                try {
                    syncQueue()
                } catch (e: Exception) {
                    return "ACK [56@0] {queue_changed} ${e.message}"
                }
                "OK"
            }
            "get_property" -> {
                if (args.isEmpty()) return "ACK [2@0] {get_property} missing name"
                handleGetProperty(args[0])
            }
            "set_property" -> {
                if (args.size < 2) return "ACK [2@0] {set_property} missing arguments"
                handleSetProperty(args[0], args[1])
            }
            else -> "ACK [5@0] {$cmd} unknown command"
        }
    }

    private suspend fun handlePlay(args: List<String>): String {
        if (args.isEmpty()) return "ACK [2@0] {play} missing queue position"

        val pos = args[0].toIntOrNull() ?: return "ACK [2@0] {play} invalid position"

        var nextPos = -1
        var seekPos = -1.0
        for (arg in args.drop(1)) {
            when {
                arg.startsWith("next=") -> nextPos = arg.removePrefix("next=").toIntOrNull() ?: -1
                arg.startsWith("seek=") -> seekPos = arg.removePrefix("seek=").toDoubleOrNull() ?: -1.0
            }
        }

        val q = queue
        if (pos < 0 || pos >= q.size) return "ACK [50@0] {play} position $pos out of range"

        val item = q[pos]
        val url = resolveStreamUrl(item) ?: return "ACK [50@0] {play} cannot resolve URL for position $pos"

        return withContext(Dispatchers.Main) {
            val p = player ?: return@withContext "ACK [56@0] {play} player not initialized"

            offlineIndexes.clear()
            val songId = item.songId
            val localPath = if (songId.isNotBlank()) MelodyApp.instance.offlineManager.getLocalPath(songId) else null
            val resolvedUrl = localPath ?: url
            val isOffline = localPath != null

            streamStartOffset = 0.0
            p.stop()
            p.clearMediaItems()

            // Build media items list: current + optional next
            val mediaItems = mutableListOf<MediaItem>()
            val startPositionMs: Long

            if (seekPos > 0 && isTranscodedUrl(resolvedUrl)) {
                // Transcoded streams: seek via URL parameter
                streamStartOffset = seekPos
                mediaItems.add(mediaItemFor(item, urlWithStart(resolvedUrl, seekPos)))
                startPositionMs = C.TIME_UNSET
            } else {
                mediaItems.add(mediaItemFor(item, resolvedUrl))
                startPositionMs = if (seekPos > 0) (seekPos * 1000).toLong() else C.TIME_UNSET
            }
            if (isOffline) offlineIndexes.add(0)

            // Preload next track
            if (nextPos in 0 until q.size) {
                val nextItem = q[nextPos]
                val nextLocalPath = if (nextItem.songId.isNotBlank()) MelodyApp.instance.offlineManager.getLocalPath(nextItem.songId) else null
                val nextUrl = nextLocalPath ?: resolveStreamUrl(nextItem)
                if (nextUrl != null) {
                    if (nextLocalPath != null) offlineIndexes.add(1)
                    mediaItems.add(mediaItemFor(nextItem, nextUrl))
                }
            }

            // Apply replay gain for this track
            rgTrackGain = item.rgTrack
            rgAlbumGain = item.rgAlbum
            applyReplayGain(p)

            // Use setMediaItems with start position — ExoPlayer respects this
            // before prepare, unlike seekTo which can be ignored
            p.setMediaItems(mediaItems, 0, startPositionMs)
            p.prepare()
            p.play()

            curPos = pos
            pendingNextPos = nextPos

            "OK"
        }
    }

    private suspend fun handlePreload(args: List<String>): String {
        if (args.isEmpty()) return "ACK [2@0] {preload} missing queue position"

        val pos = args[0].toIntOrNull() ?: return "ACK [2@0] {preload} invalid position"
        val q = queue
        if (pos < 0 || pos >= q.size) return "ACK [50@0] {preload} position $pos out of range"

        val item = q[pos]
        val localPath = if (item.songId.isNotBlank()) MelodyApp.instance.offlineManager.getLocalPath(item.songId) else null
        val url = localPath ?: resolveStreamUrl(item) ?: return "ACK [50@0] {preload} cannot resolve URL for position $pos"

        return withContext(Dispatchers.Main) {
            val p = player ?: return@withContext "ACK [56@0] {preload} player not initialized"

            // Remove finished items before current and preloaded items after current
            val cur = p.currentMediaItemIndex
            // Remove items after current first
            while (p.mediaItemCount > cur + 1) {
                p.removeMediaItem(p.mediaItemCount - 1)
            }
            // Remove finished items before current
            if (cur > 0) {
                for (i in 0 until cur) {
                    p.removeMediaItem(0)
                }
                offlineIndexes.clear()
            }

            p.addMediaItem(mediaItemFor(item, url))

            "OK"
        }
    }

    private suspend fun handleGetProperty(name: String): String {
        return withContext(Dispatchers.Main) {
            val p = player ?: return@withContext "ACK [56@0] {get_property} player not initialized"
            val value = when (name) {
                "pause" -> (!p.playWhenReady || p.playbackState == Player.STATE_IDLE).toString()
                "time-pos" -> (p.currentPosition / 1000.0 + streamStartOffset).toString()
                "duration" -> {
                    val d = p.duration
                    if (d != C.TIME_UNSET) (d / 1000.0 + streamStartOffset).toString() else "0.0"
                }
                "volume" -> (userVolume * 100).toInt().toString()
                else -> return@withContext "ACK [56@0] {get_property} unknown property: $name"
            }
            "value: $value\nOK"
        }
    }

    private suspend fun handleSetProperty(name: String, value: String): String {
        return withContext(Dispatchers.Main) {
            val p = player ?: return@withContext "ACK [56@0] {set_property} player not initialized"
            when (name) {
                "pause" -> {
                    if (value == "true" || value == "1" || value == "yes") {
                        p.pause()
                    } else {
                        if (p.playbackState == Player.STATE_IDLE && p.mediaItemCount > 0) p.prepare()
                        p.play()
                    }
                }
                "time-pos" -> {
                    val secs = value.toDoubleOrNull() ?: 0.0
                    val currentUri = p.currentMediaItem?.localConfiguration?.uri?.toString() ?: ""
                    if (isTranscodedUrl(currentUri)) {
                        streamStartOffset = secs
                        reloadWithOffset(p, currentUri, secs)
                    } else {
                        streamStartOffset = 0.0
                        p.seekTo((secs * 1000).toLong())
                    }
                }
                "volume" -> {
                    val vol = value.toDoubleOrNull() ?: 100.0
                    userVolume = (vol / 100.0).toFloat().coerceIn(0f, 1f)
                    applyReplayGain(p)
                }
                "replaygain" -> {
                    replaygainMode = value
                    applyReplayGain(p)
                }
            }
            "OK"
        }
    }

    // ---- State reporting ----

    private suspend fun reportState() {
        val msg = withContext(Dispatchers.Main) {
            val p = player ?: return@withContext null
            val st = when {
                p.playbackState == Player.STATE_IDLE || p.mediaItemCount == 0 -> "stop"
                !p.playWhenReady -> "pause"
                else -> "play"
            }
            val e = p.currentPosition / 1000.0 + streamStartOffset
            val d = if (p.duration != C.TIME_UNSET) p.duration / 1000.0 + streamStartOffset else 0.0
            val v = userVolume * 100.0
            String.format(java.util.Locale.US, "agent_state %s %d %.3f %.3f %.0f", st, curPos, e, d, v)
        }
        if (msg != null) sendToServer(msg)
    }

    // ---- Queue sync ----

    /**
     * Sync queue by opening a dedicated short-lived WebSocket connection.
     * This avoids deadlocks: the agent command loop can't use MpdClient
     * because MpdClient may be blocked waiting for a server response that
     * itself is blocked waiting for this agent to respond.
     */
    private suspend fun syncQueue() {
        val app = MelodyApp.instance
        val host = app.mpd.serverHost
        val port = app.mpd.serverPort
        val wsScheme = if (app.mpd.useSSL) "wss" else "ws"
        val wsUrl = "$wsScheme://$host:$port/mpd"
        android.util.Log.d("PlaybackService", "Syncing queue via $wsUrl")

        val linesCh = Channel<String>(Channel.UNLIMITED)
        val client = OkHttpClient.Builder()
            .connectTimeout(5, TimeUnit.SECONDS)
            .readTimeout(10, TimeUnit.SECONDS)
            .build()
        val request = Request.Builder().url(wsUrl).build()

        val ws = client.newWebSocket(request, object : WebSocketListener() {
            override fun onMessage(webSocket: WebSocket, text: String) {
                text.split('\n').filter { it.isNotEmpty() }.forEach { linesCh.trySend(it) }
            }
            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                linesCh.close()
            }
            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                linesCh.close()
            }
        })

        try {
            withTimeout(10000) {
                // Read greeting
                val greeting = linesCh.receive()
                if (!greeting.startsWith("OK MPD")) {
                    throw Exception("bad greeting: $greeting")
                }

                // Send playlistinfo
                ws.send("playlistinfo\n")

                // Read response
                val items = mutableListOf<QueueEntry>()
                var current = mutableMapOf<String, String>()

                while (true) {
                    val line = linesCh.receive()
                    if (line == "OK") break
                    if (line.startsWith("ACK")) throw Exception(line)

                    val idx = line.indexOf(": ")
                    if (idx < 0) continue
                    val k = line.substring(0, idx)
                    val v = line.substring(idx + 2)

                    if (k == "file" && current.isNotEmpty()) {
                        items.add(parseQueueEntry(current))
                        current = mutableMapOf()
                    }
                    current[k] = v
                }
                if (current.containsKey("file")) {
                    items.add(parseQueueEntry(current))
                }

                queue = items
            }
        } finally {
            ws.close(1000, "done")
            client.dispatcher.executorService.shutdown()
        }
    }

    private fun parseQueueEntry(kv: Map<String, String>): QueueEntry {
        return QueueEntry(
            position = kv["Pos"]?.toIntOrNull() ?: 0,
            file = kv["file"] ?: "",
            songId = kv["X-SongId"] ?: "",
            title = kv["Title"] ?: "",
            artist = kv["Artist"] ?: kv["AlbumArtist"] ?: "",
            album = kv["Album"] ?: "",
            albumId = kv["X-AlbumId"] ?: "",
            duration = kv["duration"]?.toDoubleOrNull() ?: kv["Time"]?.toDoubleOrNull() ?: 0.0,
            rgTrack = kv["X-ReplayGainTrack"]?.toDoubleOrNull() ?: 0.0,
            rgAlbum = kv["X-ReplayGainAlbum"]?.toDoubleOrNull() ?: 0.0
        )
    }

    // ---- URL resolution ----

    private fun resolveStreamUrl(item: QueueEntry): String? {
        if (item.songId.isBlank()) return null

        // Check offline cache first
        val localPath = MelodyApp.instance.offlineManager.getLocalPath(item.songId)
        if (localPath != null) return localPath

        // Build HTTP stream URL
        val mpd = MelodyApp.instance.mpd
        return "${mpd.httpBaseUrl}/stream/${item.songId}"
    }

    private fun mediaItemFor(item: QueueEntry, url: String): MediaItem {
        val metadata = MediaMetadata.Builder()
            .setTitle(item.title.ifBlank { item.file.substringAfterLast('/') })
            .setArtist(item.artist)
            .setAlbumTitle(item.album)
            .setArtworkUri(coverUri(item.albumId))
            .build()
        return MediaItem.Builder()
            .setUri(url)
            .setMediaId(item.songId.ifBlank { item.file })
            .setMediaMetadata(metadata)
            .build()
    }

    private fun coverUri(albumId: String): Uri? {
        if (albumId.isBlank()) return null
        return Uri.parse("${MelodyApp.instance.mpd.httpBaseUrl}/cover/$albumId?size=512")
    }

    // ---- Transcoding helpers ----

    private fun isTranscodedUrl(url: String): Boolean {
        val uri = android.net.Uri.parse(url)
        return !uri.getQueryParameter("format").isNullOrBlank() ||
               (uri.getQueryParameter("max_bitrate")?.toIntOrNull() ?: 0) > 0
    }

    private fun reloadWithOffset(p: ExoPlayer, currentUri: String, seekSecs: Double) {
        val newUrl = urlWithStart(currentUri, seekSecs)
        val idx = p.currentMediaItemIndex
        val wasPlaying = p.playWhenReady

        val items = mutableListOf<MediaItem>()
        for (i in 0 until p.mediaItemCount) {
            if (i == idx) {
                items.add(p.getMediaItemAt(i).buildUpon().setUri(newUrl).build())
            } else {
                items.add(p.getMediaItemAt(i))
            }
        }
        p.stop()
        p.clearMediaItems()
        p.setMediaItems(items, idx, C.TIME_UNSET)
        p.prepare()
        if (wasPlaying) p.play()
    }

    private fun urlWithStart(url: String, startSecs: Double): String {
        val uri = android.net.Uri.parse(url)
        val builder = uri.buildUpon().clearQuery()
        uri.queryParameterNames.forEach { key ->
            if (key != "start") builder.appendQueryParameter(key, uri.getQueryParameter(key))
        }
        if (startSecs > 0) {
            builder.appendQueryParameter("start", String.format(java.util.Locale.US, "%.3f", startSecs))
        }
        return builder.build().toString()
    }

    val isCurrentTrackOffline: Boolean
        get() {
            val p = player ?: return false
            return offlineIndexes.contains(p.currentMediaItemIndex)
        }

    private fun applyReplayGain(p: ExoPlayer) {
        val gainDb = when (replaygainMode) {
            "track" -> rgTrackGain
            "album" -> rgAlbumGain
            else -> 0.0
        }
        val rgFactor = if (gainDb != 0.0) Math.pow(10.0, gainDb / 20.0).toFloat().coerceIn(0f, 1f) else 1.0f
        p.volume = (userVolume * rgFactor).coerceIn(0f, 1f)
    }

    companion object {
        var instance: PlaybackService? = null
            private set
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        // Keep running for device discovery and playback
    }

    override fun onDestroy() {
        stateReporterJob?.cancel()
        agentWs?.close(1000, "service destroyed")
        agentJob?.cancel()
        scope.cancel()
        mediaSession?.release()
        mediaSession = null
        player?.release()
        player = null
        instance = null
        super.onDestroy()
    }
}

// MPD command parsing (same as server/agent)
private fun parseCommand(line: String): Pair<String, List<String>> {
    var cmd = ""
    val args = mutableListOf<String>()
    val current = StringBuilder()
    var inQuote = false
    var escaped = false
    var first = true

    for (r in line) {
        if (escaped) {
            current.append(r)
            escaped = false
            continue
        }
        if (r == '\\' && inQuote) {
            escaped = true
            continue
        }
        if (r == '"') {
            inQuote = !inQuote
            continue
        }
        if (r == ' ' && !inQuote) {
            if (current.isNotEmpty()) {
                if (first) {
                    cmd = current.toString()
                    first = false
                } else {
                    args.add(current.toString())
                }
                current.clear()
            }
            continue
        }
        current.append(r)
    }
    if (current.isNotEmpty()) {
        if (first) cmd = current.toString()
        else args.add(current.toString())
    }
    return Pair(cmd, args)
}

private fun mpdQuote(s: String): String {
    val escaped = s.replace("\\", "\\\\").replace("\"", "\\\"")
    return "\"$escaped\""
}
