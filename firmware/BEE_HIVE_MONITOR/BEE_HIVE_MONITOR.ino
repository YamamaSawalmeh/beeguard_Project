/*
  ============================================================================
  BEE HIVE MONITOR - مشروع مراقبة خلية النحل
  ============================================================================
  المكونات:
    - INMP441 (مايك I2S)
    - MicroSD (تخزين التسجيلات)
    - شاشة LCD I2C 16x2
    - زرين (Record / Analyze)
    - DHT11 (حرارة ورطوبة)
    - Edge Impulse Model (ff_inferencing) لتصنيف Active / Queen_Loss

  التوصيلات:
  ----------------------------------------------------------------------------
  INMP441 (I2S Mic)
    SCK/BCLK -> GPIO 27
    WS/LRCL  -> GPIO 13
    SD/DOUT  -> GPIO 34
    VCC      -> 3.3V
    GND      -> GND

  MicroSD (SPI)
    CS   -> GPIO 5
    SCK  -> GPIO 18
    MISO -> GPIO 19
    MOSI -> GPIO 26
    VCC  -> 5V (حسب الموديول)
    GND  -> GND

  الأزرار (INPUT_PULLUP - موصولة بين GPIO و GND)
    RECORD  -> GPIO 32
    ANALYZE -> GPIO 33

  شاشة LCD (I2C)
    SDA -> GPIO 21
    SCL -> GPIO 25
    VCC -> حسب الشاشة
    GND -> GND

  DHT11
    DATA -> GPIO 4   *** افتراضي - غيّره إذا وصلت الحساس على جي بي آيو مختلف ***

  ----------------------------------------------------------------------------
  المكتبات المطلوبة (Library Manager):
    - LiquidCrystal I2C  (Frank de Brabander أو Marco Schwartz)
    - DHT sensor library (Adafruit)
    - Adafruit Unified Sensor (تبعية لمكتبة DHT)
    - ff_inferencing (مكتبتك الخاصة - Active / Queen_Loss)
  ============================================================================
*/

#include <driver/i2s.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ff_inferencing.h>

// ============================================================================
// 0) ESP-NOW — إرسال الحرارة/الرطوبة/نتيجة الصوت للمتحكم الرئيسي
// ============================================================================
// هاي البورد كانت بتقرأ DHT11 وبتشغّل الموديل بس النتيجة كانت بتوقف عالشاشة
// وما بتوصل لحدا. هلأ بتبعت كل شي عبر ESP-NOW للمتحكم الرئيسي، وهو بيرفعها
// على Firebase فتظهر بالتطبيق.
//
// ⚠️ ESP-NOW بيشتغل بس إذا كل البوردات على نفس قناة الواي فاي، والقناة بتتحدد
//    من الراوتر اللي البورد متصلة فيه. لهيك لازم SSID و PASSWORD يكونوا نفسهم
//    بالضبط بالملفات الثلاثة: هذا الملف، esp32cam_hive_guard.ino و
//    BeeGuard_Main_Controller.ino.
#define WIFI_SSID     "MSI"
#define WIFI_PASSWORD "123456789"

// MAC المتحكم الرئيسي (ESP32 #1)
uint8_t ESP32_MAIN_MAC[] = { 0xEC, 0xE3, 0x34, 0x45, 0xC6, 0x94 };

// نفس الشكل بالضبط الموجود بالمتحكم الرئيسي — لازم يضلوا متطابقين بايت ببايت
#define MSG_SENSOR 1
#define MSG_CAM    2

typedef struct __attribute__((packed)) {
  uint8_t msgType;         // MSG_SENSOR
  float   temperature;
  float   humidity;
  char    soundResult[20];
  uint8_t confidence;      // 0..100
  uint8_t freshAnalysis;   // 1 = نتيجة تحليل جديدة، 0 = تحديث حرارة/رطوبة فقط
} SensorData;

// آخر نتيجة تصنيف — بتنبعت مع كل تحديث دوري كمان
char lastSoundLabel[20] = "Unknown";
int  lastSoundConfidence = 0;

bool espNowReady = false;
unsigned long lastEspNowSend = 0;
const unsigned long ESPNOW_SEND_INTERVAL = 20000UL;   // كل 20 ثانية

