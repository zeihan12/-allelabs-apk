#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <FluxGarage_RoboEyes.h>  // Library animasi mata FluxGarage
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <time.h>
#include <esp_sleep.h>
#include <ESPmDNS.h>
#include "qrcode.h"         // QR bawaan ESP32 core (esp_qrcode) - TIDAK perlu library ricmoo
#include <NimBLEDevice.h>   // NimBLE-Arduino 2.x (by h2zero)
#if __has_include("host/ble_hs.h")
  #include "host/ble_hs.h"
#else
  #include "nimble/nimble/host/include/host/ble_hs.h"
#endif

// ==========================================
// INCLUDE FILE HEADER VIDEO ANIMASI (Slot 1-3)
// ==========================================
#include "videoframe1.h"
#include "videoframe2.h"
#include "videoframe3.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1

Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// RoboEyes memakai display SH1106 yang sama dengan seluruh firmware.
RoboEyes<Adafruit_SH1106G> roboEyes(display);

#define BUTTON_PIN 3 
#define SLOT_SIZE 1024

const char* ssid = "Alle Labs Keychain";
const char* password = "allelabs123";

WebServer server(80);
Preferences prefs;

// ==========================================
// LOCAL FIRMWARE UPDATE (OTA)
// ==========================================
bool otaUploadOK = false;
bool otaUploadStarted = false;
bool otaUploadError = false;
String otaUploadMessage = "Belum ada proses update.";

bool isHotspotActive = false;
unsigned long buttonPressStart = 0;
bool isButtonPressed = false;

// Memori slot untuk Slot 4 & 5 (Custom Web)
uint8_t slotData[6][1024]; 
bool slotHasData[6] = {false, false, false, false, false, false};
int currentSlot = 1;

// Playback metadata (AMS iPhone + optional extended Android companion payload)
volatile uint32_t trackDurationSec = 0;
volatile uint32_t trackPositionSec = 0;
volatile uint8_t phoneVolumePct = 255; // 255 = unknown
unsigned long lastVolumeOverlayMs = 0;
unsigned long lastPlaybackUpdateMs = 0;
uint8_t pendingShortClicks = 0;
unsigned long clickSequenceDeadline = 0;

// OLED power saving and persistent slot
const unsigned long SCREEN_IDLE_TIMEOUT_MS = 120000UL; // 2 menit tanpa aktivitas
const unsigned long SCREEN_OFF_DURATION_MS = 30000UL;  // 30 detik
unsigned long lastUserActivityMs = 0;
unsigned long screenOffStartedMs = 0;
bool screenIsOff = false;

// Phone-synced clock. Android companion may append Unix time as line 7.
uint32_t syncedUnixTime = 0;
unsigned long syncedAtMillis = 0;
bool clockSynced = false;
int16_t clockTimezoneOffsetMin = 0;
uint8_t slot6Theme = 0; // 0=Vinyl, 1=Cassette, 2=Cyberpunk, 3=CD, 4=Winamp, 5=Spectrum Matrix


// Variabel Kontrol Animasi Video
unsigned long lastFrameTime = 0;
int videoFrameIndex = 0;
const int frameDelayMs = 100; // Kecepatan putar video (100ms = 10 FPS)

// Hitung total frame secara otomatis dari masing-masing file header video
const int totalFramesV1 = sizeof(video_frames_1) / sizeof(video_frames_1[0]);
const int totalFramesV2 = sizeof(video_frames_2) / sizeof(video_frames_2[0]);
const int totalFramesV3 = sizeof(video_frames_3) / sizeof(video_frames_3[0]);

// ==========================================
// SPOTIFY MODE (SLOT 6) - GLOBAL
// ==========================================
NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* chrCmd = nullptr; // [ANDROID] notify perintah tombol -> app Android (UUID 9ABC)

bool bleActive = false;                 // BLE sedang menyala (advertising/connected)
volatile bool bleConnected = false;     // iPhone terhubung
volatile bool bleSecured = false;       // sudah paired/encrypted
bool amsReady = false;                  // subscribe AMS sukses
unsigned long lastAmsAttempt = 0;

String songTitle = "";
String songArtist = "";
String marqueeText = "";
volatile bool songDirty = true;         // teks marquee perlu dibangun ulang
int marqueeX = SCREEN_WIDTH;
float vinylAngle = 0;
int titleX = SCREEN_WIDTH;
int artistX = SCREEN_WIDTH;
float eqPhase = 0;
volatile bool amsPlaying = false;       // status play/pause dari iPhone
volatile bool amsPlayKnown = false;     // true kalau info play/pause sudah diterima
volatile bool androidMode = false;      // true kalau HP Android mengirim data lewat characteristic tulis
bool bleSecReqSent = false;             // permintaan pairing (iPhone) sudah dikirim
unsigned long bleConnMillis = 0;        // waktu koneksi terakhir
uint8_t amsFailCount = 0;               // jumlah kegagalan AMS berturut-turut

void updateOLEDText(String line1, String line2, String line3) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setCursor(0, 0);
  display.println("= ALLE LABS =");
  display.setCursor(0, 20); display.println(line1);
  display.setCursor(0, 35); display.println(line2);
  display.setCursor(0, 50); display.println(line3);
  display.display();
}

// ==========================================
// PERSISTENT NVS STORAGE (KHUSUS SLOT 4 & 5)
// ==========================================
void loadCustomSlots() {
  prefs.begin("keychain", true); 
  for (int i = 4; i <= 5; i++) {
    String keyData = "slot" + String(i);
    String keyFlag = "flag" + String(i);

    slotHasData[i] = prefs.getBool(keyFlag.c_str(), false);
    if (slotHasData[i]) {
      prefs.getBytes(keyData.c_str(), slotData[i], SLOT_SIZE);
    } else {
      memset(slotData[i], 0, SLOT_SIZE);
    }
  }
  prefs.end();
  
  slotHasData[1] = true;
  slotHasData[2] = true;
  slotHasData[3] = true;
}

void saveCustomSlotToNVS(int slot, const uint8_t* data) {
  if (slot < 4 || slot > 5) return;
  memcpy(slotData[slot], data, SLOT_SIZE);
  slotHasData[slot] = true;

  prefs.begin("keychain", false);
  prefs.putBytes(("slot" + String(slot)).c_str(), data, SLOT_SIZE);
  prefs.putBool(("flag" + String(slot)).c_str(), true);
  prefs.end();
}

void deleteCustomSlotFromNVS(int slot) {
  if (slot < 4 || slot > 5) return;
  slotHasData[slot] = false;
  memset(slotData[slot], 0, SLOT_SIZE);

  prefs.begin("keychain", false);
  prefs.remove(("slot" + String(slot)).c_str());
  prefs.putBool(("flag" + String(slot)).c_str(), false);
  prefs.end();
}

// ==========================================
// SPOTIFY MODE (SLOT 6) - BLE + AMS + UI
// ==========================================

// Ubah UTF-8 jadi ASCII (font bawaan GFX hanya ASCII)
String cleanUtf8(const uint8_t* d, size_t n) {
  String out;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = d[i];
    if (c >= 0x20 && c < 0x7F) out += (char)c;
    else if (c >= 0xC0) out += '?';   // lead byte karakter non-ASCII
  }
  return out;
}

static ble_uuid128_t amsSvcUuid;
static ble_uuid128_t amsEntityUuid;
static ble_uuid128_t amsRemoteUuid;
uint16_t amsRemoteHandle = 0;

uint16_t bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
uint16_t amsSvcStart = 0, amsSvcEnd = 0, amsEntityHandle = 0, amsCccdHandle = 0;
volatile uint8_t amsState = 0;          // 0=idle 1=proses 2=siap 3=gagal
volatile bool bleNeedRestart = false;   // minta restart BLE dari loop()

void initAmsUuids() {
  const uint8_t s[16] = {0xdc,0xf8,0x55,0xad,0x02,0xc5,0xf4,0x8e,0x3a,0x43,0x36,0x0f,0x2b,0x50,0xd3,0x89};
  const uint8_t e[16] = {0x02,0xc1,0x96,0xba,0x92,0xbb,0x0c,0x9a,0x1f,0x41,0x8d,0x80,0xce,0xab,0x7c,0x2f};
  const uint8_t r[16] = {0xc2,0x51,0xca,0xf7,0x56,0x0e,0xdf,0xb8,0x8a,0x4a,0xb1,0x57,0xd8,0x81,0x3c,0x9b};
  amsSvcUuid.u.type = BLE_UUID_TYPE_128;
  amsEntityUuid.u.type = BLE_UUID_TYPE_128;
  amsRemoteUuid.u.type = BLE_UUID_TYPE_128;
  memcpy(amsSvcUuid.value, s, 16);
  memcpy(amsEntityUuid.value, e, 16);
  memcpy(amsRemoteUuid.value, r, 16);
}

uint32_t parseUnsignedField(const String& value, int fieldIndex) {
  int start = 0;
  for (int i = 0; i < fieldIndex; i++) {
    int comma = value.indexOf(',', start);
    if (comma < 0) return 0;
    start = comma + 1;
  }
  int end = value.indexOf(',', start);
  String part = (end < 0) ? value.substring(start) : value.substring(start, end);
  part.trim();
  return (uint32_t)part.toInt();
}

void amsHandleEntityData(const uint8_t* d, size_t len) {
  if (len < 3) return;
  String val = cleanUtf8(d + 3, len - 3);
  if (d[0] == 0x02) {                              // Entity Track
    if (d[1] == 0x00) songArtist = val;            // Artist
    else if (d[1] == 0x02) songTitle = val;        // Title
    else if (d[1] == 0x03) trackDurationSec = (uint32_t)val.toFloat(); // Duration
    else return;
  } else if (d[0] == 0x00 && d[1] == 0x01) {       // Player PlaybackInfo: state,rate,elapsed
    amsPlaying = (val.length() > 0 && val[0] == '1');
    amsPlayKnown = true;
    trackPositionSec = parseUnsignedField(val, 2);
    lastPlaybackUpdateMs = millis();
  } else if (d[0] == 0x00 && d[1] == 0x02) {       // Player Volume (0.0 - 1.0)
    float v = val.toFloat();
    if (v < 0) v = 0; if (v > 1) v = 1;
    phoneVolumePct = (uint8_t)(v * 100.0f + 0.5f);
    lastVolumeOverlayMs = millis();
  } else {
    return;
  }
  songDirty = true;
  Serial.printf("[AMS] entity %d attr %d = %s\n", d[0], d[1], val.c_str());
}

void amsFail(const char* msg, int code) {
  amsState = 3;
  amsFailCount++;
  Serial.printf("[AMS] GAGAL: %s (kode=%d)\n", msg, code);
}

static int amsOnWritePlayer(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg) {
  if (err->status != 0) Serial.printf("[AMS] request Player gagal (kode=%d)\n", err->status);
  amsState = 2;
  amsReady = true;
  songDirty = true;
  Serial.println("[AMS] siap, menunggu metadata lagu");
  return 0;
}

