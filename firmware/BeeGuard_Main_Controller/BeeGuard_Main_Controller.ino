/*
 * BeeGuard — ESP32 #1 (Main Controller) — v5.0
 *
 * WHAT CHANGED vs v4.0
 *  1. ENTRANCE ANGLES FLIPPED. What the firmware drove as "open" was physically
 *     the narrow position. ENTRANCE_OPEN_ANGLE / ENTRANCE_NARROW_ANGLE are now
 *     swapped so the app's labels match the gate.
 *  2. REAL CLOCK. uptimeStamp() ("1234s") is gone. NTP gives wall-clock
 *     timestamps + epoch on alerts, RFID logs, detections and daily rollover.
 *  3. ULTRASONIC FIXED. A pulseIn() timeout used to compute as a FULL tank —
 *     a dead sensor reported 100%. Now: median of 5 pings, timeout => NAN =>
 *     the last good value is held and a sensor fault is raised. Speed of sound
 *     is temperature-compensated from the hive DHT reading.
 *  4. SCALE FIXED. Calibration + tare persist in /calibration so weight no
 *     longer resets to zero on every reboot. A failed read holds the last good
 *     value instead of publishing 0.0 (which used to look like a lost hive).
 *  5. EVENT SYSTEM. Every actuator and sensor transition raises an /alerts
 *     entry with severity + real timestamp. Level alerts are throttled — the
 *     old code appended a new node every 30 s forever while a level was low.
 *  6. HONEY ESTIMATION. Weight gain is tracked against a persisted baseline,
 *     rolled into /production/history daily, and harvests are detected from
 *     the weight drop after a collection.
 *  7. NON-BLOCKING PUMPS. Smoke (4 s) and feeding (up to 12 s) no longer
 *     delay() the main loop, so RFID and alerts stay responsive — and a pump
 *     can actually be stopped while it is running.
 *  8. TYPED ESP-NOW. Packets carry a msgType byte instead of being told apart
 *     by sizeof(), and sound labels are normalised to the app's vocabulary.
 *
 * ─── GPIO MAPPING ─────────────────────────────────────────
 * Main Door Servo (Honey Collection)  → GPIO 18
 * Lock Servo                          → GPIO 19
 * Entrance Servo                      → GPIO 13
 * Feeding Pump Relay                  → GPIO 33  (ACTIVE LOW)
 * Smoke Pump Relay                    → GPIO 32  (ACTIVE LOW)
 * Ultrasonic TRIG                     → GPIO 26
 * Ultrasonic ECHO                     → GPIO 16
 * HX711 DT                            → GPIO 17
 * HX711 SCK                           → GPIO 15
 * RFID SDA (SS)                       → GPIO 5
 * RFID SCK                            → GPIO 14
 * RFID MOSI                           → GPIO 25
 * RFID MISO                           → GPIO 27
 * RFID RST                            → GPIO 4
 *
 * Libraries: Firebase ESP32 Client (Mobizt), ESP32Servo, HX711 (Bogdan
 * Necula), MFRC522 (GithubCommunity).
 */

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include <HX711.h>
#include <SPI.h>
#include <MFRC522.h>

// ═══════════════════════════════════════════════════════════
// WIFI — MUST BE IDENTICAL ON ALL THREE BOARDS
// ═══════════════════════════════════════════════════════════
// ESP-NOW only reaches peers that sit on the same radio channel, and a board's
// channel is whichever channel its access point uses. If this controller joins
// one network and the camera joins another, esp_now_send() still returns
// ESP_OK but the packet is never received. Keep these three lines the same in
// BeeGuard_Main_Controller.ino, esp32cam_hive_guard.ino and BEE_HIVE_MONITOR.ino.
#define WIFI_SSID     "MSI"
#define WIFI_PASSWORD "123456789"

// ─── Firebase ─────────────────────────────────────────────
#define API_KEY      "AIzaSyCqzB6EBkAenCuURUXDoji7N67xjDTb8SI"
#define DATABASE_URL "https://beeguard-smartbee-default-rtdb.europe-west1.firebasedatabase.app"

// ─── Camera node (for the app's live view) ────────────────
#define CAMERA_STREAM_URL   "http://192.168.137.150/"
#define CAMERA_STATUS_URL   "http://192.168.137.150:81/status"

// ─── Time ─────────────────────────────────────────────────
#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"
#define GMT_OFFSET_SEC  (3 * 3600)   // Jordan = UTC+3
#define DST_OFFSET_SEC  0

// ─── GPIO Pins ────────────────────────────────────────────
#define SERVO_DOOR_PIN      18   // Honey collection door
#define SERVO_LOCK_PIN      19   // Hive lock
#define SERVO_ENTRANCE_PIN  13   // Entrance narrowing

#define RELAY_FEED_PIN      33   // Feeding pump  (ACTIVE LOW)
#define RELAY_SMOKE_PIN     32   // Smoke pump    (ACTIVE LOW)

#define TRIG_PIN            26   // Ultrasonic
#define ECHO_PIN            16

#define HX711_DT            17   // Load cell
#define HX711_SCK           15

#define RFID_SS_PIN         5
#define RFID_RST_PIN        4

// ─── Servo angles ─────────────────────────────────────────
// FLIPPED in v5.0: the gate is mounted so that 180° is the wide/open position
// and 0° is the narrowed one. Before this swap "Open Entrance" in the app
// physically narrowed the gate and vice-versa. If the servo is ever remounted,
// swap these two numbers again — nothing else needs to change.
#define DOOR_CLOSED_ANGLE     0
#define DOOR_OPEN_ANGLE       180
#define LOCK_LOCKED_ANGLE     90     // boot / secured position
#define LOCK_UNLOCKED_ANGLE   180
#define ENTRANCE_OPEN_ANGLE   180
#define ENTRANCE_NARROW_ANGLE 0

// ─── Timing ───────────────────────────────────────────────
#define SMOKE_DURATION_MS   4000UL  // smoke pump burst during honey collection
#define FEED_MS_PER_ML      120UL   // 0.12 s/ml -> 50ml=6s, 100ml=12s
#define FEED_MAX_MS         30000UL // hard safety ceiling on one pump run

const unsigned long UPLOAD_INTERVAL   = 30000;  // sensor push cadence
const unsigned long COMMAND_INTERVAL  = 700;    // command poll cadence
const unsigned long NODE_TIMEOUT_MS   = 150000; // peer considered offline after
const unsigned long ALERT_COOLDOWN_MS = 900000; // 15 min between repeat level alerts

// How long the entrance stays narrowed after the last hornet leaves before it
// reopens on its own. Previously it never reopened: one sighting left the hive
// constricted until somebody opened the app, which costs the colony ventilation
// and foraging traffic for as long as nobody notices. The delay stops it
// flapping when a hornet drifts in and out of frame.
// Set to 0 to disable auto-reopen and require a manual tap instead.
const unsigned long HORNET_REOPEN_DELAY_MS = 10UL * 60UL * 1000UL;

// ─── Feeding container geometry ───────────────────────────
// Defaults; overridden at boot by /calibration if present so the tank can be
// re-measured from the app without reflashing.
float sensorToTopCm    = 4.0;   // ultrasonic face down to the container rim
float containerHeightCm = 3.5;  // rim to inside floor
float containerDiameterCm = 9.0;// for the remaining-volume estimate