// التحليل التلقائي: بدون هذا، التطبيق بيضل يعرض نتيجة قديمة لحد ما حدا يوقف
// جنب الخلية ويضغط الزر.
const unsigned long AUTO_ANALYZE_INTERVAL = 15UL * 60UL * 1000UL;  // كل 15 دقيقة
unsigned long lastAutoAnalyze = 0;

// ============================================================================
// 1) تعريف البنات (Pins)
// ============================================================================

// ---- INMP441 (I2S) ----
#define I2S_BCLK   GPIO_NUM_27
#define I2S_WS     GPIO_NUM_13
#define I2S_SD     GPIO_NUM_34
#define I2S_PORT            I2S_NUM_0
#define I2S_SAMPLE_RATE     (16000)
#define I2S_READ_LEN        (16 * 1024)
#define BYTES_PER_SAMPLE    (2)

// ---- MicroSD (SPI) ----
#define SD_CS    5
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  26
SPIClass sdSPI(VSPI);

// ---- الأزرار ----
#define BTN_RECORD   32
#define BTN_ANALYZE  33
#define DEBOUNCE_MS  250

// ---- شاشة LCD I2C ----
// العنوان الشائع 0x27 (لو الشاشة ما ظهرت، جرب 0x3F أو اعمل I2C scan)
#define LCD_ADDR  0x27
#define LCD_COLS  16
#define LCD_ROWS  2
#define LCD_SDA   21
#define LCD_SCL   25
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ---- DHT11 ----
#define DHT_PIN   4      // *** تأكد من هذا الرقم حسب توصيلتك الفعلية ***
#define DHT_TYPE  DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// ============================================================================
// 2) إعدادات التسجيل
// ============================================================================
#define RECORD_MINUTES        1
#define RECORD_SECONDS_LONG   (RECORD_MINUTES * 60UL)
#define ANALYZE_SECONDS       10

#define TARGET_DBFS   -20.0
#define PEAK_LIMIT    0.99
const int headerSize = 44;

const char* ANALYZE_RAW_FILE  = "/analyze_raw.wav";
const char* ANALYZE_PROC_FILE = "/analyze_proc.wav";

// ============================================================================
// 3) فلاتر معالجة الصوت (نفس الكود الأصلي)
// ============================================================================
static double dc_x_prev = 0.0;
static double dc_y_prev = 0.0;
const double DC_FILTER_R = 0.995;

static inline double dc_block(double x) {
  double y = x - dc_x_prev + DC_FILTER_R * dc_y_prev;
  dc_x_prev = x;
  dc_y_prev = y;
  return y;
}

struct BiquadCoeffs { double b0, b1, b2, a1, a2; };
struct BiquadState  { double z1 = 0.0, z2 = 0.0; };

static const BiquadCoeffs sections[4] = {
  { 0.0086086972,  0.0172173944,  0.0086086972, -0.9321840550,  0.2476487208 },
  { 1.0000000000,  2.0000000000,  1.0000000000, -1.1438490990,  0.6042562289 },
  { 1.0000000000, -2.0000000000,  1.0000000000, -1.9235752203,  0.9253348864 },
  { 1.0000000000, -2.0000000000,  1.0000000000, -1.9708723639,  0.9724296607 }
};
static BiquadState states[4];

static inline double bandpass_filter_sample(double x) {
  double in = x;
  for (int s = 0; s < 4; s++) {
    double out = sections[s].b0 * in + states[s].z1;
    states[s].z1 = sections[s].b1 * in - sections[s].a1 * out + states[s].z2;
    states[s].z2 = sections[s].b2 * in - sections[s].a2 * out;
    in = out;
  }
  return in;
}

static void resetFilters() {
  dc_x_prev = 0.0;
  dc_y_prev = 0.0;
  for (int s = 0; s < 4; s++) { states[s].z1 = 0.0; states[s].z2 = 0.0; }
}

// ============================================================================
// 4) بفر التصنيف (Edge Impulse) - نافذة ثانية واحدة = 16000 عينة
// ============================================================================
static int16_t clsBuffer[EI_CLASSIFIER_RAW_SAMPLE_COUNT];

static int classify_signal_get_data(size_t offset, size_t length, float *out_ptr) {
  numpy::int16_to_float(&clsBuffer[offset], out_ptr, length);
  return 0;
}