static int amsOnWriteReq(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg) {
  if (err->status != 0) { amsFail("write request", err->status); return 0; }
  static const uint8_t reqPlayer[3] = {0x00, 0x01, 0x02};   // Player: PlaybackInfo(1) + Volume(2)
  int rc = ble_gattc_write_flat(conn, amsEntityHandle, reqPlayer, sizeof(reqPlayer), amsOnWritePlayer, NULL);
  if (rc != 0) { amsState = 2; amsReady = true; songDirty = true; }
  return 0;
}

static int amsOnWriteCccd(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg) {
  if (err->status != 0) { amsFail("subscribe (CCCD)", err->status); return 0; }
  static const uint8_t req[4] = {0x02, 0x00, 0x02, 0x03};   // Track: Artist(0) + Title(2) + Duration(3)
  int rc = ble_gattc_write_flat(conn, amsEntityHandle, req, sizeof(req), amsOnWriteReq, NULL);
  if (rc != 0) amsFail("kirim write request", rc);
  return 0;
}

static int amsOnDsc(uint16_t conn, const struct ble_gatt_error* err, uint16_t chrValHandle,
                    const struct ble_gatt_dsc* dsc, void* arg) {
  if (err->status == 0) {
    if (ble_uuid_u16(&dsc->uuid.u) == 0x2902 && amsCccdHandle == 0) amsCccdHandle = dsc->handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE) {
    if (amsCccdHandle == 0) { amsFail("CCCD tidak ditemukan", 0); return 0; }
    static const uint8_t on[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(conn, amsCccdHandle, on, sizeof(on), amsOnWriteCccd, NULL);
    if (rc != 0) amsFail("kirim write CCCD", rc);
  } else {
    amsFail("discover descriptor", err->status);
  }
  return 0;
}

static int amsOnChr(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_chr* chr, void* arg) {
  if (err->status == 0) {
    if (ble_uuid_cmp(&chr->uuid.u, &amsEntityUuid.u) == 0) amsEntityHandle = chr->val_handle;
    if (ble_uuid_cmp(&chr->uuid.u, &amsRemoteUuid.u) == 0) amsRemoteHandle = chr->val_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE) {
    if (amsEntityHandle == 0) { amsFail("Entity Update char tidak ditemukan", 0); return 0; }
    int rc = ble_gattc_disc_all_dscs(conn, amsEntityHandle, amsSvcEnd, amsOnDsc, NULL);
    if (rc != 0) amsFail("mulai discover descriptor", rc);
  } else {
    amsFail("discover characteristic", err->status);
  }
  return 0;
}

static int amsOnSvc(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc, void* arg) {
  if (err->status == 0) {
    amsSvcStart = svc->start_handle;
    amsSvcEnd = svc->end_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE) {
    if (amsSvcStart == 0) { amsFail("service AMS tidak ditemukan di iPhone", 0); return 0; }
    int rc = ble_gattc_disc_all_chrs(conn, amsSvcStart, amsSvcEnd, amsOnChr, NULL);
    if (rc != 0) amsFail("mulai discover characteristic", rc);
  } else {
    amsFail("discover service", err->status);
  }
  return 0;
}

static ble_uuid16_t ctsSvcUuid;
uint16_t ctsSvcStart = 0, ctsSvcEnd = 0, ctsTimeHandle = 0;
volatile uint8_t ctsState = 0;          // 0=idle 1=proses 2=siap 3=gagal
uint8_t ctsFailCount = 0;
unsigned long lastCtsAttempt = 0;
unsigned long lastCtsSync = 0;

void resetCtsState() {
  ctsState = 0;
  ctsFailCount = 0;
  ctsSvcStart = ctsSvcEnd = ctsTimeHandle = 0;
  lastCtsAttempt = 0;
  lastCtsSync = 0;
}

static void ctsFail(const char* msg, int code) {
  ctsState = 3;
  ctsFailCount++;
  Serial.printf("[CTS] GAGAL: %s (kode=%d)\n", msg, code);
}

static uint32_t localTimeToEpoch(int y, int m, int d, int hh, int mm, int ss) {
  y -= (m <= 2) ? 1 : 0;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long mp = (m + 9) % 12;
  long doy = (153 * mp + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468L;
  return (uint32_t)(days * 86400L + hh * 3600L + mm * 60L + ss);
}

static int ctsOnRead(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg) {
  if (err->status == BLE_HS_EDONE) return 0;
  if (err->status != 0 || attr == NULL || attr->om == NULL) { ctsFail("baca waktu", err->status); return 0; }
  uint8_t buf[16];
  uint16_t len = OS_MBUF_PKTLEN(attr->om);
  if (len > sizeof(buf)) len = sizeof(buf);
  if (len < 7 || os_mbuf_copydata(attr->om, 0, len, buf) != 0) { ctsFail("data waktu tidak valid", len); return 0; }
  int year = buf[0] | (buf[1] << 8);
  if (year < 2020 || year > 2100 || buf[2] < 1 || buf[2] > 12 || buf[3] < 1 || buf[3] > 31) {
    ctsFail("nilai waktu di luar rentang", year);
    return 0;
  }
  syncedUnixTime = localTimeToEpoch(year, buf[2], buf[3], buf[4], buf[5], buf[6]);
  syncedAtMillis = millis();
  clockTimezoneOffsetMin = 0;
  clockSynced = true;
  ctsState = 2;
  lastCtsSync = millis();
  Serial.printf("[CTS] sinkron: %04d-%02d-%02d %02d:%02d:%02d\n", year, buf[2], buf[3], buf[4], buf[5], buf[6]);
  return 0;
}

static int ctsOnChr(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_chr* chr, void* arg) {
  if (err->status == 0) {
    if (ble_uuid_u16(&chr->uuid.u) == 0x2A2B) ctsTimeHandle = chr->val_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE) {
    if (ctsTimeHandle == 0) { ctsFail("Current Time char tidak ditemukan", 0); return 0; }
    int rc = ble_gattc_read(conn, ctsTimeHandle, ctsOnRead, NULL);
    if (rc != 0) ctsFail("mulai baca waktu", rc);
  } else {
    ctsFail("discover characteristic", err->status);
  }
  return 0;
}

static int ctsOnSvc(uint16_t conn, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc, void* arg) {
  if (err->status == 0) {
    ctsSvcStart = svc->start_handle;
    ctsSvcEnd = svc->end_handle;
    return 0;
  }
  if (err->status == BLE_HS_EDONE) {
    if (ctsSvcStart == 0) { ctsFail("service CTS tidak ditemukan di iPhone", 0); return 0; }
    int rc = ble_gattc_disc_all_chrs(conn, ctsSvcStart, ctsSvcEnd, ctsOnChr, NULL);
    if (rc != 0) ctsFail("mulai discover characteristic", rc);
  } else {
    ctsFail("discover service", err->status);
  }
  return 0;
}

static void ctsBegin() {
  ctsSvcUuid.u.type = BLE_UUID_TYPE_16;
  ctsSvcUuid.value = 0x1805;
  ctsSvcStart = ctsSvcEnd = ctsTimeHandle = 0;
  ctsState = 1;
  int rc = ble_gattc_disc_svc_by_uuid(bleConnHandle, &ctsSvcUuid.u, ctsOnSvc, NULL);
  if (rc != 0) ctsFail("mulai discover service", rc);
}

static void ctsReadTime() {
  ctsState = 1;
  int rc = ble_gattc_read(bleConnHandle, ctsTimeHandle, ctsOnRead, NULL);
  if (rc != 0) ctsFail("baca ulang waktu", rc);
}

void ctsClientLoop() {
  if (!bleActive || !bleConnected || !bleSecured || androidMode) return;
  if (bleConnHandle == BLE_HS_CONN_HANDLE_NONE) return;
  bool amsFinished = (amsState == 2) || (amsState == 3 && amsFailCount >= 3);
  if (!amsFinished) return;
  unsigned long now = millis();
  if (now - bleConnMillis < 5000) return;
  if (ctsState == 1) {
    if (now - lastCtsAttempt > 8000) ctsFail("timeout", 0);
    return;
  }
  if (ctsState == 2) {
    if (now - lastCtsSync > 1800000UL) { lastCtsAttempt = now; ctsReadTime(); }
    return;
  }
  if (ctsFailCount >= 3) return;
  if (lastCtsAttempt != 0 && now - lastCtsAttempt < 3000) return;
  lastCtsAttempt = now;
  Serial.println("[CTS] mulai discovery...");
  ctsBegin();
}

static int amsGapEvent(struct ble_gap_event* ev, void* arg) {
  switch (ev->type) {
    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (ev->notify_rx.attr_handle != amsEntityHandle) return 0;
      uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
      uint8_t buf[256];
      if (len > sizeof(buf)) len = sizeof(buf);
      if (os_mbuf_copydata(ev->notify_rx.om, 0, len, buf) == 0) amsHandleEntityData(buf, len);
      return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT:
      bleConnected = false;
      bleSecured = false;
      amsReady = false;
      amsState = 0;
      androidMode = false;
      amsFailCount = 0;
      bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
      songTitle = ""; songArtist = ""; amsPlaying = false; amsPlayKnown = false;
      songDirty = true;
      bleSecReqSent = false;
      lastAmsAttempt = 0;
      resetCtsState();

      if (bleActive) {
        NimBLEDevice::getAdvertising()->start();
        Serial.println("[BLE] advertising dimulai kembali setelah disconnect");
      }
      Serial.println("[BLE] terputus");
      return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      struct ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) {
        ble_store_util_delete_peer(&d.peer_id_addr);
      }
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
      return 0;
  }
}

#define AMS_CMD_TOGGLE_PLAY_PAUSE 0x02
#define AMS_CMD_NEXT_TRACK        0x03
#define AMS_CMD_PREVIOUS_TRACK    0x04

static int amsOnWriteRemote(uint16_t conn, const struct ble_gatt_error* err, struct ble_gatt_attr* attr, void* arg) {
  Serial.printf("[AMS] remote command hasil: %s (kode=%d)\n", err->status == 0 ? "OK" : "GAGAL", err->status);
  return 0;
}

void sendAmsRemoteCommand(uint8_t command) {
  // [ANDROID] HP Android: kirim perintah lewat notify characteristic 9ABC
  if (androidMode) {
    if (!bleConnected || chrCmd == nullptr) {
      Serial.println("[ANDROID] Remote Command belum siap");
      return;
    }
    chrCmd->setValue(&command, 1);
    bool ok = chrCmd->notify();
    Serial.printf("[ANDROID] kirim remote command 0x%02X: %s\n", command, ok ? "terkirim" : "GAGAL");
    return;
  }

  // iPhone: AMS Remote Command
  if (!bleConnected || !bleSecured || amsRemoteHandle == 0 || bleConnHandle == BLE_HS_CONN_HANDLE_NONE) {
    Serial.println("[AMS] Remote Command belum siap");
    return;
  }
  int rc = ble_gattc_write_flat(bleConnHandle, amsRemoteHandle, &command, sizeof(command), amsOnWriteRemote, NULL);
  Serial.printf("[AMS] kirim remote command 0x%02X: %s (rc=%d)\n", command, rc == 0 ? "terkirim" : "GAGAL", rc);
}

void amsBegin() {
  initAmsUuids();
  amsSvcStart = amsSvcEnd = amsEntityHandle = amsCccdHandle = amsRemoteHandle = 0;
  amsState = 1;
  ble_gap_set_event_cb(bleConnHandle, amsGapEvent, NULL);
  int rc = ble_gattc_disc_svc_by_uuid(bleConnHandle, &amsSvcUuid.u, amsOnSvc, NULL);
  if (rc != 0) amsFail("mulai discover service", rc);
}

class BleServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    bleConnHandle = info.getConnHandle();
    songDirty = true;
    bleConnMillis = millis();
    bleSecReqSent = false;
    androidMode = false;
    amsFailCount = 0;
    resetCtsState();
    Serial.printf("[BLE] terhubung: %s | bonded=%d | encrypted=%d\n",
                  info.getAddress().toString().c_str(), info.isBonded() ? 1 : 0, info.isEncrypted() ? 1 : 0);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
    bleSecured = false;
    amsReady = false;
    amsState = 0;
    androidMode = false;
    amsFailCount = 0;
    bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
    bleSecReqSent = false;
    lastAmsAttempt = 0;
    resetCtsState();
    songTitle = ""; songArtist = ""; amsPlaying = false; amsPlayKnown = false;
    songDirty = true;

    if (bleActive) {
      NimBLEDevice::getAdvertising()->start();
      Serial.println("[BLE] advertising dimulai kembali setelah disconnect");
    }
    Serial.printf("[BLE DEBUG] DISCONNECTED, reason: %d\n", reason);
    Serial.println("[BLE] terputus");
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (!info.isEncrypted()) {
      Serial.println("[BLE] enkripsi gagal; bond dipertahankan, koneksi diputus");
      bleSecured = false;
      NimBLEDevice::getServer()->disconnect(info.getConnHandle());
      return;
    }

    bleSecured = true;
    bleSecReqSent = false;
    lastAmsAttempt = 0;
    songDirty = true;
    Serial.printf("[BLE] paired & encrypted (bonded=%d)\n", info.isBonded() ? 1 : 0);
  }
};
BleServerCB bleServerCB;

class NowPlayingWriteCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    const uint8_t* d = v.data();
    size_t n = v.length();
    androidMode = true;

    String fields[8];
    size_t fieldStart = 0; int field = 0;
    for (size_t i = 0; i <= n && field < 8; i++) {
      if (i == n || d[i] == '\n') {
        fields[field++] = cleanUtf8(d + fieldStart, i - fieldStart);
        fieldStart = i + 1;
      }
    }
    songTitle = fields[0];
    songArtist = fields[1];
    if (field >= 3 && fields[2].length()) {
      amsPlaying = (fields[2][0] == '1');
      amsPlayKnown = true;
    }
    if (field >= 4) trackDurationSec = (uint32_t)fields[3].toInt();
    if (field >= 5) { trackPositionSec = (uint32_t)fields[4].toInt(); lastPlaybackUpdateMs = millis(); }
    if (field >= 6) {
      // [ANDROID] overlay volume hanya muncul saat volume BERUBAH (bukan tiap data keepalive)
      int vol = fields[5].toInt();
      if (vol >= 0 && vol <= 100) {
        if (phoneVolumePct != 255 && (uint8_t)vol != phoneVolumePct) lastVolumeOverlayMs = millis();
        phoneVolumePct = (uint8_t)vol;
      }
    }
    if (field >= 7) { uint32_t epoch = (uint32_t)fields[6].toInt(); if (epoch > 1700000000UL) { syncedUnixTime = epoch; syncedAtMillis = millis(); clockSynced = true; } }
    if (field >= 8) { int tz = fields[7].toInt(); if (tz >= -720 && tz <= 840) clockTimezoneOffsetMin = (int16_t)tz; }
    songDirty = true;
    Serial.printf("[ANDROID] %s - %s (%s)\n", songTitle.c_str(), songArtist.c_str(), amsPlaying ? "play" : "pause");
  }
};
NowPlayingWriteCB nowPlayingCB;

// [ANDROID] App Android subscribe ke characteristic 9ABC -> langsung ditandai sebagai mode Android
// (supaya permintaan pairing iPhone tidak sempat terkirim ke HP Android).
class RemoteCmdCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    if (subValue & 1) {
      androidMode = true;
      songDirty = true;
      Serial.println("[ANDROID] tombol remote aktif (subscribe)");
    }
  }
};
RemoteCmdCB remoteCmdCB;

