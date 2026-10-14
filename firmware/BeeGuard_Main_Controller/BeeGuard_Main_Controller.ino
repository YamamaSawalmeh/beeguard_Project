#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include <Firebase_ESP_Client.h>
#include <ESP32Servo.h>
#include <HX711.h>
#include <SPI.h>
#include <MFRC522.h>

#define WIFI_SSID     "MSI"
#define WIFI_PASSWORD "123456789"

#define API_KEY      "AIzaSyCqzB6EBkAenCuURUXDoji7N67xjDTb8SI"
#define DATABASE_URL "https://beeguard-smartbee-default-rtdb.europe-west1.firebasedatabase.app"

#define CAMERA_STREAM_URL   "http://192.168.137.150/"
#define CAMERA_STATUS_URL   "http://192.168.137.150:81/status"

#define NTP_SERVER_1    "pool.ntp.org"
#define NTP_SERVER_2    "time.google.com"
#define GMT_OFFSET_SEC  (3 * 3600)
#define DST_OFFSET_SEC  0

#define SERVO_DOOR_PIN      18
#define SERVO_LOCK_PIN      19
#define SERVO_ENTRANCE_PIN  13

#define RELAY_FEED_PIN      33
#define RELAY_SMOKE_PIN     32

#define TRIG_PIN            26
#define ECHO_PIN            16

#define HX711_DT            17
#define HX711_SCK           15

#define RFID_SS_PIN         5
#define RFID_RST_PIN        4

#define DOOR_CLOSED_ANGLE     0
#define DOOR_OPEN_ANGLE       180
#define LOCK_LOCKED_ANGLE     90
#define LOCK_UNLOCKED_ANGLE   180
#define ENTRANCE_OPEN_ANGLE   180
#define ENTRANCE_NARROW_ANGLE 0

#define SMOKE_DURATION_MS   4000UL
#define FEED_MS_PER_ML      120UL
#define FEED_MAX_MS         30000UL

const unsigned long UPLOAD_INTERVAL   = 30000;
const unsigned long COMMAND_INTERVAL  = 700;
const unsigned long NODE_TIMEOUT_MS   = 150000;
const unsigned long ALERT_COOLDOWN_MS = 900000;

const unsigned long HORNET_REOPEN_DELAY_MS = 10UL * 60UL * 1000UL;

float sensorToTopCm    = 4.0;
float containerHeightCm = 3.5;
float containerDiameterCm = 9.0;

float scaleFactor = -7050.0;
long  tareOffset  = 0;

float honeyFraction   = 0.85;
float harvestDropKg   = 0.50;
float baselineWeight  = 4.0;

#define MSG_SENSOR 1
#define MSG_CAM    2

typedef struct __attribute__((packed)) {
  uint8_t msgType;
  float   temperature;
  float   humidity;
  char    soundResult[20];
  uint8_t confidence;
  uint8_t freshAnalysis;
} SensorData;

typedef struct __attribute__((packed)) {
  uint8_t  msgType;
  uint8_t  hornetDetected;
  uint8_t  count;
  uint32_t secondsSinceLast;
} CamData;

struct FeedingData {
  float distance;
  float waterHeight;
  float percentage;
  float remainingMl;
  bool  valid;
};

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;
Servo          servoDoor;
Servo          servoLock;
Servo          servoEntrance;
HX711          scale;
MFRC522        rfid(RFID_SS_PIN, RFID_RST_PIN);

unsigned long lastSensorUpload = 0;
unsigned long lastCommandPoll  = 0;

bool   doorOpen         = false;
bool   lockState        = true;
String entranceState    = "open";
bool   lastCollectHoney = false;

unsigned long smokeOffAt = 0;
unsigned long feedOffAt  = 0;
int           feedingMl  = 0;

static inline unsigned long deadlineAt(unsigned long durationMs) {
  unsigned long d = millis() + durationMs;
  return (d == 0) ? 1UL : d;
}

float receivedTemp       = NAN;
float receivedHumidity   = NAN;
String receivedSound     = "Unknown";
int   receivedConfidence = 0;
bool  hornetDetected     = false;
int   hornetCount        = 0;
bool  lastHornetState    = false;

