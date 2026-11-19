/*
 * Powerbeats Pro 2 -> "pb-hr-esp32-bridge" heart-rate relay
 * Seeed XIAO ESP32-C6  ·  NimBLE-Arduino 2.x (h2zero)
 *
 * With heart-rate sharing switched on, the Powerbeats Pro 2 advertise a standard
 * BLE Heart Rate Service (0x180D) as "Powerbeats HR". This board connects to
 * them as a client and re-publishes the same reading as an ordinary chest strap
 * under BRIDGE_NAME, so anything that understands a BLE HR strap can use it.
 *
 * Ported from https://github.com/HuskerMinion/hr-bridge, which does the same
 * job for a Pixel Watch -> treadmill.
 *
 * Copyright (c) 2026 kaxap
 * Copyright (c) 2026 HuskerMinion
 * MIT License -- see LICENSE.
 *
 * ---------------------------------------------------------------------------
 * SETUP: Arduino IDE -> Tools -> Manage Libraries -> install "NimBLE-Arduino"
 *        (h2zero), version 2.x. Do NOT also include BLEDevice.h in this sketch;
 *        arduino-esp32's bundled BLE wrapper and this library cannot be mixed.
 *        Board: "XIAO_ESP32C6".
 * ---------------------------------------------------------------------------
 *
 * SERIAL COMMANDS (115200 baud)
 *   s = rescan for the earbuds    d = drop the earbuds link
 *   p = pause/resume scanning     a = stop and restart advertising
 *   v = verbose logging on/off    i = print status now      r = reboot
 *   b = read battery now          g = list the earbuds' GATT services
 */

#include <NimBLEDevice.h>

// ============================== TUNING =====================================
// Name the bridge advertises. It goes in the primary advertising packet next to
// the flags (3 bytes) and the 0x180D service UUID (4 bytes), which leaves room
// for 22 characters in the 31-byte packet.
static constexpr char BRIDGE_NAME[] = "pb-hr-esp32-bridge";
static_assert(sizeof(BRIDGE_NAME) - 1 <= 22, "BRIDGE_NAME must be 22 characters or fewer");

// The earbuds are found by advertised name. Matching on the 0x180D service alone
// would latch onto any chest strap or watch nearby, and the address is a
// rotating private address, so it can't be remembered either.
static const char*    SOURCE_NAME_PREFIX   = "Powerbeats";

// Battery level. The earbuds' heart-rate link has no Battery Service, but like
// AirPods they broadcast Apple "proximity pairing" adverts that carry each bud's
// charge in 10% steps. While bridging, the bridge scans for those for a few
// seconds once a minute and publishes the lower of the two buds as a standard
// Battery Service (0x180F). The adverts come from a different, rotating address
// than "Powerbeats HR", so they're matched by Apple model ID and the strongest
// signal wins. 0x201D = Powerbeats Pro 2 (observed). 0 = accept any AirPods or
// Beats model; with `v` on, every such advert is logged with its model ID.
static const bool     RELAY_BATTERY          = true;
static const uint16_t SOURCE_APPLE_MODEL     = 0x201D;
static const uint32_t BATTERY_SCAN_EVERY_MS  = 60000;
static const uint32_t BATTERY_RETRY_MS       = 15000;  // until the first reading
static const uint32_t BATTERY_SCAN_LEN_MS    = 5000;

// If the earbuds refuse to let us subscribe without encryption, pair with them
// (Just Works, no PIN) and retry once. The receiver side never needs pairing.
static const bool     SOURCE_ALLOW_PAIRING = true;

static const bool     USE_EXTERNAL_ANTENNA = false;  // true = u.FL connector

// Every reading from the earbuds is forwarded the moment it arrives. This timer
// only re-sends the last value when nothing new has come in for a while, so a
// receiver keeps hearing from us (with the contact bit cleared once stale). It
// is a bit longer than the earbuds' ~1 s cadence so it doesn't fire between two
// on-time readings and send a duplicate.
static const uint32_t KEEPALIVE_MS         = 1200;

// How long a reading stays trustworthy. Past this we keep notifying but clear
// the sensor-contact bit -- see pushHR() for why that matters.
static const uint32_t HR_STALE_MS          = 20000;

// Time allowed for the FIRST reading on a new earbuds link before dropping it
// and searching again. The Powerbeats sent their first reading ~8 s after
// connecting, so a minute is plenty without parking on a dead link for long.
static const uint32_t FIRST_HR_GRACE_MS    = 60000;

// A link that WAS streaming and has gone quiet. This one can be tight.
static const uint32_t ZOMBIE_MS            = 30000;

static const uint32_t SCAN_RETRY_MS        = 5000;
static const uint32_t HEARTBEAT_MS         = 60000;  // idle status line

