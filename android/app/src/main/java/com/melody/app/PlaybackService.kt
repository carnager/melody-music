package com.melody.app

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import kotlinx.coroutines.*
import okhttp3.*

@androidx.annotation.OptIn(androidx.media3.common.util.UnstableApi::class)
class PlaybackService : Service() {
    private var player: ExoPlayer? = null
    private var agentWs: WebSocket? = null
    private var agentJob: Job? = null
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    // Track which playlist indices are playing from offline cache
    private val offlineIndexes = mutableSetOf<Int>()
    // Offset in seconds from start= parameter (ffmpeg -ss). ExoPlayer's position 0
    // corresponds to this offset in the actual track.
    @Volatile var streamStartOffset: Double = 0.0

    @Volatile var codecInfo: String = ""
        private set
    @Volatile private var replaygainMode: String = "off"
    @Volatile private var rgTrackGain: Double = 0.0
    @Volatile private var rgAlbumGain: Double = 0.0
    @Volatile private var userVolume: Float = 1.0f  // volume set by server (0..1)

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
                }
                override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
                    // Reset offset on natural track changes (end of track -> next)
                    if (reason == Player.MEDIA_ITEM_TRANSITION_REASON_AUTO) {
                        streamStartOffset = 0.0
                    }
                    updateCodecInfo()
                    fetchAndApplyReplayGain()
                }
                override fun onPlayerError(error: androidx.media3.common.PlaybackException) {
                    android.util.Log.e("PlaybackService", "ExoPlayer error: ${error.errorCodeName} — ${error.message}")
                }
            })
        }
        android.util.Log.d("PlaybackService", "ExoPlayer initialized")
        val prefs = getSharedPreferences("melody", MODE_PRIVATE)
        replaygainMode = prefs.getString("replaygain", "off") ?: "off"
        registerAgent()
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
                // Server disconnected — stop streamed playback but keep offline tracks.
                withContext(Dispatchers.Main) {
                    val p = player
                    if (p != null && !isCurrentTrackOffline) {
                        p.stop()
                        p.clearMediaItems()
                        streamStartOffset = 0.0
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

        val lineChannel = kotlinx.coroutines.channels.Channel<String>(kotlinx.coroutines.channels.Channel.UNLIMITED)
        val connected = CompletableDeferred<Boolean>()

        val client = OkHttpClient.Builder()
            .readTimeout(0, java.util.concurrent.TimeUnit.SECONDS)
            .pingInterval(30, java.util.concurrent.TimeUnit.SECONDS)
            .build()
        val request = Request.Builder().url(wsUrl).build()

        val ws = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                android.util.Log.d("PlaybackService", "Agent WebSocket connected")
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                text.lines().filter { it.isNotEmpty() }.forEach { line ->
                    lineChannel.trySend(line)
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                android.util.Log.e("PlaybackService", "Agent WebSocket error: ${t.message}")
                connected.complete(false)
                lineChannel.close()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                android.util.Log.d("PlaybackService", "Agent WebSocket closed: $reason")
                lineChannel.close()
            }
        })

        agentWs = ws

        val greeting = lineChannel.receive()
        if (!greeting.startsWith("OK MPD")) {
            ws.close(1000, "bad greeting")
            return
        }

        val regCmd = buildString {
            append("agent_register ${mpdQuote(name)}")
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
        android.util.Log.d("PlaybackService", "Agent registered as $name")

        // If resume-on-connect is enabled, tell server to unpause via the MpdClient
        val prefs = getSharedPreferences("melody", MODE_PRIVATE)
        if (prefs.getBoolean("resume_on_connect", false)) {
            try {
                MelodyApp.instance.mpd.resume()
            } catch (e: Exception) {
                android.util.Log.e("PlaybackService", "Resume-on-connect failed: ${e.message}")
            }
        }

        for (line in lineChannel) {
            if (line.isBlank()) continue
            val response = handleAgentCommand(line)
            ws.send("$response\n")
        }

        android.util.Log.d("PlaybackService", "Agent connection ended")
    }

    /** Check if a URL has transcoding parameters (format/max_bitrate). */
    private fun isTranscodedUrl(url: String): Boolean {
        val uri = android.net.Uri.parse(url)
        return !uri.getQueryParameter("format").isNullOrBlank() ||
               (uri.getQueryParameter("max_bitrate")?.toIntOrNull() ?: 0) > 0
    }

    /** Reload the current transcoded stream at a new offset (Subsonic-style seek). */
    private fun reloadWithOffset(p: ExoPlayer, currentUri: String, seekSecs: Double) {
        val newUrl = urlWithStart(currentUri, seekSecs)
        val idx = p.currentMediaItemIndex
        val wasPlaying = p.playWhenReady
        streamStartOffset = seekSecs

        // Rebuild entire playlist with new URL at current index.
        // This guarantees ExoPlayer creates a fresh media source and HTTP request.
        val items = mutableListOf<MediaItem>()
        for (i in 0 until p.mediaItemCount) {
            if (i == idx) {
                items.add(MediaItem.fromUri(newUrl))
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

    /** Build a new URL with start= offset, replacing any existing start= param. */
    private fun urlWithStart(url: String, startSecs: Double): String {
        val uri = android.net.Uri.parse(url)
        val builder = uri.buildUpon().clearQuery()
        // Copy all params except start=
        uri.queryParameterNames.forEach { key ->
            if (key != "start") builder.appendQueryParameter(key, uri.getQueryParameter(key))
        }
        if (startSecs > 0) {
            builder.appendQueryParameter("start", String.format(java.util.Locale.US, "%.3f", startSecs))
        }
        return builder.build().toString()
    }

    private suspend fun handleAgentCommand(line: String): String {
        val parts = parseCommand(line)
        val cmd = parts.first
        val args = parts.second

        if (cmd == "ping") return "OK"

        return withContext(Dispatchers.Main) {
            val p = player ?: return@withContext "ACK [56@0] {$cmd} player not initialized"

            when (cmd) {
                "loadfile" -> {
                    if (args.size < 2) return@withContext "ACK [2@0] {loadfile} missing arguments"
                    val url = args[0]
                    val mode = args[1]
                    val songId = extractSongIdFromUrl(url)
                    val localPath = if (songId.isNotBlank()) MelodyApp.instance.offlineManager.getLocalPath(songId) else null
                    val isOffline = localPath != null
                    val resolvedUrl = localPath ?: resolveUrl(url, songId)

                    // Parse start= offset from URL (baked in by server for transcoded handoff)
                    val startParam = android.net.Uri.parse(url).getQueryParameter("start")
                    val startOffset = startParam?.toDoubleOrNull() ?: 0.0

                    when (mode) {
                        "replace" -> {
                            offlineIndexes.clear()
                            // Offline files: ExoPlayer can seek natively, no offset hack needed
                            streamStartOffset = if (isOffline) 0.0 else startOffset
                            if (isOffline) offlineIndexes.add(0)
                            p.pause()
                            p.clearMediaItems()
                            p.addMediaItem(MediaItem.fromUri(resolvedUrl))
                            p.prepare()
                        }
                        "append", "append-play" -> {
                            val idx = p.mediaItemCount
                            if (isOffline) offlineIndexes.add(idx)
                            p.addMediaItem(MediaItem.fromUri(resolvedUrl))
                            if (p.playbackState == Player.STATE_IDLE) p.prepare()
                        }
                    }
                    "OK"
                }

                "playlist_clear" -> {
                    offlineIndexes.clear()
                    streamStartOffset = 0.0
                    p.stop()
                    p.clearMediaItems()
                    "OK"
                }

                "playlist_remove" -> {
                    if (args.isEmpty()) return@withContext "ACK [2@0] {playlist_remove} missing index"
                    p.removeMediaItem(args[0].toIntOrNull() ?: 0)
                    "OK"
                }

                "playlist_move" -> {
                    if (args.size < 2) return@withContext "ACK [2@0] {playlist_move} missing arguments"
                    p.moveMediaItem(args[0].toIntOrNull() ?: 0, args[1].toIntOrNull() ?: 0)
                    "OK"
                }

                "get_property" -> {
                    if (args.isEmpty()) return@withContext "ACK [2@0] {get_property} missing name"
                    val name = args[0]
                    val value = when (name) {
                        "pause" -> (!p.playWhenReady).toString()
                        "time-pos" -> {
                            val pos = p.currentPosition / 1000.0
                            (pos + streamStartOffset).toString()
                        }
                        "duration" -> {
                            val d = p.duration
                            if (d != C.TIME_UNSET) (d / 1000.0 + streamStartOffset).toString() else "0.0"
                        }
                        "playlist-pos" -> p.currentMediaItemIndex.toString()
                        "playlist-count" -> p.mediaItemCount.toString()
                        "volume" -> (userVolume * 100).toInt().toString()
                        else -> ""
                    }
                    "value: $value\nOK"
                }

                "set_property" -> {
                    if (args.size < 2) return@withContext "ACK [2@0] {set_property} missing arguments"
                    val name = args[0]
                    val value = args[1]
                    when (name) {
                        "pause" -> {
                            if (value == "true") {
                                p.pause()
                            } else {
                                if (p.playbackState == Player.STATE_IDLE && p.mediaItemCount > 0) {
                                    p.prepare()
                                }
                                p.play()
                            }
                        }
                        "playlist-pos" -> {
                            streamStartOffset = 0.0
                            p.seekToDefaultPosition(value.toIntOrNull() ?: 0)
                        }
                        "time-pos" -> {
                            val seekSecs = value.toDoubleOrNull() ?: 0.0
                            val currentUri = p.currentMediaItem?.localConfiguration?.uri?.toString() ?: ""
                            if (isTranscodedUrl(currentUri)) {
                                reloadWithOffset(p, currentUri, seekSecs)
                            } else {
                                p.seekTo((seekSecs * 1000).toLong())
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
                            fetchAndApplyReplayGain()
                        }
                    }
                    "OK"
                }

                "mpv_command" -> {
                    if (args.isEmpty()) return@withContext "ACK [2@0] {mpv_command} missing command"
                    when (args[0]) {
                        "playlist-next" -> {
                            if (p.currentMediaItemIndex + 1 < p.mediaItemCount) {
                                streamStartOffset = 0.0
                                p.seekToDefaultPosition(p.currentMediaItemIndex + 1)
                            }
                        }
                        "playlist-prev" -> {
                            if (p.currentMediaItemIndex > 0) {
                                streamStartOffset = 0.0
                                p.seekToDefaultPosition(p.currentMediaItemIndex - 1)
                            }
                        }
                        "seek" -> {
                            if (args.size >= 2) {
                                val seekSecs = args[1].toDoubleOrNull() ?: 0.0
                                val currentUri = p.currentMediaItem?.localConfiguration?.uri?.toString() ?: ""
                                if (isTranscodedUrl(currentUri)) {
                                    reloadWithOffset(p, currentUri, seekSecs)
                                } else {
                                    p.seekTo((seekSecs * 1000).toLong())
                                }
                            }
                        }
                    }
                    "OK"
                }

                "handoff" -> {
                    if (args.size < 3) return@withContext "ACK [2@0] {handoff} missing arguments"
                    val playlistPos = args[0].toIntOrNull() ?: 0
                    val timePos = args[1].toDoubleOrNull() ?: 0.0
                    val paused = args[2] == "true" || args[2] == "1"

                    android.util.Log.d("PlaybackService", "handoff: pos=$playlistPos timePos=$timePos paused=$paused")

                    p.pause()
                    p.seekToDefaultPosition(playlistPos)

                    // Wait for media to load
                    for (i in 1..200) {
                        if (p.playbackState == Player.STATE_READY) break
                        delay(50)
                    }

                    if (timePos > 0) {
                        val currentUri = p.currentMediaItem?.localConfiguration?.uri?.toString() ?: ""
                        if (isTranscodedUrl(currentUri)) {
                            // The start= offset is baked into the URL for transcoded streams.
                            // Ensure streamStartOffset reflects the actual position so the
                            // seekbar and get_property time-pos report correctly.
                            val startParam = android.net.Uri.parse(currentUri).getQueryParameter("start")
                            streamStartOffset = startParam?.toDoubleOrNull() ?: 0.0
                        } else {
                            p.seekTo((timePos * 1000).toLong())
                        }
                    }

                    if (!paused) {
                        p.play()
                        // Wait for playback to actually start
                        for (i in 1..100) {
                            if (p.playbackState == Player.STATE_READY && p.playWhenReady) break
                            if (p.playbackState == Player.STATE_IDLE) break // error occurred
                            delay(50)
                        }
                        if (p.playbackState == Player.STATE_IDLE) {
                            android.util.Log.e("PlaybackService", "handoff: playback failed to start")
                        }
                    } else {
                        p.pause()
                    }
                    android.util.Log.d("PlaybackService", "handoff: done, state=${p.playbackState} offset=$streamStartOffset paused=$paused")
                    "OK"
                }

                else -> "ACK [5@0] {$cmd} unknown command"
            }
        }
    }

    private fun extractSongIdFromUrl(url: String): String {
        val prefix = "/api/v1/stream/"
        val idx = url.indexOf(prefix)
        if (idx < 0) return ""
        val rest = url.substring(idx + prefix.length)
        return rest.substringBefore("?").substringBefore("/")
    }

    private fun resolveUrl(url: String, songId: String): String {
        if (songId.isNotBlank()) {
            val localPath = MelodyApp.instance.offlineManager.getLocalPath(songId)
            if (localPath != null) return localPath
        }
        // Rewrite stream URL to use the same host/scheme the app is connected to,
        // so it works over external HTTPS as well as local HTTP.
        val mpd = MelodyApp.instance.mpd
        val prefix = "/api/v1/stream/"
        val idx = url.indexOf(prefix)
        if (idx >= 0) {
            val pathAndQuery = url.substring(idx)
            return "${mpd.httpBaseUrl.removeSuffix("/api/v1")}$pathAndQuery"
        }
        return url
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

    private fun fetchAndApplyReplayGain() {
        scope.launch {
            try {
                val lines = MelodyApp.instance.mpd.cmd("currentsong")
                var trackGain = 0.0
                var albumGain = 0.0
                for (line in lines) {
                    when {
                        line.startsWith("X-ReplayGainTrack: ") ->
                            trackGain = line.substringAfter(": ").toDoubleOrNull() ?: 0.0
                        line.startsWith("X-ReplayGainAlbum: ") ->
                            albumGain = line.substringAfter(": ").toDoubleOrNull() ?: 0.0
                    }
                }
                rgTrackGain = trackGain
                rgAlbumGain = albumGain
                withContext(Dispatchers.Main) {
                    player?.let { applyReplayGain(it) }
                }
            } catch (e: Exception) {
                android.util.Log.e("PlaybackService", "Failed to fetch RG data: ${e.message}")
            }
        }
    }

    companion object {
        var instance: PlaybackService? = null
            private set
    }

    override fun onTaskRemoved(rootIntent: Intent?) {
        // Keep running for device discovery and playback
    }

    override fun onDestroy() {
        agentWs?.close(1000, "service destroyed")
        agentJob?.cancel()
        scope.cancel()
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
