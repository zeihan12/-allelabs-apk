package com.allelabs.nowplaying

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.service.notification.NotificationListenerService
import android.util.Log
import java.util.UUID

/**
 * Membaca lagu yang sedang diputar (Spotify, YouTube Music, dll.) lewat MediaSession,
 * lalu mengirim "judul\nartis\n1/0" ke ESP32 lewat BLE. Tidak perlu pairing.
 */
@SuppressLint("MissingPermission")
class NowPlayingService : NotificationListenerService() {

    companion object {
        private const val TAG = "AlleLabsNP"
        private const val DEVICE_NAME = "AlleLabs"
        private const val MAX_BYTES = 150
        private val SERVICE_UUID: UUID = UUID.fromString("0000abcd-0000-1000-8000-00805f9b34fb")
        private val CHAR_UUID: UUID = UUID.fromString("00005678-0000-1000-8000-00805f9b34fb")
    }

    private val main = Handler(Looper.getMainLooper())
    private var sessionManager: MediaSessionManager? = null
    private var controller: MediaController? = null
    private var gatt: BluetoothGatt? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private var scanning = false
    private var lastSent = ""
    private var receiverRegistered = false
    private var scanStartedAt = 0L
    private var gattStartedAt = 0L
    private val scanRetry = Runnable { startScan() }

    private val listenerComponent by lazy { ComponentName(this, NowPlayingService::class.java) }

    // ---------------- MediaSession ----------------