void startBLE() {
  if (bleActive) return;
  bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
  amsState = 0;
  bleNeedRestart = false;
  resetCtsState();

  NimBLEDevice::init("AlleLabs");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(0x03);
  NimBLEDevice::setSecurityRespKey(0x03);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&bleServerCB, false);

  NimBLEService* svc = bleServer->createService("ABCD");
  NimBLECharacteristic* chr = svc->createCharacteristic(
      "1234", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  chr->setValue("Alle Labs");

  NimBLECharacteristic* chrNow = svc->createCharacteristic(
      "5678", NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  chrNow->setCallbacks(&nowPlayingCB);

  // [ANDROID] characteristic notify untuk perintah tombol (play/pause/next/prev)
  chrCmd = svc->createCharacteristic("9ABC", NIMBLE_PROPERTY::NOTIFY);
  chrCmd->setCallbacks(&remoteCmdCB);

  bleServer->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName("AlleLabs");
  adv->addServiceUUID("ABCD");

  const uint8_t solicitAMS[] = {
    0x11, 0x15,
    0xDC,0xF8,0x55,0xAD,0x02,0xC5,0xF4,0x8E,0x3A,0x43,0x36,0x0F,0x2B,0x50,0xD3,0x89
  };
  NimBLEAdvertisementData sr;
  sr.addData(solicitAMS, sizeof(solicitAMS));
  adv->setScanResponseData(sr);
  bool ok = adv->start();

  bleActive = true;
  songDirty = true;
  Serial.printf("[BLE] advertising start: %s\n", ok ? "OK" : "GAGAL");
}

void stopBLE() {
  if (!bleActive) return;
  bleActive = false;
  NimBLEDevice::deinit(true);
  bleServer = nullptr;
  chrCmd = nullptr;
  bleConnected = false; bleSecured = false; amsReady = false;
  amsState = 0; bleNeedRestart = false; androidMode = false; amsFailCount = 0;
  resetCtsState();
  bleConnHandle = BLE_HS_CONN_HANDLE_NONE;
  songTitle = ""; songArtist = ""; amsPlaying = false; amsPlayKnown = false;
  songDirty = true;
  Serial.println("[BLE] dimatikan");
}

void clearBleBonds() {
  if (bleActive) stopBLE();
  NimBLEDevice::init("AlleLabs");
  NimBLEDevice::deleteAllBonds();
  NimBLEDevice::deinit(true);
  Serial.println("[BLE] semua bond dihapus");
  updateOLEDText("Pairing direset", "Hapus juga di HP:", "Forget This Device");
  delay(2500);
}

void toggleBLE() {
  if (bleActive) {
    stopBLE();
    updateOLEDText("Bluetooth: OFF", "", "");
  } else {
    startBLE();
    updateOLEDText("Bluetooth: ON", "Cari di iPhone:", "AlleLabs");
  }
  delay(1200);
}

void bleClientLoop() {
  if (bleActive && bleConnected && !bleSecured && !androidMode && !bleSecReqSent &&
      bleConnHandle != BLE_HS_CONN_HANDLE_NONE && millis() - bleConnMillis > 4000) {
    bleSecReqSent = true;
    NimBLEDevice::startSecurity(bleConnHandle);
  }
  if (androidMode) return;
  if (!bleActive || !bleSecured || bleConnHandle == BLE_HS_CONN_HANDLE_NONE) return;
  if (amsState == 3 && amsFailCount >= 3) return;
  if (amsState == 2) return;
  if (amsState == 1) {
    if (millis() - lastAmsAttempt > 8000) amsFail("timeout", 0);
    return;
  }
  if (millis() - lastAmsAttempt < 3000) return;
  lastAmsAttempt = millis();
  Serial.println("[AMS] mulai discovery...");
  amsBegin();
}

// ---------- Helper Visual Grafis ----------
void drawArc(int cx, int cy, int r, float a0, float span) {
  float step = 1.0f / r;
  for (float t = 0; t < span; t += step) {
    display.drawPixel(cx + (int)roundf(r * cosf(a0 + t)), cy + (int)roundf(r * sinf(a0 + t)), SH110X_WHITE);
  }
}

void drawArmLine(int x0, int y0, int x1, int y1) {
  display.drawLine(x0 - 1, y0, x1 - 1, y1, SH110X_BLACK);
  display.drawLine(x0 + 2, y0, x1 + 2, y1, SH110X_BLACK);
  display.drawLine(x0, y0, x1, y1, SH110X_WHITE);
  display.drawLine(x0 + 1, y0, x1 + 1, y1, SH110X_WHITE);
}

void drawScrollText(const String& txt, int size, int y, int& x, int areaX, int speed) {
  int cw = 6 * size;
  int w = txt.length() * cw;
  int areaW = SCREEN_WIDTH - areaX;
  display.setTextSize(size);
  if (w <= areaW) {
    display.setCursor(areaX, y);
    display.print(txt);
    x = SCREEN_WIDTH;
    return;
  }
  String padded = txt + "   ";
  int pw = padded.length() * cw;
  display.setCursor(x, y);
  display.print(padded);
  x -= speed;
  if (x < areaX - pw) x = SCREEN_WIDTH;
}

// =========================================================================
// RENDER KUMPULAN TEMA KREATIF SLOT 6
// =========================================================================

// --- TEMA 0: PIRINGAN VINYL ---
void drawVinylArtwork(bool playing) {
  const int cx = 24, cy = 32, R = 22;
  if (playing) {
    vinylAngle += 0.30f;
    if (vinylAngle > TWO_PI) vinylAngle -= TWO_PI;
  }

  display.drawCircle(cx, cy, R, SH110X_WHITE);
  display.drawCircle(cx, cy, R - 1, SH110X_WHITE);
  for (int k = 0; k < 3; k++) {
    drawArc(cx, cy, 18, vinylAngle + k * 2.094f, 1.5f);
    drawArc(cx, cy, 14, vinylAngle + 1.0f + k * 2.094f, 1.3f);
  }
  display.fillCircle(cx, cy, 9, SH110X_WHITE);
  display.fillTriangle(cx, cy,
                       cx + (int)(9 * cosf(vinylAngle)),        cy + (int)(9 * sinf(vinylAngle)),
                       cx + (int)(9 * cosf(vinylAngle + 1.1f)), cy + (int)(9 * sinf(vinylAngle + 1.1f)),
                       SH110X_BLACK);
  display.fillCircle(cx, cy, 2, SH110X_BLACK);

  display.fillCircle(50, 8, 3, SH110X_WHITE);
  if (playing) {
    drawArmLine(50, 8, 42, 28);
    drawArmLine(42, 28, 36, 38);
  } else {
    drawArmLine(50, 8, 52, 30);
    drawArmLine(52, 30, 50, 37);
  }
}

// --- TEMA 1: FULL SCREEN CASSETTE (TEMA KASET KECE) ---
void drawCassetteTheme(bool hasSong, bool playing, const String& title, const String& artist, uint32_t shownPos, uint32_t duration, const char* timeText) {
  if (hasSong) {
    // Teks Judul Lagu berjalan untuk label atas kaset (y=6)
    drawScrollText(title, 1, 6, titleX, 16, 2);
  }
  
  // MASKING teks agar tidak tembus melewati area label kaset
  display.fillRect(0, 0, 15, 14, SH110X_BLACK); 
  display.fillRect(113, 0, 15, 14, SH110X_BLACK);

  // Body Kaset Full Horizontal
  display.drawRoundRect(4, 0, 120, 42, 5, SH110X_WHITE);
  
  // Kotak Area Label (Stiker Kaset)
  display.drawRect(14, 4, 100, 24, SH110X_WHITE);
  
  // Jendela Pita (Pill shape memanjang di tengah)
  display.drawRoundRect(30, 14, 68, 12, 6, SH110X_WHITE);
  
  // Koordinat Lingkaran Reel (Kumparan) Kiri dan Kanan
  int r1x = 42, r2x = 86, ry = 20;
  
  // Animasi perputaran pita
  if (playing) {
    vinylAngle += 0.25f;
    if (vinylAngle > TWO_PI) vinylAngle -= TWO_PI;
  }
  // Gambar ruji reel yang berputar
  for (int i = 0; i < 3; i++) {
    float a = vinylAngle + i * (TWO_PI / 3.0f);
    display.drawLine(r1x, ry, r1x + (int)(4 * cosf(a)), ry + (int)(4 * sinf(a)), SH110X_WHITE);
    display.drawLine(r2x, ry, r2x + (int)(4 * cosf(a)), ry + (int)(4 * sinf(a)), SH110X_WHITE);
  }
  
  // Ketebalan pita (Progress visual: kiri menyusut, kanan membesar seiring lagu)
  if (duration > 0) {
    float progress = (float)shownPos / duration;
    int rLeft = 10 - (int)(progress * 4);  
    int rRight = 6 + (int)(progress * 4);  
    display.drawCircle(r1x, ry, rLeft, SH110X_WHITE);
    display.drawCircle(r2x, ry, rRight, SH110X_WHITE);
  } else {
    display.drawCircle(r1x, ry, 8, SH110X_WHITE);
    display.drawCircle(r2x, ry, 8, SH110X_WHITE);
  }
  
  // Hub cincin dalam reel
  display.drawCircle(r1x, ry, 2, SH110X_WHITE);
  display.drawCircle(r2x, ry, 2, SH110X_WHITE);

  // Trapesium Bawah (Magnetic Tape reader area)
  display.drawLine(24, 41, 32, 32, SH110X_WHITE);
  display.drawLine(104, 41, 96, 32, SH110X_WHITE);
  display.drawLine(32, 32, 96, 32, SH110X_WHITE);
  
  // Baut Kaset (4 titik di pojok luar kaset)
  display.drawPixel(8, 4, SH110X_WHITE);
  display.drawPixel(119, 4, SH110X_WHITE);
  display.drawPixel(8, 37, SH110X_WHITE);
  display.drawPixel(119, 37, SH110X_WHITE);

  if (hasSong) {
    // Nama Artis berjalan di bagian bawah badan kaset
    drawScrollText(artist, 1, 45, artistX, 4, 2);
    
    // Tampilkan waktu lagu di pojok kiri bawah
    display.setCursor(4, 55);
    display.print(timeText);
    
    // Status Play/Pause di pojok kanan bawah
    display.setCursor(88, 55);
    display.print(playing ? "[PLAY]" : "[PAUSE]");
    
    // Progress bar linear halus di dasar layer
    if (duration > 0) {
      int fillW = (int)((uint64_t)shownPos * 128 / duration);
      display.fillRect(0, 63, fillW, 1, SH110X_WHITE);
    }
  } else {
    display.setCursor(22, 48);
    display.print("NO TAPE LOADED");
  }
}

// --- TEMA 2: CYBERPUNK HUD ---
void drawCyberpunkTheme(bool hasSong, bool playing, const String& title, const String& artist, uint32_t shownPos, uint32_t duration, const char* timeText) {
  // Corner Sci-Fi Brackets
  display.drawFastHLine(0, 0, 10, SH110X_WHITE); display.drawFastVLine(0, 0, 10, SH110X_WHITE);
  display.drawFastHLine(118, 0, 10, SH110X_WHITE); display.drawFastVLine(127, 0, 10, SH110X_WHITE);
  display.drawFastHLine(0, 63, 10, SH110X_WHITE); display.drawFastVLine(0, 54, 10, SH110X_WHITE);
  display.drawFastHLine(118, 63, 10, SH110X_WHITE); display.drawFastVLine(127, 54, 10, SH110X_WHITE);

  // Header System
  display.setTextSize(1);
  display.setCursor(14, 3);
  display.print("SYS::HUD v2.0");
  display.drawFastHLine(14, 12, 100, SH110X_WHITE);

  if (hasSong) {
    drawScrollText(title, 2, 16, titleX, 4, 3);
    drawScrollText(String(">> ") + artist, 1, 35, artistX, 4, 2);

    if (duration > 0) {
      display.drawRect(4, 46, 120, 5, SH110X_WHITE);
      int fillW = (int)((uint64_t)shownPos * 116 / duration);
      if (fillW > 0) display.fillRect(6, 48, fillW, 1, SH110X_WHITE);
    }

    display.setCursor(4, 53);
    display.print(playing ? "[RUN]" : "[STP]");
    display.setCursor(80, 53);
    display.print(timeText);
  } else {
    display.setCursor(20, 28); display.print("STATUS: SEARCHING");
    display.setCursor(20, 40); display.print("BLE DISCONNECTED");
  }
}

// --- TEMA 3: COMPACT DISC (CD PLAYER) ---
void drawCDArtwork(bool playing) {
  const int cx = 24, cy = 32, R = 22;
  if (playing) {
    vinylAngle += 0.20f;
    if (vinylAngle > TWO_PI) vinylAngle -= TWO_PI;
  }

  display.drawCircle(cx, cy, R, SH110X_WHITE);
  display.drawCircle(cx, cy, 7, SH110X_WHITE);
  display.drawCircle(cx, cy, 3, SH110X_WHITE);

  if (playing) {
    for (int i = 0; i < 2; i++) {
      float a = vinylAngle + i * 3.14159f;
      int x1 = cx + (int)(8 * cosf(a));
      int y1 = cy + (int)(8 * sinf(a));
      int x2 = cx + (int)(21 * cosf(a + 0.3f));
      int y2 = cy + (int)(21 * sinf(a + 0.3f));
      display.drawLine(x1, y1, x2, y2, SH110X_WHITE);
    }
  }
}

// --- TEMA 4: WINAMP 90s PLAYER ---
void drawWinampTheme(bool hasSong, bool playing, const String& title, const String& artist, uint32_t shownPos, uint32_t duration, const char* timeText) {
  display.drawRect(0, 0, 128, 64, SH110X_WHITE);
  display.fillRect(0, 0, 128, 10, SH110X_WHITE);
  display.setTextColor(SH110X_BLACK);
  display.setTextSize(1);
  display.setCursor(3, 1);
  display.print("WINAMP - ALLE LABS");

  display.setTextColor(SH110X_WHITE);
  if (hasSong) {
    display.drawRect(4, 13, 50, 18, SH110X_WHITE);
    display.setCursor(8, 18);
    display.print(timeText);

    display.setCursor(60, 14); display.print("320 kbps");
    display.setCursor(60, 23); display.print(playing ? "[PLAYING]" : "[PAUSED]");

    if (playing) eqPhase += 0.5f;
    for (int i = 0; i < 16; i++) {
      int h = playing ? 2 + (int)(fabsf(sinf(eqPhase + i * 0.7f) * cosf(eqPhase * 0.4f + i * 0.3f)) * 14) : 2;
      display.fillRect(4 + i * 7, 48 - h, 5, h, SH110X_WHITE);
      display.drawPixel(4 + i * 7 + 2, 48 - h - 2, SH110X_WHITE);
    }

    display.drawFastHLine(0, 51, 128, SH110X_WHITE);
    drawScrollText(title + " - " + artist, 1, 54, titleX, 2, 3);
  } else {
    display.setCursor(15, 30); display.print("WINAMP IS IDLE");
  }
}

// --- TEMA 5: SPECTRUM MATRIX (VISUALIZER FOCUS) ---
void drawSpectrumMatrixTheme(bool hasSong, bool playing, const String& title, const String& artist, uint32_t shownPos, uint32_t duration, const char* timeText) {
  if (hasSong) {
    display.fillRect(0, 0, 128, 14, SH110X_WHITE);
    display.setTextColor(SH110X_BLACK);
    drawScrollText(title + " - " + artist, 1, 3, titleX, 2, 3);

    display.setTextColor(SH110X_WHITE);
    if (playing) eqPhase += 0.7f;
    for (int i = 0; i < 20; i++) {
      int h = playing ? 3 + (int)(fabsf(sinf(eqPhase + i * 0.5f) * sinf(i * 0.8f)) * 36) : 3;
      display.fillRect(2 + i * 6, 54 - h, 4, h, SH110X_WHITE);
    }

    display.drawFastHLine(0, 56, 128, SH110X_WHITE);
    if (duration > 0) {
      int fillW = (int)((uint64_t)shownPos * 128 / duration);
      display.fillRect(0, 55, fillW, 2, SH110X_WHITE);
    }
    display.setCursor(40, 57);
    display.print(timeText);
  } else {
    display.setTextSize(1);
    display.setCursor(15, 25); display.print("NO AUDIO SIGNAL");
  }
}

// =========================================================================
// SPOTIFY SCREEN MAIN DISPATCHER
// =========================================================================
void drawSpotifyScreen() {
  const int AREA_X = 56;
  bool hasSong = songTitle.length() > 0 || songArtist.length() > 0;
  bool playing = hasSong && (amsPlayKnown ? amsPlaying : true);
  char timeText[18];
  bool showTime = false;

  if (songDirty) {
    songDirty = false;
    titleX = SCREEN_WIDTH;
    artistX = SCREEN_WIDTH;
  }

  // Format Waktu Lagu
  if (trackDurationSec > 0) {
    uint32_t shownPos = trackPositionSec;
    if (playing && lastPlaybackUpdateMs > 0) shownPos += (millis() - lastPlaybackUpdateMs) / 1000UL;
    if (shownPos > trackDurationSec) shownPos = trackDurationSec;
    snprintf(timeText, sizeof(timeText), "%lu:%02lu/%lu:%02lu", (unsigned long)(shownPos/60), (unsigned long)(shownPos%60), (unsigned long)(trackDurationSec/60), (unsigned long)(trackDurationSec%60));
    if (strlen(timeText) > 9) snprintf(timeText, sizeof(timeText), "%lu:%02lu", (unsigned long)(shownPos/60), (unsigned long)(shownPos%60));
    showTime = true;
  } else {
    snprintf(timeText, sizeof(timeText), "00:00");
  }

  display.setTextColor(SH110X_WHITE);
  display.setTextWrap(false);

  // SWITCHING LAYOUT TEMA SLOT 6
  switch (slot6Theme) {
    case 1:
      drawCassetteTheme(hasSong, playing, songTitle, songArtist, trackPositionSec, trackDurationSec, timeText);
      break;

    case 2:
      drawCyberpunkTheme(hasSong, playing, songTitle, songArtist, trackPositionSec, trackDurationSec, timeText);
      break;

    case 4:
      drawWinampTheme(hasSong, playing, songTitle, songArtist, trackPositionSec, trackDurationSec, timeText);
      break;

    case 5:
      drawSpectrumMatrixTheme(hasSong, playing, songTitle, songArtist, trackPositionSec, trackDurationSec, timeText);
      break;

    case 0: // Vinyl (Default)
    case 3: // CD
    default:
      // Tampilan Berbagi Area Layar (Vinyl & CD)
      if (hasSong) {
        display.setTextSize(1);
        display.setCursor(AREA_X, 2);
        display.print(playing ? "NOW PLAYING" : "PAUSED");
        drawScrollText(songTitle.length() ? songTitle : String("-"), 2, 16, titleX, AREA_X, 3);
        drawScrollText(songArtist, 1, 38, artistX, AREA_X, 2);

        if (trackDurationSec > 0) {
          uint32_t shownPos = trackPositionSec;
          if (playing && lastPlaybackUpdateMs > 0) shownPos += (millis() - lastPlaybackUpdateMs) / 1000UL;
          if (shownPos > trackDurationSec) shownPos = trackDurationSec;
          int barW = 70;
          display.drawRect(AREA_X, 46, barW, 3, SH110X_WHITE);
          int fillW = (int)((uint64_t)shownPos * (barW - 2) / trackDurationSec);
          if (fillW > 0) display.fillRect(AREA_X + 1, 47, fillW, 1, SH110X_WHITE);
        }

        if (playing) eqPhase += 0.6f;
        for (int i = 0; i < 8; i++) {
          int h = 3;
          if (playing) h = 3 + (int)(fabsf(sinf(eqPhase + i * 0.9f) * cosf(eqPhase * 0.5f + i * 0.5f)) * 11);
          display.fillRect(AREA_X + i * 9, 62 - h, 7, h, SH110X_WHITE);
        }
      } else {
        const char* l1; const char* l2;
        if (!bleActive)         { l1 = "Tahan 2 dtk"; l2 = "= Bluetooth"; }
        else if (!bleConnected) { l1 = "Mencari";      l2 = "iPhone..."; }
        else if (androidMode)   { l1 = "Android OK";  l2 = "Putar lagu"; }
        else if (!bleSecured)   { l1 = "Pairing...";   l2 = ""; }
        else if (!amsReady)     { l1 = "Menyambung";   l2 = "ke musik..."; }
        else                    { l1 = "Putar lagu";   l2 = "di HP"; }
        display.setCursor(AREA_X, 2);  display.print("ALLE LABS");
        display.setCursor(AREA_X, 24); display.print(l1);
        display.setCursor(AREA_X, 36); display.print(l2);
      }

      // Mask area kiri supaya artwork tidak ditimpa teks scroll
      display.fillRect(0, 0, 55, 64, SH110X_BLACK);

      // Gambar Artwork Sesuai Tema
      if (slot6Theme == 3) drawCDArtwork(playing);
      else drawVinylArtwork(playing);

      if (showTime) {
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.print(timeText);
      }
      break;
  }

  // Overlay Volume Telepon Global
  if (phoneVolumePct <= 100 && millis() - lastVolumeOverlayMs < 1400) {
    display.fillRect(2, 54, 48, 10, SH110X_BLACK);
    display.drawRect(2, 54, 32, 7, SH110X_WHITE);
    display.fillRect(3, 55, (int)(phoneVolumePct * 28 / 100), 5, SH110X_WHITE);
    display.setTextSize(1); display.setCursor(36, 54); display.print(String(phoneVolumePct) + "%");
  }
  display.setTextWrap(true);
}

void drawCenteredText(const char* txt, int y, int size) {
  int w = strlen(txt) * 6 * size - size;
  int x = (SCREEN_WIDTH - w) / 2;
  if (x < 0) x = 0;
  display.setTextSize(size);
  display.setCursor(x, y);
  display.print(txt);
}

// ==========================================
// ROBOEYES (Slot 7) - animasi ekspresi otomatis
// ==========================================
static unsigned long roboMoodUntil = 0;
static unsigned long roboNextMoodAt = 0;
static uint8_t roboExpression = 0;

static void updateRoboEyesMood(unsigned long now) {
  if ((long)(now - roboMoodUntil) >= 0 && roboExpression != 0) {
    roboExpression = 0;
    roboEyes.setMood(DEFAULT);
    roboNextMoodAt = now + (unsigned long)random(2500, 5500);
  }

  if (roboExpression == 0 && (long)(now - roboNextMoodAt) >= 0) {
    roboExpression = (uint8_t)random(1, 6);
    switch (roboExpression) {
      case 1: roboEyes.setMood(HAPPY); break;
      case 2: roboEyes.setMood(TIRED); break;
      case 3: roboEyes.setMood(ANGRY); break;
      case 4: roboEyes.setMood(ANGRY); break;
      case 5: roboEyes.setMood(TIRED); break;
    }
    roboMoodUntil = now + (unsigned long)random(2200, 4200);
  }
}

static void initRoboEyes() {
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 20);
  roboEyes.setDisplayColors(SH110X_BLACK, SH110X_WHITE);
  roboEyes.setWidth(48, 48);
  roboEyes.setHeight(42, 42);
  roboEyes.setBorderradius(16, 16);
  roboEyes.setSpacebetween(12);
  roboEyes.setPosition(DEFAULT);
  roboEyes.setMood(DEFAULT);
  roboEyes.setAutoblinker(true, 2, 3);
  roboEyes.setIdleMode(true, 3, 3);
  roboEyes.setCuriosity(true);

  unsigned long now = millis();
  roboExpression = 0;
  roboMoodUntil = 0;
  roboNextMoodAt = now + (unsigned long)random(2500, 5000);
}