// ─── Scale calibration ────────────────────────────────────
// SCALE_FACTOR converts raw HX711 counts to kilograms; it is load-cell
// specific and MUST be measured (see /calibration/scale_factor). tareOffset is
// the raw reading of the empty hive and is persisted so weight survives a
// reboot instead of re-zeroing with the hive already on the cell.
float scaleFactor = -7050.0;
long  tareOffset  = 0;

// ─── Honey model ──────────────────────────────────────────
// Not all of a hive's weight gain is harvestable honey — some becomes wax,
// brood and bee mass. honeyFraction scales net gain into an *estimate*; raise
// or lower it in /calibration/honey_fraction once real harvests are weighed.
float honeyFraction   = 0.85;
float harvestDropKg   = 0.50;   // weight drop that counts as a harvest
float baselineWeight  = 4.0;    // approx empty-box weight so the honey estimate is never blank; re-measure via Set Baseline

// ═══════════════════════════════════════════════════════════
// ESP-NOW WIRE FORMAT — keep byte-identical on every board
// ═══════════════════════════════════════════════════════════
#define MSG_SENSOR 1
#define MSG_CAM    2

typedef struct __attribute__((packed)) {
  uint8_t msgType;         // MSG_SENSOR
  float   temperature;
  float   humidity;
  char    soundResult[20];
  uint8_t confidence;      // 0..100
  uint8_t freshAnalysis;   // 1 = new classification, 0 = environment-only tick
} SensorData;

typedef struct __attribute__((packed)) {
  uint8_t  msgType;            // MSG_CAM
  uint8_t  hornetDetected;     // 1 = hornet in the current frame
  uint8_t  count;              // boxes in the current frame
  uint32_t secondsSinceLast;   // since last confirmed detection, 0xFFFFFFFF = never
} CamData;

// ─── Feeding level reading ────────────────────────────────
// Declared up here, not next to readFeedingLevel(): the Arduino builder injects
// prototypes for every function immediately before the FIRST function in the
// file, so a return type defined later in the file is not yet visible and the
// build fails on a line that does not appear in the source.
struct FeedingData {
  float distance;      // NAN on sensor fault
  float waterHeight;   // cm of solution left
  float percentage;    // 0..100
  float remainingMl;
  bool  valid;
};

// ─── Objects ──────────────────────────────────────────────
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;
Servo          servoDoor;
Servo          servoLock;
Servo          servoEntrance;
HX711          scale;
MFRC522        rfid(RFID_SS_PIN, RFID_RST_PIN);

// ─── Scheduling ───────────────────────────────────────────
unsigned long lastSensorUpload = 0;
unsigned long lastCommandPoll  = 0;

// ─── Actuator state ───────────────────────────────────────
bool   doorOpen         = false;  // honey door
bool   lockState        = true;   // true = LOCKED (secure boot default)
String entranceState    = "open";
bool   lastCollectHoney = false;

// Non-blocking pump timers. 0 = idle; otherwise the millis() deadline.
unsigned long smokeOffAt = 0;
unsigned long feedOffAt  = 0;
int           feedingMl  = 0;

// millis() + duration can land on exactly 0 once per 49.7-day wrap, and 0 is
// our "idle" sentinel — the deadline would then never be seen and the relay
// would stay latched on. Nudging to 1 costs one millisecond and removes the
// case entirely.
static inline unsigned long deadlineAt(unsigned long durationMs) {
  unsigned long d = millis() + durationMs;
  return (d == 0) ? 1UL : d;
}

// ─── Sensor state ─────────────────────────────────────────
float receivedTemp       = NAN;
float receivedHumidity   = NAN;
String receivedSound     = "Unknown";
int   receivedConfidence = 0;
bool  hornetDetected     = false;
int   hornetCount        = 0;
bool  lastHornetState    = false;

// Auto-reopen bookkeeping. entranceNarrowedByHornet distinguishes "the system
// narrowed this" from "the beekeeper narrowed this" — only the former is
// reopened automatically. hornetClearedAt is 0 when no timer is running.
bool          entranceNarrowedByHornet = false;
unsigned long hornetClearedAt          = 0;

// Seconds since the camera's last confirmed detection, as the camera reports
// it. 0xFFFFFFFF = it has never seen one. The camera has no clock, so it sends
// an age and we convert it here where NTP is available — which also means a
// reboot of this controller recovers the real detection time from the camera
// instead of losing it.
#define CAM_NEVER_DETECTED 0xFFFFFFFFUL
uint32_t camSecondsSinceDetection = CAM_NEVER_DETECTED;

float lastGoodWeight     = NAN;
float lastGoodLevelPct   = NAN;
float lastGoodWaterCm    = NAN;

unsigned long lastAudioPacket = 0;
unsigned long lastCamPacket   = 0;
bool audioOnline = false;
bool camOnline   = false;

// ─── Hand-off from the ESP-NOW callback to loop() ─────────
// The receive callback runs in the Wi-Fi task, which has a ~3.5 KB stack and
// must not block. Doing Firebase writes there overflows that stack, and
// touching a String there races the loop task's reads of the same String
// (heap free while it is being read). So the callback only copies scalars and
// raises this flag; every String and Firebase operation happens in loop().
volatile bool    pendingSoundUpdate = false;
volatile bool    pendingSoundFresh  = false;
volatile uint8_t pendingSoundConf   = 0;
char             pendingSoundLabel[21] = "Unknown";

// ─── Production tracking ──────────────────────────────────
String todayDate         = "";
float  todayStartWeight  = NAN;
float  weeklyProduction  = 0.0;
float  monthlyProduction = 0.0;
int    currentWeek       = -1;
int    currentMonth      = -1;
float  totalHarvested    = 0.0;
float  prevWeightSample  = NAN;

bool timeSynced = false;

// ═══════════════════════════════════════════════════════════
// TIME HELPERS
// ═══════════════════════════════════════════════════════════

bool syncTime() {
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  struct tm t;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&t, 500)) {
      timeSynced = true;
      return true;
    }
  }
  Serial.println("[TIME] NTP sync FAILED — timestamps will fall back to uptime");
  return false;
}

time_t nowEpoch() {
  time_t n;
  time(&n);
  return n;
}