bool          entranceNarrowedByHornet = false;
unsigned long hornetClearedAt          = 0;

#define CAM_NEVER_DETECTED 0xFFFFFFFFUL
uint32_t camSecondsSinceDetection = CAM_NEVER_DETECTED;

float lastGoodWeight     = NAN;
float lastGoodLevelPct   = NAN;
float lastGoodWaterCm    = NAN;

unsigned long lastAudioPacket = 0;
unsigned long lastCamPacket   = 0;
bool audioOnline = false;
bool camOnline   = false;

volatile bool    pendingSoundUpdate = false;
volatile bool    pendingSoundFresh  = false;
volatile uint8_t pendingSoundConf   = 0;
char             pendingSoundLabel[21] = "Unknown";

String todayDate         = "";
float  todayStartWeight  = NAN;
float  weeklyProduction  = 0.0;
float  monthlyProduction = 0.0;
int    currentWeek       = -1;
int    currentMonth      = -1;
float  totalHarvested    = 0.0;
float  prevWeightSample  = NAN;

bool timeSynced = false;

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

String nowStamp() {
  struct tm t;
  if (!getLocalTime(&t, 100)) {
    return "uptime " + String(millis() / 1000) + "s";
  }
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String nowClock() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return "--:--";
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

String stampFromEpoch(time_t e) {
  struct tm t;
  if (!timeSynced || e <= 0) return "Unknown";
  localtime_r(&e, &t);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  return String(buf);
}

String todayKey() {
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

uint16_t alertSeq = 0;

String alertKey() {

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
  return true;
}

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
  json.set("severity",  severity);
  json.set("message",   message);
  json.set("timestamp", nowStamp());
  json.set("epoch",     (int)nowEpoch());
  json.set("read",      false);

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

void sweepServo(Servo &s, int fromAngle, int toAngle) {
  int step = (toAngle >= fromAngle) ? 5 : -5;
  for (int pos = fromAngle; (step > 0) ? (pos <= toAngle) : (pos >= toAngle); pos += step) {
    s.write(pos);
    delay(15);
  }
  s.write(toAngle);
}

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

void startSmoke() {
  digitalWrite(RELAY_SMOKE_PIN, LOW);
  smokeOffAt = deadlineAt(SMOKE_DURATION_MS);
  Firebase.RTDB.setBool(&fbdo, "/commands/smoke_pump", true);
  pushEvent("smoke", "info", "Smoke pump running for 4 s.");
}

void stopSmoke(bool announce) {
  digitalWrite(RELAY_SMOKE_PIN, HIGH);
  smokeOffAt = 0;
  Firebase.RTDB.setBool(&fbdo, "/commands/smoke_pump", false);
  if (announce) pushEvent("smoke", "info", "Smoke pump stopped.");
}

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
  stopSmoke(false);
  sweepServo(servoDoor, DOOR_OPEN_ANGLE, DOOR_CLOSED_ANGLE);
  doorOpen = false;
  Firebase.RTDB.setBool(&fbdo, "/hive_status/door_open", false);
  pushEvent("door_closed", "info", "Honey door closed.");
}

void startFeeding(int ml) {
  unsigned long runMs = (unsigned long)ml * FEED_MS_PER_ML;
  if (runMs > FEED_MAX_MS) runMs = FEED_MAX_MS;

  feedingMl = ml;
  feedOffAt = deadlineAt(runMs);
  digitalWrite(RELAY_FEED_PIN, LOW);
  Firebase.RTDB.setBool(&fbdo, "/commands/pump", true);

  Serial.printf("[FEED] Dispensing %d ml (%lu ms)\n", ml, runMs);
  pushEvent("feeding", "info", "Feeding started: " + String(ml) + " ml.");
}

void stopFeeding() {
  digitalWrite(RELAY_FEED_PIN, HIGH);
  feedOffAt = 0;
  Firebase.RTDB.setBool(&fbdo, "/commands/pump",  false);

  Firebase.RTDB.setInt(&fbdo,  "/commands/feed_ml", 0);
  pushEvent("feeding", "info", "Feeding finished: " + String(feedingMl) + " ml dispensed.");
  feedingMl = 0;
}

void serviceActuators() {
  unsigned long now = millis();
  if (smokeOffAt != 0 && (long)(now - smokeOffAt) >= 0) stopSmoke(true);
  if (feedOffAt  != 0 && (long)(now - feedOffAt)  >= 0) stopFeeding();
}

float speedOfSoundCmPerUs(float tempC) {
  float t = isnan(tempC) ? 25.0 : tempC;
  return (331.3 + 0.606 * t) / 10000.0;
}

float pingOnce(float tempC) {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return NAN;

  float distance = (duration * speedOfSoundCmPerUs(tempC)) / 2.0;

  if (distance < 2.0 || distance > 400.0) return NAN;
  return distance;
}

float readDistanceCm(float tempC) {
  const int N = 5;
  float samples[N];
  int   valid = 0;

  for (int i = 0; i < N; i++) {
    float d = pingOnce(tempC);
    if (!isnan(d)) samples[valid++] = d;
    delay(35);
  }
  if (valid < 3) return NAN;

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

  float waterHeight = containerHeightCm - (data.distance - sensorToTopCm);
  waterHeight = constrain(waterHeight, 0.0f, containerHeightCm);

  float radius = containerDiameterCm / 2.0;
  data.waterHeight = waterHeight;
  data.percentage  = constrain((waterHeight / containerHeightCm) * 100.0f, 0.0f, 100.0f);
  data.remainingMl = 3.14159265 * radius * radius * waterHeight;
  data.valid       = true;

  lastGoodWaterCm  = data.waterHeight;
  lastGoodLevelPct = data.percentage;
  return data;
}

float readWeight() {
  if (!scale.wait_ready_timeout(400)) {
    Serial.println("[HX711] Not ready — holding last good weight");
    return NAN;
  }
  float w = scale.get_units(5);
  if (isnan(w) || w < -50.0 || w > 500.0) return NAN;
  return w;
}

void applyTare() {
  if (!scale.wait_ready_timeout(1000)) {

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

int calcHealthScore(float temp, float hum, float weight, const String &sound) {
  int score = 100;
  if (!isnan(temp) && (temp > 36.0 || temp < 32.0)) score -= 20;
  if (!isnan(hum)  && (hum  > 75.0 || hum  < 40.0)) score -= 15;

  if (sound != "Normal" && sound != "Unknown")      score -= 30;
  if (hornetDetected)                               score -= 20;
  if (!isnan(weight) && weight < 5.0)               score -= 15;
  return constrain(score, 0, 100);
}

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

float estimateHoney(float weight) {
  if (isnan(weight)) return 0.0;
  float gain = weight - baselineWeight;
  if (gain <= 0) return 0.0;
  return gain * honeyFraction;
}

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

  if (!isnan(prevWeightSample)) {
    float delta = weight - prevWeightSample;
    if (delta <= -harvestDropKg && doorOpen) {
      float harvested = -delta;
      totalHarvested += harvested;
      Firebase.RTDB.setFloat(&fbdo,  "/production/last_harvest_kg",   harvested);
      Firebase.RTDB.setString(&fbdo, "/production/last_harvest_time", nowStamp());
      Firebase.RTDB.setFloat(&fbdo,  "/production/total_harvested",   totalHarvested);

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
  prod.set("today_weight",       weight);
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

void uploadSensorData() {

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

void readAndExecuteCommands() {
  if (!Firebase.RTDB.getJSON(&fbdo, "/commands")) return;

  FirebaseJson *cmd = fbdo.jsonObjectPtr();
  if (cmd == nullptr) return;
  FirebaseJsonData r;

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

  if (feedOffAt == 0 && cmd->get(r, "feed_ml")) {
    int ml = r.intValue;
    if (ml > 0) startFeeding(ml);
  }

  if (cmd->get(r, "entrance")) {
    String val = r.stringValue;
    if (val.length() > 0 && val != entranceState) {

      entranceNarrowedByHornet = false;
      hornetClearedAt = 0;
      if (val == "narrow") narrowEntrance("app request");
      else                 openEntrance("app request");
    }
  }

  if (cmd->get(r, "tare_scale") && r.boolValue) {
    applyTare();
  }

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

void servicePendingSound() {
  if (!pendingSoundUpdate) return;
  pendingSoundUpdate = false;

  bool fresh = pendingSoundFresh;
  pendingSoundFresh = false;

  char label[21];
  memcpy(label, pendingSoundLabel, sizeof(label));
  label[20] = '\0';
  int conf = pendingSoundConf;

  String sound = normaliseSound(String(label));

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

void handleHornet() {
  if (hornetDetected == lastHornetState) return;

  if (hornetDetected) {
    Serial.println("[HORNET] Detected — narrowing entrance");
    narrowEntrance("hornet detected");
    entranceNarrowedByHornet = true;
    hornetClearedAt = 0;

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

    hornetClearedAt = millis();
    if (hornetClearedAt == 0) hornetClearedAt = 1;
    pushEvent("hornet_clear", "info",
              "Hornet no longer visible at the entrance.");
  }
  lastHornetState = hornetDetected;
}

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

void handleEspNowPacket(const uint8_t *data, int len) {
  if (len < 1) return;

  switch (data[0]) {
    case MSG_SENSOR: {
      if (len != sizeof(SensorData)) return;
      SensorData in;
      memcpy(&in, data, sizeof(in));

      receivedTemp       = in.temperature;
      receivedHumidity   = in.humidity;
      receivedConfidence = in.confidence;
      lastAudioPacket    = millis();

      memcpy(pendingSoundLabel, in.soundResult, 20);
      pendingSoundLabel[20] = '\0';
      pendingSoundConf = in.confidence;
      if (in.freshAnalysis) pendingSoundFresh = true;

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
    if (v > 0) baselineWeight = v;
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

void setup() {
  Serial.begin(115200);

  digitalWrite(RELAY_FEED_PIN,  HIGH);
  digitalWrite(RELAY_SMOKE_PIN, HIGH);
  pinMode(RELAY_FEED_PIN,  OUTPUT);
  pinMode(RELAY_SMOKE_PIN, OUTPUT);
  digitalWrite(RELAY_FEED_PIN,  HIGH);
  digitalWrite(RELAY_SMOKE_PIN, HIGH);

  pinMode(TRIG_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  pinMode(ECHO_PIN, INPUT);

  servoDoor.attach(SERVO_DOOR_PIN);
  servoLock.attach(SERVO_LOCK_PIN);
  servoEntrance.attach(SERVO_ENTRANCE_PIN);
  servoDoor.write(DOOR_CLOSED_ANGLE);
  servoLock.write(LOCK_LOCKED_ANGLE);
  servoEntrance.write(ENTRANCE_OPEN_ANGLE);
  doorOpen      = false;
  lockState     = true;
  entranceState = "open";

  scale.begin(HX711_DT, HX711_SCK);
  scale.set_scale(scaleFactor);
  Serial.println("[HX711] Scale ready");

  SPI.begin(14, 27, 25, 5);
  rfid.PCD_Init();
  Serial.println("[RFID] Ready");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
  Serial.println("[ESP-NOW] MAC: " + WiFi.macAddress());

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

  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  config.cert.data    = nullptr;
  config.cert.file    = "";
  Firebase.signUp(&config, &auth, "", "");
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  fbdo.setResponseSize(4096);
  config.timeout.serverResponse = 10 * 1000;

  for (int i = 0; i < 100 && !Firebase.ready(); i++) delay(100);
  Serial.println("[Firebase] Connected");

  loadCalibration();

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

  Firebase.RTDB.setFloat(&fbdo, "/production/baseline_weight", baselineWeight);

  Firebase.RTDB.setBool(&fbdo, "/hive_status/door_open", false);
  publishEntrance();

  pushEvent("system", "info", "Main controller started and connected.");
  Serial.println("[Firebase] Initial state published");
}

void loop() {

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