    private val sessionsListener =
        MediaSessionManager.OnActiveSessionsChangedListener { list -> pickController(list) }

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) { push() }
        override fun onPlaybackStateChanged(state: PlaybackState?) { push() }
        override fun onSessionDestroyed() { push() }
    }

    // Cek ulang sesi aktif tiap 3 detik (kalau pindah dari satu aplikasi musik ke yang lain)
    private val pollRunnable = object : Runnable {
        override fun run() {
            try {
                pickController(sessionManager?.getActiveSessions(listenerComponent))
            } catch (e: SecurityException) {
                Log.e(TAG, "Akses notifikasi belum diberikan", e)
            }
            watchdog()
            main.postDelayed(this, 3000)
        }
    }

    private fun pickController(list: List<MediaController>?) {
        val best = list?.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING }
            ?: list?.firstOrNull()
        if (best?.sessionToken != controller?.sessionToken) {
            controller?.unregisterCallback(controllerCallback)
            controller = best
            controller?.registerCallback(controllerCallback, main)
            lastSent = ""
        }
        push()
    }

    override fun onListenerConnected() {
        AppState.listenerRunning = true
        AppState.connected = false
        AppState.bleStatus = "Mencari keychain..."
        sessionManager = getSystemService(MediaSessionManager::class.java)
        try {
            sessionManager?.addOnActiveSessionsChangedListener(sessionsListener, listenerComponent)
        } catch (e: SecurityException) {
            Log.e(TAG, "Gagal menambah listener sesi", e)
        }
        if (!receiverRegistered) {
            registerReceiver(btReceiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED))
            receiverRegistered = true
        }
        main.post(pollRunnable)
        startScan()
    }

    override fun onListenerDisconnected() {
        AppState.listenerRunning = false
        AppState.connected = false
        main.removeCallbacks(pollRunnable)
        main.removeCallbacks(pushRunnable)
        main.removeCallbacks(scanRetry)
        if (receiverRegistered) {
            try { unregisterReceiver(btReceiver) } catch (_: Exception) {}
            receiverRegistered = false
        }
        try { sessionManager?.removeOnActiveSessionsChangedListener(sessionsListener) } catch (_: Exception) {}
        controller?.unregisterCallback(controllerCallback)
        controller = null
        stopScan()
        gatt?.close()
        gatt = null
        writeChar = null
    }

    // ---------------- Kirim ke ESP32 ----------------

    private val pushRunnable = Runnable { push() }

    private fun push() {
        main.removeCallbacks(pushRunnable)

        val c = controller
        val md = c?.metadata
        val title = (md?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: "").replace("\n", " ")
        val artist = (md?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: md?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST) ?: "").replace("\n", " ")
        val playing = c?.playbackState?.state == PlaybackState.STATE_PLAYING

        AppState.title = title
        AppState.artist = artist
        AppState.playing = playing
        if (writeChar == null) return

        val payload = "$title\n$artist\n${if (playing) 1 else 0}"
        if (payload == lastSent) return

        if (writeData(title, artist, playing)) lastSent = payload
        else main.postDelayed(pushRunnable, 400)     // GATT sibuk, coba lagi
    }

    private fun writeData(title: String, artist: String, playing: Boolean): Boolean {
        val g = gatt ?: return false
        val ch = writeChar ?: return false

        // Potong aman supaya status play/pause di akhir tidak hilang
        var head = "$title\n$artist".toByteArray(Charsets.UTF_8)
        if (head.size > MAX_BYTES - 2) {
            head = String(head.copyOf(MAX_BYTES - 2), Charsets.UTF_8).toByteArray(Charsets.UTF_8)
        }
        val bytes = head + "\n${if (playing) 1 else 0}".toByteArray(Charsets.UTF_8)

        return if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            legacyWrite(g, ch, bytes)
        }
    }

    @Suppress("DEPRECATION")
    private fun legacyWrite(g: BluetoothGatt, ch: BluetoothGattCharacteristic, bytes: ByteArray): Boolean {
        ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        ch.value = bytes
        return g.writeCharacteristic(ch)
    }

    // ---------------- BLE: scan & koneksi ----------------

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            stopScan()
            connect(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            scheduleScan(5000)
        }
    }

    private fun startScan() {
        if (scanning || gatt != null) return
        try {
            val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
            if (adapter == null || !adapter.isEnabled) {
                AppState.bleStatus = "Bluetooth HP mati"
                scheduleScan(5000)
                return
            }
            val filter = ScanFilter.Builder().setDeviceName(DEVICE_NAME).build()
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_POWER).build()
            adapter.bluetoothLeScanner.startScan(listOf(filter), settings, scanCallback)
            scanning = true
            scanStartedAt = SystemClock.elapsedRealtime()
            AppState.bleStatus = "Mencari keychain..."
        } catch (e: Exception) {
            // Biasanya izin Bluetooth belum diberikan: coba lagi nanti
            Log.w(TAG, "Scan gagal: ${e.message}")
            AppState.bleStatus = "Izin Bluetooth belum diberikan"
            scheduleScan(5000)
        }
    }

    private fun stopScan() {
        if (!scanning) return
        try {
            val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
            adapter?.bluetoothLeScanner?.stopScan(scanCallback)
        } catch (_: Exception) {}
        scanning = false
    }

    private fun connect(device: BluetoothDevice) {
        AppState.bleStatus = "Menyambung..."
        gattStartedAt = SystemClock.elapsedRealtime()
        main.post {
            gatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }
    }

    private fun cleanupAndRescan(g: BluetoothGatt) {
        try { g.close() } catch (_: Exception) {}
        if (gatt !== g) return          // koneksi lama, abaikan
        gatt = null
        writeChar = null
        lastSent = ""
        AppState.connected = false
        AppState.bleStatus = "Mencari keychain..."
        scheduleScan(2000)
    }

    private fun scheduleScan(delayMs: Long) {
        main.removeCallbacks(scanRetry)
        main.postDelayed(scanRetry, delayMs)
    }

    /** Buang semua status BLE lama (dipakai saat Bluetooth HP dimatikan/dinyalakan). */
    private fun resetBle() {
        stopScan()
        scanning = false                 // scanner lama sudah tidak berlaku
        try { gatt?.close() } catch (_: Exception) {}
        gatt = null
        writeChar = null
        lastSent = ""
        AppState.connected = false
    }

    private val btReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                BluetoothAdapter.STATE_TURNING_OFF, BluetoothAdapter.STATE_OFF -> {
                    resetBle()
                    AppState.bleStatus = "Bluetooth HP mati"
                }
                BluetoothAdapter.STATE_ON -> {
                    resetBle()
                    AppState.bleStatus = "Mencari keychain..."
                    scheduleScan(1500)
                }
            }
        }
    }

    /** Pengaman: kalau scan/koneksi macet, ulangi otomatis. */
    private fun watchdog() {
        val now = SystemClock.elapsedRealtime()
        val g = gatt
        if (g == null) {
            if (!scanning) startScan()
            else if (now - scanStartedAt > 20000) { stopScan(); startScan() }
        } else if (!AppState.connected && now - gattStartedAt > 15000) {
            cleanupAndRescan(g)
        }
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                if (!g.requestMtu(247)) g.discoverServices()
            } else {
                main.post { cleanupAndRescan(g) }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val ch = g.getService(SERVICE_UUID)?.getCharacteristic(CHAR_UUID)
            if (ch == null) {
                Log.w(TAG, "Characteristic Alle Labs tidak ditemukan")
                AppState.bleStatus = "Firmware keychain belum diperbarui"
                return
            }
            writeChar = ch
            lastSent = ""
            AppState.connected = true
            AppState.bleStatus = "Terhubung"
            main.post { push() }
        }
    }
}