// "2026-08-20 14:33:10" — falls back to uptime so a failed NTP sync still
// produces something ordered rather than an empty string.
String nowStamp() {
  struct tm t;
  if (!getLocalTime(&t, 100)) {
    return "uptime " + String(millis() / 1000) + "s";
  }
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String nowClock() {  // "14:33"
  struct tm t;
  if (!getLocalTime(&t, 100)) return "--:--";
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

// Format an arbitrary epoch (not just "now") — used to turn the camera's
// "seconds since last detection" into a wall-clock time.
String stampFromEpoch(time_t e) {
  struct tm t;
  if (!timeSynced || e <= 0) return "Unknown";
  localtime_r(&e, &t);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String todayKey() {  // "2026-08-20"
  struct tm t;
  if (!getLocalTime(&t, 100)) return "1970-01-01";
  char buf[12];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

int isoWeek() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return -1;
  char buf[4];
  strftime(buf, sizeof(buf), "%V", &t);
  return atoi(buf);
}

int monthNumber() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return -1;
  return t.tm_mon + 1;
}

// ═══════════════════════════════════════════════════════════
// ALERTS / EVENT FEED
// ═══════════════════════════════════════════════════════════
// Two entry points:
//   pushEvent()     — edge-triggered. Something just happened; always record it.
//   pushLevelAlert()— level-triggered. A reading is out of range and will stay
//                     that way; record it at most once per cooldown so the
//                     feed doesn't grow by one node every 30 s.

uint16_t alertSeq = 0;

String alertKey() {
  // Zero-padded epoch keeps Firebase's lexicographic key order equal to time
  // order, so the app can read the newest alerts without a sort.
  char buf[32];
  snprintf(buf, sizeof(buf), "evt_%010lu_%03u",
           (unsigned long)nowEpoch(), (unsigned)(alertSeq++ % 1000));
  return String(buf);
}

#define MAX_THROTTLES 16
struct AlertThrottle {
  String        key;
  unsigned long lastMs;
  bool          used;
};
AlertThrottle throttles[MAX_THROTTLES];

bool allowAlert(const String &key, unsigned long cooldownMs) {
  unsigned long now = millis();
  for (int i = 0; i < MAX_THROTTLES; i++) {
    if (throttles[i].used && throttles[i].key == key) {
      // Unsigned subtraction stays correct across the 49-day millis() rollover.
      if (now - throttles[i].lastMs < cooldownMs) return false;
      throttles[i].lastMs = now;
      return true;
    }
  }
  for (int i = 0; i < MAX_THROTTLES; i++) {
    if (!throttles[i].used) {
      throttles[i].used   = true;
      throttles[i].key    = key;
      throttles[i].lastMs = now;
      return true;
    }
  }
  return true; // table full: don't suppress
}

// Clear a throttle so the next occurrence reports immediately. Called when a
// reading returns to normal, so "low water" alerts again on the next dip
// instead of waiting out the cooldown.
void resetAlert(const String &key) {
  for (int i = 0; i < MAX_THROTTLES; i++) {
    if (throttles[i].used && throttles[i].key == key) {
      throttles[i].used = false;
      return;
    }
  }
}

void writeAlert(const String &type, const String &severity, const String &message) {
  FirebaseJson json;
  json.set("type",      type);
  json.set("severity",  severity);   // info | warning | critical
  json.set("message",   message);
  json.set("timestamp", nowStamp());
  json.set("epoch",     (int)nowEpoch());
  json.set("read",      false);

  // One PUT for the whole node — the previous version issued four separate
  // REST writes per alert.
  //
  // The path is built into a named String first: `"/alerts/" + alertKey()` has
  // static type StringSumHelper, and the library's path parameter is a
  // template over a fixed set of string types. Assigning to a String sidesteps
  // any question of whether that helper type is in the set.
  String alertPath = "/alerts/" + alertKey();
  Firebase.RTDB.setJSON(&fbdo, alertPath, &json);
  Serial.printf("[ALERT/%s] %s: %s\n", severity.c_str(), type.c_str(), message.c_str());
}

void pushEvent(const String &type, const String &severity, const String &message) {
  writeAlert(type, severity, message);
}

void pushLevelAlert(const String &throttleKey, const String &type,
                    const String &severity, const String &message) {
  if (!allowAlert(throttleKey, ALERT_COOLDOWN_MS)) return;
  writeAlert(type, severity, message);
}

// ═══════════════════════════════════════════════════════════
// SERVO HELPER
// ═══════════════════════════════════════════════════════════

void sweepServo(Servo &s, int fromAngle, int toAngle) {
  int step = (toAngle >= fromAngle) ? 5 : -5;
  for (int pos = fromAngle; (step > 0) ? (pos <= toAngle) : (pos >= toAngle); pos += step) {
    s.write(pos);
    delay(15);
  }
  s.write(toAngle);
}

// ═══════════════════════════════════════════════════════════
// HIVE LOCK (RFID) — GPIO19
// ═══════════════════════════════════════════════════════════

void publishLockState() {
  Firebase.RTDB.setBool(&fbdo, "/security/locked", lockState);
  Firebase.RTDB.setBool(&fbdo, "/hive_status/locked", lockState);
}

void unlockHive() {
  Serial.println("[LOCK] Unlocking");
  sweepServo(servoLock, LOCK_LOCKED_ANGLE, LOCK_UNLOCKED_ANGLE);
  lockState = false;
  publishLockState();
}

void lockHive() {
  Serial.println("[LOCK] Locking");
  sweepServo(servoLock, LOCK_UNLOCKED_ANGLE, LOCK_LOCKED_ANGLE);
  lockState = true;
  publishLockState();
}

// ═══════════════════════════════════════════════════════════
// ENTRANCE (Hornet) — GPIO13
// ═══════════════════════════════════════════════════════════

void publishEntrance() {
  Firebase.RTDB.setString(&fbdo, "/hornet_detection/entrance_status", entranceState);
  Firebase.RTDB.setString(&fbdo, "/hive_status/entrance_status",      entranceState);
  Firebase.RTDB.setString(&fbdo, "/commands/entrance",                entranceState);
}

void openEntrance(const String &reason) {
  if (entranceState == "open") return;
  Serial.println("[ENTRANCE] Opening");
  sweepServo(servoEntrance, ENTRANCE_NARROW_ANGLE, ENTRANCE_OPEN_ANGLE);
  entranceState = "open";
  publishEntrance();
  pushEvent("entrance_open", "info", "Entrance opened (" + reason + ").");
}

void narrowEntrance(const String &reason) {
  if (entranceState == "narrow") return;
  Serial.println("[ENTRANCE] Narrowing");
  sweepServo(servoEntrance, ENTRANCE_OPEN_ANGLE, ENTRANCE_NARROW_ANGLE);
  entranceState = "narrow";
  publishEntrance();
  pushEvent("entrance_narrow", "warning", "Entrance narrowed (" + reason + ").");
}

// ═══════════════════════════════════════════════════════════
// HONEY COLLECTION — door GPIO18 + smoke GPIO32
// ═══════════════════════════════════════════════════════════

void startSmoke() {
  digitalWrite(RELAY_SMOKE_PIN, LOW);   // ACTIVE LOW = ON
  smokeOffAt = deadlineAt(SMOKE_DURATION_MS);
  Firebase.RTDB.setBool(&fbdo, "/commands/smoke_pump", true);
  pushEvent("smoke", "info", "Smoke pump running for 4 s.");
}

void stopSmoke(bool announce) {
  digitalWrite(RELAY_SMOKE_PIN, HIGH);  // OFF
  smokeOffAt = 0;
  Firebase.RTDB.setBool(&fbdo, "/commands/smoke_pump", false);
  if (announce) pushEvent("smoke", "info", "Smoke pump stopped.");
}

// Open the door and start the smoke burst. The door is LEFT OPEN — closing is
// a separate manual command (collect_honey = false).
void collectHoney() {
  Serial.println("[HONEY] Start — opening door");
  sweepServo(servoDoor, DOOR_CLOSED_ANGLE, DOOR_OPEN_ANGLE);
  doorOpen = true;
  Firebase.RTDB.setBool(&fbdo, "/hive_status/door_open", true);
  pushEvent("door_opened", "info", "Honey door opened for collection.");
  startSmoke();
}

void stopHoneyCollection() {
  Serial.println("[HONEY] Stop — closing door");
  stopSmoke(false);   // safety: never leave the smoke relay latched
  sweepServo(servoDoor, DOOR_OPEN_ANGLE, DOOR_CLOSED_ANGLE);
  doorOpen = false;
  Firebase.RTDB.setBool(&fbdo, "/hive_status/door_open", false);
  pushEvent("door_closed", "info", "Honey door closed.");
}

// ═══════════════════════════════════════════════════════════
// FEEDING — pump GPIO33, volume-based, non-blocking
// ═══════════════════════════════════════════════════════════

void startFeeding(int ml) {
  unsigned long runMs = (unsigned long)ml * FEED_MS_PER_ML;
  if (runMs > FEED_MAX_MS) runMs = FEED_MAX_MS;

  feedingMl = ml;
  feedOffAt = deadlineAt(runMs);
  digitalWrite(RELAY_FEED_PIN, LOW);    // ACTIVE LOW = ON
  Firebase.RTDB.setBool(&fbdo, "/commands/pump", true);

  Serial.printf("[FEED] Dispensing %d ml (%lu ms)\n", ml, runMs);
  pushEvent("feeding", "info", "Feeding started: " + String(ml) + " ml.");
}

void stopFeeding() {
  digitalWrite(RELAY_FEED_PIN, HIGH);   // OFF
  feedOffAt = 0;
  Firebase.RTDB.setBool(&fbdo, "/commands/pump",  false);
  // One-shot: clear the request so it doesn't re-trigger on the next poll.
  Firebase.RTDB.setInt(&fbdo,  "/commands/feed_ml", 0);
  pushEvent("feeding", "info", "Feeding finished: " + String(feedingMl) + " ml dispensed.");
  feedingMl = 0;
}

// Runs every loop: turns pumps off when their deadline passes. Keeping this
// out of the command path means a 12 s feed no longer blocks RFID or alerts.
void serviceActuators() {
  unsigned long now = millis();
  if (smokeOffAt != 0 && (long)(now - smokeOffAt) >= 0) stopSmoke(true);
  if (feedOffAt  != 0 && (long)(now - feedOffAt)  >= 0) stopFeeding();
}

// ═══════════════════════════════════════════════════════════
// ULTRASONIC — FEEDING SOLUTION LEVEL
// ═══════════════════════════════════════════════════════════

// Sound travels at 331.3 + 0.606*T m/s. Ignoring temperature costs ~0.6% per
// °C of error, which on this 3.5 cm tank is worth correcting for since we
// already have a hive temperature from the audio board.
float speedOfSoundCmPerUs(float tempC) {
  float t = isnan(tempC) ? 25.0 : tempC;
  return (331.3 + 0.606 * t) / 10000.0;
}

// One ping. Returns NAN when the echo never comes back — the old code let a
// timeout fall through as distance = 0, which the level formula then turned
// into a FULL tank. A dead sensor read as 100%.
float pingOnce(float tempC) {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return NAN;

  float distance = (duration * speedOfSoundCmPerUs(tempC)) / 2.0;

  // HC-SR04 is only trustworthy from about 2 cm to 400 cm.
  if (distance < 2.0 || distance > 400.0) return NAN;
  return distance;
}

// Median of 5 pings. A median (not a mean) is what we want here: a single
// spurious echo off the tank wall is discarded outright rather than dragging
// the average.
float readDistanceCm(float tempC) {
  const int N = 5;
  float samples[N];
  int   valid = 0;

  for (int i = 0; i < N; i++) {
    float d = pingOnce(tempC);
    if (!isnan(d)) samples[valid++] = d;
    delay(35);   // let the previous burst decay before the next ping
  }
  if (valid < 3) return NAN;   // majority of pings failed: report a fault

  for (int i = 1; i < valid; i++) {
    float k = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > k) { samples[j + 1] = samples[j]; j--; }
    samples[j + 1] = k;
  }
  return samples[valid / 2];
}

FeedingData readFeedingLevel(float tempC) {
  FeedingData data;
  data.distance = readDistanceCm(tempC);

  if (isnan(data.distance)) {
    data.valid       = false;
    data.waterHeight = isnan(lastGoodWaterCm)  ? 0.0 : lastGoodWaterCm;
    data.percentage  = isnan(lastGoodLevelPct) ? 0.0 : lastGoodLevelPct;
    data.remainingMl = 0.0;
    return data;
  }

  // Geometry: the sensor sits sensorToTopCm above the rim, the tank is
  // containerHeightCm deep. Echo distance therefore runs from sensorToTopCm
  // (full) to sensorToTopCm + containerHeightCm (empty).
  float waterHeight = containerHeightCm - (data.distance - sensorToTopCm);
  waterHeight = constrain(waterHeight, 0.0f, containerHeightCm);

  float radius = containerDiameterCm / 2.0;
  data.waterHeight = waterHeight;
  data.percentage  = constrain((waterHeight / containerHeightCm) * 100.0f, 0.0f, 100.0f);
  data.remainingMl = 3.14159265 * radius * radius * waterHeight;  // 1 cm³ = 1 ml
  data.valid       = true;

  lastGoodWaterCm  = data.waterHeight;
  lastGoodLevelPct = data.percentage;
  return data;
}

// ═══════════════════════════════════════════════════════════
// HX711 — HIVE WEIGHT
// ═══════════════════════════════════════════════════════════

// Returns kilograms, or NAN if the cell didn't answer. Returning NAN rather
// than 0.0 matters: 0.0 flows into the production maths as "the hive lost all
// its weight" and fires a bogus harvest event.
float readWeight() {
  if (!scale.wait_ready_timeout(400)) {
    Serial.println("[HX711] Not ready — holding last good weight");
    return NAN;
  }
  float w = scale.get_units(5);
  if (isnan(w) || w < -50.0 || w > 500.0) return NAN;   // clearly bogus
  return w;
}

void applyTare() {
  if (!scale.wait_ready_timeout(1000)) {
    // Clear the request even on failure. Leaving it set meant the 700 ms
    // command poll re-entered here forever, each attempt blocking a second and
    // appending a fresh /alerts node — the exact flood this version set out to
    // remove. Throttled too, so a dead cell reports once per cooldown.
    Firebase.RTDB.setBool(&fbdo, "/commands/tare_scale", false);
    pushLevelAlert("tare_fault", "sensor_fault", "warning",
                   "Tare failed: load cell did not respond.");
    return;
  }
  scale.tare(20);
  tareOffset = scale.get_offset();
  Firebase.RTDB.setInt(&fbdo, "/calibration/tare_offset", (int)tareOffset);
  Firebase.RTDB.setBool(&fbdo, "/commands/tare_scale", false);
  pushEvent("calibration", "info", "Scale tared. New zero saved.");
}

// ═══════════════════════════════════════════════════════════
// HEALTH SCORE
// ═══════════════════════════════════════════════════════════

int calcHealthScore(float temp, float hum, float weight, const String &sound) {
  int score = 100;
  if (!isnan(temp) && (temp > 36.0 || temp < 32.0)) score -= 20;
  if (!isnan(hum)  && (hum  > 75.0 || hum  < 40.0)) score -= 15;
  // "Unknown" means the audio board hasn't reported yet — that's a gap in our
  // knowledge, not evidence of a sick hive, so it must not cost 30 points.
  if (sound != "Normal" && sound != "Unknown")      score -= 30;
  if (hornetDetected)                               score -= 20;
  if (!isnan(weight) && weight < 5.0)               score -= 15;
  return constrain(score, 0, 100);
}

// ═══════════════════════════════════════════════════════════
// SOUND LABEL NORMALISATION
// ═══════════════════════════════════════════════════════════
// The Edge Impulse model emits its own class names ("Active", "Queen_Loss").
// The app and the health score speak "Normal" / "Swarming" / "Queen Loss".
// Translate once, here, so a model retrain only needs this table updated.

String normaliseSound(const String &raw) {
  String s = raw;
  s.trim();
  String lower = s;
  lower.toLowerCase();

  if (lower == "active" || lower == "normal" || lower == "healthy") return "Normal";
  if (lower == "queen_loss" || lower == "queenloss" || lower == "queen loss") return "Queen Loss";
  if (lower == "swarm" || lower == "swarming") return "Swarming";
  if (lower.length() == 0) return "Unknown";
  return s;
}

// ═══════════════════════════════════════════════════════════
// PRODUCTION / HONEY ESTIMATION
// ═══════════════════════════════════════════════════════════

float estimateHoney(float weight) {
  if (isnan(weight)) return 0.0;
  float gain = weight - baselineWeight;
  if (gain <= 0) return 0.0;
  return gain * honeyFraction;
}

// Close out a finished day into /production/history so the app can chart real
// numbers instead of multiplying today's figure by made-up factors.
void rollOverDay(const String &finishedDate, float finishedGain, float endWeight) {
  FirebaseJson day;
  day.set("gain",   finishedGain);
  day.set("weight", endWeight);
  day.set("honey",  finishedGain > 0 ? finishedGain * honeyFraction : 0.0);
  String dayPath = "/production/history/" + finishedDate;
  Firebase.RTDB.setJSON(&fbdo, dayPath, &day);
  Firebase.RTDB.setFloat(&fbdo, "/production/yesterday_production", finishedGain);
  Firebase.RTDB.setFloat(&fbdo, "/production/yesterday_weight",     endWeight);
}

void updateProduction(float weight) {
  if (isnan(weight)) return;

  String today = todayKey();
  int    week  = isoWeek();
  int    month = monthNumber();

  // First reading ever, or the date changed while we were running.
  if (todayDate.length() == 0) {
    todayDate        = today;
    todayStartWeight = weight;
    currentWeek      = week;
    currentMonth     = month;
  } else if (today != todayDate) {
    float finishedGain = isnan(todayStartWeight) ? 0.0 : (weight - todayStartWeight);
    if (finishedGain < 0) finishedGain = 0;
    rollOverDay(todayDate, finishedGain, weight);

    if (week  != currentWeek)  { weeklyProduction  = 0.0; currentWeek  = week;  }
    if (month != currentMonth) { monthlyProduction = 0.0; currentMonth = month; }

    todayDate        = today;
    todayStartWeight = weight;
    Firebase.RTDB.setString(&fbdo, "/production/today_date",        todayDate);
    Firebase.RTDB.setFloat(&fbdo,  "/production/today_start_weight", todayStartWeight);
  }

  float todayGain = isnan(todayStartWeight) ? 0.0 : (weight - todayStartWeight);
  if (todayGain < 0) todayGain = 0;

  // Harvest detection: a sudden drop between two samples is honey leaving the
  // hive, not the colony shrinking. Only counted while the door is open, so
  // an inspection or a knock doesn't register as a harvest.
  if (!isnan(prevWeightSample)) {
    float delta = weight - prevWeightSample;
    if (delta <= -harvestDropKg && doorOpen) {
      float harvested = -delta;
      totalHarvested += harvested;
      Firebase.RTDB.setFloat(&fbdo,  "/production/last_harvest_kg",   harvested);
      Firebase.RTDB.setString(&fbdo, "/production/last_harvest_time", nowStamp());
      Firebase.RTDB.setFloat(&fbdo,  "/production/total_harvested",   totalHarvested);
      // The removed honey is gone from the hive, so the baseline follows it
      // down — otherwise estimated_honey would stay high after a harvest.
      baselineWeight = weight;
      Firebase.RTDB.setFloat(&fbdo, "/production/baseline_weight", baselineWeight);
      pushEvent("harvest", "info",
                "Harvest recorded: " + String(harvested, 2) + " kg removed.");
    } else if (delta >= 0.10) {
      weeklyProduction  += delta;
      monthlyProduction += delta;
    }
  }
  prevWeightSample = weight;

  FirebaseJson prod;
  prod.set("hive_weight",        weight);
  prod.set("today_weight",       weight);          // kept for older app builds
  prod.set("baseline_weight",    baselineWeight);
  prod.set("estimated_honey",    estimateHoney(weight));
  prod.set("today_production",   todayGain);
  prod.set("today_start_weight", isnan(todayStartWeight) ? weight : todayStartWeight);
  prod.set("today_date",         todayDate);
  prod.set("weekly_production",  weeklyProduction);
  prod.set("monthly_production", monthlyProduction);
  prod.set("total_harvested",    totalHarvested);
  prod.set("updated_at",         nowStamp());
  Firebase.RTDB.updateNode(&fbdo, "/production", &prod);
}

// ═══════════════════════════════════════════════════════════
// FIREBASE — UPLOAD SENSOR DATA
// ═══════════════════════════════════════════════════════════

void uploadSensorData() {
  // This function can occupy the loop for a long time: the ultrasonic burst
  // takes ~300 ms, the load cell up to 400 ms, and each Firebase write can
  // block for up to config.timeout.serverResponse (10 s) on a bad link. A pump
  // deadline that falls inside that window would not be serviced until the
  // whole upload finished, so the relay is checked between the slow steps.
  // FEED_MAX_MS is a deadline, not a hardware limit — nothing else stops a
  // stuck pump.
  FeedingData feeding = readFeedingLevel(receivedTemp);
  serviceActuators();

  float weight = readWeight();
  serviceActuators();
  if (isnan(weight)) {
    if (!isnan(lastGoodWeight)) weight = lastGoodWeight;
    pushLevelAlert("scale_fault", "sensor_fault", "warning",
                   "Load cell not responding — showing last known weight.");
  } else {
    lastGoodWeight = weight;
    resetAlert("scale_fault");
  }

  int health = calcHealthScore(receivedTemp, receivedHumidity, weight, receivedSound);

  // Peer liveness. Reported so the app can show which node went quiet instead
  // of silently displaying stale numbers as if they were live.
  unsigned long now = millis();
  audioOnline = (lastAudioPacket != 0) && (now - lastAudioPacket < NODE_TIMEOUT_MS);
  camOnline   = (lastCamPacket   != 0) && (now - lastCamPacket   < NODE_TIMEOUT_MS);

  FirebaseJson hive;
  if (!isnan(receivedTemp))     hive.set("temperature", receivedTemp);
  if (!isnan(receivedHumidity)) hive.set("humidity",    receivedHumidity);
  if (!isnan(weight))           hive.set("weight",      weight);
  hive.set("water_level",       feeding.percentage);
  hive.set("water_height_cm",   feeding.waterHeight);
  hive.set("water_remaining_ml", feeding.remainingMl);
  hive.set("water_sensor_ok",   feeding.valid);
  hive.set("sound_result",      receivedSound);
  hive.set("sound_confidence",  receivedConfidence);
  hive.set("health_score",      health);
  hive.set("door_open",         doorOpen);
  hive.set("locked",            lockState);
  hive.set("entrance_status",   entranceState);
  hive.set("audio_node_online", audioOnline);
  hive.set("camera_node_online", camOnline);
  hive.set("last_sync",         nowStamp());
  hive.set("last_sync_epoch",   (int)nowEpoch());
  Firebase.RTDB.updateNode(&fbdo, "/hive_status", &hive);
  serviceActuators();

  updateProduction(weight);
  serviceActuators();

  FirebaseJson ai;
  ai.set("sound_result",  receivedSound);
  ai.set("confidence",    receivedConfidence);
  ai.set("last_analysis", nowStamp());
  ai.set("node_online",   audioOnline);
  Firebase.RTDB.updateNode(&fbdo, "/ai_status", &ai);

  // ── Hornet / camera panel ─────────────────────────────
  // Keeps the app's detection card fresh even between hornet events, and
  // rebuilds the last-detection time from the camera's reported age so a
  // controller reboot doesn't erase it.
  FirebaseJson hornet;
  hornet.set("detected",        hornetDetected);
  hornet.set("detection_count", hornetCount);
  hornet.set("camera_online",   camOnline);
  if (camSecondsSinceDetection != CAM_NEVER_DETECTED) {
    time_t detectedAt = nowEpoch() - (time_t)camSecondsSinceDetection;
    hornet.set("last_detection",       stampFromEpoch(detectedAt));
    hornet.set("last_detection_epoch", (int)detectedAt);
    hornet.set("last_confirmed",       true);
  } else {
    hornet.set("last_detection", "No detection yet");
    hornet.set("last_confirmed", false);
  }
  Firebase.RTDB.updateNode(&fbdo, "/hornet_detection", &hornet);

  FirebaseJson cam;
  cam.set("online",     camOnline);
  cam.set("last_seen",  camOnline ? nowStamp() : String("offline"));
  cam.set("stream_url", CAMERA_STREAM_URL);
  cam.set("status_url", CAMERA_STATUS_URL);
  Firebase.RTDB.updateNode(&fbdo, "/camera", &cam);
  serviceActuators();

  // ── Level-triggered warnings (throttled) ──────────────
  if (!feeding.valid) {
    pushLevelAlert("water_sensor", "sensor_fault", "warning",
                   "Ultrasonic sensor not responding — feeding level unknown.");
  } else {
    resetAlert("water_sensor");
    if (feeding.percentage < 10.0) {
      pushLevelAlert("water_critical", "water", "critical",
                     "Feeding level critical: " + String(feeding.percentage, 0) +
                     "% (" + String(feeding.remainingMl, 0) + " ml left). Refill now.");
    } else if (feeding.percentage < 20.0) {
      pushLevelAlert("water_low", "water", "warning",
                     "Feeding level low: " + String(feeding.percentage, 0) +
                     "% (" + String(feeding.remainingMl, 0) + " ml left).");
    } else {
      resetAlert("water_critical");
      resetAlert("water_low");
    }
  }

  if (!isnan(receivedTemp)) {
    if (receivedTemp > 36.0) {
      pushLevelAlert("temp_high", "temperature", "critical",
                     "Hive too hot: " + String(receivedTemp, 1) + " C.");
    } else if (receivedTemp < 32.0) {
      pushLevelAlert("temp_low", "temperature", "warning",
                     "Hive too cold: " + String(receivedTemp, 1) + " C.");
    } else {
      resetAlert("temp_high");
      resetAlert("temp_low");
    }
  }

  if (!isnan(receivedHumidity)) {
    if (receivedHumidity > 75.0) {
      pushLevelAlert("hum_high", "humidity", "warning",
                     "Humidity high: " + String(receivedHumidity, 0) + "%.");
    } else if (receivedHumidity < 40.0) {
      pushLevelAlert("hum_low", "humidity", "warning",
                     "Humidity low: " + String(receivedHumidity, 0) + "%.");
    } else {
      resetAlert("hum_high");
      resetAlert("hum_low");
    }
  }

  if (!audioOnline) {
    pushLevelAlert("audio_offline", "node_offline", "warning",
                   "Audio/DHT board has stopped reporting.");
  } else {
    resetAlert("audio_offline");
  }
  if (!camOnline) {
    pushLevelAlert("cam_offline", "node_offline", "warning",
                   "Camera board has stopped reporting.");
  } else {
    resetAlert("cam_offline");
  }

  Serial.printf("[Firebase] T:%.1f H:%.1f W:%.2fkg Honey:%.2fkg Feed:%.0f%% Lock:%s Door:%s\n",
                receivedTemp, receivedHumidity, weight, estimateHoney(weight),
                feeding.percentage, lockState ? "LOCKED" : "UNLOCKED",
                doorOpen ? "OPEN" : "CLOSED");
}

// ═══════════════════════════════════════════════════════════
// FIREBASE — READ & EXECUTE COMMANDS
// ═══════════════════════════════════════════════════════════

// One GET for the whole /commands node instead of one per key. The previous
// version issued three reads every 200 ms, which kept the radio permanently
// busy and made each command wait behind the others' round-trips.
void readAndExecuteCommands() {
  if (!Firebase.RTDB.getJSON(&fbdo, "/commands")) return;

  FirebaseJson *cmd = fbdo.jsonObjectPtr();
  if (cmd == nullptr) return;
  FirebaseJsonData r;

  // Honey collection: true = open door + smoke, false = close door.
  if (cmd->get(r, "collect_honey")) {
    bool val = r.boolValue;
    if (val && !lastCollectHoney) {
      lastCollectHoney = true;
      collectHoney();
    } else if (!val && lastCollectHoney) {
      lastCollectHoney = false;
      stopHoneyCollection();
    }
  }

  // Feeding: app writes a volume in ml; we run once and reset it to 0.
  // Ignored while a feed is already in flight so a slow Firebase round-trip
  // can't start a second overlapping pour.
  if (feedOffAt == 0 && cmd->get(r, "feed_ml")) {
    int ml = r.intValue;
    if (ml > 0) startFeeding(ml);
  }

  // Entrance open / narrow.
  if (cmd->get(r, "entrance")) {
    String val = r.stringValue;
    if (val.length() > 0 && val != entranceState) {
      // A manual command takes ownership of the gate: cancel the automatic
      // reopen so the system does not move it back under the beekeeper.
      entranceNarrowedByHornet = false;
      hornetClearedAt = 0;
      if (val == "narrow") narrowEntrance("app request");
      else                 openEntrance("app request");
    }
  }

  // Re-zero the scale on request (used after the hive is set up or moved).
  if (cmd->get(r, "tare_scale") && r.boolValue) {
    applyTare();
  }

  // Adopt the current weight as the "no harvestable honey" reference.
  if (cmd->get(r, "set_baseline") && r.boolValue) {
    if (!isnan(lastGoodWeight)) {
      baselineWeight = lastGoodWeight;
      Firebase.RTDB.setFloat(&fbdo, "/production/baseline_weight", baselineWeight);
      pushEvent("calibration", "info",
                "Honey baseline set to " + String(baselineWeight, 2) + " kg.");
    }
    Firebase.RTDB.setBool(&fbdo, "/commands/set_baseline", false);
  }
}

// ═══════════════════════════════════════════════════════════
// RFID — ANY CARD TOGGLES THE HIVE LOCK (GPIO19)
// ═══════════════════════════════════════════════════════════

void checkRFID() {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String cardUID = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) cardUID += "0";
    cardUID += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) cardUID += " ";
  }
  cardUID.toUpperCase();
  Serial.printf("[RFID] Card: %s\n", cardUID.c_str());

  String timeStr = nowStamp();

  if (lockState) unlockHive();
  else           lockHive();

  FirebaseJson sec;
  sec.set("rfid_status", "granted");
  sec.set("last_card",   cardUID);
  sec.set("last_time",   timeStr);
  sec.set("locked",      lockState);
  Firebase.RTDB.updateNode(&fbdo, "/security", &sec);

  FirebaseJson logEntry;
  logEntry.set("card_id",   cardUID);
  logEntry.set("access",    "granted");
  logEntry.set("action",    lockState ? "locked" : "unlocked");
  logEntry.set("timestamp", timeStr);
  logEntry.set("epoch",     (int)nowEpoch());
  char logKey[32];
  snprintf(logKey, sizeof(logKey), "log_%010lu", (unsigned long)nowEpoch());
  String logPath = "/rfid_logs/" + String(logKey);
  Firebase.RTDB.setJSON(&fbdo, logPath, &logEntry);

  pushEvent("rfid", "info",
            lockState ? "Hive locked with card " + cardUID
                      : "Hive unlocked with card " + cardUID);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ═══════════════════════════════════════════════════════════