// Scan duty cycle while looking for the earbuds (milliseconds).
static const uint16_t SCAN_INTERVAL_MS     = 500;
static const uint16_t SCAN_WINDOW_MS       = 100;    // ~20% duty cycle

// Connection parameters toward the earbuds.
// Units: 1.25 ms intervals, 10 ms supervision timeout.
static const uint16_t CONN_MIN_INTERVAL    = 24;     // 30 ms
static const uint16_t CONN_MAX_INTERVAL    = 40;     // 50 ms
static const uint16_t CONN_LATENCY         = 0;
static const uint16_t CONN_TIMEOUT         = 600;    // 6000 ms

// Right after connecting, the earbuds ask for a ~500 ms connection interval. A
// reading can only cross at a connection event, so accepting that adds ~250 ms
// on average. With this on, the bridge refuses any request slower than
// CONN_MAX_INTERVAL and the link stays at 30-50 ms. It may cost the earbuds
// some battery. Set false to accept whatever they ask for.
static const bool     KEEP_FAST_SOURCE_INTERVAL = true;

static bool verbose = false;             // 'v' toggles at runtime

// ---------------------------- STATUS LED -----------------------------------
// The XIAO ESP32-C6's onboard user LED (yellow, GPIO15). It is active-LOW.
//   blink-blink-blink-blink...   searching for the earbuds (fast, continuous)
//   blink-blink ... blink-blink  bridging: earbuds streaming, being relayed
//   solid on                     error -- the serial log says which one
#define STATUS_LED_PIN      LED_BUILTIN
#define STATUS_LED_ON       LOW
#define STATUS_LED_OFF      HIGH

// A failed connect/subscribe keeps the LED solid for this long, or until the
// next successful connection. Searching carries on underneath either way.
static const uint32_t CONNECT_ERROR_HOLD_MS = 15000;
// No receiver and not advertising for this long counts as an error.
static const uint32_t ADV_DOWN_ERROR_MS     = 5000;
// ===========================================================================

static const NimBLEUUID SVC_HEART_RATE((uint16_t)0x180D);
static const NimBLEUUID CHR_HR_MEASUREMENT((uint16_t)0x2A37);
static const NimBLEUUID CHR_BODY_SENSOR_LOC((uint16_t)0x2A38);
static const NimBLEUUID SVC_BATTERY((uint16_t)0x180F);
static const NimBLEUUID CHR_BATTERY_LEVEL((uint16_t)0x2A19);

static NimBLEServer*           pServer       = nullptr;
static NimBLECharacteristic*   pHRChar       = nullptr;
static NimBLEAdvertising*      pAdvertising  = nullptr;
static NimBLEClient*           pClient       = nullptr;
static NimBLEAdvertisedDevice* pSourceDevice = nullptr;

static volatile bool     receiverConnected = false;
static volatile bool     wantConnect       = false;
static volatile bool     wantAdvertise     = false;
static bool              scanning          = false;
static bool              scanEnabled       = true;

// lastHR is the value we hand to receivers and is deliberately kept across
// earbuds reconnects, so a receiver keeps seeing the last known number (with
// the contact bit cleared) instead of a zero. lastHRms is per link: 0 means
// "no reading yet on this link".
static volatile uint16_t lastHR   = 0;
static volatile uint32_t lastHRms = 0;

static uint32_t lastNotifyMs  = 0;

// Low-latency hand-off from the BLE callback to loop(). The callback stores the
// reading, sets hrPending and wakes the loop task, which forwards it at once.
static TaskHandle_t      loopTaskHandle = nullptr;
static volatile bool     hrPending      = false;
static volatile uint32_t hrRxUs         = 0;      // micros() when it arrived
static uint32_t          relayLastUs    = 0;      // arrival -> forwarded
static uint32_t          relayMaxUs     = 0;
static uint16_t          receiverHandle = 0xFFFF;

// Battery, filled from Apple proximity adverts (see RELAY_BATTERY).
static NimBLECharacteristic* pBattChar      = nullptr;
static bool              batteryScanning = false;  // current scan is a short battery scan
static uint32_t          lastBattScanMs  = 0;
static int               battPct         = -1;     // what we publish, -1 = unknown
static volatile bool     battFresh       = false;  // new candidate from the scan callback
static volatile int      battBestRssi    = -128;   // strongest advert this scan
static volatile uint8_t  battPodA = 0xF, battPodB = 0xF, battCase = 0xF, battFlags = 0;
static uint32_t lastScanMs    = 0;
static uint32_t lastBeatMs    = 0;
static uint32_t linkedSinceMs = 0;
static uint32_t advOffSinceMs = 0;