void drawRoboEyesScreen() {
  unsigned long now = millis();
  updateRoboEyesMood(now);
  roboEyes.drawEyes();

  if (roboExpression == 4 || roboExpression == 5) {
    int y = (roboExpression == 5) ? 42 : 46;
    display.fillCircle(35, y, 2, SH110X_WHITE);
    display.drawLine(35, y + 2, 33, y + 6, SH110X_WHITE);
    display.fillCircle(93, y, 2, SH110X_WHITE);
    display.drawLine(93, y + 2, 95, y + 6, SH110X_WHITE);
    if (roboExpression == 5) {
      display.drawLine(33, y + 7, 32, y + 11, SH110X_WHITE);
      display.drawLine(95, y + 7, 96, y + 11, SH110X_WHITE);
    }
  }
}

// ==========================================
// RENDER TAMPILAN LAYAR OLED
// ==========================================
void renderCurrentSlot() {
  display.clearDisplay();
  
  if (currentSlot == 1) {
    display.drawBitmap(0, 0, video_frames_1[videoFrameIndex], 128, 64, SH110X_WHITE, SH110X_BLACK);
  } 
  else if (currentSlot == 2) {
    display.drawBitmap(0, 0, video_frames_2[videoFrameIndex], 128, 64, SH110X_WHITE, SH110X_BLACK);
  } 
  else if (currentSlot == 3) {
    display.drawBitmap(0, 0, video_frames_3[videoFrameIndex], 128, 64, SH110X_WHITE, SH110X_BLACK);
  } 
  else if (currentSlot == 6) {
    drawSpotifyScreen();
  }
  else if (currentSlot == 7) {
    drawRoboEyesScreen();
  }
  else {
    if (!slotHasData[currentSlot]) {
      // Mengubah pesan saat slot custom kosong untuk mengarahkan ke domain lokal
      updateOLEDText("Slot " + String(currentSlot) + " Kosong", "Buka Web:", "allelabs.local");
      return;
    }
    display.drawBitmap(0, 0, slotData[currentSlot], 128, 64, SH110X_WHITE, SH110X_BLACK);
  }

  if (currentSlot >= 1 && currentSlot <= 5 && bleActive) {
    display.fillCircle(122, 5, 3, bleConnected ? SH110X_WHITE : SH110X_BLACK);
    display.drawCircle(122, 5, 3, SH110X_WHITE);
  }
  display.display();
}

uint8_t hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