// ============================================================================
// 5) حالة النظام
// ============================================================================
enum SystemState { STATE_IDLE, STATE_RECORDING, STATE_ANALYZING, STATE_DONE_MSG };
volatile SystemState sysState = STATE_IDLE;

unsigned long lastBtnRecord  = 0;
unsigned long lastBtnAnalyze = 0;
unsigned long lastSensorUpdate = 0;
unsigned long doneMsgSince = 0;

float lastTemp = NAN;
float lastHum  = NAN;

// كرت SD جاهز؟ إذا لأ، منعطّل التسجيل والتحليل بس منضل نبعث الحرارة والرطوبة.
bool sdReady = false;

// ============================================================================
// 5.5) ESP-NOW — التهيئة والإرسال
// ============================================================================

// موديل Edge Impulse بيطلع أسماء أصناف خاصة فيه ("Active" / "Queen_Loss")،
// بينما التطبيق والمتحكم الرئيسي بيفهموا "Normal" / "Swarming" / "Queen Loss".
// منترجم هون مرة وحدة قبل الإرسال. إذا دربت الموديل من جديد بأسماء غير، عدّل
// هاي الدالة بس.
void normalizeLabel(const char *raw, char *out, size_t outLen) {
  String s = String(raw);
  s.trim();
  String lower = s;
  lower.toLowerCase();

  const char *mapped;
  if (lower == "active" || lower == "normal" || lower == "healthy") {
    mapped = "Normal";
  } else if (lower == "queen_loss" || lower == "queenloss" || lower == "queen loss") {
    mapped = "Queen Loss";
  } else if (lower == "swarm" || lower == "swarming") {
    mapped = "Swarming";
  } else if (lower.length() == 0) {
    mapped = "Unknown";
  } else {
    mapped = s.c_str();
  }

  strncpy(out, mapped, outLen - 1);
  out[outLen - 1] = '\0';
}

void espNowInit() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // النوم بيخلي ESP-NOW يضيّع باكتات

  // منتصل بنفس الراوتر تبع المتحكم الرئيسي عشان نضمن نفس القناة. ما منستنى
  // للأبد — إذا الواي فاي مش موجود، التسجيل والتحليل لازم يضلوا يشتغلوا.
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("[WiFi] NOT connected — ESP-NOW may be on the wrong channel");
  }
  Serial.printf("[WiFi] Channel: %d  (must match the main controller)\n", WiFi.channel());
  Serial.println("[ESP-NOW] My MAC: " + WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] INIT FAILED");
    return;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, ESP32_MAIN_MAC, 6);
  peer.channel = 0;        // 0 = استخدم قناة الواي فاي الحالية
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[ESP-NOW] Failed to add main controller as peer");
    return;
  }

  espNowReady = true;
  Serial.println("[ESP-NOW] Ready — sending to EC:E3:34:45:C6:94");
}

void sendSensorPacket(bool freshAnalysis) {
  // الطابع الزمني قبل الفحص: لو ESP-NOW ما جهز، وتركنا lastEspNowSend ما
  // بتتحدث، شرط الإرسال بـ loop() بيضل صحيح للأبد وبتصير refreshDht()
  // تنستدعى آلاف المرات بالثانية — وقراءة DHT بتقفل المقاطعات ~٢٥ ملي ثانية
  // وبدها ثانية بين قراءة وقراءة، فالبورد بتتعلق وكل القراءات بتفشل.
  lastEspNowSend = millis();
  if (!espNowReady) return;

  SensorData pkt = {};
  pkt.msgType = MSG_SENSOR;

  // NAN بينبعت زي ما هو — المتحكم الرئيسي بيتعامل معها صح (بيتجاهل القراءة
  // بدل ما يكتبها). لو بعتنا 0.0 بدلها، بيستقبلها كأنها قراءة حقيقية:
  // "الخلية باردة ٠ درجة" و "الرطوبة ٠٪" تنبيهات كاذبة، و health_score
  // بينقص ٣٥ نقطة، وسرعة الصوت بتتحسب غلط.
  pkt.temperature   = lastTemp;
  pkt.humidity      = lastHum;
  pkt.confidence    = (uint8_t)constrain(lastSoundConfidence, 0, 100);
  pkt.freshAnalysis = freshAnalysis ? 1 : 0;
  strncpy(pkt.soundResult, lastSoundLabel, sizeof(pkt.soundResult) - 1);

  esp_err_t res = esp_now_send(ESP32_MAIN_MAC, (uint8_t *)&pkt, sizeof(pkt));
  lastEspNowSend = millis();

  if (res == ESP_OK) {
    Serial.printf("[ESP-NOW] Sent T=%.1f H=%.1f %s %d%% fresh=%d\n",
                  pkt.temperature, pkt.humidity, pkt.soundResult,
                  pkt.confidence, pkt.freshAnalysis);
  } else {
    Serial.printf("[ESP-NOW] Send failed (%d)\n", res);
  }
}

