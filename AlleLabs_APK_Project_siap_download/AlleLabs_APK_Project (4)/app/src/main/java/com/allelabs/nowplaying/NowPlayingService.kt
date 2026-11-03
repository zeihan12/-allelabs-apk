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
import android.service.notification.NotificationListenerService
import android.util.Log
import java.util.UUID

/**
 * Membaca lagu yang sedang diputar lewat MediaSession, lalu mengirim "judul\nartis\n1/0"
 * ke ESP32 lewat BLE. Tidak perlu pairing.
 *
 * Tahan banting terhadap keychain yang dimatikan/dinyalakan lagi (koneksi lama "basi"):
 * - liveness check tiap 2 detik; tanpa balasan 3 detik = koneksi dianggap mati
 * - sambung ulang bergantian: langsung ke alamat yang diingat, lalu scan
 * - cache GATT dibersihkan tiap sambungan baru
 */
@SuppressLint("MissingPermission")
class NowPlayingService : NotificationListenerService() {

    companion object {
        private const val TAG = "AlleLabsNP"
        private const val DEVICE_NAME = "AlleLabs"
        private const val MAX_BYTES = 150
        private const val WRITE_TIMEOUT_MS = 3000L      // write tanpa balasan = koneksi mati
        private const val CONNECT_TIMEOUT_MS = 12000L   // sambung terlalu lama = ulangi
        private const val SCAN_RESTART_MS = 25000L      // scan lama tanpa hasil = restart
        private const val KEEPALIVE_MS = 2000L          // kirim ulang data berkala
        private val SERVICE_UUID: UUID = UUID.fromString("0000abcd-0000-1000-8000-00805f9b34fb")
        private val CHAR_UUID: UUID = UUID.fromString("00005678-0000-1000-8000-00805f9b34fb")
    }

    private val main = Handler(Looper.getMainLooper())
    private var sessionManager: MediaSessionManager? = null
    private var controller: MediaController? = null
    private var gatt: BluetoothGatt? = null
    private var writeChar: BluetoothGattCharacteristic? = null
    private var lastDevice: BluetoothDevice? = null
    private var scanning = false
    private var lastSent = ""
    private var reconnectAttempt = 0

    private var writeInFlight = false
    private var writeStartedAt = 0L
    private var lastWriteAt = 0L
    private var scanStartedAt = 0L
    private var connectStartedAt = 0L

    private val listenerComponent by lazy { ComponentName(this, NowPlayingService::class.java) }

    // ---------------- MediaSession ----------------