// SOUND RESULT — consumed in loop(), never in the RX callback
// ═══════════════════════════════════════════════════════════

void servicePendingSound() {
  if (!pendingSoundUpdate) return;
  pendingSoundUpdate = false;

  bool fresh = pendingSoundFresh;
  pendingSoundFresh = false;

  // Copy out before doing anything slow: another packet may land mid-way.
  char label[21];
  memcpy(label, pendingSoundLabel, sizeof(label));
  label[20] = '\0';
  int conf = pendingSoundConf;

  String sound = normaliseSound(String(label));

  // Only a fresh classification updates the verdict; the periodic environment
  // ticks carry the same label we already logged.
  if (!fresh) {
    if (receivedSound == "Unknown") receivedSound = sound;
    return;
  }

  bool changed = (sound != receivedSound);
  receivedSound = sound;

  FirebaseJson h;
  h.set("time",       nowClock());
  h.set("result",     sound);
  h.set("confidence", conf);
  h.set("epoch",      (int)nowEpoch());
  char key[24];
  snprintf(key, sizeof(key), "h_%010lu", (unsigned long)nowEpoch());
  String historyPath = "/ai_history/" + String(key);
  Firebase.RTDB.setJSON(&fbdo, historyPath, &h);

  if (sound == "Swarming") {
    pushEvent("sound", "critical",
              "Swarming detected (" + String(conf) +
              "% confidence). The colony may leave.");
  } else if (sound == "Queen Loss") {
    pushEvent("sound", "critical",
              "Queen loss detected (" + String(conf) +
              "% confidence). Inspect the hive.");
  } else if (changed && sound == "Normal") {
    pushEvent("sound", "info", "Hive sound back to normal.");
  }
}