// قراءة DHT مستقلة عن الشاشة: الشاشة بتتحدث بس بحالة IDLE، بس الحساس لازم
// يضل يقرأ حتى وقت التسجيل عشان التطبيق ما يوقف عنده الرقم.
void refreshDht() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t) && !isnan(h)) {
    lastTemp = t;
    lastHum  = h;
  }
}

// ============================================================================
// setup()
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BTN_RECORD, INPUT_PULLUP);
  pinMode(BTN_ANALYZE, INPUT_PULLUP);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcdShowIdle();

  dht.begin();
  refreshDht();          // قراءة أولى فورية عشان أول باكت يطلع بقيم حقيقية

  i2sInit();

  // SD Card على بنات مخصصة
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) {
    // قبل هيك كان الكود بيعلق هون بـ while(1) للأبد. هلأ صار هذا اللوح مسؤول
    // كمان عن إرسال الحرارة والرطوبة للتطبيق، فما بصير كرت SD خربان يوقّف
    // البورد كلها — منكمل بدون تسجيل/تحليل.
    sdReady = false;
    Serial.println("!!! فشل تهيئة SD Card — التسجيل والتحليل معطلين");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("SD Card ERROR");
    lcd.setCursor(0, 1); lcd.print("Sensors only");
    delay(2500);
  } else {
    sdReady = true;
    Serial.println("SD Card جاهزة");
  }

  // ESP-NOW بعد SD: تهيئة الواي فاي بتاخد ذاكرة، ومنحب نعرف كم ضل فاضي قبل
  // ما يشتغل Edge Impulse (الموديل + بفر التصنيف بياخدوا عشرات الكيلوبايت).
  Serial.printf("[MEM] Free heap before WiFi: %u bytes\n", ESP.getFreeHeap());
  espNowInit();
  Serial.printf("[MEM] Free heap after  WiFi: %u bytes\n", ESP.getFreeHeap());

  sendSensorPacket(false);   // أول تحديث فوري للتطبيق
  lastAutoAnalyze = millis();

  lcdShowIdle();
  Serial.println("جاهز! زر RECORD يبدأ تسجيل، زر ANALYZE يحلل عينة 10 ثواني.");
}

// ============================================================================
// loop()
// ============================================================================
void loop() {
  handleButtons();
  updateSensorDisplay();

  // رجوع الشاشة لحالة IDLE بعد عرض رسالة "خلص" لمدة معينة
  if (sysState == STATE_DONE_MSG && millis() - doneMsgSince > 4000) {
    sysState = STATE_IDLE;
    lcdShowIdle();
  }

  // ── إرسال دوري للتطبيق ───────────────────────────────────
  // مستقل عن حالة النظام: حتى وقت التسجيل الطويل لازم الحرارة والرطوبة
  // يضلوا يوصلوا، لأن التسجيل بياخد دقايق والتطبيق ما بصير يوقف عالقيمة القديمة.
  if (millis() - lastEspNowSend >= ESPNOW_SEND_INTERVAL) {
    refreshDht();
    sendSensorPacket(false);
  }

  // ── تحليل تلقائي كل 15 دقيقة ─────────────────────────────
  // بس إذا النظام فاضي وكرت SD شغال — التحليل بيسجل ملف مؤقت على SD.
  if (sysState == STATE_IDLE && sdReady &&
      millis() - lastAutoAnalyze >= AUTO_ANALYZE_INTERVAL) {
    lastAutoAnalyze = millis();
    Serial.println("[AUTO] بدء تحليل دوري");
    startAnalysis();
  }
}

