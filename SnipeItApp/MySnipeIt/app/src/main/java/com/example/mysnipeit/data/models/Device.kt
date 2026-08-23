package com.example.mysnipeit.data.models


data class Device(
    val id: String,
    val name: String,
    val location: String,
    val longitude: Double,
    val latitude: Double,
    val status: DeviceStatus,
    val batteryLevel: Int,
    val ipAddress: String
)

enum class DeviceStatus {
    ACTIVE,
    INACTIVE,
    SCANNING,
    ERROR
}