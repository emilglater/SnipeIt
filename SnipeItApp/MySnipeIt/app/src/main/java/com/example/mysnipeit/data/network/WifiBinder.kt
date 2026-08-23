package com.example.mysnipeit.data.network

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiManager
import android.util.Log
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/**
 * Binds the Android process to the currently joined WiFi network so that all
 * sockets (WebSocket, HTTP, RTSP) are routed through it instead of cellular.
 *
 * The RPi5 access point ("MyHotspot", gateway 10.42.1.1) has no Internet upstream,
 * which would otherwise cause Android to prefer cellular when a SIM is present.
 *
 * Also detects the gateway IP of the joined network and falls back to a known
 * default if detection fails.
 */
object WifiBinder {
    private const val TAG = "WifiBinder"

    /** Default RPi5 AP gateway. Used as fallback when DHCP info isn't readable. */
    const val FALLBACK_GATEWAY = "10.42.1.1"

    private var callback: ConnectivityManager.NetworkCallback? = null
    private var boundNetwork: Network? = null

    /**
     * Suspends until the process is bound to a WiFi network.
     * Returns true on success, false if no callback fired (caller can still try
     * with the fallback IP, but RTSP/HTTP may end up on cellular if a SIM exists).
     */
    suspend fun bindToWifi(context: Context): Boolean = suspendCancellableCoroutine { cont ->
        val cm = context.applicationContext
            .getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager

        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .build()

        val cb = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                Log.d(TAG, "WiFi network available, binding process")
                cm.bindProcessToNetwork(network)
                boundNetwork = network
                if (cont.isActive) cont.resume(true)
            }

            override fun onLost(network: Network) {
                Log.w(TAG, "WiFi network lost")
                if (boundNetwork == network) {
                    cm.bindProcessToNetwork(null)
                    boundNetwork = null
                }
            }
        }

        callback = cb
        cm.requestNetwork(request, cb)

        cont.invokeOnCancellation {
            try {
                cm.unregisterNetworkCallback(cb)
            } catch (_: Exception) { }
        }
    }

    /** Returns the gateway IP of the joined WiFi, or [FALLBACK_GATEWAY]. */
    fun getGatewayIp(context: Context): String {
        return try {
            @Suppress("DEPRECATION")
            val wm = context.applicationContext
                .getSystemService(Context.WIFI_SERVICE) as WifiManager
            @Suppress("DEPRECATION")
            val gw = wm.dhcpInfo?.gateway ?: 0
            if (gw == 0) {
                Log.w(TAG, "Gateway unknown, using fallback $FALLBACK_GATEWAY")
                FALLBACK_GATEWAY
            } else {
                // dhcpInfo stores the IP as a little-endian 32-bit int.
                val ip = "${gw and 0xff}.${(gw shr 8) and 0xff}." +
                        "${(gw shr 16) and 0xff}.${(gw shr 24) and 0xff}"
                Log.d(TAG, "Detected gateway: $ip")
                ip
            }
        } catch (e: Exception) {
            Log.e(TAG, "Gateway detection failed: ${e.message}")
            FALLBACK_GATEWAY
        }
    }

    fun release(context: Context) {
        try {
            val cm = context.applicationContext
                .getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
            cm.bindProcessToNetwork(null)
            callback?.let { cm.unregisterNetworkCallback(it) }
        } catch (e: Exception) {
            Log.e(TAG, "Release failed: ${e.message}")
        } finally {
            callback = null
            boundNetwork = null
            Log.d(TAG, "WiFi binding released")
        }
    }
}