// Error conditions, all shown as a solid LED (see errorReason()).
static bool     fatalError     = false;  // BLE stack setup failed; needs a reboot
static uint32_t sourceErrorMs  = 0;      // when the last connect/subscribe failed
static uint32_t advDownSinceMs = 0;      // when advertising was first seen down

// Remembered so the status line can print on change instead of on a timer.
static int prevSource = -1, prevReceiver = -1, prevAdv = -1;

static bool sourceLinked()   { return pClient != nullptr && pClient->isConnected(); }
static bool advertisingNow() { return pAdvertising && pAdvertising->isAdvertising(); }
static bool hrFresh() {
  uint32_t t = lastHRms;
  return t != 0 && (millis() - t < HR_STALE_MS);
}

static void printStatus(const char* tag) {
  uint32_t now = millis();
  uint32_t t   = lastHRms;
  Serial.printf("[%6lus] %-9s earbuds=%d receiver=%d adv=%d scan=%d hr=%u age=%lds batt=%d%% free=%lu\n",
                (unsigned long)(now / 1000), tag,
                sourceLinked() ? 1 : 0, receiverConnected ? 1 : 0,
                advertisingNow() ? 1 : 0, scanning ? 1 : 0,
                (unsigned)lastHR,
                t ? (long)((now - t) / 1000) : -1L,
                battPct,
                (unsigned long)ESP.getFreeHeap());
  lastBeatMs = now;
}

// Connection intervals on both links plus how long a reading sits in the
// bridge before it's forwarded -- the parts of the latency we can see.
static void printTiming() {
  float srcIvl = sourceLinked() ? pClient->getConnInfo().getConnInterval() * 1.25f : 0.0f;
  float rcvIvl = 0.0f;
  if (receiverConnected && receiverHandle != 0xFFFF && pServer != nullptr) {
    rcvIvl = pServer->getPeerInfoByHandle(receiverHandle).getConnInterval() * 1.25f;
  }
  Serial.printf("          timing    earbuds-interval=%.2fms receiver-interval=%.2fms "
                "relay-last=%.2fms relay-max=%.2fms\n",
                srcIvl, rcvIvl, relayLastUs / 1000.0f, relayMaxUs / 1000.0f);
}

// Print only when something actually changed, so a quiet system stays quiet.
static void statusIfChanged() {
  int s = sourceLinked() ? 1 : 0;
  int r = receiverConnected ? 1 : 0;
  int a = advertisingNow() ? 1 : 0;
  if (s != prevSource || r != prevReceiver || a != prevAdv) {
    prevSource = s; prevReceiver = r; prevAdv = a;
    printStatus("change");
  }
}

// ------------------------------ status LED -------------------------------
// Blink patterns: alternating ON/OFF durations in ms, starting with ON.
static const uint16_t LED_PATTERN_SEARCH[] = { 70, 130 };             // ~5 Hz, nonstop
static const uint16_t LED_PATTERN_BRIDGE[] = { 90, 160, 90, 1100 };   // blink-blink, pause

enum LedMode { LED_SEARCHING, LED_BRIDGING, LED_ERROR };

static int         ledLevel     = -1;    // impossible value forces the first write
static int         ledMode      = -1;
static const char* ledError     = nullptr;
static uint32_t    ledModeSince = 0;

static void writeLed(bool on) {
  int level = on ? STATUS_LED_ON : STATUS_LED_OFF;
  if (level == ledLevel) return;
  ledLevel = level;
  digitalWrite(STATUS_LED_PIN, level);
}

// What is wrong right now, or nullptr if nothing is.
static const char* errorReason(uint32_t now) {
  if (fatalError) return "BLE setup failed (reboot with 'r')";
  if (advDownSinceMs != 0 && now - advDownSinceMs > ADV_DOWN_ERROR_MS)
    return "cannot advertise to receivers";
  if (sourceErrorMs != 0 && now - sourceErrorMs < CONNECT_ERROR_HOLD_MS)
    return "earbuds connection failed";
  if (sourceLinked()) {
    // Linked but silent: measured from the last reading, or from the link
    // coming up if there hasn't been one yet.
    uint32_t t     = lastHRms;
    uint32_t since = t ? t : linkedSinceMs;
    if (since != 0 && now >= since && now - since > HR_STALE_MS)
      return t ? "heart rate stopped" : "earbuds connected but sent no heart rate";
  }
  return nullptr;
}

static bool patternLevel(const uint16_t* steps, size_t count, uint32_t elapsed) {
  uint32_t period = 0;
  for (size_t i = 0; i < count; i++) period += steps[i];
  uint32_t t = elapsed % period;
  for (size_t i = 0; i < count; i++) {
    if (t < steps[i]) return (i % 2) == 0;   // even steps are ON
    t -= steps[i];
  }
  return false;
}