// ============================================================================
// الأزرار
// ============================================================================
void handleButtons() {
  if (sysState != STATE_IDLE) return; // ما نستقبل ضغطات وإحنا مشغولين

  // بدون كرت SD ما في مكان نكتب فيه ملف الصوت، فالزرين معطلين.
  if (!sdReady) return;

  if (digitalRead(BTN_RECORD) == LOW && millis() - lastBtnRecord > DEBOUNCE_MS) {
    lastBtnRecord = millis();
    startLongRecording();
  }

  if (digitalRead(BTN_ANALYZE) == LOW && millis() - lastBtnAnalyze > DEBOUNCE_MS) {
    lastBtnAnalyze = millis();
    // ضغطة يدوية بتأجل التحليل التلقائي الجاي، عشان ما يصير تحليلين ورا بعض
    lastAutoAnalyze = millis();
    startAnalysis();
  }
}

// ============================================================================
// عرض الحرارة/الرطوبة (فقط بحالة IDLE، كل ثانيتين، سطر ثاني بالشاشة)
// ============================================================================
void updateSensorDisplay() {
  if (sysState != STATE_IDLE) return;
  if (millis() - lastSensorUpdate < 2000) return;
  lastSensorUpdate = millis();

  refreshDht();

  lcd.setCursor(0, 1);
  char line[17];
  if (!isnan(lastTemp) && !isnan(lastHum)) {
    snprintf(line, sizeof(line), "T:%.1fC H:%.0f%%  ", lastTemp, lastHum);
  } else {
    snprintf(line, sizeof(line), "DHT11 read err  ");
  }
  lcd.print(line);
}

// ============================================================================
// شاشات LCD
// ============================================================================
void lcdShowIdle() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BEE HIVE MONITOR");
  lcd.setCursor(0, 1);
  lcd.print("RECORD / ANALYZE");
  lastSensorUpdate = 0; // يفرض تحديث فوري
}

// ============================================================================
// I2S init
// ============================================================================
void i2sInit() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false
  };
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);

  const i2s_pin_config_t pin_config = {
    // mck_io_num أول عنصر بالـ struct بإصدارات IDF الحديثة. لو تركناه، بياخد
    // قيمة 0 وبتروح إشارة MCLK على GPIO 0 (وهو strapping pin) بدل ما تنعطل.
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);
}

// ============================================================================
// WAV Header
// ============================================================================
void wavHeader(byte *header, uint32_t wavSize) {
  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  uint32_t fileSize = wavSize + headerSize - 8;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 0x10; header[17] = 0x00; header[18] = 0x00; header[19] = 0x00;
  header[20] = 0x01; header[21] = 0x00;
  header[22] = 0x01; header[23] = 0x00;
  header[24] = 0x80; header[25] = 0x3E; header[26] = 0x00; header[27] = 0x00;
  header[28] = 0x00; header[29] = 0x7D; header[30] = 0x00; header[31] = 0x00;
  header[32] = 0x02; header[33] = 0x00;
  header[34] = 0x10; header[35] = 0x00;
  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (byte)(wavSize & 0xFF);
  header[41] = (byte)((wavSize >> 8) & 0xFF);
  header[42] = (byte)((wavSize >> 16) & 0xFF);
  header[43] = (byte)((wavSize >> 24) & 0xFF);
}

// ============================================================================
// 6) التسجيل الطويل (30 دقيقة) -> SD مباشرة، اسم ملف تلقائي BEE_XXX.wav
// ============================================================================
void getNextRecordingName(char *outName, size_t outLen) {
  for (int i = 1; i < 1000; i++) {
    snprintf(outName, outLen, "/BEE_%03d.wav", i);
    if (!SD.exists(outName)) return;
  }
  // لو تعبّت كل الأسماء، رجّع اسم ثابت (نادرًا ما بيصير)
  snprintf(outName, outLen, "/BEE_OVERFLOW.wav");
}

void startLongRecording() {
  sysState = STATE_RECORDING;
  xTaskCreate(longRecordTask, "longRecordTask", 1024 * 12, NULL, 1, NULL);
}

