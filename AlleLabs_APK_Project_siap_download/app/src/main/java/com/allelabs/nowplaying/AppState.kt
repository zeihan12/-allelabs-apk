package com.allelabs.nowplaying

/** Status bersama antara service (latar belakang) dan layar aplikasi. */
object AppState {
    @Volatile var listenerRunning = false          // akses notifikasi aktif & service berjalan
    @Volatile var connected = false                // terhubung ke keychain
    @Volatile var bleStatus = "Mencari keychain..."
    @Volatile var title = ""
    @Volatile var artist = ""
    @Volatile var playing = false
}