static void updateLed() {
  uint32_t now = millis();
  const char* err = errorReason(now);

  LedMode mode;
  if (err != nullptr)                   mode = LED_ERROR;
  else if (sourceLinked() && hrFresh()) mode = LED_BRIDGING;
  else                                  mode = LED_SEARCHING;  // incl. waiting for 1st reading

  if (mode != ledMode || err != ledError) {
    if (mode != ledMode) ledModeSince = now;   // start each pattern on its first blink
    ledMode  = mode;
    ledError = err;
    if (mode == LED_ERROR)         Serial.printf("[led] ERROR: %s\n", err);
    else if (mode == LED_BRIDGING) Serial.println("[led] bridging");
    else                           Serial.println("[led] searching");
  }

  uint32_t elapsed = now - ledModeSince;
  switch (mode) {
    case LED_ERROR:
      writeLed(true);
      break;
    case LED_BRIDGING:
      writeLed(patternLevel(LED_PATTERN_BRIDGE,
                            sizeof(LED_PATTERN_BRIDGE) / sizeof(LED_PATTERN_BRIDGE[0]), elapsed));
      break;
    default:
      writeLed(patternLevel(LED_PATTERN_SEARCH,
                            sizeof(LED_PATTERN_SEARCH) / sizeof(LED_PATTERN_SEARCH[0]), elapsed));
      break;
  }
}

// ------------------------- HR out to the receiver ------------------------
static void pushHR() {
  if (pHRChar == nullptr) return;

  uint16_t hr = lastHR;
  if (hr == 0) return;                   // nothing to say yet -- never send 0 bpm

  // Flags byte. Bit 0 = 0 -> 8-bit value. Bit 2 = contact sensor supported,
  // bit 1 = contact currently detected.
  //   0x06 = supported and detected  -> a live reading
  //   0x04 = supported, NOT detected -> "still here, but I've lost the reading"
  //
  // Receivers tend to hang up on 0 bpm. Holding the last value with the contact
  // bit cleared is the spec's way of saying the number is no longer
  // trustworthy, and keeps the link alive without pretending it's current.
  uint8_t payload[2] = { (uint8_t)(hrFresh() ? 0x06 : 0x04),
                         (uint8_t)(hr > 255 ? 255 : hr) };
  pHRChar->setValue(payload, 2);

  if (receiverConnected) pHRChar->notify();
}

// ------------------------- HR in from the earbuds ------------------------
static void onSourceHR(NimBLERemoteCharacteristic* pChr, uint8_t* pData, size_t length, bool isNotify) {
  if (length < 2) return;

  uint16_t hr;
  if (pData[0] & 0x01) {                 // flags bit 0 selects the value width
    if (length < 3) return;
    hr = (uint16_t)pData[1] | ((uint16_t)pData[2] << 8);
  } else {
    hr = pData[1];
  }
  if (hr == 0) return;                   // a 0 is "no reading", not a heart rate

  bool firstOfSession = (lastHRms == 0);
  lastHR    = hr;
  lastHRms  = millis();
  hrRxUs    = micros();
  hrPending = true;
  // Wake loop() now instead of letting it finish its 20 ms nap. The actual
  // notify happens there -- no BLE calls from inside BLE callbacks.
  if (loopTaskHandle != nullptr) xTaskNotifyGive(loopTaskHandle);

  if (firstOfSession) Serial.printf("Heart rate flowing: %u bpm\n", (unsigned)hr);
  if (verbose) Serial.printf("  hr %u (flags 0x%02X, %u bytes)\n", (unsigned)hr, pData[0], (unsigned)length);
}

// ------------------------------ callbacks --------------------------------
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    receiverConnected = true;
    receiverHandle    = connInfo.getConnHandle();
    Serial.printf(">>> RECEIVER CONNECTED (%s, interval %.2f ms)\n",
                  connInfo.getAddress().toString().c_str(),
                  connInfo.getConnInterval() * 1.25f);
  }
  void onConnParamsUpdate(NimBLEConnInfo& connInfo) override {
    Serial.printf("Receiver connection interval now %.2f ms\n",
                  connInfo.getConnInterval() * 1.25f);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    receiverConnected = false;
    receiverHandle    = 0xFFFF;
    wantAdvertise     = true;            // restarted from loop(), never from here
    // 0x213 = remote user terminated (the receiver chose to hang up)
    // 0x208 = supervision timeout (the link was starved)
    Serial.printf(">>> RECEIVER DISCONNECTED (reason 0x%02X)\n", reason);
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    if (verbose) Serial.println("  earbuds link up");
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    // 0x216 = we ended it, 0x213 = the earbuds ended it, 0x208 = timeout
    Serial.printf("Earbuds disconnected (reason 0x%02X).\n", reason);
  }
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    Serial.printf("Earbuds pairing %s (encrypted=%d bonded=%d)\n",
                  connInfo.isEncrypted() ? "ok" : "FAILED",
                  connInfo.isEncrypted() ? 1 : 0, connInfo.isBonded() ? 1 : 0);
  }
  bool onConnParamsUpdateRequest(NimBLEClient* c, const ble_gap_upd_params* p) override {
    // Their interval is part of the end-to-end latency -- see
    // KEEP_FAST_SOURCE_INTERVAL. Always logged.
    bool accept = !KEEP_FAST_SOURCE_INTERVAL || p->itvl_min <= CONN_MAX_INTERVAL;
    Serial.printf("Earbuds asked for interval %.2f-%.2f ms (latency %u): %s\n",
                  p->itvl_min * 1.25f, p->itvl_max * 1.25f, (unsigned)p->latency,
                  accept ? "accepted" : "refused, keeping the fast interval");
    return accept;
  }
};