    private val sessionsListener =
        MediaSessionManager.OnActiveSessionsChangedListener { list -> pickController(list) }

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) { push() }
        override fun onPlaybackStateChanged(state: PlaybackState?) { push() }
        override fun onSessionDestroyed() { push() }
    }

    private val pollRunnable = object : Runnable {
        override fun run() {
            try {
                pickController(sessionManager?.getActiveSessions(listenerComponent))
            } catch (e: SecurityException) {
                Log.e(TAG, "Akses notifikasi belum diberikan", e)
            }
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

    // ---------------- Pengawas koneksi ----------------

    private val watchdogRunnable = object : Runnable {
        override fun run() {
            val now = System.currentTimeMillis()
            val g = gatt
            when {
                AppState.reconnectRequested -> {
                    AppState.reconnectRequested = false
                    forceReconnect("tombol sambung ulang")
                }
                writeInFlight && now - writeStartedAt > WRITE_TIMEOUT_MS ->
                    forceReconnect("data tidak dibalas keychain")
                g != null && !AppState.connected && now - connectStartedAt > CONNECT_TIMEOUT_MS ->
                    forceReconnect("sambung terlalu lama")
                scanning && now - scanStartedAt > SCAN_RESTART_MS -> {
                    AppState.log("scan di-restart")
                    stopScan()
                    startScan()
                }
                writeChar != null && !writeInFlight && now - lastWriteAt >= KEEPALIVE_MS -> {
                    lastSent = ""
                    push()
                }
            }
            main.postDelayed(this, 1000)
        }
    }

    // Bluetooth HP dimatikan/dinyalakan
    private val btReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, BluetoothAdapter.ERROR)) {
                BluetoothAdapter.STATE_OFF -> {
                    AppState.log("Bluetooth HP mati")
                    AppState.bleStatus = "Bluetooth HP mati"
                    resetConnection()
                }
                BluetoothAdapter.STATE_ON -> {
                    AppState.log("Bluetooth HP menyala")
                    AppState.bleStatus = "Mencari keychain..."
                    main.postDelayed({ startScan() }, 1000)
                }
            }
        }
    }

    override fun onListenerConnected() {
        AppState.listenerRunning = true
        AppState.connected = false
        AppState.bleStatus = "Mencari keychain..."
        AppState.log("service aktif")
        sessionManager = getSystemService(MediaSessionManager::class.java)
        try {
            sessionManager?.addOnActiveSessionsChangedListener(sessionsListener, listenerComponent)
        } catch (e: SecurityException) {
            Log.e(TAG, "Gagal menambah listener sesi", e)
        }
        try { registerReceiver(btReceiver, IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)) }
        catch (_: Exception) {}
        main.post(pollRunnable)
        main.post(watchdogRunnable)
        startScan()
    }

    override fun onListenerDisconnected() {
        AppState.listenerRunning = false
        AppState.connected = false
        AppState.log("service berhenti")
        main.removeCallbacks(pollRunnable)
        main.removeCallbacks(watchdogRunnable)
        main.removeCallbacks(pushRunnable)
        main.removeCallbacks(reconnectTask)
        try { unregisterReceiver(btReceiver) } catch (_: Exception) {}
        try { sessionManager?.removeOnActiveSessionsChangedListener(sessionsListener) } catch (_: Exception) {}
        controller?.unregisterCallback(controllerCallback)
        controller = null
        stopScan()
        resetConnection()
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
        if (writeInFlight) return false              // tunggu balasan write sebelumnya

        var head = "$title\n$artist".toByteArray(Charsets.UTF_8)
        if (head.size > MAX_BYTES - 2) {
            head = String(head.copyOf(MAX_BYTES - 2), Charsets.UTF_8).toByteArray(Charsets.UTF_8)
        }
        val bytes = head + "\n${if (playing) 1 else 0}".toByteArray(Charsets.UTF_8)

        writeInFlight = true
        writeStartedAt = System.currentTimeMillis()
        lastWriteAt = writeStartedAt

        val ok = if (Build.VERSION.SDK_INT >= 33) {
            g.writeCharacteristic(ch, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                BluetoothStatusCodes.SUCCESS
        } else {
            legacyWrite(g, ch, bytes)
        }
        if (!ok) writeInFlight = false
        return ok
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
            if (gatt != null || !scanning) return
            stopScan()
            lastDevice = result.device
            AppState.log("keychain ditemukan (${result.device.address})")
            connect(result.device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            AppState.log("scan gagal, kode $errorCode")
            main.postDelayed({ startScan() }, 5000)
        }
    }

    private fun startScan() {
        if (scanning || gatt != null) return
        try {
            val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
            if (adapter == null || !adapter.isEnabled) {
                AppState.bleStatus = "Bluetooth HP mati"
                main.postDelayed({ startScan() }, 5000)
                return
            }
            val filter = ScanFilter.Builder().setDeviceName(DEVICE_NAME).build()
            val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
            adapter.bluetoothLeScanner.startScan(listOf(filter), settings, scanCallback)
            scanning = true
            scanStartedAt = System.currentTimeMillis()
            AppState.bleStatus = "Mencari keychain..."
            AppState.log("mulai scan")
        } catch (e: Exception) {
            Log.w(TAG, "Scan gagal: ${e.message}")
            AppState.bleStatus = "Izin Bluetooth belum diberikan"
            main.postDelayed({ startScan() }, 5000)
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
        if (gatt != null) return
        AppState.bleStatus = "Menyambung..."
        AppState.log("menyambung ke ${device.address}")
        connectStartedAt = System.currentTimeMillis()
        gatt = device.connectGatt(this, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    /** Bersihkan koneksi lama sepenuhnya (tanpa menjadwalkan sambungan baru). */
    private fun resetConnection() {
        val g = gatt
        gatt = null
        writeChar = null
        writeInFlight = false
        lastSent = ""
        scanning = false
        AppState.connected = false
        try { g?.disconnect() } catch (_: Exception) {}
        try { g?.close() } catch (_: Exception) {}
    }

    // Sambung ulang bergantian: langsung ke alamat yang diingat, lalu scan
    private val reconnectTask = Runnable {
        if (gatt != null || scanning) return@Runnable
        val dev = lastDevice
        if (dev != null && reconnectAttempt % 2 == 1) connect(dev) else startScan()
    }

    private fun forceReconnect(reason: String) {
        AppState.log("sambung ulang: $reason")
        resetConnection()
        AppState.bleStatus = "Mencari keychain..."
        reconnectAttempt++
        main.removeCallbacks(reconnectTask)
        main.postDelayed(reconnectTask, 1200)
    }

    /** Bersihkan cache GATT Android supaya struktur layanan terbaca ulang dari keychain. */
    private fun refreshGattCache(g: BluetoothGatt) {
        try { g.javaClass.getMethod("refresh").invoke(g) } catch (_: Exception) {}
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            AppState.log("GATT state=$newState status=$status")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                refreshGattCache(g)
                g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                if (!g.requestMtu(247)) g.discoverServices()
            } else {
                main.post {
                    if (g === gatt) forceReconnect("koneksi terputus")
                    else try { g.close() } catch (_: Exception) {}
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val ch = g.getService(SERVICE_UUID)?.getCharacteristic(CHAR_UUID)
            if (ch == null) {
                AppState.log("layanan keychain tidak ditemukan")
                AppState.bleStatus = "Firmware keychain belum diperbarui"
                return
            }
            main.post {
                if (g !== gatt) return@post
                AppState.log("layanan ditemukan")
                writeChar = ch
                lastSent = ""
                writeInFlight = false
                push()
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int
        ) {
            main.post {
                if (g !== gatt) return@post
                writeInFlight = false
                if (status == BluetoothGatt.GATT_SUCCESS) {
                    if (!AppState.connected) {
                        AppState.connected = true
                        AppState.bleStatus = "Terhubung"
                        reconnectAttempt = 0
                        AppState.log("terhubung, data diterima keychain")
                    }
                } else {
                    forceReconnect("write gagal, status=$status")
                }
            }
        }
    }
}
