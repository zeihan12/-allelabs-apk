package com.allelabs.nowplaying

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Status bersama antara service (latar belakang) dan layar aplikasi. */
object AppState {
    @Volatile var listenerRunning = false          // akses notifikasi aktif & service berjalan
    @Volatile var connected = false                // terhubung ke keychain
    @Volatile var bleStatus = "Mencari keychain..."
    @Volatile var title = ""
    @Volatile var artist = ""
    @Volatile var playing = false
    @Volatile var reconnectRequested = false       // tombol "Sambung ulang" ditekan

    private val lines = ArrayList<String>()

    @Synchronized
    fun log(msg: String) {
        val t = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        lines.add("$t $msg")
        while (lines.size > 8) lines.removeAt(0)
    }

    @Synchronized
    fun logText(): String = lines.joinToString("\n")
}