// Apple "proximity pairing" advert (manufacturer data, company 0x004C, type 0x07):
//   [0-1] 4C 00  [2] 07  [3] length  [4] prefix  [5-6] model (LE)  [7] status
//   [8] pod batteries, one per nibble  [9] charging flags (hi) | case battery (lo)
// Battery nibbles are 0-10 (x10%), 0xF = unknown / not connected.
static void parseAppleProximity(const NimBLEAdvertisedDevice* dev) {
  for (uint8_t i = 0; i < dev->getManufacturerDataCount(); i++) {
    std::string md = dev->getManufacturerData(i);
    if (md.size() < 10) continue;
    const uint8_t* d = (const uint8_t*)md.data();
    if (d[0] != 0x4C || d[1] != 0x00 || d[2] != 0x07) continue;

    uint16_t model = (uint16_t)d[5] | ((uint16_t)d[6] << 8);
    int      rssi  = dev->getRSSI();
    if (verbose) {
      Serial.printf("  apple-07 model=0x%04X rssi=%d pods=%X/%X case=%X flags=%X raw=",
                    model, rssi, d[8] >> 4, d[8] & 0x0F, d[9] & 0x0F, d[9] >> 4);
      for (size_t k = 0; k < md.size(); k++) Serial.printf("%02X", d[k]);
      Serial.println();
    }
    if (SOURCE_APPLE_MODEL != 0 && model != SOURCE_APPLE_MODEL) continue;
    if (rssi <= battBestRssi) continue;  // someone else's, further away

    battBestRssi = rssi;
    battPodA     = d[8] >> 4;
    battPodB     = d[8] & 0x0F;
    battCase     = d[9] & 0x0F;
    battFlags    = d[9] >> 4;
    battFresh    = true;
  }
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (RELAY_BATTERY) parseAppleProximity(dev);

    if (verbose && dev->haveName()) {
      Serial.printf("  seen \"%s\" %s rssi=%d%s\n",
                    dev->getName().c_str(), dev->getAddress().toString().c_str(),
                    dev->getRSSI(),
                    dev->isAdvertisingService(SVC_HEART_RATE) ? " [HR]" : "");
    }
    if (wantConnect || sourceLinked()) return;
    if (!dev->haveName()) return;
    if (dev->getName().rfind(SOURCE_NAME_PREFIX, 0) != 0) return;   // starts-with

    Serial.printf("Found \"%s\" (rssi %d)\n", dev->getName().c_str(), dev->getRSSI());
    // Copy the device -- the pointer we are handed belongs to the scan results.
    if (pSourceDevice != nullptr) delete pSourceDevice;
    pSourceDevice = new NimBLEAdvertisedDevice(*dev);
    wantConnect = true;
  }
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    scanning        = false;             // the watchdog in loop() restarts it
    batteryScanning = false;
  }
};

static ServerCallbacks serverCallbacks;
static ClientCallbacks clientCallbacks;
static ScanCallbacks   scanCallbacks;

// ------------------------------ scanning ---------------------------------
// durationMs = 0 scans until stopped (looking for the earbuds); anything else
// is a short battery scan while already linked.
static void startScan(uint32_t durationMs = 0) {
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->stop();
  pScan->clearResults();                 // NimBLE filters duplicates against this
  battBestRssi    = -128;                // pick the strongest advert per scan
  batteryScanning = (durationMs != 0);
  scanning   = pScan->start(durationMs, false, true);   // restart clears the cache
  lastScanMs = millis();
  if (!scanning) {
    batteryScanning = false;
    Serial.println("Scan failed to start, will retry.");
    if (durationMs == 0) sourceErrorMs = millis();
  } else if (verbose) {
    Serial.println(durationMs ? "  battery scan" : "  scanning");
  }
}