// ==========================================
// WEB STUDIO INTERFACE (ALLE LABS THEME)
// ==========================================
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1.0'>";
  html += "<title>Alle Labs Studio</title>";
  html += "<style>";
  html += "body { background: #000000; color: #ffffff; font-family: monospace; text-align: center; padding: 15px; margin: 0; }";
  html += "h2 { color: #ffffff; margin-bottom: 5px; border-bottom: 2px solid #ffffff; display: inline-block; padding-bottom: 5px; }";
  html += ".subtitle { font-size: 11px; color: #aaaaaa; margin-bottom: 15px; text-transform: uppercase; }";
  html += ".box { background: #111111; border: 2px solid #ffffff; border-radius: 4px; padding: 15px; margin-bottom: 15px; }";
  html += "canvas { background: #000; border: 2px solid #ffffff; max-width: 100%; cursor: crosshair; touch-action: none; display: block; margin: 0 auto; }";
  html += "button { background: #ffffff; color: #000000; border: 2px solid #ffffff; padding: 10px 16px; margin: 5px; font-weight: bold; font-family: monospace; cursor: pointer; transition: 0.1s; }";
  html += "button:hover { background: #000000; color: #ffffff; }";
  html += "button.del { background: #000000; color: #ffffff; border: 2px solid #ffffff; }";
  html += "button.del:hover { background: #ffffff; color: #000000; }";
  html += "select, input[type='file'], input[type='range'] { width: 100%; background: #000000; color: #ffffff; border: 2px solid #ffffff; padding: 8px; margin-top: 6px; font-family: monospace; box-sizing: border-box; outline: none; }";
  html += ".grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }";
  html += ".slot-card { background: #000000; padding: 10px; border: 1px solid #ffffff; font-size: 12px; }";
  html += ".slider-group { text-align: left; font-size: 11px; color: #cccccc; margin-top: 10px; }";
  html += ".ota-note {font-size:11px;color:#bbbbbb;line-height:1.6;text-align:left;margin-top:10px;}";
  html += ".progress-wrap {width:100%;height:14px;border:1px solid #fff;margin-top:12px;box-sizing:border-box;}";
  html += ".progress-bar {height:100%;width:0%;background:#fff;transition:width .15s;}";
  html += "#otaStatus {font-size:11px;color:#fff;margin-top:10px;white-space:pre-wrap;}";
  html += "</style></head><body>";
  
  html += "<h2>ALLE LABS STUDIO</h2>";
  html += "<div class='subtitle'>Domain: allelabs.local | Slot 1-3: Video | Slot 4-5: Custom Web</div>";
  html += "<div class='box'><b>HOTSPOT TERHUBUNG</b><br>WiFi: Alle Labs Keychain<br>Password: allelabs123<br>Web Studio: <a style='color:white' href='http://allelabs.local'>http://allelabs.local</a><br><small>Scan QR pada layar OLED untuk menyambungkan HP.</small></div>";
  
  html += "<div class='box'><b>PILIH SLOT CUSTOM (4 & 5):</b><select id='s'>";
  html += "<option value='4'>SLOT 4" + String(slotHasData[4] ? " [ISI]" : " [KOSONG]") + "</option>";
  html += "<option value='5'>SLOT 5" + String(slotHasData[5] ? " [ISI]" : " [KOSONG]") + "</option>";
  html += "</select></div>";

  html += "<div class='box'><b>LUKIS / UPLOAD GAMBAR</b><br><br>";
  html += "<canvas id='cv' width='128' height='64' style='width:256px;height:128px;'></canvas><br>";
  
  html += "<div style='margin-top:10px;'><button onclick='rotateImage()'>PUTAR 90 DERAJAT</button></div>";
  
  html += "<div class='slider-group'>Metode Konversi Warna: <select id='ditherMode' onchange='requestRedraw()'>";
  html += "<option value='bayer'>Bayer 8x8 Dithering (Super Halus)</option>";
  html += "<option value='threshold'>Solid Threshold (Hitam/Putih Pekat)</option>";
  html += "<option value='floyd'>Floyd-Steinberg (Halus, Detail)</option>";
  html += "<option value='atkinson'>Atkinson Dithering (Cegah Blok Hitam)</option>";
  html += "</select></div>";

  html += "<div class='slider-group'>Zoom (Skala): <input type='range' id='zoom' min='10' max='300' value='100' oninput='requestRedraw()'></div>";
  html += "<div class='slider-group'>Geser X: <input type='range' id='posX' min='-128' max='128' value='0' oninput='requestRedraw()'></div>";
  html += "<div class='slider-group'>Geser Y: <input type='range' id='posY' min='-64' max='64' value='0' oninput='requestRedraw()'></div>";
  html += "<div class='slider-group'>Kecerahan: <input type='range' id='bright' min='-100' max='100' value='0' oninput='requestRedraw()'></div>";
  html += "<div class='slider-group'>Kontras: <input type='range' id='contrast' min='-100' max='100' value='0' oninput='requestRedraw()'></div>";

  html += "<div style='text-align:left; font-size:11px; margin-top:10px; color:#cccccc;'>Pilih Foto/Gambar:</div>";
  html += "<input type='file' id='imgIn' accept='image/*'><br><br>";
  html += "<button onclick='saveCanvas()'>SIMPAN</button>";
  html += "<button class='del' onclick='clr()'>BERSIHKAN</button></div>";

  html += "<div class='box'><b>MANAJEMEN SLOT 4 & 5</b><br><br><div class='grid'>";
  for (int i = 4; i <= 5; i++) {
    html += "<div class='slot-card'>SLOT " + String(i) + "<br>";
    html += (slotHasData[i] ? "<span style='color:#ffffff; font-weight:bold;'>[ISI]</span><br><button class='del' onclick='del(" + String(i) + ")'>HAPUS</button>" : "<span style='color:#666666;'>[KOSONG]</span>");
    html += "</div>";
  }
  html += "</div></div>";

  // PEMILIHAN TEMA SLOT 6 (SPOTIFY PLAYER)
  html += "<div class='box'><b>TEMA DISPLAY SLOT 6 (SPOTIFY PLAYER)</b><br><br><select id='slot6Theme' onchange=\"fetch('/theme?mode='+this.value).then(r=>{if(!r.ok)alert('Gagal menyimpan tema');})\">";
  html += String("<option value='0'") + String(slot6Theme == 0 ? " selected" : "") + ">1. PIRINGAN VINYL (CLASSIC)</option>";
  html += String("<option value='1'") + String(slot6Theme == 1 ? " selected" : "") + ">2. KASET PITA (FULLSCREEN KECE)</option>";
  html += String("<option value='2'") + String(slot6Theme == 2 ? " selected" : "") + ">3. CYBERPUNK HUD (FUTURISTIC)</option>";
  html += String("<option value='3'") + String(slot6Theme == 3 ? " selected" : "") + ">4. COMPACT DISC (CD PLAYER)</option>";
  html += String("<option value='4'") + String(slot6Theme == 4 ? " selected" : "") + ">5. WINAMP 90s PLAYER</option>";
  html += String("<option value='5'") + String(slot6Theme == 5 ? " selected" : "") + ">6. SPECTRUM MATRIX (FULL EQ)</option></select></div>";

  html += "<div class='box'><b>FIRMWARE UPDATE (OTA LOKAL)</b><br><br>";
  html += "<form id='otaForm' enctype='multipart/form-data'>";
  html += "<input type='file' id='firmwareFile' name='firmware' accept='.bin,application/octet-stream' required>";
  html += "<button type='submit' id='otaButton'>UPLOAD & UPDATE</button></form>";
  html += "<div class='progress-wrap'><div class='progress-bar' id='otaProgress'></div></div>";
  html += "<div id='otaStatus'>Pilih file firmware .bin untuk memulai update.</div>";
  html += "<div class='ota-note'>Gunakan file .bin firmware utama yang dibuat untuk board dan partisi yang sama. Jangan matikan perangkat atau menutup halaman selama proses berlangsung.</div>";
  html += "</div>";

  html += "<script>";
  html += "const c = document.getElementById('cv'), x = c.getContext('2d');";
  html += "let isD = false; let lastX = 0; let lastY = 0;";
  html += "let rawImageObj = null; let timeoutId = null;";
  
  html += "x.fillStyle = 'black'; x.fillRect(0, 0, 128, 64); x.strokeStyle = 'white'; x.lineWidth = 2; x.lineCap = 'round'; x.lineJoin = 'round';";
  
  html += "function getPos(e) {";
  html += "  const r = c.getBoundingClientRect();";
  html += "  let clientX = e.clientX, clientY = e.clientY;";
  html += "  if(e.touches && e.touches.length > 0) { clientX = e.touches[0].clientX; clientY = e.touches[0].clientY; }";
  html += "  let px = Math.floor((clientX - r.left) / (r.width / 128));";
  html += "  let py = Math.floor((clientY - r.top) / (r.height / 64));";
  html += "  return {x: px, y: py};";
  html += "}";

  html += "function startDraw(e) {";
  html += "  isD = true; rawImageObj = null;";
  html += "  let p = getPos(e);";
  html += "  lastX = p.x; lastY = p.y;";
  html += "  x.fillStyle = 'white'; x.fillRect(p.x, p.y, 1, 1);";
  html += "}";

  html += "function moveDraw(e) {";
  html += "  if(!isD) return;";
  html += "  let p = getPos(e);";
  html += "  x.beginPath();";
  html += "  x.moveTo(lastX, lastY);";
  html += "  x.lineTo(p.x, p.y);";
  html += "  x.strokeStyle = 'white';";
  html += "  x.lineWidth = 2;";
  html += "  x.stroke();";
  html += "  lastX = p.x; lastY = p.y;";
  html += "}";

  html += "c.addEventListener('mousedown', startDraw);";
  html += "c.addEventListener('mousemove', moveDraw);";
  html += "window.addEventListener('mouseup', () => isD = false);";

  html += "c.addEventListener('touchstart', e => { startDraw(e); e.preventDefault(); });";
  html += "c.addEventListener('touchmove', e => { moveDraw(e); e.preventDefault(); });";
  html += "window.addEventListener('touchend', () => isD = false);";

  html += "document.getElementById('imgIn').onchange = e => {";
  html += "  const f = e.target.files[0]; if(!f) return;";
  html += "  const fr = new FileReader();";
  html += "  fr.onload = ev => {";
  html += "    const im = new Image();";
  html += "    im.onload = () => {";
  html += "      rawImageObj = im;";
  html += "      document.getElementById('zoom').value = 100;";
  html += "      document.getElementById('posX').value = 0;";
  html += "      document.getElementById('posY').value = 0;";
  html += "      document.getElementById('bright').value = 0;";
  html += "      document.getElementById('contrast').value = 0;";
  html += "      processAndDrawImage();";
  html += "    };";
  html += "    im.src = ev.target.result;";
  html += "  };";
  html += "  fr.readAsDataURL(f);";
  html += "};";

  html += "function requestRedraw() {";
  html += "  if(!rawImageObj) return;";
  html += "  clearTimeout(timeoutId);";
  html += "  timeoutId = setTimeout(processAndDrawImage, 25);";
  html += "}";

  html += "const bayer8x8 = [";
  html += "  [ 0, 32,  8, 40,  2, 34, 10, 42],";
  html += "  [48, 16, 56, 24, 50, 18, 58, 26],";
  html += "  [12, 44,  4, 36, 14, 46,  6, 38],";
  html += "  [60, 28, 52, 20, 62, 30, 54, 22],";
  html += "  [ 3, 35, 11, 43,  1, 33,  9, 41],";
  html += "  [51, 19, 59, 27, 49, 17, 57, 25],";
  html += "  [15, 47,  7, 39, 13, 45,  5, 37],";
  html += "  [63, 31, 55, 23, 61, 29, 53, 21]";
  html += "];";

  html += "function processAndDrawImage() {";
  html += "  if(!rawImageObj) return;";
  html += "  let zoom = parseInt(document.getElementById('zoom').value) / 100;";
  html += "  let pX = parseInt(document.getElementById('posX').value);";
  html += "  let pY = parseInt(document.getElementById('posY').value);";
  html += "  let b = parseInt(document.getElementById('bright').value);";
  html += "  let cVal = parseInt(document.getElementById('contrast').value);";
  html += "  let mode = document.getElementById('ditherMode').value;";
  html += "  let factor = (259 * (cVal + 255)) / (255 * (259 - cVal));";

  html += "  let hRatio = 128 / rawImageObj.width;";
  html += "  let vRatio = 64 / rawImageObj.height;";
  html += "  let ratio = Math.min(hRatio, vRatio);";

  html += "  let drawW = rawImageObj.width * ratio * zoom;";
  html += "  let drawH = rawImageObj.height * ratio * zoom;";
  html += "  let drawX = (128 - drawW) / 2 + pX;";
  html += "  let drawY = (64 - drawH) / 2 + pY;";

  html += "  x.fillStyle = 'black'; x.fillRect(0, 0, 128, 64);";
  html += "  x.drawImage(rawImageObj, drawX, drawY, drawW, drawH);";

  html += "  let imgData = x.getImageData(0, 0, 128, 64);";
  html += "  let d = imgData.data;";
  
  html += "  let grayBuf = new Float32Array(128 * 64);";
  html += "  for(let y=0; y<64; y++) {";
  html += "    for(let xCoord=0; xCoord<128; xCoord++) {";
  html += "      let idx = (y * 128 + xCoord) * 4;";
  html += "      let g = d[idx] * 0.299 + d[idx+1] * 0.587 + d[idx+2] * 0.114;";
  html += "      g = factor * (g - 128) + 128 + b;";
  html += "      grayBuf[y * 128 + xCoord] = Math.max(0, Math.min(255, g));";
  html += "    }";
  html += "  }";

  html += "  if(mode === 'threshold') {";
  html += "    for(let i=0; i<grayBuf.length; i++) {";
  html += "      let v = grayBuf[i] >= 128 ? 255 : 0;";
  html += "      d[i*4] = v; d[i*4+1] = v; d[i*4+2] = v;";
  html += "    }";
  html += "  } else if(mode === 'bayer') {";
  html += "    for(let y=0; y<64; y++) {";
  html += "      for(let xCoord=0; xCoord<128; xCoord++) {";
  html += "        let idx = (y * 128 + xCoord) * 4;";
  html += "        let mapVal = (bayer8x8[y % 8][xCoord % 8] / 64.0 - 0.5) * 64;";
  html += "        let v = grayBuf[y * 128 + xCoord] + mapVal >= 128 ? 255 : 0;";
  html += "        d[idx] = v; d[idx+1] = v; d[idx+2] = v;";
  html += "      }";
  html += "    }";
  html += "  } else if(mode === 'floyd') {";
  html += "    for(let y=0; y<64; y++) {";
  html += "      for(let xCoord=0; xCoord<128; xCoord++) {";
  html += "        let idx = y * 128 + xCoord;";
  html += "        let oldPixel = grayBuf[idx];";
  html += "        let newPixel = oldPixel >= 128 ? 255 : 0;";
  html += "        grayBuf[idx] = newPixel;";
  html += "        let err = oldPixel - newPixel;";
  html += "        if(xCoord + 1 < 128) grayBuf[idx + 1] += err * 7 / 16;";
  html += "        if(y + 1 < 64) {";
  html += "          if(xCoord > 0) grayBuf[idx + 127] += err * 3 / 16;";
  html += "          grayBuf[idx + 128] += err * 5 / 16;";
  html += "          if(xCoord + 1 < 128) grayBuf[idx + 129] += err * 1 / 16;";
  html += "        }";
  html += "        let dIdx = idx * 4;";
  html += "        d[dIdx] = newPixel; d[dIdx+1] = newPixel; d[dIdx+2] = newPixel;";
  html += "      }";
  html += "    }";
  html += "  } else if(mode === 'atkinson') {";
  html += "    for(let y=0; y<64; y++) {";
  html += "      for(let xCoord=0; xCoord<128; xCoord++) {";
  html += "        let idx = y * 128 + xCoord;";
  html += "        let oldPixel = grayBuf[idx];";
  html += "        let newPixel = oldPixel >= 128 ? 255 : 0;";
  html += "        grayBuf[idx] = newPixel;";
  html += "        let err = (oldPixel - newPixel) / 8;";
  html += "        if(xCoord + 1 < 128) grayBuf[idx + 1] += err;";
  html += "        if(xCoord + 2 < 128) grayBuf[idx + 2] += err;";
  html += "        if(y + 1 < 64) {";
  html += "          if(xCoord > 0) grayBuf[idx + 127] += err;";
  html += "          grayBuf[idx + 128] += err;";
  html += "          if(xCoord + 1 < 128) grayBuf[idx + 129] += err;";
  html += "        }";
  html += "        if(y + 2 < 64) grayBuf[idx + 256] += err;";
  html += "        let dIdx = idx * 4;";
  html += "        d[dIdx] = newPixel; d[dIdx+1] = newPixel; d[dIdx+2] = newPixel;";
  html += "      }";
  html += "    }";
  html += "  }";

  html += "  x.putImageData(imgData, 0, 0);";
  html += "}";

  html += "function rotateImage() {";
  html += "  if(!rawImageObj) return;";
  html += "  let tempCanvas = document.createElement('canvas'); tempCanvas.width = rawImageObj.height; tempCanvas.height = rawImageObj.width;";
  html += "  let tCtx = tempCanvas.getContext('2d');";
  html += "  tCtx.translate(tempCanvas.width / 2, tempCanvas.height / 2);";
  html += "  tCtx.rotate(90 * Math.PI / 180);";
  html += "  tCtx.drawImage(rawImageObj, -rawImageObj.width / 2, -rawImageObj.height / 2);";
  
  html += "  let rotatedImg = new Image();";
  html += "  rotatedImg.src = tempCanvas.toDataURL();";
  html += "  rotatedImg.onload = () => {";
  html += "    rawImageObj = rotatedImg;";
  html += "    processAndDrawImage();";
  html += "  };";
  html += "}";

  html += "function clr() { x.fillStyle='black'; x.fillRect(0,0,128,64); rawImageObj = null; }";

  html += "function saveCanvas() {";
  html += "  const s = document.getElementById('s').value;";
  html += "  const id = x.getImageData(0,0,128,64).data; let h = '';";
  html += "  for(let y=0; y<64; y++) {";
  html += "    for(let col=0; col<128; col+=8) {";
  html += "      let bt = 0;";
  html += "      for(let b=0; b<8; b++) {";
  html += "        if(id[((y*128)+(col+b))*4] > 128) bt |= (1 << (7-b));";
  html += "      }";
  html += "      h += bt.toString(16).padStart(2, '0');";
  html += "    }";
  html += "  }";
  html += "  fetch('/draw?slot=' + s, {method:'POST', body:h}).then(r => {";
  html += "    if(r.ok) { alert('Berhasil disimpan ke slot ' + s); location.reload(); }";
  html += "    else alert('Gagal menyimpan.');";
  html += "  });";
  html += "}";

  html += "document.getElementById('otaForm').addEventListener('submit', function(e) {";
  html += "  e.preventDefault();";
  html += "  const file = document.getElementById('firmwareFile').files[0];";
  html += "  const status = document.getElementById('otaStatus');";
  html += "  const bar = document.getElementById('otaProgress');";
  html += "  const btn = document.getElementById('otaButton');";
  html += "  if(!file) { status.textContent = 'Pilih file .bin terlebih dahulu.'; return; }";
  html += "  if(!file.name.toLowerCase().endsWith('.bin')) { status.textContent = 'File harus berformat .bin'; return; }";
  html += "  if(!confirm('Mulai update firmware? Jangan matikan perangkat selama proses.')) return;";
  html += "  const data = new FormData(); data.append('firmware', file, file.name);";
  html += "  const xhr = new XMLHttpRequest(); xhr.open('POST', '/update', true);";
  html += "  btn.disabled = true; bar.style.width = '0%'; status.textContent = 'Mengirim firmware...';";
  html += "  xhr.upload.onprogress = function(ev) { if(ev.lengthComputable) { const p = Math.round(ev.loaded / ev.total * 100); bar.style.width = p + '%'; status.textContent = 'Upload: ' + p + '%'; } };";
  html += "  xhr.onload = function() { status.textContent = xhr.responseText || 'Respons kosong.'; if(xhr.status >= 200 && xhr.status < 300) { bar.style.width = '100%'; status.textContent += '\\nPerangkat akan restart. Sambungkan kembali ke hotspot setelah sekitar 20-40 detik.'; } else { btn.disabled = false; } };";
  html += "  xhr.onerror = function() { status.textContent = 'Koneksi terputus. Periksa perangkat sebelum mencoba lagi.'; btn.disabled = false; };";
  html += "  xhr.send(data);";
  html += "});";

  html += "function del(s) {";
  html += "  if(confirm('Hapus Slot ' + s + '?')) {";
  html += "    fetch('/delete?slot=' + s).then(r => { if(r.ok) location.reload(); else alert('Gagal menghapus.'); });";
  html += "  }";
  html += "}";
  html += "</script></body></html>";

  server.send(200, "text/html", html);
}

