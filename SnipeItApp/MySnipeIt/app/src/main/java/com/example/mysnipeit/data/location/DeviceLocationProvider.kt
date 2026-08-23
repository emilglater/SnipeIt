package com.example.mysnipeit.data.location

import android.annotation.SuppressLint
import android.content.Context
import android.location.Location
import android.os.Looper
import android.util.Log
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority
import com.google.android.gms.maps.model.LatLng
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Thin wrapper around [FusedLocationProviderClient] that exposes the device's
 * current GPS position as a [StateFlow]<LatLng?> (null = no permission, no
 * fix yet, or provider stopped).
 *
 * The eventual ballistics calculator reads the latest value here as one of
 * its inputs. The map screen reads it for the "Your Location" marker.
 *
 * Lifecycle: caller must invoke [start] after the location permission has
 * been granted, and [stop] on teardown (we do that in
 * SniperViewModel.onCleared()).
 */
class DeviceLocationProvider(context: Context) {

    private val client: FusedLocationProviderClient =
        LocationServices.getFusedLocationProviderClient(context.applicationContext)

    private val _location = MutableStateFlow<LatLng?>(null)
    val location: StateFlow<LatLng?> = _location.asStateFlow()

    // Altitude in metres, MSL when the underlying GPS provider reports it.
    // Null when the fix has no vertical component (some indoor / cellular-
    // assisted fixes don't include altitude). The ballistic solver tolerates
    // null by falling back to the target's altitude (treats the shot as
    // level), so this flow stays null rather than guessing.
    private val _altitudeM = MutableStateFlow<Double?>(null)
    val altitudeM: StateFlow<Double?> = _altitudeM.asStateFlow()

    private val callback = object : LocationCallback() {
        override fun onLocationResult(result: LocationResult) {
            result.lastLocation?.let { loc ->
                _location.value = LatLng(loc.latitude, loc.longitude)
                _altitudeM.value = if (loc.hasAltitude()) loc.altitude else null
            }
        }
    }

    private var started = false

    @SuppressLint("MissingPermission")  // caller guarantees permission via start()
    fun start() {
        if (started) return

        // Seed with the last cached fix so the UI gets *something* before the
        // first fresh callback (which can take 5-10s in a cold-start).
        client.lastLocation.addOnSuccessListener { loc: Location? ->
            if (loc != null && _location.value == null) {
                _location.value = LatLng(loc.latitude, loc.longitude)
                if (loc.hasAltitude()) _altitudeM.value = loc.altitude
                Log.d(TAG, "Seeded with last known location: ${loc.latitude}, ${loc.longitude}")
            }
        }

        // 10s default cadence is plenty for ballistics + map; fast updates
        // would just burn battery without changing the firing solution.
        val request = LocationRequest.Builder(Priority.PRIORITY_HIGH_ACCURACY, 10_000L)
            .setMinUpdateIntervalMillis(5_000L)
            .build()

        client.requestLocationUpdates(request, callback, Looper.getMainLooper())
        started = true
        Log.d(TAG, "Location updates started")
    }

    fun stop() {
        if (!started) return
        client.removeLocationUpdates(callback)
        started = false
        Log.d(TAG, "Location updates stopped")
    }

    companion object {
        private const val TAG = "DeviceLocationProvider"
    }
}
