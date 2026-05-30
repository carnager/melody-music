package com.melody.app

import android.app.Application
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiInfo
import android.os.Build
import android.os.Handler
import android.os.Looper

class MelodyApp : Application() {
    lateinit var mpd: MpdClient
        private set
    lateinit var offlineManager: OfflineManager
        private set

    private var networkCallback: ConnectivityManager.NetworkCallback? = null
    private val handler = Handler(Looper.getMainLooper())
    private var pendingNetworkApply: Runnable? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        offlineManager = OfflineManager(this)
        applyServerForCurrentNetwork()
        startNetworkMonitor()
    }

    fun applyServerForCurrentNetwork() {
        val prefs = getSharedPreferences("melody", Context.MODE_PRIVATE)
        val ssid = getCurrentSSID()
        val onHome = isOnHomeWifi(ssid)
        val server = if (onHome) {
            prefs.getString("server", "") ?: ""
        } else {
            val ext = prefs.getString("external_server", "") ?: ""
            ext.ifBlank { prefs.getString("server", "") ?: "" }
        }
        android.util.Log.d("MelodyApp", "Network: onHome=$onHome server=$server ssid=$ssid")

        // Parse host:port — handle plain "host:port" and URLs like "https://host:port"
        val addr = parseServerAddress(server)

        if (::mpd.isInitialized) {
            val oldHost = mpd.serverHost
            val oldPort = mpd.serverPort
            val oldSSL = mpd.useSSL
            if (addr.host != oldHost || addr.port != oldPort || addr.ssl != oldSSL) {
                android.util.Log.d("MelodyApp", "Switching MPD: ${schemeName(oldSSL)}://$oldHost:$oldPort -> ${schemeName(addr.ssl)}://${addr.host}:${addr.port}")
                mpd.disconnect()
                mpd = MpdClient(addr.host, addr.port, addr.ssl)
                if (addr.host.isNotBlank()) mpd.connect()
                PlaybackService.instance?.reconnect()
            }
        } else {
            mpd = MpdClient(addr.host, addr.port, addr.ssl)
            if (addr.host.isNotBlank()) mpd.connect()
            android.util.Log.d("MelodyApp", "Initial MPD: ${addr.host}:${addr.port}")
        }
    }

    private fun schemeName(ssl: Boolean) = if (ssl) "https" else "http"

    private data class ServerAddress(val host: String, val port: Int, val ssl: Boolean)

    private fun parseServerAddress(server: String): ServerAddress {
        if (server.isBlank()) return ServerAddress("", 6701, false)
        val ssl = server.startsWith("https://")
        val defaultPort = if (ssl) 443 else 6701
        // Strip protocol prefix if present
        val stripped = server.replace(Regex("^https?://"), "")
        // Split remaining "host:port" or just "host"
        val lastColon = stripped.lastIndexOf(':')
        return if (lastColon > 0) {
            val host = stripped.substring(0, lastColon)
            val port = stripped.substring(lastColon + 1).toIntOrNull() ?: defaultPort
            ServerAddress(host, port, ssl)
        } else {
            ServerAddress(stripped, defaultPort, ssl)
        }
    }

    fun isOnMobileData(): Boolean {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = cm.activeNetwork ?: return false
        val caps = cm.getNetworkCapabilities(network) ?: return false
        return caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)
    }

    fun isOnHomeWifi(): Boolean = isOnHomeWifi(getCurrentSSID())

    private fun isOnHomeWifi(currentSSID: String?): Boolean {
        val prefs = getSharedPreferences("melody", Context.MODE_PRIVATE)
        val homeSSID = prefs.getString("home_wifi_ssid", "") ?: ""
        val onWifi = isOnWifi()
        if (homeSSID.isBlank()) {
            // No home SSID configured — assume any WiFi connection is home
            return onWifi
        }
        if (currentSSID == null) {
            if (onWifi) {
                android.util.Log.d("MelodyApp", "SSID unavailable on WiFi; keeping local server preference")
                return true
            }
            return false
        }
        return currentSSID == homeSSID
    }

    private fun isOnWifi(): Boolean {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = cm.activeNetwork ?: return false
        val caps = cm.getNetworkCapabilities(network) ?: return false
        return caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
    }

    fun getCurrentSSID(): String? {
        @Suppress("DEPRECATION")
        val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as android.net.wifi.WifiManager
        @Suppress("DEPRECATION")
        val ssid = wm.connectionInfo?.ssid?.removeSurrounding("\"")?.takeIf { it != "<unknown ssid>" && it.isNotBlank() }
        if (ssid != null) return ssid

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            val network = cm.activeNetwork ?: return null
            val caps = cm.getNetworkCapabilities(network) ?: return null
            if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return null
            val wifiInfo = caps.transportInfo as? WifiInfo
            return wifiInfo?.ssid?.removeSurrounding("\"")?.takeIf { it != "<unknown ssid>" }
        }
        return null
    }

    /** Debounced network apply — waits for transitions to settle before switching servers. */
    private fun scheduleNetworkApply() {
        pendingNetworkApply?.let { handler.removeCallbacks(it) }
        val r = Runnable { applyServerForCurrentNetwork() }
        pendingNetworkApply = r
        handler.postDelayed(r, 1500)
    }

    private fun startNetworkMonitor() {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
            .build()
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { scheduleNetworkApply() }
            override fun onCapabilitiesChanged(network: Network, networkCapabilities: NetworkCapabilities) { scheduleNetworkApply() }
            override fun onLost(network: Network) { scheduleNetworkApply() }
        }
        networkCallback = callback
        cm.registerNetworkCallback(request, callback)
    }

    fun updateServer(server: String) {
        getSharedPreferences("melody", Context.MODE_PRIVATE)
            .edit().putString("server", server).apply()
        applyServerForCurrentNetwork()
    }

    companion object {
        lateinit var instance: MelodyApp
            private set
    }
}