// Turn the latest advert into one percentage and publish it if it changed.
static int podPct(uint8_t n) { return n <= 10 ? n * 10 : -1; }

static void commitBattery() {
  if (!battFresh) return;
  battFresh = false;
  int a = podPct(battPodA), b = podPct(battPodB);
  int pct = (a >= 0 && b >= 0) ? (a < b ? a : b) : (a >= 0 ? a : b);
  if (pct < 0 || pct == battPct) return;

  battPct = pct;
  Serial.printf("Earbuds battery %d%% (buds %d/%d%%, case %d%%)\n",
                pct, a, b, podPct(battCase));
  if (pBattChar != nullptr) {
    uint8_t v = (uint8_t)pct;
    pBattChar->setValue(&v, 1);
    if (receiverConnected) pBattChar->notify();
  }
}

static void dropSource(const char* why) {
  Serial.printf("Dropping earbuds link: %s\n", why);
  if (pClient != nullptr) pClient->disconnect();
  linkedSinceMs = 0;
  lastScanMs    = 0;                     // let the watchdog rescan immediately
}

// ------------------------------- connect ---------------------------------
static bool subscribeHR(NimBLERemoteCharacteristic* chr) {
  bool useNotify = chr->canNotify();
  if (!useNotify && !chr->canIndicate()) return false;
  return chr->subscribe(useNotify, onSourceHR);
}

static void connectToSource() {
  if (pSourceDevice == nullptr) return;

  NimBLEDevice::getScan()->stop();
  scanning = false;

  if (pClient == nullptr) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks, false);
    pClient->setConnectionParams(CONN_MIN_INTERVAL, CONN_MAX_INTERVAL,
                                 CONN_LATENCY, CONN_TIMEOUT);
  }

  if (!pClient->connect(pSourceDevice)) {
    Serial.println("Connect to earbuds failed, will rescan.");
    sourceErrorMs = millis();
    lastScanMs    = millis();
    return;
  }

  // Discover the whole GATT table up front, before subscribing. Rediscovering
  // later would delete the characteristic objects our notify callback hangs on.
  if (!pClient->discoverAttributes()) {
    Serial.println("Full service discovery failed, looking up heart rate only.");
  }

  NimBLERemoteService* svc = pClient->getService(SVC_HEART_RATE);
  NimBLERemoteCharacteristic* chr =
      svc ? svc->getCharacteristic(CHR_HR_MEASUREMENT) : nullptr;

  if (chr == nullptr) {
    Serial.println("Earbuds have no heart rate characteristic -- is HR sharing on?");
    pClient->disconnect();
    sourceErrorMs = millis();
    lastScanMs    = millis();
    return;
  }

  bool ok = subscribeHR(chr);
  if (!ok && SOURCE_ALLOW_PAIRING && pClient->isConnected()) {
    // NimBLE already retries a CCCD write that fails for lack of encryption,
    // but some peers reject it differently. One explicit pair-and-retry.
    Serial.println("Subscribe refused, trying to pair with the earbuds...");
    if (pClient->secureConnection()) ok = subscribeHR(chr);
  }
  if (!ok) {
    Serial.println("Could not subscribe to the earbuds' heart rate.");
    pClient->disconnect();
    sourceErrorMs = millis();
    lastScanMs    = millis();
    return;
  }
  sourceErrorMs = 0;                     // a good connection clears any old error

  // Start this link's clock fresh. Without this, lastHRms still holds a
  // timestamp from the LAST link, the staleness check takes the "was streaming,
  // now silent" branch, and this new connection is killed instantly instead of
  // getting the first-reading grace period. lastHR is left alone on purpose.
  lastHRms      = 0;
  linkedSinceMs = millis();
  Serial.printf("Earbuds connected (interval %.2f ms), waiting for first reading...\n",
                pClient->getConnInfo().getConnInterval() * 1.25f);
}

// Print the earbuds' services and characteristics as discovered at connect.
// Uses the cached table only -- a refresh would break the HR subscription.
static void dumpSourceGatt() {
  if (!sourceLinked()) { Serial.println("[cmd] earbuds not connected"); return; }
  Serial.println("[cmd] earbuds GATT table (R=read W=write w=write-no-rsp N=notify I=indicate):");
  for (NimBLERemoteService* s : pClient->getServices(false)) {
    Serial.printf("  service %s\n", s->getUUID().toString().c_str());
    for (NimBLERemoteCharacteristic* c : s->getCharacteristics(false)) {
      Serial.printf("    char %-38s %c%c%c%c%c\n", c->getUUID().toString().c_str(),
                    c->canRead() ? 'R' : '-', c->canWrite() ? 'W' : '-',
                    c->canWriteNoResponse() ? 'w' : '-', c->canNotify() ? 'N' : '-',
                    c->canIndicate() ? 'I' : '-');
    }
  }
}

