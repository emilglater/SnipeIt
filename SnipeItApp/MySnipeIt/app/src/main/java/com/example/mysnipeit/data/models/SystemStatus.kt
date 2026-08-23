package com.example.mysnipeit.data.models

data class SystemStatus(
    val connectionStatus: ConnectionState,
    val batteryLevel: Int?,
    val cameraStatus: Boolean,
    val gpsStatus: Boolean,
    val rangefinderStatus: Boolean,
    val microphoneStatus: Boolean,
    val lastHeartbeat: Long,
    val cpuTemperature: Double? = null,
    val gpsLatitude: Double? = null,
    val gpsLongitude: Double? = null
)

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    ERROR
}