void longRecordTask(void *arg) {
  char filename[32];
  getNextRecordingName(filename, sizeof(filename));

  Serial.printf("بدء تسجيل طويل: %s\n", filename);

  File f = SD.open(filename, FILE_WRITE);
  if (!f) {
    Serial.println("!!! فشل فتح ملف على SD");
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("SD WRITE ERROR");
    delay(2000);
    sysState = STATE_DONE_MSG;
    doneMsgSince = millis();
    vTaskDelete(NULL);
    return;
  }

  const uint32_t totalBytes = I2S_SAMPLE_RATE * BYTES_PER_SAMPLE * RECORD_SECONDS_LONG;
  byte header[headerSize];
  wavHeader(header, totalBytes);
  f.write(header, headerSize);

  resetFilters();

  uint8_t *i2s_read_buff = (uint8_t *)malloc(I2S_READ_LEN);
  int16_t *out_buff = (int16_t *)malloc(I2S_READ_LEN / 2);
  size_t bytes_read;

  // تفريغ أول قراءتين (زي الكود الأصلي)
  i2s_read(I2S_PORT, i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
  i2s_read(I2S_PORT, i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);

  uint32_t written = 0;
  unsigned long lastLcdUpdate = 0;

  while (written < totalBytes) {
    i2s_read(I2S_PORT, i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
    int32_t *samples32 = (int32_t *)i2s_read_buff;
    size_t sampleCount = bytes_read / 4;

    for (size_t i = 0; i < sampleCount; i++) {
      double dc_removed = dc_block((double)samples32[i]) / 65536.0;
      double filtered = bandpass_filter_sample(dc_removed);

      if (filtered > 32767.0) filtered = 32767.0;
      if (filtered < -32768.0) filtered = -32768.0;
      out_buff[i] = (int16_t)filtered;
    }

    size_t bytesToWrite = sampleCount * 2;
    f.write((uint8_t *)out_buff, bytesToWrite);
    written += bytesToWrite;

    // تحديث الشاشة كل ثانية تقريبًا
    if (millis() - lastLcdUpdate > 1000) {
      lastLcdUpdate = millis();
      uint32_t elapsedSec   = written / (I2S_SAMPLE_RATE * BYTES_PER_SAMPLE);
      uint32_t remainingSec = RECORD_SECONDS_LONG - elapsedSec;
      lcdShowRecordingProgress(elapsedSec, remainingSec);
    }
  }

  f.close();
  free(i2s_read_buff);
  free(out_buff);

  Serial.printf("خلص التسجيل: %s\n", filename);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("RECORDING");
  lcd.setCursor(0, 1); lcd.print("COMPLETE");
  delay(1200);
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("Saved to SD Card");
  lcd.setCursor(0, 1); lcd.print(filename);
  delay(2000);

  sysState = STATE_DONE_MSG;
  doneMsgSince = millis();
  vTaskDelete(NULL);
}

void lcdShowRecordingProgress(uint32_t elapsedSec, uint32_t remainingSec) {
  char line1[17], line2[17];
  uint32_t em = elapsedSec / 60,   es = elapsedSec % 60;
  uint32_t rm = remainingSec / 60, rs = remainingSec % 60;

  snprintf(line1, sizeof(line1), "RECORDING");
  snprintf(line2, sizeof(line2), "%02lu:%02lu / Rem %02lu:%02lu",
           (unsigned long)em, (unsigned long)es, (unsigned long)rm, (unsigned long)rs);

  // السطر الثاني ممكن يطلع أطول من 16 خانة، منقصّه
  lcd.setCursor(0, 0); lcd.print("RECORDING       ");
  lcd.setCursor(0, 1);
  char short2[17];
  snprintf(short2, sizeof(short2), "T:%02lu:%02lu R:%02lu:%02lu",
           (unsigned long)em, (unsigned long)es, (unsigned long)rm, (unsigned long)rs);
  lcd.print(short2);
}

// ============================================================================
// 7) التحليل: تسجيل عينة 10 ثواني -> معالجة (DC+Bandpass+Normalize) -> تصنيف
// ============================================================================
void startAnalysis() {
  sysState = STATE_ANALYZING;
  xTaskCreate(analyzeTask, "analyzeTask", 1024 * 20, NULL, 1, NULL);
}

void analyzeTask(void *arg) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("ANALYZING...");
  lcd.setCursor(0, 1); lcd.print("Recording 10s");

  // ---- الخطوة أ: تسجيل 10 ثواني + DC block + Band-pass ----
  const uint32_t sampleTotal = I2S_SAMPLE_RATE * ANALYZE_SECONDS; // 160000 عينة
  const uint32_t totalBytes  = sampleTotal * BYTES_PER_SAMPLE;

  SD.remove(ANALYZE_RAW_FILE);
  SD.remove(ANALYZE_PROC_FILE);

  File rawFile = SD.open(ANALYZE_RAW_FILE, FILE_WRITE);
  if (!rawFile) {
    Serial.println("!!! فشل فتح ملف التحليل المؤقت على SD");
    analysisFail("SD WRITE ERR");
    return;
  }

  byte header[headerSize];
  wavHeader(header, totalBytes);
  rawFile.write(header, headerSize);

  resetFilters();
  double sumSquares = 0.0;
  double peakAbs = 0.0;
  uint32_t samplesDone = 0;

  uint8_t *i2s_read_buff = (uint8_t *)malloc(I2S_READ_LEN);
  int16_t *out_buff = (int16_t *)malloc(I2S_READ_LEN / 2);
  size_t bytes_read;

  // ٢٤ كيلوبايت متصلة — بعد ما الواي فاي وموديل Edge Impulse أخدوا حصتهم من
  // الذاكرة، هاد الطلب ممكن يفشل فعلاً. بدون هذا الفحص بنكتب على مؤشر NULL
  // والبورد بتعمل panic.
  if (!i2s_read_buff || !out_buff) {
    Serial.println("!!! فشل حجز الذاكرة للتحليل");
    free(i2s_read_buff);
    free(out_buff);
    rawFile.close();
    analysisFail("NO MEMORY");
    return;
  }

  i2s_read(I2S_PORT, i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
  i2s_read(I2S_PORT, i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);

  uint32_t written = 0;
  while (written < totalBytes) {
    i2s_read(I2S_PORT, i2s_read_buff, I2S_READ_LEN, &bytes_read, portMAX_DELAY);
    int32_t *samples32 = (int32_t *)i2s_read_buff;
    size_t sampleCount = bytes_read / 4;

    for (size_t i = 0; i < sampleCount; i++) {
      double dc_removed = dc_block((double)samples32[i]) / 65536.0;
      double filtered = bandpass_filter_sample(dc_removed);

      sumSquares += filtered * filtered;
      double a = fabs(filtered);
      if (a > peakAbs) peakAbs = a;
      samplesDone++;

      if (filtered > 32767.0) filtered = 32767.0;
      if (filtered < -32768.0) filtered = -32768.0;
      out_buff[i] = (int16_t)filtered;
    }

    size_t bytesToWrite = sampleCount * 2;
    rawFile.write((uint8_t *)out_buff, bytesToWrite);
    written += bytesToWrite;
  }

  rawFile.close();
  free(i2s_read_buff);
  free(out_buff);

  // ---- الخطوة ب: التطبيع (Normalization) ----
  lcd.setCursor(0, 1); lcd.print("Processing...   ");

  double rms = (samplesDone > 0) ? sqrt(sumSquares / (double)samplesDone) : 0.0;
  double target_rms = pow(10.0, TARGET_DBFS / 20.0) * 32767.0;
  double gain = (rms > 1e-9) ? (target_rms / rms) : 1.0;

  double peak_after = peakAbs * gain;
  double peak_limit_int16 = PEAK_LIMIT * 32767.0;
  if (peak_after > peak_limit_int16 && peakAbs > 1e-9) {
    gain = peak_limit_int16 / peakAbs;
  }

  Serial.printf("Analyze RMS=%.1f Peak=%.1f Gain=%.4f\n", rms, peakAbs, gain);

  File inFile  = SD.open(ANALYZE_RAW_FILE, FILE_READ);
  File outFile = SD.open(ANALYZE_PROC_FILE, FILE_WRITE);
  if (!inFile || !outFile) {
    Serial.println("!!! فشل فتح ملفات التطبيع");
    // analysisFail بتنتهي بـ vTaskDelete، يعني الـ destructors ما بتنعمل أبداً
    // وأي ملف فتح بيضل مفتوح. مع التحليل التلقائي كل ١٥ دقيقة، هاد التسريب
    // بيتراكم لحد ما تخلص الـ file descriptors.
    inFile.close();
    outFile.close();
    analysisFail("SD READ ERR");
    return;
  }

  wavHeader(header, totalBytes);
  outFile.write(header, headerSize);
  inFile.seek(headerSize);

  const size_t CHUNK = 2048;
  static int16_t buf[CHUNK];
  size_t n;
  while ((n = inFile.read((uint8_t *)buf, CHUNK * 2)) > 0) {
    size_t count = n / 2;
    for (size_t i = 0; i < count; i++) {
      double v = (double)buf[i] * gain;
      if (v > 32767.0) v = 32767.0;
      if (v < -32768.0) v = -32768.0;
      buf[i] = (int16_t)v;
    }
    outFile.write((uint8_t *)buf, count * 2);
  }
  inFile.close();
  outFile.close();

  // ---- الخطوة ج: التصنيف على كل نافذة ثانية (10 نوافذ) ----
  lcd.setCursor(0, 1); lcd.print("Classifying...  ");

  File pf = SD.open(ANALYZE_PROC_FILE, FILE_READ);
  if (!pf) {
    Serial.println("!!! فشل فتح الملف المعالج للتصنيف");
    analysisFail("SD READ ERR");
    return;
  }
  pf.seek(headerSize);

  const size_t windowBytes = EI_CLASSIFIER_RAW_SAMPLE_COUNT * sizeof(int16_t);
  double sumScores[EI_CLASSIFIER_LABEL_COUNT] = {0};
  int windowCount = 0;
  size_t rn;

  while ((rn = pf.read((uint8_t *)clsBuffer, windowBytes)) == windowBytes) {
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    signal.get_data = &classify_signal_get_data;

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);

    if (r == EI_IMPULSE_OK) {
      for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        sumScores[ix] += result.classification[ix].value;
        Serial.printf("  [win %d] %s: ", windowCount, result.classification[ix].label);
        ei_printf_float(result.classification[ix].value);
        Serial.println();
      }
      windowCount++;
    } else {
      Serial.printf("ERR: run_classifier فشل (%d)\n", r);
    }
  }
  pf.close();

  // ---- الخطوة د: عرض النتيجة على LCD ----
  if (windowCount == 0) {
    analysisFail("NO DATA");
    return;
  }

  int bestIdx = 0;
  double bestAvg = -1.0;
  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    double avg = sumScores[ix] / windowCount;
    if (avg > bestAvg) {
      bestAvg = avg;
      bestIdx = ix;
    }
  }

  const char *bestLabel = ei_classifier_inferencing_categories[bestIdx];
  int bestPercent = (int)round(bestAvg * 100.0);

  Serial.printf("النتيجة النهائية: %s = %d%%\n", bestLabel, bestPercent);

  // ── إرسال النتيجة للمتحكم الرئيسي -> Firebase -> التطبيق ──
  // بنترجم اسم الصنف لمفردات التطبيق قبل الإرسال، ومنحدّث الحرارة والرطوبة
  // بنفس الباكت عشان يوصلوا مع بعض.
  normalizeLabel(bestLabel, lastSoundLabel, sizeof(lastSoundLabel));
  lastSoundConfidence = bestPercent;
  // ما منقرأ DHT من هون: هاي الدالة بتشتغل بتاسك منفصل، ومكتبة DHT مش
  // reentrant (بتكتب بـ buffer مشترك والمقاطعات مقفولة). لو قرأنا هون
  // بالتوازي مع loop() القراءتين بيخربوا بعض. آخر قيمة من loop() بتكفي —
  // عمرها ٢٠ ثانية بأسوأ حالة.
  sendSensorPacket(true);   // fresh = 1 -> المتحكم بيسجلها بـ ai_history

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("ANALYSIS RESULT");
  lcd.setCursor(0, 1);
  char resLine[17];
  snprintf(resLine, sizeof(resLine), "%s: %d%%", lastSoundLabel, bestPercent);
  lcd.print(resLine);

  delay(4000);

  sysState = STATE_DONE_MSG;
  doneMsgSince = millis();
  vTaskDelete(NULL);
}

void analysisFail(const char *reason) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("ANALYSIS FAILED");
  lcd.setCursor(0, 1); lcd.print(reason);
  delay(2500);
  sysState = STATE_DONE_MSG;
  doneMsgSince = millis();
  vTaskDelete(NULL);
}
