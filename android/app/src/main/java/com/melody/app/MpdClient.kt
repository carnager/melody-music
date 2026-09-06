package com.melody.app

import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import okhttp3.*
import java.util.concurrent.TimeUnit

/**
 * MPD client over WebSocket. Connects to melodyd's /mpd WebSocket endpoint
 * and speaks the MPD text protocol for all control/browsing operations.
 *
 * HTTP is used only for audio streaming and cover art (binary payloads).
 */
class MpdClient(val serverHost: String, val serverPort: Int = 6701, val useSSL: Boolean = serverPort == 443) {
    private var ws: WebSocket? = null
    private var lines = Channel<String>(Channel.UNLIMITED)
    private val mutex = Mutex()
    // Guards connected/ws/connectGeneration writes: they are mutated both from
    // coroutines holding `mutex` and from OkHttp callback threads, which cannot
    // take a coroutine Mutex.
    private val connLock = Any()
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
    @Volatile var connected = false
        private set
    private var reconnectJob: Job? = null
    @Volatile private var connectGeneration = 0L

    // Idle connection for instant notifications
    private var idleWs: WebSocket? = null
    private var idleLines = Channel<String>(Channel.UNLIMITED)
    private var idleJob: Job? = null
    @Volatile private var idleGeneration = 0L
    var onIdleNotification: ((Set<String>) -> Unit)? = null
    var onReconnected: (() -> Unit)? = null

    // Command connection: short read timeout (commands respond quickly),
    // but with ping to keep the long-lived WebSocket alive through NAT/mobile.
    private val cmdClient = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        .readTimeout(10, TimeUnit.SECONDS)
        .pingInterval(30, TimeUnit.SECONDS)
        .build()