// ═══════════════════════════════════════════════════════════
// HORNET RESPONSE
// ═══════════════════════════════════════════════════════════

// Edge-triggered so we narrow once per event rather than every 30 s, and so
// the "all clear" is reported exactly once too.
void handleHornet() {
  if (hornetDetected == lastHornetState) return;

  if (hornetDetected) {
    Serial.println("[HORNET] Detected — narrowing entrance");
    narrowEntrance("hornet detected");
    entranceNarrowedByHornet = true;
    hornetClearedAt = 0;   // cancel any pending reopen

    FirebaseJson h;
    h.set("detected",             true);
    h.set("last_confirmed",       true);
    h.set("detection_count",      hornetCount);
    h.set("last_detection",       nowStamp());
    h.set("last_detection_epoch", (int)nowEpoch());
    Firebase.RTDB.updateNode(&fbdo, "/hornet_detection", &h);

    pushEvent("hornet", "critical",
              "Hornet detected (" + String(hornetCount) +
              " in frame). Entrance narrowed automatically.");
  } else {
    Firebase.RTDB.setBool(&fbdo, "/hornet_detection/detected", false);
    // Start the reopen countdown. 0 is the "no timer" sentinel, so nudge it.
    hornetClearedAt = millis();
    if (hornetClearedAt == 0) hornetClearedAt = 1;
    pushEvent("hornet_clear", "info",
              "Hornet no longer visible at the entrance.");
  }
  lastHornetState = hornetDetected;
}