void handleDraw() {
  String slotStr = server.arg("slot");
  int slot = slotStr.toInt();
  if (slot < 4 || slot > 5) slot = 4;

  if (server.hasArg("plain")) {
    String hexPayload = server.arg("plain");
    if (hexPayload.length() >= 2048) {
      uint8_t buffer[1024];
      for (int i = 0; i < 1024; i++) {
        buffer[i] = (hexNibble(hexPayload[i * 2]) << 4) | hexNibble(hexPayload[i * 2 + 1]);
      }
      saveCustomSlotToNVS(slot, buffer);
      currentSlot = slot;
      prefs.begin("keychain", false); prefs.putUChar("lastSlot", (uint8_t)currentSlot); prefs.end();
      lastUserActivityMs = millis();
      renderCurrentSlot();
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(400, "text/plain", "Error");
}

void handleDeleteSlot() {
  String slotStr = server.arg("slot");
  int slot = slotStr.toInt();
  if (slot >= 4 && slot <= 5) {
    deleteCustomSlotFromNVS(slot);
    if (currentSlot == slot) renderCurrentSlot();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Invalid");
  }
}

void handleTheme() {
  int mode = server.arg("mode").toInt();
  if (mode < 0 || mode > 5) { server.send(400, "text/plain", "Tema tidak valid"); return; }
  slot6Theme = (uint8_t)mode;
  prefs.begin("keychain", false); prefs.putUChar("slot6Theme", slot6Theme); prefs.end();
  if (currentSlot == 6) renderCurrentSlot();
  server.send(200, "text/plain", "OK");
}

void handleFirmwareUploadStream() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaUploadOK = false;
    otaUploadStarted = false;
    otaUploadError = false;
    otaUploadMessage = "Memulai update...";
    String filename = upload.filename;
    filename.toLowerCase();

    if (!filename.endsWith(".bin")) {
      otaUploadError = true;
      otaUploadMessage = "Ditolak: file harus berekstensi .bin.";
      return;
    }
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaUploadError) return;

    if (!otaUploadStarted) {
      if (upload.currentSize == 0 || upload.buf[0] != 0xE9) {
        otaUploadError = true;
        otaUploadMessage = "Ditolak: file bukan image firmware ESP32 yang valid.";
        return;
      }

      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        otaUploadError = true;
        otaUploadMessage = "Gagal memulai OTA. Periksa partisi OTA dan ruang flash.";
        Update.printError(Serial);
        return;
      }
      otaUploadStarted = true;
    }

    size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      otaUploadError = true;
      otaUploadMessage = "Gagal menulis firmware ke flash.";
      Update.printError(Serial);
      Update.abort();
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    if (!otaUploadError && otaUploadStarted) {
      if (Update.end(true)) {
        otaUploadOK = true;
        otaUploadMessage = "Firmware berhasil ditulis dan diverifikasi. Perangkat akan restart.";
      } else {
        otaUploadError = true;
        otaUploadMessage = "Update gagal saat finalisasi firmware.";
        Update.printError(Serial);
      }
    } else if (!otaUploadError) {
      otaUploadError = true;
      otaUploadMessage = "File firmware kosong atau tidak terbaca.";
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    otaUploadError = true;
    otaUploadOK = false;
    otaUploadMessage = "Upload dibatalkan.";
    if (otaUploadStarted) Update.abort();
  }
}

