package com.example.mysnipeit.data.network

import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Network Testing Utility - FINAL WORKING VERSION
 *
 * Note: Android's InetAddress.isReachable() is unreliable on many devices.
 * We use TCP socket connection as a more reliable test.
 */
class NetworkTester {

    companion object {
        private const val TAG = "NetworkTester"
        private const val CONNECTION_TIMEOUT = 5000 // 5 seconds
    }

    /**
     * Test if device is reachable via TCP connection
     * This is more reliable than InetAddress.isReachable() on Android
     */
    suspend fun pingDevice(ipAddress: String): Boolean = withContext(Dispatchers.IO) {
        return@withContext try {
            Log.d(TAG, "🔍 Testing connectivity to $ipAddress...")

            // Try to connect to port 8555 as a "ping" test
            // If server is running, this is the most reliable test
            val connected = testPort(ipAddress, 8555)

            if (connected) {
                Log.d(TAG, "✅ Device reachable: $ipAddress")
            } else {
                Log.d(TAG, "❌ Device not reachable: $ipAddress")
            }

            connected
        } catch (e: Exception) {
            Log.e(TAG, "❌ Connectivity test error for $ipAddress: ${e.message}")
            false
        }
    }

    /**
     * Test if specific ports are open
     */
    suspend fun scanPorts(
        ipAddress: String,
        ports: List<Int>
    ): Map<Int, Boolean> = withContext(Dispatchers.IO) {
        val results = mutableMapOf<Int, Boolean>()

        for (port in ports) {
            results[port] = testPort(ipAddress, port)
        }

        Log.d(TAG, "📊 Port scan results for $ipAddress: $results")
        return@withContext results
    }

    /**
     * Test if a specific port is open using TCP socket
     * This is the most reliable method on Android
     */
    private suspend fun testPort(ipAddress: String, port: Int): Boolean = withContext(Dispatchers.IO) {
        return@withContext try {
            Log.d(TAG, "🔍 Testing port $port on $ipAddress...")

            val connected = withTimeoutOrNull(CONNECTION_TIMEOUT.toLong()) {
                try {
                    val socket = Socket()
                    val socketAddress = InetSocketAddress(ipAddress, port)
                    socket.connect(socketAddress, CONNECTION_TIMEOUT)
                    socket.close()
                    true
                } catch (e: Exception) {
                    Log.d(TAG, "Port $port test exception: ${e.javaClass.simpleName}")
                    false
                }
            } ?: false

            if (connected) {
                Log.d(TAG, "✅ Port $port is OPEN")
            } else {
                Log.d(TAG, "❌ Port $port is CLOSED")
            }

            connected
        } catch (e: Exception) {
            Log.e(TAG, "❌ Error testing port $port: ${e.message}")
            false
        }
    }
}