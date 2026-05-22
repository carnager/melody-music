package com.melody.app

import android.app.Application
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiInfo
import android.os.Build

class MelodyApp : Application() {
    lateinit var mpd: MpdClient
        private set
    lateinit var offlineManager: OfflineManager
        private set

    private var networkCallback: ConnectivityManager.NetworkCallback? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        offlineManager = OfflineManager(this)
        applyServerForCurrentNetwork()
        startNetworkMonitor()
    }

    fun applyServerForCurrentNetwork() {
        val prefs = getSharedPreferences("melody", Context.MODE_PRIVATE)
        val onHome = isOnHomeWifi()
        val server = if (onHome) {
            prefs.getString("server", "") ?: ""
        } else {
            val ext = prefs.getString("external_server", "") ?: ""
            ext.ifBlank { prefs.getString("server", "") ?: "" }
        }
        android.util.Log.d("MelodyApp", "Network: onHome=$onHome server=$server ssid=${getCurrentSSID()}")

        // Parse host:port — handle plain "host:port" and URLs like "https://host:port"
        val (host, port) = parseServerAddress(server)

        if (::mpd.isInitialized) {
            val oldHost = mpd.serverHost
            val oldPort = mpd.serverPort
            if (host != oldHost || port != oldPort) {
                android.util.Log.d("MelodyApp", "Switching MPD: $oldHost:$oldPort -> $host:$port")
                mpd.disconnect()
                mpd = MpdClient(host, port)
                if (host.isNotBlank()) mpd.connect()
                PlaybackService.instance?.reconnect()
            }
        } else {
            mpd = MpdClient(host, port)
            if (host.isNotBlank()) mpd.connect()
            android.util.Log.d("MelodyApp", "Initial MPD: $host:$port")
        }
    }

    private fun parseServerAddress(server: String): Pair<String, Int> {
        if (server.isBlank()) return "" to 6701
        // Strip protocol prefix if present
        val stripped = server.replace(Regex("^https?://"), "")
        // Split remaining "host:port" or just "host"
        val lastColon = stripped.lastIndexOf(':')
        return if (lastColon > 0) {
            val host = stripped.substring(0, lastColon)
            val port = stripped.substring(lastColon + 1).toIntOrNull() ?: 6701
            host to port
        } else {
            stripped to 6701
        }
    }

    fun isOnHomeWifi(): Boolean {
        val prefs = getSharedPreferences("melody", Context.MODE_PRIVATE)
        val homeSSID = prefs.getString("home_wifi_ssid", "") ?: ""
        if (homeSSID.isBlank()) return true
        val currentSSID = getCurrentSSID() ?: return true // unknown = assume home
        return currentSSID == homeSSID
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

    private fun startNetworkMonitor() {
        val cm = getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addTransportType(NetworkCapabilities.TRANSPORT_CELLULAR)
            .build()
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) { applyServerForCurrentNetwork() }
            override fun onLost(network: Network) { applyServerForCurrentNetwork() }
            override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
                applyServerForCurrentNetwork()
            }
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