// Reopen the entrance once the hornets have stayed away long enough. Only
// undoes the system's own narrowing — a gate the beekeeper narrowed by hand
// stays where they put it.
void serviceEntranceReopen() {
  if (hornetClearedAt == 0 || HORNET_REOPEN_DELAY_MS == 0) return;

  if (!entranceNarrowedByHornet || entranceState != "narrow") {
    hornetClearedAt = 0;
    return;
  }
  if ((long)(millis() - (hornetClearedAt + HORNET_REOPEN_DELAY_MS)) < 0) return;

  hornetClearedAt = 0;
  entranceNarrowedByHornet = false;
  openEntrance("no hornets for 10 minutes");
}

// ═══════════════════════════════════════════════════════════
// ESP-NOW CALLBACK  (keep it light: store values, act in loop())
// ═══════════════════════════════════════════════════════════

void handleEspNowPacket(const uint8_t *data, int len) {
  if (len < 1) return;

  switch (data[0]) {
    case MSG_SENSOR: {
      if (len != sizeof(SensorData)) return;
      SensorData in;
      memcpy(&in, data, sizeof(in));

      // Scalars only — no String, no Firebase, no getLocalTime(). See the
      // comment on pendingSoundUpdate.
      receivedTemp       = in.temperature;
      receivedHumidity   = in.humidity;
      receivedConfidence = in.confidence;
      lastAudioPacket    = millis();

      memcpy(pendingSoundLabel, in.soundResult, 20);
      pendingSoundLabel[20] = '\0';
      pendingSoundConf = in.confidence;
      if (in.freshAnalysis) pendingSoundFresh = true;
      // Set last: loop() treats this flag as "the fields above are ready".
      pendingSoundUpdate = true;
      break;
    }

    case MSG_CAM: {
      if (len != sizeof(CamData)) return;
      CamData in;
      memcpy(&in, data, sizeof(in));
      hornetDetected = (in.hornetDetected != 0);
      hornetCount    = in.count;
      camSecondsSinceDetection = in.secondsSinceLast;
      lastCamPacket  = millis();
      break;
    }

    default:
      break;
  }
}