// --------------------------- serial commands -----------------------------
static void handleSerial() {
  while (Serial.available()) {
    int c = Serial.read();
    switch (c) {
      case 's': scanEnabled = true; startScan(); Serial.println("[cmd] rescan"); break;
      case 'p':
        scanEnabled = !scanEnabled;
        Serial.printf("[cmd] scanning %s\n", scanEnabled ? "ENABLED" : "PAUSED");
        if (!scanEnabled) { NimBLEDevice::getScan()->stop(); scanning = false; }
        break;
      case 'a':
        pAdvertising->stop();
        delay(50);
        pAdvertising->start();
        Serial.printf("[cmd] advertising restarted, on=%d\n", advertisingNow() ? 1 : 0);
        break;
      case 'd': dropSource("manual"); break;
      case 'v': verbose = !verbose; Serial.printf("[cmd] verbose %s\n", verbose ? "ON" : "OFF"); break;
      case 'i': printStatus("info"); printTiming(); break;
      case 'g': dumpSourceGatt(); break;
      case 'b':
        lastBattScanMs = 0;              // loop() starts one as soon as it can
        Serial.printf("[cmd] battery scan requested (last %d%%)\n", battPct);
        break;
      case 'r': Serial.println("[cmd] reboot"); delay(100); ESP.restart(); break;
      default: break;
    }
  }
}

// -------------------------------- setup ----------------------------------
void setup() {
  loopTaskHandle = xTaskGetCurrentTaskHandle();   // setup() and loop() share a task

  Serial.begin(115200);
  delay(300);

  Serial.printf("\n=== Powerbeats -> %s HR bridge ===\n", BRIDGE_NAME);
  Serial.println("s=rescan p=pause a=re-advertise d=drop v=verbose i=info b=battery g=gatt r=reboot");

  // The XIAO_ESP32C6 variant already enables the RF switch (GPIO3 LOW) and
  // selects the onboard antenna (GPIO14 LOW) before setup() runs. Only touch
  // GPIO14 if an external u.FL antenna is wanted.
  if (USE_EXTERNAL_ANTENNA) {
    pinMode(WIFI_ANT_CONFIG, OUTPUT);
    digitalWrite(WIFI_ANT_CONFIG, HIGH);
  }

  pinMode(STATUS_LED_PIN, OUTPUT);
  writeLed(false);

  if (!NimBLEDevice::init(BRIDGE_NAME)) {  // also the GAP Device Name (0x2A00)
    Serial.println("FATAL: BLE stack failed to start.");
    fatalError = true;
    return;                              // loop() just holds the LED on
  }
  NimBLEDevice::setPower(9);
  if (SOURCE_ALLOW_PAIRING) {
    // Lets us pair with the earbuds if they insist (Just Works: bonding, no
    // MITM, secure connections). Nothing on our own server requires it.
    NimBLEDevice::setSecurityAuth(true, false, true);
  }

  // ---------------- peripheral side (faces the receiver) ----------------
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);

  NimBLEService* pService = pServer->createService(SVC_HEART_RATE);

  pHRChar = pService->createCharacteristic(
              CHR_HR_MEASUREMENT,
              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  // NimBLE creates the CCCD (0x2902) itself. Adding one by hand creates a
  // duplicate and can leave a receiver unable to subscribe.
  uint8_t initial[2] = { 0x04, 0x00 };   // valid from the very first read
  pHRChar->setValue(initial, 2);

  NimBLECharacteristic* pLocChar =
      pService->createCharacteristic(CHR_BODY_SENSOR_LOC, NIMBLE_PROPERTY::READ);
  // 1 = Chest, the value a strap reports and what equipment expects. (5 = Ear
  // lobe would be the honest answer, if a receiver ever cares.)
  uint8_t sensorLocation = 1;
  pLocChar->setValue(&sensorLocation, 1);

  if (RELAY_BATTERY) {
    // Standard Battery Service. Left empty until the first reading arrives
    // rather than reporting a made-up number.
    NimBLEService* pBattService = pServer->createService(SVC_BATTERY);
    pBattChar = pBattService->createCharacteristic(
                  CHR_BATTERY_LEVEL, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  }

  if (!pServer->start()) {               // NimBLE 2.x starts all services here
    Serial.println("FATAL: GATT server failed to start.");
    fatalError = true;
    return;
  }

  // Name in the primary packet so passive scanners see it too; appearance in
  // the scan response.
  pAdvertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);                // LE General Discoverable, no BR/EDR
  advData.setCompleteServices(SVC_HEART_RATE);
  advData.setName(BRIDGE_NAME);
  pAdvertising->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setAppearance(0x0341);        // Heart Rate Belt
  pAdvertising->setScanResponseData(scanData);
  pAdvertising->enableScanResponse(true);

  pAdvertising->setMinInterval(0x30);    // 30 ms, like a real strap
  pAdvertising->setMaxInterval(0x60);    // 60 ms
  pAdvertising->start();

  // ---------------- central side (faces the earbuds) ----------------
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setActiveScan(true);            // the name may only be in the scan response
  pScan->setInterval(SCAN_INTERVAL_MS);
  pScan->setWindow(SCAN_WINDOW_MS);
  startScan();

  Serial.printf("Ready. Advertising as %s, looking for \"%s*\".\n",
                BRIDGE_NAME, SOURCE_NAME_PREFIX);
}