    // Idle connection: no read timeout (idle waits indefinitely for server
    // notifications), with ping to survive NAT/carrier proxy timeouts.
    private val idleClient = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.SECONDS)
        .pingInterval(30, TimeUnit.SECONDS)
        .build()

    val isConfigured: Boolean
        get() = serverHost.isNotBlank()

    private val scheme: String get() = if (useSSL) "https" else "http"
    private val wsScheme: String get() = if (useSSL) "wss" else "ws"

    val httpBaseUrl: String
        get() = "$scheme://$serverHost:$serverPort/api/v1"

    // ---- Connection ----

    fun connect() {
        scope.launch { doConnect() }
        startReconnectLoop()
    }

    private suspend fun doConnect(force: Boolean = false) = mutex.withLock {
        if (!force && connected && ws != null) return@withLock

        val generation: Long
        synchronized(connLock) {
            generation = ++connectGeneration
            ws?.close(1000, "reconnecting")
            ws = null
            connected = false
        }
        val connLines = Channel<String>(Channel.UNLIMITED)
        val partialLine = StringBuilder()
        lines = connLines

        val wsUrl = "$wsScheme://$serverHost:$serverPort/mpd"
        android.util.Log.d("MpdClient", "Connecting command WebSocket to $wsUrl")
        val request = Request.Builder().url(wsUrl).build()
        val newWs = cmdClient.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                if (generation != connectGeneration) return
                android.util.Log.d("MpdClient", "WebSocket connected to $wsUrl")
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                if (generation != connectGeneration) return
                // Buffer partial lines across WebSocket messages.
                // The server uses bufio.Writer which may split a line
                // across multiple WebSocket frames.
                val data = partialLine.toString() + text
                partialLine.clear()
                val parts = data.split('\n')
                // Last element is either empty (text ended with \n) or a partial line
                for (i in 0 until parts.size - 1) {
                    val line = parts[i]
                    if (line.isNotEmpty()) connLines.trySend(line)
                }
                val last = parts.last()
                if (last.isNotEmpty()) {
                    partialLine.append(last)
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                val code = response?.code?.let { " response=$it" } ?: ""
                synchronized(connLock) {
                    if (generation != connectGeneration) return
                    android.util.Log.e("MpdClient", "WebSocket error for $wsUrl:$code ${t.message}")
                    connected = false
                    ws = null
                }
                // Close lines channel so any blocked cmd() call fails immediately
                // instead of waiting for the full command timeout.
                connLines.close()
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                synchronized(connLock) {
                    if (generation != connectGeneration) return
                    android.util.Log.d("MpdClient", "WebSocket closed: $reason")
                    connected = false
                    ws = null
                }
                connLines.close()
            }
        })
        synchronized(connLock) { ws = newWs }

        // Consume the MPD greeting before releasing the mutex
        try {
            val greeting = withTimeout(10000) { connLines.receive() }
            if (greeting.startsWith("OK MPD")) {
                connected = true
                android.util.Log.d("MpdClient", "MPD greeting: $greeting")
            } else {
                // A non-MPD peer (captive portal, proxy) answered. Tear the
                // socket down fully — leaving ws non-null would stop the
                // reconnect loop from ever retrying.
                android.util.Log.e("MpdClient", "Unexpected greeting: $greeting")
                synchronized(connLock) {
                    connected = false
                    ws?.close(1000, "bad greeting")
                    ws = null
                }
            }
        } catch (e: Exception) {
            android.util.Log.e("MpdClient", "Failed to read greeting: ${e.message}")
            synchronized(connLock) {
                connected = false
                ws?.close(1000, "greeting failed")
                ws = null
            }
        }
    }

    private fun startReconnectLoop() {
        reconnectJob?.cancel()
        reconnectJob = scope.launch {
            while (true) {
                delay(3000)
                if (!connected && ws == null && serverHost.isNotBlank()) {
                    android.util.Log.d("MpdClient", "Reconnecting...")
                    doConnect()
                    if (connected) {
                        android.util.Log.d("MpdClient", "Reconnected successfully")
                        onReconnected?.invoke()
                    }
                }
            }
        }
    }

    suspend fun reconnectNow() {
        android.util.Log.d("MpdClient", "Reconnect requested")
        doConnect()
        if (connected) onReconnected?.invoke()
    }

    private fun markCommandConnectionDead(reason: String) {
        android.util.Log.d("MpdClient", "Command WebSocket dead: $reason")
        synchronized(connLock) {
            connected = false
            connectGeneration++
            ws?.close(1000, reason)
            ws = null
        }
        lines.close()
    }

    fun disconnect() {
        reconnectJob?.cancel()
        reconnectJob = null
        idleJob?.cancel()
        idleJob = null
        idleGeneration++
        idleWs?.close(1000, "bye")
        idleWs = null
        synchronized(connLock) {
            connectGeneration++
            ws?.close(1000, "bye")
            ws = null
            connected = false
        }
        // Release the OkHttp executors so discarded clients (server switch)
        // don't leak threads.
        scope.cancel()
        cmdClient.dispatcher.executorService.shutdown()
        idleClient.dispatcher.executorService.shutdown()
    }

    // ---- Idle connection for instant notifications ----

    fun startIdle() {
        idleJob?.cancel()
        idleJob = scope.launch {
            while (true) {
                try {
                    if (!connected) {
                        delay(2000)
                        continue
                    }
                    connectIdle()
                    listenForIdle()
                } catch (_: Exception) {}
                idleWs?.close(1000, "reconnecting idle")
                idleWs = null
                delay(2000)
            }
        }
    }

    private suspend fun connectIdle() {
        idleWs?.close(1000, "reconnecting idle")
        val generation = ++idleGeneration
        val connLines = Channel<String>(Channel.UNLIMITED)
        val partialLine = StringBuilder()
        idleLines = connLines

        val wsUrl = "$wsScheme://$serverHost:$serverPort/mpd"
        android.util.Log.d("MpdClient", "Connecting idle WebSocket to $wsUrl")
        val request = Request.Builder().url(wsUrl).build()
        val latch = CompletableDeferred<Boolean>()

        val newIdleWs = idleClient.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {}
            override fun onMessage(webSocket: WebSocket, text: String) {
                if (generation != idleGeneration) return
                val data = partialLine.toString() + text
                partialLine.clear()
                val parts = data.split('\n')
                for (i in 0 until parts.size - 1) {
                    val line = parts[i]
                    if (line.isNotEmpty()) connLines.trySend(line)
                }
                val last = parts.last()
                if (last.isNotEmpty()) partialLine.append(last)
            }
            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                if (generation != idleGeneration) return
                val code = response?.code?.let { " response=$it" } ?: ""
                android.util.Log.e("MpdClient", "Idle WebSocket error for $wsUrl:$code ${t.message}")
                latch.complete(false)
                idleWs = null
                connLines.close()
                // If idle dies, the command connection is likely dead too
                // (same network event). Proactively tear it down so the
                // reconnect loop kicks in immediately.
                synchronized(connLock) {
                    if (connected) {
                        android.util.Log.d("MpdClient", "Idle died, closing command WS too")
                        connected = false
                        connectGeneration++
                        ws?.close(1000, "idle failed")
                        ws = null
                    }
                }
            }
            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                if (generation != idleGeneration) return
                latch.complete(false)
                idleWs = null
                connLines.close()
            }
        })
        idleWs = newIdleWs

        // Consume greeting
        try {
            val greeting = withTimeout(10000) { connLines.receive() }
            if (greeting.startsWith("OK MPD")) {
                latch.complete(true)
            } else {
                throw Exception("bad greeting")
            }
        } catch (e: Exception) {
            idleWs?.close(1000, "greeting failed")
            idleWs = null
            throw e
        }
    }

    private suspend fun listenForIdle() {
        while (true) {
            val w = idleWs ?: return
            w.send("idle player playlist mixer options database stored_playlist rating\n")

            val changed = mutableSetOf<String>()
            while (true) {
                val line = idleLines.receive()
                if (line == "OK") break
                if (line.startsWith("changed: ")) {
                    changed.add(line.removePrefix("changed: "))
                }
            }
            if (changed.isNotEmpty()) {
                onIdleNotification?.invoke(changed)
            }
        }
    }

    // ---- Low-level command interface ----

    private fun ensureConnected(): WebSocket {
        val w = ws
        if (w == null || !connected) throw MpdSendException("not connected")
        return w
    }

    suspend fun cmd(command: String, idempotent: Boolean = false): List<String> =
        withCommandRetry(idempotent) {
            mutex.withLock {
                // NonCancellable wraps the entire send+read to prevent cancellation
                // from leaving stale response data in the channel
                kotlinx.coroutines.withContext(kotlinx.coroutines.NonCancellable) {
                    val w = ensureConnected()
                    if (!w.send("$command\n")) throw MpdSendException("send failed")
                    withTimeout(10000) { readUntilOK() }
                }
            }
        }

    suspend fun cmdBatch(commands: List<String>, idempotent: Boolean = false): List<List<String>> =
        withCommandRetry(idempotent) {
            mutex.withLock {
                kotlinx.coroutines.withContext(kotlinx.coroutines.NonCancellable) {
                    val w = ensureConnected()
                    val batch = buildString {
                        appendLine("command_list_ok_begin")
                        commands.forEach { appendLine(it) }
                        appendLine("command_list_end")
                    }
                    if (!w.send(batch)) throw MpdSendException("send failed")
                    withTimeout(10000) { readBatchResponse() }
                }
            }
        }

    // Retries a command after reconnecting. Non-idempotent commands (add, move,
    // rate, ...) are only retried when the send itself failed — after a response
    // timeout the command may already have been applied, and blindly re-running
    // it would duplicate queue entries or double-move tracks.
    private suspend fun <T> withCommandRetry(idempotent: Boolean, block: suspend () -> T): T {
        try {
            return block()
        } catch (e: Exception) {
            if (e is CancellationException && e !is TimeoutCancellationException) throw e
            if (e is MpdAckException) throw e
            val delivered = e !is MpdSendException
            markCommandConnectionDead(e.message ?: e.javaClass.simpleName)
            if (delivered && !idempotent) {
                // Reconnect in the background so the client recovers, but
                // surface the failure instead of re-running the command.
                doConnect()
                if (connected) onReconnected?.invoke()
                throw MpdException("command may not have completed: ${e.message}")
            }
        }

        doConnect()
        if (!connected) throw MpdException("reconnect failed")
        onReconnected?.invoke()
        return try {
            block()
        } catch (e: Exception) {
            if (e is CancellationException && e !is TimeoutCancellationException) throw e
            if (e is MpdAckException) throw e
            markCommandConnectionDead(e.message ?: e.javaClass.simpleName)
            throw e
        }
    }

    private suspend fun readUntilOK(): List<String> {
        val result = mutableListOf<String>()
        while (true) {
            val line = lines.receive()
            if (line == "OK") return result
            if (line.startsWith("ACK")) throw MpdAckException(line)
            result.add(line)
        }
    }

    private suspend fun readBatchResponse(): List<List<String>> {
        val results = mutableListOf<List<String>>()
        var current = mutableListOf<String>()
        while (true) {
            val line = lines.receive()
            when {
                line == "list_OK" -> {
                    results.add(current)
                    current = mutableListOf()
                }
                line == "OK" -> {
                    results.add(current)
                    return results
                }
                line.startsWith("ACK") -> throw MpdAckException(line)
                else -> current.add(line)
            }
        }
    }

    // ---- MPD quoting helpers ----

    private fun mpdEscape(s: String): String {
        val escaped = s.replace("\\", "\\\\").replace("\"", "\\\"")
        return "\"$escaped\""
    }

    private fun mpdFilterEq(tag: String, value: String): String {
        val escaped = value.replace("\\", "\\\\").replace("\"", "\\\"").replace("'", "\\'")
        return "\"($tag == '$escaped')\""
    }

    // ---- Parsing helpers ----

    private fun parseKV(lines: List<String>): Map<String, String> {
        val map = mutableMapOf<String, String>()
        for (line in lines) {
            val idx = line.indexOf(": ")
            if (idx > 0) {
                map[line.substring(0, idx)] = line.substring(idx + 2)
            }
        }
        return map
    }

    private fun parseGroups(lines: List<String>, separator: String): List<Map<String, String>> {
        val groups = mutableListOf<Map<String, String>>()
        var current = mutableMapOf<String, String>()
        for (line in lines) {
            val idx = line.indexOf(": ")
            if (idx < 0) continue
            val key = line.substring(0, idx)
            val value = line.substring(idx + 2)
            if (key.equals(separator, ignoreCase = true) && current.isNotEmpty()) {
                groups.add(current)
                current = mutableMapOf()
            }
            current[key] = value
        }
        if (current.isNotEmpty()) groups.add(current)
        return groups
    }

    // ---- Library browsing ----

    suspend fun getArtists(): List<String> {
        val lines = cmd("list AlbumArtist", idempotent = true)
        return lines.mapNotNull { line ->
            if (line.startsWith("AlbumArtist: ")) line.substringAfter("AlbumArtist: ")
            else null
        }.filter { it.isNotBlank() }.distinct().sorted()
    }

    suspend fun getAlbums(artist: String): List<Album> {
        val lines = cmd("list Album ${mpdFilterEq("AlbumArtist", artist)} group Date group AlbumArtist", idempotent = true)
        val groups = parseGroups(lines, "AlbumArtist")
        return groups.mapNotNull { g ->
            val name = g["Album"] ?: return@mapNotNull null
            if (name.isBlank()) return@mapNotNull null
            Album(
                id = g["X-AlbumId"] ?: "",
                albumArtist = g["AlbumArtist"] ?: artist,
                album = name,
                date = g["Date"] ?: ""
            )
        }.sortedBy { it.date + it.album }
    }

    suspend fun getAllAlbums(): List<Album> {
        val lines = cmd("list Album group Date group AlbumArtist", idempotent = true)
        val groups = parseGroups(lines, "AlbumArtist")
        return groups.mapNotNull { g ->
            val name = g["Album"] ?: return@mapNotNull null
            if (name.isBlank()) return@mapNotNull null
            Album(
                id = g["X-AlbumId"] ?: "",
                albumArtist = g["AlbumArtist"] ?: "",
                album = name,
                date = g["Date"] ?: ""
            )
        }
    }

    suspend fun getAllAlbumsLatest(): List<Album> {
        val lines = cmd("list Album group Date group AlbumArtist sort latest", idempotent = true)
        val groups = parseGroups(lines, "AlbumArtist")
        return groups.mapNotNull { g ->
            val name = g["Album"] ?: return@mapNotNull null
            if (name.isBlank()) return@mapNotNull null
            Album(
                id = g["X-AlbumId"] ?: "",
                albumArtist = g["AlbumArtist"] ?: "",
                album = name,
                date = g["Date"] ?: ""
            )
        }
    }

    suspend fun getTracks(artist: String, album: String): List<Track> {
        val lines = cmd("find ${mpdFilterEq("AlbumArtist", artist)} ${mpdFilterEq("Album", album)}", idempotent = true)
        val groups = parseGroups(lines, "file")
        return groups.map { g ->
            Track(
                id = g["file"] ?: "",
                songId = g["X-SongId"] ?: "",
                title = g["Title"] ?: "",
                artist = g["Artist"] ?: "",
                album = g["Album"] ?: "",
                trackNumber = g["Track"]?.toIntOrNull() ?: 0,
                albumId = g["X-AlbumId"] ?: "",
                duration = g["duration"]?.toDoubleOrNull() ?: g["Time"]?.toDoubleOrNull() ?: 0.0,
                uri = g["file"] ?: "",
                rating = g["X-Rating"]?.toIntOrNull() ?: 0,
                disc = g["Disc"]?.toIntOrNull() ?: 1
            )
        }.sortedWith(compareBy({ it.disc }, { it.trackNumber }))
    }

    // ---- Search ----

    private fun parseRatingFilter(w: String): Triple<String, String, String>? {
        val wl = w.lowercase()
        for (prefix in listOf("albumrating", "rating")) {
            if (!wl.startsWith(prefix)) continue
            val rest = w.substring(prefix.length)
            for (op in listOf(">=", "<=", ">", "<", "=")) {
                if (rest.startsWith(op)) {
                    val value = rest.substring(op.length)
                    if (value.isNotEmpty()) {
                        val mpdOp = if (op == "=") "==" else op
                        return Triple(prefix, mpdOp, value)
                    }
                }
            }
        }
        return null
    }

    // parseTagQuery recognizes tag-qualified tokens like "artist:foo",
    // "album:bar", "title:baz", "date:1994" — same syntax as the TUI.
    private fun parseTagQuery(w: String): Pair<String, String>? {
        val idx = w.indexOf(':')
        if (idx <= 0 || idx == w.length - 1) return null
        val value = w.substring(idx + 1)
        return when (w.substring(0, idx).lowercase()) {
            "artist" -> "Artist" to value
            "albumartist" -> "AlbumArtist" to value
            "album" -> "Album" to value
            "title" -> "Title" to value
            "date", "year" -> "Date" to value
            else -> null
        }
    }

    private fun searchTextTerms(query: String): List<String> {
        return query.trim().split("\\s+".toRegex())
            .filter { parseRatingFilter(it) == null && parseTagQuery(it) == null }
            .map { it.lowercase() }
    }

    private fun mpdEscapeFilterValue(v: String): String {
        return v.replace("\\", "\\\\").replace("'", "\\'")
    }

    private fun buildSearchCmd(query: String): String {
        val words = query.trim().split("\\s+".toRegex())
        val textParts = mutableListOf<String>()
        val filters = mutableListOf<String>()
        for (w in words) {
            val rf = parseRatingFilter(w)
            val tq = parseTagQuery(w)
            if (rf != null) {
                filters.add("(${rf.first} ${rf.second} '${rf.third}')")
            } else if (tq != null) {
                filters.add("(${tq.first} contains '${mpdEscapeFilterValue(tq.second)}')")
            } else {
                textParts.add(w)
            }
        }
        if (filters.isEmpty()) {
            return "search any ${mpdEscape(textParts.joinToString(" "))}"
        }
        val parts = mutableListOf<String>()
        if (textParts.isNotEmpty()) {
            val escaped = textParts.joinToString(" ").replace("\\", "\\\\").replace("'", "\\'")
            parts.add("\"(any contains '$escaped')\"")
        }
        for (f in filters) parts.add("\"$f\"")
        return "search ${parts.joinToString(" ")}"
    }

    suspend fun search(query: String): SearchResult {
        val lines = cmd(buildSearchCmd(query), idempotent = true)
        val groups = parseGroups(lines, "file")
        val terms = searchTextTerms(query)
        val tracks = mutableListOf<Track>()
        val albumSet = mutableSetOf<String>()
        val albums = mutableListOf<Album>()

        for (g in groups) {
            tracks.add(Track(
                id = g["file"] ?: "",
                songId = g["X-SongId"] ?: "",
                title = g["Title"] ?: "",
                artist = g["Artist"] ?: "",
                album = g["Album"] ?: "",
                trackNumber = g["Track"]?.toIntOrNull() ?: 0,
                albumId = g["X-AlbumId"] ?: "",
                duration = g["duration"]?.toDoubleOrNull() ?: g["Time"]?.toDoubleOrNull() ?: 0.0,
                uri = g["file"] ?: "",
                rating = g["X-Rating"]?.toIntOrNull() ?: 0
            ))
            val aa = g["AlbumArtist"] ?: g["Artist"] ?: ""
            val alb = g["Album"] ?: ""
            val date = g["Date"] ?: ""
            val key = "$aa\u0000$alb\u0000$date"
            if (alb.isNotBlank() && key !in albumSet) {
                // Only show album if artist or album name matches the text query
                val albumText = "$aa $alb".lowercase()
                if (terms.isEmpty() || terms.all { albumText.contains(it) }) {
                    albumSet.add(key)
                    albums.add(Album(
                        id = g["X-AlbumId"] ?: "",
                        albumArtist = aa,
                        album = alb,
                        date = g["Date"] ?: ""
                    ))
                }
            }
        }
        return SearchResult(albums.sortedBy { it.date + it.album }, tracks)
    }

    // ---- Status ----

    suspend fun getStatus(): PlaybackStatus? {
        if (!isConfigured) return null
        return try {
            val results = cmdBatch(listOf("status", "currentsong", "replay_gain_status"), idempotent = true)
            val statusMap = parseKV(results.getOrElse(0) { emptyList() })
            val songMap = parseKV(results.getOrElse(1) { emptyList() })
            val rgMap = parseKV(results.getOrElse(2) { emptyList() })

            val state = statusMap["state"] ?: "stop"
            val elapsed = statusMap["elapsed"]?.toDoubleOrNull() ?: 0.0
            val duration = statusMap["duration"]?.toDoubleOrNull()
                ?: songMap["duration"]?.toDoubleOrNull()
                ?: songMap["Time"]?.toDoubleOrNull()
                ?: 0.0

            PlaybackStatus(
                state = when (state) {
                    "play" -> "playing"
                    "pause" -> "paused"
                    else -> "stopped"
                },
                title = songMap["Title"] ?: "",
                artist = songMap["Artist"] ?: songMap["AlbumArtist"] ?: "",
                album = songMap["Album"] ?: "",
                date = songMap["Date"] ?: "",
                albumId = songMap["X-AlbumId"] ?: "",
                timePos = elapsed,
                duration = duration,
                rating = songMap["X-Rating"]?.toIntOrNull() ?: 0,
                songId = songMap["X-SongId"] ?: "",
                currentSongPos = statusMap["song"]?.toIntOrNull() ?: -1,
                playlistVersion = statusMap["playlist"]?.toIntOrNull() ?: 0,
                playlistLength = statusMap["playlistlength"]?.toIntOrNull() ?: 0,
                repeat = statusMap["repeat"] == "1",
                random = statusMap["random"] == "1",
                single = statusMap["single"] == "1",
                consume = statusMap["consume"] == "1",
                replayGainMode = rgMap["replay_gain_mode"] ?: "off",
                volume = statusMap["volume"]?.toIntOrNull() ?: -1
            )
        } catch (e: Exception) { null }
    }

    // ---- Playback control ----

    suspend fun play(pos: Int? = null) {
        if (pos != null) cmd("play $pos") else cmd("play")
    }
    suspend fun pause() { cmd("pause 1") }
    suspend fun resume() { cmd("pause 0") }
    suspend fun stop() { cmd("stop") }
    suspend fun next() { cmd("next") }
    suspend fun prev() { cmd("previous") }
    suspend fun seek(position: Double) { cmd("seekcur $position") }
    suspend fun setVolume(volume: Int) { cmd("setvol ${volume.coerceIn(0, 100)}") }

    // ---- Queue ----

    suspend fun getQueue(): List<QueueItem> {
        if (!isConfigured) return emptyList()
        return try {
            val lines = cmd("playlistinfo", idempotent = true)
            val groups = parseGroups(lines, "file")
            groups.map { g ->
                QueueItem(
                    position = g["Pos"]?.toIntOrNull() ?: 0,
                    songId = g["X-SongId"] ?: g["Id"] ?: "",
                    title = g["Title"] ?: "",
                    artist = g["Artist"] ?: "",
                    album = g["Album"] ?: "",
                    albumId = g["X-AlbumId"] ?: "",
                    duration = g["duration"]?.toDoubleOrNull() ?: g["Time"]?.toDoubleOrNull() ?: 0.0,
                    current = false,
                    uri = g["file"] ?: "",
                    rating = g["X-Rating"]?.toIntOrNull() ?: 0,
                    priority = g["Prio"]?.toIntOrNull() ?: 0,
                    queueId = g["Id"]?.toIntOrNull() ?: -1
                )
            }
        } catch (e: Exception) { emptyList() }
    }

    suspend fun queuePlay(position: Int) { cmd("play $position") }
    suspend fun queueRemove(position: Int) { cmd("delete $position") }
    suspend fun queueMove(from: Int, to: Int) { cmd("move $from $to") }
    suspend fun queueClear() { cmd("clear") }
    suspend fun queueShuffle() { cmd("shuffle") }
    suspend fun setPriority(position: Int, priority: Int) { cmd("prio $priority $position") }
    suspend fun addWithPriority(uri: String, priority: Int) { cmd("addidprio ${mpdEscape(uri)} $priority") }

    // ---- Library maintenance ----

    suspend fun updateLibrary() { cmd("update", idempotent = true) }

    // ---- Ratings ----

    suspend fun rateTrack(songId: String, rating: Int) {
        cmd("rate $songId $rating")
    }

    suspend fun rateAlbum(albumArtist: String, album: String, date: String, rating: Int) {
        cmd("albumrate ${mpdEscape(albumArtist)} ${mpdEscape(album)} ${mpdEscape(date)} $rating")
    }

    data class AlbumRatingResult(val rating: Int, val computed: Double)

    suspend fun getAlbumRating(albumArtist: String, album: String, date: String): AlbumRatingResult {
        val lines = cmd("getalbumrating ${mpdEscape(albumArtist)} ${mpdEscape(album)} ${mpdEscape(date)}", idempotent = true)
        var rating = 0
        var computed = 0.0
        for (line in lines) {
            if (line.startsWith("rating: ")) rating = line.substringAfter("rating: ").trim().toIntOrNull() ?: 0
            if (line.startsWith("computed: ")) computed = line.substringAfter("computed: ").trim().toDoubleOrNull() ?: 0.0
        }
        return AlbumRatingResult(rating, computed)
    }

    // ---- Lyrics ----

    data class LyricsResult(val text: String, val type: String) // type: "synced" or "plain"

    suspend fun getLyrics(uri: String): LyricsResult? {
        return try {
            val lines = cmd("readlyrics ${mpdEscape(uri)}", idempotent = true)
            var text = ""
            var type = "plain"
            for (line in lines) {
                if (line.startsWith("X-Lyrics-Type: ")) {
                    type = line.removePrefix("X-Lyrics-Type: ")
                } else if (line.startsWith("X-Lyrics: ")) {
                    val escaped = line.removePrefix("X-Lyrics: ")
                    text = escaped.replace("\\n", "\n").replace("\\\\", "\\")
                }
            }
            if (text.isNotBlank()) LyricsResult(text, type) else null
        } catch (_: Exception) { null }
    }

    // ---- Add to queue ----

    suspend fun addAlbum(artist: String, album: String, mode: String) {
        when (mode) {
            "replace" -> {
                cmd("clear")
                cmd("findadd ${mpdFilterEq("AlbumArtist", artist)} ${mpdFilterEq("Album", album)}")
                cmd("play")
            }
            "insert" -> {
                val statusLines = cmd("status")
                val statusMap = parseKV(statusLines)
                val currentPos = statusMap["song"]?.toIntOrNull() ?: -1
                val insertPos = if (currentPos >= 0) currentPos + 1 else 0
                val beforeCount = statusMap["playlistlength"]?.toIntOrNull() ?: 0
                cmd("findadd ${mpdFilterEq("AlbumArtist", artist)} ${mpdFilterEq("Album", album)}")
                val afterLines = cmd("status")
                val afterMap = parseKV(afterLines)
                val afterCount = afterMap["playlistlength"]?.toIntOrNull() ?: 0
                val added = afterCount - beforeCount
                for (i in 0 until added) {
                    cmd("move ${beforeCount + i} ${insertPos + i}")
                }
            }
            else -> cmd("findadd ${mpdFilterEq("AlbumArtist", artist)} ${mpdFilterEq("Album", album)}")
        }
    }

    suspend fun addTrack(uri: String, mode: String) {
        when (mode) {
            "replace" -> {
                cmd("clear")
                cmd("add ${mpdEscape(uri)}")
                cmd("play")
            }
            "insert" -> {
                val statusLines = cmd("status")
                val statusMap = parseKV(statusLines)
                val currentPos = statusMap["song"]?.toIntOrNull() ?: -1
                val insertPos = if (currentPos >= 0) currentPos + 1 else 0
                cmd("add ${mpdEscape(uri)}")
                val afterLines = cmd("status")
                val afterMap = parseKV(afterLines)
                val lastPos = (afterMap["playlistlength"]?.toIntOrNull() ?: 1) - 1
                if (lastPos > insertPos) cmd("move $lastPos $insertPos")
            }
            else -> cmd("add ${mpdEscape(uri)}")
        }
    }

    /** Replaces the queue with the given tracks and starts playing at [startIndex]. */
    suspend fun replaceQueueWithTracks(uris: List<String>, startIndex: Int) {
        val cmds = mutableListOf("clear")
        uris.forEach { cmds.add("add ${mpdEscape(it)}") }
        cmds.add("play ${startIndex.coerceIn(0, (uris.size - 1).coerceAtLeast(0))}")
        cmdBatch(cmds)
    }

    /** Replaces the queue with an album in shuffled order and starts playing. */
    suspend fun playAlbumShuffled(artist: String, album: String) {
        cmdBatch(listOf(
            "clear",
            "findadd ${mpdFilterEq("AlbumArtist", artist)} ${mpdFilterEq("Album", album)}",
            "shuffle",
            "play"
        ))
    }

    suspend fun addAllArtistAlbums(artist: String, mode: String) {
        when (mode) {
            "replace" -> {
                cmd("clear")
                cmd("findadd ${mpdFilterEq("AlbumArtist", artist)}")
                cmd("play")
            }
            "insert" -> {
                val statusLines = cmd("status")
                val statusMap = parseKV(statusLines)
                val currentPos = statusMap["song"]?.toIntOrNull() ?: -1
                val insertPos = if (currentPos >= 0) currentPos + 1 else 0
                val beforeCount = statusMap["playlistlength"]?.toIntOrNull() ?: 0
                cmd("findadd ${mpdFilterEq("AlbumArtist", artist)}")
                val afterLines = cmd("status")
                val afterMap = parseKV(afterLines)
                val afterCount = afterMap["playlistlength"]?.toIntOrNull() ?: 0
                val added = afterCount - beforeCount
                for (i in 0 until added) {
                    cmd("move ${beforeCount + i} ${insertPos + i}")
                }
            }
            else -> cmd("findadd ${mpdFilterEq("AlbumArtist", artist)}")
        }
    }

    // ---- Devices / Outputs ----

    suspend fun getOutputs(): List<DeviceInfo> {
        val lines = cmd("outputs", idempotent = true)
        val groups = parseGroups(lines, "outputid")
        return groups.map { g ->
            val plugin = g["plugin"] ?: g["outputtype"] ?: ""
            DeviceInfo(
                id = g["outputid"] ?: "",
                name = g["outputname"] ?: "",
                isLocal = plugin == "local",
                type = plugin,
                // Enabled outputs stay listed while their agent is away;
                // daemons predating outputonline only list connected ones.
                online = g["outputonline"]?.let { it == "1" } ?: true,
                format = g["outputformat"] ?: "",
                maxBitrate = g["outputmaxbitrate"]?.toIntOrNull() ?: 0,
                active = g["outputenabled"] == "1",
                primary = g["outputprimary"] == "1"
            )
        }
    }

    suspend fun enableOutput(id: String) { cmd("enableoutput $id") }

    suspend fun disableOutput(id: String) { cmd("disableoutput $id") }

    suspend fun toggleOutput(id: String) { cmd("toggleoutput $id") }

    // ---- Playlists ----

    suspend fun getPlaylists(): List<PlaylistInfo> {
        val lines = cmd("listplaylists", idempotent = true)
        val groups = parseGroups(lines, "playlist")
        return groups.map { g ->
            PlaylistInfo(
                id = g["playlist"] ?: "",
                name = g["playlist"] ?: "",
                songCount = g["songs"]?.toIntOrNull() ?: 0,
                duration = g["playtime"]?.toIntOrNull() ?: 0,
                coverArt = ""
            )
        }
    }

    suspend fun getPlaylistTracks(name: String): List<Track> {
        val lines = cmd("listplaylistinfo ${mpdEscape(name)}", idempotent = true)
        val groups = parseGroups(lines, "file")
        return groups.mapIndexed { idx, g ->
            Track(
                id = g["file"] ?: "",
                songId = g["X-SongId"] ?: "",
                title = g["Title"] ?: "",
                artist = g["Artist"] ?: "",
                album = g["Album"] ?: "",
                trackNumber = idx + 1,
                albumId = g["X-AlbumId"] ?: "",
                duration = g["duration"]?.toDoubleOrNull() ?: g["Time"]?.toDoubleOrNull() ?: 0.0,
                uri = g["file"] ?: "",
                rating = g["X-Rating"]?.toIntOrNull() ?: 0
            )
        }
    }

    suspend fun loadPlaylist(name: String, mode: String) {
        when (mode) {
            "replace" -> {
                cmd("clear")
                cmd("load ${mpdEscape(name)}")
                cmd("play")
            }
            else -> cmd("load ${mpdEscape(name)}")
        }
    }

    suspend fun addToPlaylist(playlistName: String, uri: String) {
        cmd("playlistadd ${mpdEscape(playlistName)} ${mpdEscape(uri)}")
    }

    suspend fun saveQueueAsPlaylist(name: String) {
        cmd("save ${mpdEscape(name)}")
    }

    suspend fun deletePlaylist(name: String) {
        cmd("rm ${mpdEscape(name)}")
    }

    // ---- Cover art ----

    fun coverUrl(albumId: String, size: Int = 300): String? {
        if (albumId.isBlank() || !isConfigured) return null
        return "$httpBaseUrl/cover/$albumId?size=$size"
    }

    fun streamUrl(songId: String, format: String? = null, maxBitrate: Int = 0): String {
        val base = "$httpBaseUrl/stream/$songId"
        val params = mutableListOf<String>()
        if (!format.isNullOrBlank()) params.add("format=$format")
        if (maxBitrate > 0) params.add("max_bitrate=$maxBitrate")
        return if (params.isEmpty()) base else "$base?${params.joinToString("&")}"
    }
}

open class MpdException(message: String) : Exception(message)
class MpdAckException(message: String) : MpdException(message)

// Thrown when a command was definitely NOT delivered (no connection, or the
// WebSocket send failed) — always safe to retry after reconnecting.
class MpdSendException(message: String) : MpdException(message)