// The callback signature changed in ESP32 Arduino core 3.x. This compiles on
// both; ESP_ARDUINO_VERSION_MAJOR is undefined (so 0) on older cores.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataReceived(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  handleEspNowPacket(data, len);
}
#else
void onDataReceived(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
  handleEspNowPacket(data, len);
}
#endif

// ═══════════════════════════════════════════════════════════
// CALIBRATION LOAD
// ═══════════════════════════════════════════════════════════

// Read tank geometry and scale calibration back from Firebase so they survive
// a reboot and can be re-measured from the app without reflashing.
void loadCalibration() {
  if (Firebase.RTDB.getFloat(&fbdo, "/calibration/scale_factor")) {
    float v = fbdo.floatData();
    if (v != 0.0) scaleFactor = v;
  }
  if (Firebase.RTDB.getInt(&fbdo, "/calibration/tare_offset")) {
    tareOffset = fbdo.intData();
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/calibration/container_height_cm")) {
    float v = fbdo.floatData();
    if (v > 0) containerHeightCm = v;
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/calibration/sensor_to_top_cm")) {
    float v = fbdo.floatData();
    if (v > 0) sensorToTopCm = v;
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/calibration/container_diameter_cm")) {
    float v = fbdo.floatData();
    if (v > 0) containerDiameterCm = v;
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/calibration/honey_fraction")) {
    float v = fbdo.floatData();
    if (v > 0 && v <= 1.0) honeyFraction = v;
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/production/baseline_weight")) {
    float v = fbdo.floatData();
    if (v > 0) baselineWeight = v;   // a legacy 0 must not overwrite the 4kg default
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/production/total_harvested")) {
    totalHarvested = fbdo.floatData();
  }
  if (Firebase.RTDB.getString(&fbdo, "/production/today_date")) {
    todayDate = fbdo.stringData();
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/production/today_start_weight")) {
    float v = fbdo.floatData();
    if (v > 0) todayStartWeight = v;
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/production/weekly_production")) {
    weeklyProduction = fbdo.floatData();
  }
  if (Firebase.RTDB.getFloat(&fbdo, "/production/monthly_production")) {
    monthlyProduction = fbdo.floatData();
  }

  scale.set_scale(scaleFactor);
  if (tareOffset != 0) scale.set_offset(tareOffset);

  Serial.printf("[CAL] scale=%.1f tare=%ld tank=%.1fcm gap=%.1fcm dia=%.1fcm honey=%.2f baseline=%.2fkg\n",
                scaleFactor, tareOffset, containerHeightCm, sensorToTopCm,
                containerDiameterCm, honeyFraction, baselineWeight);
}

