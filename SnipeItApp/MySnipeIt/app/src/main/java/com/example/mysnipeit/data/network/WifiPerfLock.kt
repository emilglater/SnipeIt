package com.example.mysnipeit.data.network

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.PowerManager
import android.util.Log

/**
 * Holds a high-performance Wi-Fi lock (and a companion CPU wake lock) for the
 * duration of the live video view.
 *
 * Why: the Pi's soft-AP link is fragile, and Android's client-side Wi-Fi
 * power-save — the radio periodically sleeps and batches traffic — interacts
 * badly with the AP's own buffering, producing the ~20-30 s "deaf" windows
 * that freeze the RTSP stream. A [WifiManager.WifiLock] in
 * `WIFI_MODE_FULL_LOW_LATENCY` asks Android to keep the Wi-Fi radio awake and
 * latency-optimised while it's held, which attacks that root cause from the
 * client side. Pi-side traces showed suppressing power-save/scanning pushed
 * time-to-freeze from ~1 min to 5+ min.
 *
 * `WIFI_MODE_FULL_LOW_LATENCY` is API 29+; on older devices (min SDK 26) we
 * fall back to the deprecated `WIFI_MODE_FULL_HIGH_PERF`, which is the closest
 * equivalent (keeps the radio awake, just without the latency hint).
 *
 * Scope: acquire when the live view opens, release when it closes. Both locks
 * are non-reference-counted and null-guarded so double-acquire / double-release
 * are safe. Requires the `WAKE_LOCK` and `ACCESS_WIFI_STATE` permissions
 * (already in the manifest).
 */
object WifiPerfLock {
    private const val TAG = "WifiPerfLock"

    private var wifiLock: WifiManager.WifiLock? = null
    private var wakeLock: PowerManager.WakeLock? = null

    /** Acquire both locks if not already held. Idempotent. */
    fun acquire(context: Context) {
        val ctx = context.applicationContext
        try {
            if (wifiLock == null) {
                val wm = ctx.getSystemService(Context.WIFI_SERVICE) as WifiManager
                val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                } else {
                    @Suppress("DEPRECATION")
                    WifiManager.WIFI_MODE_FULL_HIGH_PERF
                }
                wifiLock = wm.createWifiLock(mode, "MySnipeIt:rtsp").apply {
                    setReferenceCounted(false)
                    acquire()
                }
                Log.d(TAG, "Wi-Fi performance lock acquired (mode=$mode)")
            }
            if (wakeLock == null) {
                val pm = ctx.getSystemService(Context.POWER_SERVICE) as PowerManager
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MySnipeIt:rtsp").apply {
                    setReferenceCounted(false)
                    acquire()
                }
                Log.d(TAG, "CPU wake lock acquired")
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to acquire locks: ${e.message}")
        }
    }

    /** Release both locks if held. Idempotent. */
    fun release() {
        try {
            wifiLock?.let { if (it.isHeld) it.release() }
            wakeLock?.let { if (it.isHeld) it.release() }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to release locks: ${e.message}")
        } finally {
            wifiLock = null
            wakeLock = null
            Log.d(TAG, "Locks released")
        }
    }
}