// --------------------------------- loop ----------------------------------
void loop() {
  if (fatalError) {
    // BLE never came up, so none of the BLE calls below are safe. Hold the LED
    // on and accept only the reboot command.
    while (Serial.available()) {
      if (Serial.read() == 'r') { delay(100); ESP.restart(); }
    }
    updateLed();
    delay(20);
    return;
  }

  uint32_t now = millis();
  handleSerial();

  if (wantConnect) {
    wantConnect = false;
    if (!sourceLinked()) connectToSource();
    // connectToSource() blocks while connecting and discovering, and stamps
    // linkedSinceMs with the time it FINISHED -- later than the `now` above. On
    // unsigned math `now - linkedSinceMs` would then underflow to a huge value
    // and trip every staleness check instantly. Re-read the clock.
    now = millis();
  }

  if (wantAdvertise) {
    wantAdvertise = false;
    pAdvertising->start();
  }

  // Safety net: not talking to a receiver and not advertising means nothing can
  // find us. Debounced by two seconds so it cannot fire in the gap between the
  // radio accepting a connection and onConnect() running.
  if (!receiverConnected && pAdvertising && !advertisingNow()) {
    if (advDownSinceMs == 0) advDownSinceMs = now;     // for the LED error state
    if (advOffSinceMs == 0) {
      advOffSinceMs = now;
    } else if (now - advOffSinceMs > 2000) {
      advOffSinceMs = 0;
      pAdvertising->start();
    }
  } else {
    advOffSinceMs  = 0;
    advDownSinceMs = 0;
  }

  if (sourceLinked()) {
    // The open-ended search scan isn't needed once linked. Short battery scans
    // are, every BATTERY_SCAN_EVERY_MS (sooner until the first reading).
    if (scanning && !batteryScanning) { NimBLEDevice::getScan()->stop(); scanning = false; }
    if (RELAY_BATTERY && !scanning &&
        (lastBattScanMs == 0 ||
         now - lastBattScanMs >= (battPct < 0 ? BATTERY_RETRY_MS : BATTERY_SCAN_EVERY_MS))) {
      lastBattScanMs = now;
      startScan(BATTERY_SCAN_LEN_MS);
    }

    uint32_t t = lastHRms;
    bool silent;
    if (t == 0) {
      silent = (linkedSinceMs != 0) && (now >= linkedSinceMs) &&
               (now - linkedSinceMs > FIRST_HR_GRACE_MS);
    } else {
      silent = (now >= t) && (now - t > ZOMBIE_MS);
    }
    if (silent) dropSource("no heart rate data, link looks stale");
  } else {
    if (scanEnabled && !scanning && (now - lastScanMs >= SCAN_RETRY_MS)) startScan();
  }

  commitBattery();

  if (hrPending) {
    // A fresh reading: forward it right away.
    hrPending = false;
    uint32_t rx = hrRxUs;
    pushHR();
    relayLastUs = micros() - rx;
    if (relayLastUs > relayMaxUs) relayMaxUs = relayLastUs;
    lastNotifyMs = millis();
    if (verbose) {
      Serial.printf("  relayed %u bpm %.2f ms after arrival%s\n", (unsigned)lastHR,
                    relayLastUs / 1000.0f, receiverConnected ? "" : " (no receiver)");
    }
  } else if (now - lastNotifyMs >= KEEPALIVE_MS) {
    // Nothing new for a while: repeat the last value so the receiver stays
    // happy. pushHR() clears the contact bit once the reading is stale.
    lastNotifyMs = now;
    pushHR();
  }

  updateLed();
  statusIfChanged();
  // millis(), not `now`: statusIfChanged() may have just stamped lastBeatMs
  // with a later time, and `now - lastBeatMs` would underflow.
  if (millis() - lastBeatMs >= HEARTBEAT_MS) printStatus("status");

  // Sleep up to 20 ms (keeps the LED patterns smooth), but wake immediately
  // when onSourceHR() signals a new reading.
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
}
