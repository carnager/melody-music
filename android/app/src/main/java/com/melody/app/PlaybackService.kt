package com.melody.app

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import dev.jdtech.mpv.MPVLib
import kotlinx.coroutines.*
import okhttp3.*

class PlaybackService : Service(), MPVLib.EventObserver {
    private var mpv: MPVLib? = null
    private var agentWs: WebSocket? = null
    private var agentJob: Job? = null
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    // Track which playlist indices are playing from offline cache
    private val offlineIndexes = mutableSetOf<Int>()
    // Pending seek after file load (handoff)
    @Volatile private var pendingSeek: Double? = null
    @Volatile private var pendingPause: Boolean? = null

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

        initMpv()
        registerAgent()
    }

    private fun initMpv() {
        val m = MPVLib.create(applicationContext) ?: run {
            android.util.Log.e("PlaybackService", "Failed to create mpv instance")
            return
        }
        m.setOptionString("vid", "no")
        m.setOptionString("vo", "null")
        m.setOptionString("ao", "audiotrack")
        m.setOptionString("idle", "yes")
        m.setOptionString("cache", "yes")
        m.setOptionString("demuxer-max-bytes", "50MiB")
        m.setOptionString("demuxer-max-back-bytes", "25MiB")

        m.init()
        m.addObserver(this)
        mpv = m
        android.util.Log.d("PlaybackService", "mpv initialized")
    }

    // MPVLib.EventObserver
    override fun eventProperty(property: String) {}
    override fun eventProperty(property: String, value: Long) {}
    override fun eventProperty(property: String, value: Double) {}
    override fun eventProperty(property: String, value: Boolean) {}
    override fun eventProperty(property: String, value: String) {}
    override fun event(eventId: Int) {
        // eventId 8 = FILE_LOADED
        if (eventId == 8) {
            applyPendingSeek("event")
        }
    }

    private fun applyPendingSeek(source: String) {
        val seek = pendingSeek
        val pause = pendingPause
        if (seek == null && pause == null) return
        pendingSeek = null
        pendingPause = null
        val m = mpv ?: return
        if (seek != null && seek > 0) {
            android.util.Log.d("PlaybackService", "applying pending seek ($source): $seek")
            // Use property set — more reliable than seek command during file load
            m.setPropertyDouble("time-pos", seek)
            // Verify and retry if needed
            Thread.sleep(100)
            val actual = m.getPropertyDouble("time-pos") ?: 0.0
            if (actual < seek - 2.0) {
                android.util.Log.d("PlaybackService", "seek retry ($source): wanted=$seek actual=$actual")
                m.command(arrayOf("seek", seek.toString(), "absolute"))
            }
        }
        if (pause != null) {
            m.setPropertyBoolean("pause", pause)
        }
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
            }
            delay(5000)
        }
    }

    private suspend fun connectAgent(name: String, format: String, bitrate: Int) {
        val app = MelodyApp.instance
        val host = app.mpd.serverHost
        val port = app.mpd.serverPort
        val wsUrl = "ws://$host:$port/mpd"

        val lineChannel = kotlinx.coroutines.channels.Channel<String>(kotlinx.coroutines.channels.Channel.UNLIMITED)
        val connected = CompletableDeferred<Boolean>()

        val client = OkHttpClient.Builder()
            .readTimeout(0, java.util.concurrent.TimeUnit.SECONDS)
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

        // Read MPD greeting
        val greeting = lineChannel.receive()
        if (!greeting.startsWith("OK MPD")) {
            ws.close(1000, "bad greeting")
            return
        }

        // Send agent_register
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

        // Command loop: server sends commands, we execute and respond
        for (line in lineChannel) {
            if (line.isBlank()) continue
            val response = handleAgentCommand(line)
            ws.send("$response\n")
        }

        android.util.Log.d("PlaybackService", "Agent connection ended")
    }

    private fun handleAgentCommand(line: String): String {
        val parts = parseCommand(line)
        val cmd = parts.first
        val args = parts.second
        val m = mpv ?: return "ACK [56@0] {$cmd} mpv not initialized"

        return when (cmd) {
            "ping" -> "OK"

            "loadfile" -> {
                if (args.size < 2) return "ACK [2@0] {loadfile} missing arguments"
                val url = args[0]
                val mode = args[1]
                val songId = extractSongIdFromUrl(url)
                val resolvedUrl = resolveUrl(url, songId)
                val isOffline = resolvedUrl != url

                when (mode) {
                    "replace" -> {
                        offlineIndexes.clear()
                        if (isOffline) offlineIndexes.add(0)
                        // Pause before loading to prevent audible playback at 0:00
                        // during handoff (handoff will set the correct pause state)
                        m.setPropertyBoolean("pause", true)
                        m.command(arrayOf("loadfile", resolvedUrl, "replace"))
                    }
                    "append", "append-play" -> {
                        val idx = (m.getPropertyInt("playlist-count") ?: 0).toInt()
                        if (isOffline) offlineIndexes.add(idx)
                        m.command(arrayOf("loadfile", resolvedUrl, mode))
                    }
                }
                "OK"
            }

            "playlist_clear" -> {
                offlineIndexes.clear()
                m.command(arrayOf("stop"))
                m.command(arrayOf("playlist-clear"))
                "OK"
            }

            "playlist_remove" -> {
                if (args.isEmpty()) return "ACK [2@0] {playlist_remove} missing index"
                m.command(arrayOf("playlist-remove", args[0]))
                "OK"
            }

            "playlist_move" -> {
                if (args.size < 2) return "ACK [2@0] {playlist_move} missing arguments"
                m.command(arrayOf("playlist-move", args[0], args[1]))
                "OK"
            }

            "get_property" -> {
                if (args.isEmpty()) return "ACK [2@0] {get_property} missing name"
                val name = args[0]
                val value = when {
                    name == "pause" -> m.getPropertyBoolean("pause")?.toString() ?: "true"
                    name == "time-pos" -> m.getPropertyDouble("time-pos")?.toString() ?: "0"
                    name == "duration" -> m.getPropertyDouble("duration")?.toString() ?: "0"
                    name == "playlist-pos" -> m.getPropertyInt("playlist-pos")?.toString() ?: "0"
                    name == "playlist-count" -> m.getPropertyInt("playlist-count")?.toString() ?: "0"
                    name == "volume" -> m.getPropertyDouble("volume")?.toString() ?: "100"
                    else -> m.getPropertyString(name) ?: ""
                }
                "value: $value\nOK"
            }

            "set_property" -> {
                if (args.size < 2) return "ACK [2@0] {set_property} missing arguments"
                val name = args[0]
                val value = args[1]
                when {
                    value == "true" || value == "false" -> m.setPropertyBoolean(name, value == "true")
                    name == "playlist-pos" -> m.setPropertyInt(name, value.toIntOrNull() ?: 0)
                    value.contains('.') -> m.setPropertyDouble(name, value.toDoubleOrNull() ?: 0.0)
                    value.toIntOrNull() != null -> m.setPropertyInt(name, value.toInt())
                    else -> m.setPropertyString(name, value)
                }
                "OK"
            }

            "mpv_command" -> {
                if (args.isEmpty()) return "ACK [2@0] {mpv_command} missing command"
                m.command(args.toTypedArray())
                "OK"
            }

            "handoff" -> {
                if (args.size < 3) return "ACK [2@0] {handoff} missing arguments"
                val playlistPos = args[0].toIntOrNull() ?: 0
                val timePos = args[1].toDoubleOrNull() ?: 0.0
                val paused = args[2] == "true" || args[2] == "1"

                android.util.Log.d("PlaybackService", "handoff: pos=$playlistPos timePos=$timePos paused=$paused")

                // Clear any stale pending state from FILE_LOADED handler
                pendingSeek = null
                pendingPause = null

                m.setPropertyBoolean("pause", true)
                m.setPropertyInt("playlist-pos", playlistPos)

                // Wait until file is loaded (duration becomes available)
                val loadDeadline = System.currentTimeMillis() + 10000
                while (System.currentTimeMillis() < loadDeadline) {
                    val dur = m.getPropertyDouble("duration") ?: 0.0
                    if (dur > 0) break
                    Thread.sleep(50)
                }

                if (timePos > 0) {
                    // Seek aggressively with retries — alternate between methods
                    for (attempt in 1..20) {
                        if (attempt % 2 == 1) {
                            m.command(arrayOf("seek", timePos.toString(), "absolute", "exact"))
                        } else {
                            m.setPropertyDouble("time-pos", timePos)
                        }
                        Thread.sleep(50)
                        val actual = m.getPropertyDouble("time-pos") ?: 0.0
                        if (actual >= timePos - 2.0) {
                            android.util.Log.d("PlaybackService", "handoff: seek ok attempt=$attempt actual=$actual target=$timePos")
                            break
                        }
                        if (attempt % 5 == 0) {
                            android.util.Log.d("PlaybackService", "handoff: seek retry attempt=$attempt actual=$actual target=$timePos")
                        }
                    }
                }

                m.setPropertyBoolean("pause", paused)
                android.util.Log.d("PlaybackService", "handoff: done, timePos=${m.getPropertyDouble("time-pos")} paused=$paused")
                "OK"
            }

            else -> "ACK [5@0] {$cmd} unknown command"
        }
    }

    private fun extractSongIdFromUrl(url: String): String {
        // Extract songId from stream URL like http://host:port/api/v1/stream/SONGID
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
        return url
    }

    val isCurrentTrackOffline: Boolean
        get() {
            val pos = mpv?.getPropertyInt("playlist-pos")?.toInt() ?: return false
            return offlineIndexes.contains(pos)
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
        mpv?.let {
            it.removeObserver(this)
            it.destroy()
        }
        mpv = null
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