void handleFirmwareUploadComplete() {
  if (otaUploadOK && !otaUploadError) {
    server.send(200, "text/plain", otaUploadMessage);
    Serial.println("[OTA] Update berhasil, restart dalam 1,5 detik.");
    delay(1500);
    ESP.restart();
  } else {
    String message = otaUploadMessage;
    if (message.length() == 0) message = "Update gagal.";
    server.send(400, "text/plain", message);
  }
}

static void oledQrDisplay(esp_qrcode_handle_t qrcode) {
  const int boxW = 62, boxH = 64;
  int size = esp_qrcode_get_size(qrcode);

  int scale = (boxW - 4) / size;
  int scaleH = (boxH - 4) / size;
  if (scaleH < scale) scale = scaleH;
  if (scale < 1) scale = 1;

  int qrPx = size * scale;
  int ox = (boxW - qrPx) / 2;
  int oy = (boxH - qrPx) / 2;

  display.fillRect(0, 0, boxW, boxH, SH110X_WHITE);
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qrcode, x, y)) {
        display.fillRect(ox + x * scale, oy + y * scale, scale, scale, SH110X_BLACK);
      }
    }
  }
}

void drawHotspotQRCode() {
  String wifiPayload = String("WIFI:T:WPA;S:") + ssid + ";P:" + password + ";;";

  display.oled_command(SH110X_DISPLAYON);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);
  display.setTextWrap(false);

  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = oledQrDisplay;
  cfg.max_qrcode_version = 4;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

  esp_err_t qrErr = esp_qrcode_generate(&cfg, wifiPayload.c_str());

  if (qrErr != ESP_OK) {
    display.setCursor(0, 0);
    display.print("SCAN TO CONNECT");
    display.setCursor(0, 24);
    display.println("QR GENERATION");
    display.println("FAILED");
    display.setCursor(0, 48);
    display.print("WiFi: allelabs.local");
    display.display();
    Serial.println("[QR] Gagal membuat QR. Payload terlalu panjang untuk versi maksimum.");
    return;
  }

  // Desain layar Hotspot dengan Domain mDNS
  display.setCursor(62, 3);  display.print("ALLE LABS");
  display.setCursor(62, 14); display.print("SCAN QR");
  display.setCursor(62, 24); display.print("= WiFi");
  display.setCursor(62, 38); display.print("Buka Web:");
  display.setCursor(62, 48); display.print("allelabs");
  display.setCursor(62, 56); display.print(".local");
  display.display();
}

void startWiFiAP() {
  WiFi.softAPdisconnect(true);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  WiFi.mode(WIFI_AP);
  delay(100);
  WiFi.setTxPower(WIFI_POWER_15dBm);

  IPAddress local_ip(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_ip, gateway, subnet);

  if (WiFi.softAP(ssid, password, 1, 0, 4)) {
    if (MDNS.begin("allelabs")) {
      Serial.println("mDNS responder started: http://allelabs.local");
    }
    server.begin();
    drawHotspotQRCode();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(8, 9); 
  delay(100);
  if (!display.begin(0x3C, true)) display.begin(0x3D, true);
  initRoboEyes();

  loadCustomSlots();
  prefs.begin("keychain", true);
  currentSlot = prefs.getUChar("lastSlot", 1);
  slot6Theme = prefs.getUChar("slot6Theme", 0);
  prefs.end();
  if (currentSlot < 1 || currentSlot > 7) currentSlot = 1;
  if (slot6Theme > 5) slot6Theme = 0;

  lastUserActivityMs = millis();
  renderCurrentSlot();

  server.on("/", handleRoot);
  server.on("/draw", HTTP_POST, handleDraw);
  server.on("/delete", handleDeleteSlot);
  server.on("/theme", handleTheme);
  server.on("/update", HTTP_POST, handleFirmwareUploadComplete, handleFirmwareUploadStream);
}

void advanceToNextSlot() {
  currentSlot++;
  if (currentSlot > 7) currentSlot = 1;
  prefs.begin("keychain", false); prefs.putUChar("lastSlot", (uint8_t)currentSlot); prefs.end();
  lastUserActivityMs = millis();
  videoFrameIndex = 0;
  songDirty = true;
  renderCurrentSlot();
}

void loop() {
  static int lastRawState = HIGH;
  static int stableState = HIGH;
  static unsigned long lastRawChangeMs = 0;
  int rawState = digitalRead(BUTTON_PIN);
  if (rawState != lastRawState) { lastRawState = rawState; lastRawChangeMs = millis(); }
  if (millis() - lastRawChangeMs >= 30) stableState = rawState;
  int buttonState = stableState;

  if (buttonState == LOW && screenIsOff) {
    screenIsOff = false;
    lastUserActivityMs = millis();
    display.oled_command(SH110X_DISPLAYON);
    renderCurrentSlot();
  }
  if (buttonState == LOW) {
    if (!isButtonPressed) {
      isButtonPressed = true;
      buttonPressStart = millis();
    }
  } else {
    if (isButtonPressed) {
      unsigned long pressDuration = millis() - buttonPressStart;
      
      // -- LOGIKA TAHAN TOMBOL DIPERCEPAT MENJADI 2 DETIK (2000 ms) --
      if (pressDuration >= 2000) {
        if (currentSlot == 6) {
          if (pressDuration >= 8000) clearBleBonds();
          else toggleBLE();
          renderCurrentSlot();
        } else {
          isHotspotActive = !isHotspotActive;
          if (isHotspotActive) {
            if (bleActive) stopBLE();
            startWiFiAP();
          } else {
            server.stop();
            MDNS.end();
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_OFF);
            
            display.clearDisplay();
            display.setTextSize(1);
            display.setTextColor(SH110X_WHITE);
            display.setCursor(15, 15);
            display.println("MODE KEYCHAIN");
            display.setCursor(20, 35);
            display.println("Slot Aktif: " + String(currentSlot));
            display.display();
            delay(1500);

            renderCurrentSlot();
          }
        }
      } 
      else if (!isHotspotActive) {
        if (currentSlot == 6 && bleActive) {
          if (pendingShortClicks < 4) pendingShortClicks++;
          clickSequenceDeadline = millis() + 500;
        } else {
          advanceToNextSlot();
        }
      }

      isButtonPressed = false;
      lastUserActivityMs = millis();
    }
  }

  if (pendingShortClicks > 0 && !isButtonPressed && (long)(millis() - clickSequenceDeadline) >= 0) {
    uint8_t clicks = pendingShortClicks;
    pendingShortClicks = 0;
    if (currentSlot == 6 && bleActive) {
      if (clicks == 1) sendAmsRemoteCommand(AMS_CMD_TOGGLE_PLAY_PAUSE);
      else if (clicks == 2) sendAmsRemoteCommand(AMS_CMD_NEXT_TRACK);
      else if (clicks == 3) sendAmsRemoteCommand(AMS_CMD_PREVIOUS_TRACK);
      else advanceToNextSlot();
    } else {
      advanceToNextSlot();
    }
  }

  bool isAnimSlot = (currentSlot >= 1 && currentSlot <= 3) || currentSlot == 6 || currentSlot == 7;
  unsigned long activeFrameDelay = (currentSlot == 7) ? 70UL : frameDelayMs;
  if (!isHotspotActive && isAnimSlot) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastFrameTime >= activeFrameDelay) {
      lastFrameTime = currentMillis;
      renderCurrentSlot();

      if (currentSlot >= 1 && currentSlot <= 3) {
        videoFrameIndex++;
        if (currentSlot == 1 && videoFrameIndex >= totalFramesV1) videoFrameIndex = 0;
        if (currentSlot == 2 && videoFrameIndex >= totalFramesV2) videoFrameIndex = 0;
        if (currentSlot == 3 && videoFrameIndex >= totalFramesV3) videoFrameIndex = 0;
      }
    }
  }

  if (bleActive) { bleClientLoop(); ctsClientLoop(); }

  if (isHotspotActive) {
    server.handleClient();
  }

  // Slot 6 adalah mode musik: OLED dan animasinya tidak pernah idle-off.
  if (currentSlot == 6) {
    if (screenIsOff) {
      display.oled_command(SH110X_DISPLAYON);
      screenIsOff = false;
    }
  } else if (!isHotspotActive) {
    // Setelah 2 menit tanpa aktivitas, perilaku bergantung pada status Bluetooth.
    if (!screenIsOff && millis() - lastUserActivityMs >= SCREEN_IDLE_TIMEOUT_MS) {
      display.oled_command(SH110X_DISPLAYOFF);
      screenIsOff = true;
      screenOffStartedMs = millis();

      // Bluetooth OFF: gunakan Deep Sleep selama 30 detik.
      if (!bleActive) {
        esp_sleep_enable_timer_wakeup((uint64_t)SCREEN_OFF_DURATION_MS * 1000ULL);
        esp_deep_sleep_start();
      }
    }

    // Bluetooth ON: CPU/Bluetooth tetap berjalan, hanya OLED yang mati 30 detik.
    if (screenIsOff && bleActive &&
        millis() - screenOffStartedMs >= SCREEN_OFF_DURATION_MS) {
      display.oled_command(SH110X_DISPLAYON);
      screenIsOff = false;
      lastUserActivityMs = millis();
      renderCurrentSlot();
    }
  }

  delay(10);
}