// ═══════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  // ── SAFE INITIAL STATE ──────────────────────────────────
  // Pumps are ACTIVE LOW: drive the latch HIGH (=OFF) BEFORE switching the pin
  // to OUTPUT so the relay never sees a brief LOW glitch at boot.
  digitalWrite(RELAY_FEED_PIN,  HIGH);
  digitalWrite(RELAY_SMOKE_PIN, HIGH);
  pinMode(RELAY_FEED_PIN,  OUTPUT);
  pinMode(RELAY_SMOKE_PIN, OUTPUT);
  digitalWrite(RELAY_FEED_PIN,  HIGH); // OFF
  digitalWrite(RELAY_SMOKE_PIN, HIGH); // OFF

  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);

  // Servos: attach and immediately drive to a known home position.
  servoDoor.attach(SERVO_DOOR_PIN);
  servoLock.attach(SERVO_LOCK_PIN);
  servoEntrance.attach(SERVO_ENTRANCE_PIN);
  servoDoor.write(DOOR_CLOSED_ANGLE);
  servoLock.write(LOCK_LOCKED_ANGLE);
  servoEntrance.write(ENTRANCE_OPEN_ANGLE);
  doorOpen      = false;
  lockState     = true;
  entranceState = "open";

  // HX711 — note there is NO tare() here any more. Taring on every boot zeroed
  // the scale with the hive already sitting on it, so the app's weight reset to
  // 0 on every power cycle. The saved offset is restored in loadCalibration().
  scale.begin(HX711_DT, HX711_SCK);
  scale.set_scale(scaleFactor);
  Serial.println("[HX711] Scale ready");

  // RFID (SPI: SCK=14, MISO=27, MOSI=25, SS=5)
  SPI.begin(14, 27, 25, 5);
  rfid.PCD_Init();
  Serial.println("[RFID] Ready");

  // WiFi
  WiFi.mode(WIFI_STA);   // STA only — no SoftAP is used, and STA is the stable mode for ESP-NOW + station
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
  Serial.println("[ESP-NOW] MAC: " + WiFi.macAddress());
  // Print the channel: if the other boards report a different number here,
  // ESP-NOW between them will not work no matter what the MACs say.
  Serial.printf("[WiFi] Channel: %d  (camera + audio boards MUST match)\n",
                WiFi.channel());

  syncTime();
  Serial.println("[TIME] " + nowStamp());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed!");
  } else {
    esp_now_register_recv_cb(onDataReceived);
    Serial.println("[ESP-NOW] Ready");
  }

  // Firebase
  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  config.cert.data    = nullptr;
  config.cert.file    = "";
  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);
  config.timeout.serverResponse = 10 * 1000;
  // loadCalibration() reads back saved values, so wait for the token exchange
  // to finish first — otherwise every read fails and the hard-coded defaults
  // silently overwrite the beekeeper's calibration.
  for (int i = 0; i < 100 && !Firebase.ready(); i++) delay(100);
  Serial.println("[Firebase] Connected");

  loadCalibration();

  // Publish a clean initial state so the app reflects reality on first launch.
  FirebaseJson cmd;
  cmd.set("collect_honey", false);
  cmd.set("feed_ml",       0);
  cmd.set("smoke_pump",    false);
  cmd.set("pump",          false);
  cmd.set("entrance",      "open");
  cmd.set("tare_scale",    false);
  cmd.set("set_baseline",  false);
  Firebase.RTDB.updateNode(&fbdo, "/commands", &cmd);

  FirebaseJson sec;
  sec.set("locked",      true);
  sec.set("rfid_status", "Waiting for Card");
  Firebase.RTDB.updateNode(&fbdo, "/security", &sec);

  // Tell the app where to find the camera, so the stream URL lives in one
  // place and can be changed without rebuilding the app.
  FirebaseJson cam;
  cam.set("stream_url", CAMERA_STREAM_URL);
  cam.set("status_url", CAMERA_STATUS_URL);
  Firebase.RTDB.updateNode(&fbdo, "/camera", &cam);

  FirebaseJson cal;
  cal.set("scale_factor",         scaleFactor);
  cal.set("tare_offset",          (int)tareOffset);
  cal.set("container_height_cm",  containerHeightCm);
  cal.set("sensor_to_top_cm",     sensorToTopCm);
  cal.set("container_diameter_cm", containerDiameterCm);
  cal.set("honey_fraction",       honeyFraction);
  Firebase.RTDB.updateNode(&fbdo, "/calibration", &cal);

  // Publish the baseline so estimated_honey is never blank in the app.
  Firebase.RTDB.setFloat(&fbdo, "/production/baseline_weight", baselineWeight);

  Firebase.RTDB.setBool(&fbdo, "/hive_status/door_open", false);
  publishEntrance();

  pushEvent("system", "info", "Main controller started and connected.");
  Serial.println("[Firebase] Initial state published");
}

// ═══════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
  // Pump deadlines are checked before the Firebase gate: a dropped connection
  // must never leave a pump latched on.
  serviceActuators();

  if (!Firebase.ready()) {
    delay(50);
    return;
  }

  unsigned long now = millis();

  if (now - lastSensorUpload >= UPLOAD_INTERVAL) {
    lastSensorUpload = now;
    uploadSensorData();
  }

  if (now - lastCommandPoll >= COMMAND_INTERVAL) {
    lastCommandPoll = now;
    readAndExecuteCommands();
  }

  servicePendingSound();
  handleHornet();
  serviceEntranceReopen();
  checkRFID();

  delay(20);
}
