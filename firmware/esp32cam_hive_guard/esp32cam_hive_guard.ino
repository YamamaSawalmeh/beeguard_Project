#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "img_converters.h"
#include <esp_now.h>

const char* ssid = "MSI";
const char* password = "123456789";

IPAddress local_IP(192, 168, 137, 150);
IPAddress gateway(192, 168, 137, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

uint8_t ESP32_MAIN_MAC[] = {
  0xEC, 0xE3, 0x34, 0x45, 0xC6, 0x94
};

uint8_t BROADCAST_MAC[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5

#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define IMG_W 160
#define IMG_H 120
#define PIXELS (IMG_W * IMG_H)

static uint8_t classBuffer[PIXELS];
static uint16_t queueBuffer[PIXELS];

#define MAX_DBOUR 6

struct Box {
  int x;
  int y;
  int w;
  int h;
};

Box dbourBoxes[MAX_DBOUR];

volatile int currentDbourCount = 0;

const int MIN_DBOUR_PIXELS = 250;
const int MAX_DBOUR_PIXELS = 4500;

const int MIN_LONG_SIDE  = 14;
const int MAX_LONG_SIDE  = 100;

const int MIN_SHORT_SIDE = 8;
const int MAX_SHORT_SIDE = 70;

const float MIN_ASPECT = 1.0;
const float MAX_ASPECT = 2.6;

const float MIN_EXTENT = 0.25;

const int MAX_DARK_VALUE = 90;
const int MIN_DARK_VALUE = 15;

const int MIN_YELLOW_VALUE = 120;   
const int MIN_YELLOW_SAT   = 60;    

const int MIN_YELLOW_PIXELS  = 12;    
const float MIN_YELLOW_RATIO = 0.03;  
const float MAX_YELLOW_RATIO = 0.45;  

#define MSG_SENSOR 1
#define MSG_CAM    2

typedef struct __attribute__((packed)) {
  uint8_t  msgType;            
  uint8_t  hornetDetected;     
  uint8_t  count;              
  uint32_t secondsSinceLast;   
} CamData;

bool lastHornetState = false;

#define NEVER_DETECTED 0xFFFFFFFFUL

volatile unsigned long lastDetectionMs = 0;
volatile bool          everDetected    = false;
volatile int           lastDetectionCount = 0;
volatile unsigned long totalDetections = 0;

volatile bool streamClientActive = false;

const unsigned long HEARTBEAT_INTERVAL = 30000UL;
unsigned long lastHeartbeat = 0;

const unsigned long BACKGROUND_SCAN_INTERVAL = 500UL;
unsigned long lastBackgroundScan = 0;

uint32_t secondsSinceLastDetection() {
  if (!everDetected) return NEVER_DETECTED;
  return (uint32_t)((millis() - lastDetectionMs) / 1000UL);
}

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"

static const char* STREAM_CONTENT_TYPE =
  "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;

static const char* STREAM_BOUNDARY =
  "\r\n--" PART_BOUNDARY "\r\n";

static const char* STREAM_PART =
  "Content-Type: image/jpeg\r\n"
  "Content-Length: %u\r\n\r\n";

void sendHornetStatus(bool detected) {

  CamData pkt;

  int frameCount = currentDbourCount;
  if (frameCount < 0)   frameCount = 0;
  if (frameCount > 255) frameCount = 255;

  pkt.msgType          = MSG_CAM;
  pkt.hornetDetected   = detected ? 1 : 0;
  pkt.count            = (uint8_t)frameCount;
  pkt.secondsSinceLast = secondsSinceLastDetection();

  esp_err_t result = esp_now_send(
    BROADCAST_MAC,
    (uint8_t*)&pkt,
    sizeof(pkt)
  );

  if (result == ESP_OK) {

    Serial.print("[ESP-NOW] Sent hornet = ");

    if (detected) {
      Serial.println("TRUE");
    } else {
      Serial.println("FALSE");
    }

  } else {

    Serial.print("[ESP-NOW] Send FAILED. Error = ");
    Serial.println(result);
  }
}

int classifyInsectPixel(uint8_t r, uint8_t g, uint8_t b) {

  uint8_t maxVal = max(r, max(g, b));
  uint8_t minVal = min(r, min(g, b));

  int delta = maxVal - minVal;

  int saturation =
    (maxVal == 0) ? 0 : (delta * 255) / maxVal;

  int hue = 0;

  if (saturation > 15) {

    if (delta == 0) {

      hue = 0;

    } else if (maxVal == r) {

      hue = 60 * (int(g) - int(b)) / delta;

      if (hue < 0)
        hue += 360;

    } else if (maxVal == g) {

      hue = 120 + 60 * (int(b) - int(r)) / delta;

    } else {

      hue = 240 + 60 * (int(r) - int(g)) / delta;
    }

    if (hue < 0)
      hue += 360;
  }

  if (saturation > 15 && hue >= 180 && hue <= 260) {
    return 0;
  }

  if (
    maxVal >= MIN_YELLOW_VALUE &&
    saturation >= MIN_YELLOW_SAT &&
    hue >= 30 && hue <= 70
  ) {
    return 2;
  }

  if (maxVal <= MAX_DARK_VALUE && maxVal >= MIN_DARK_VALUE) {
    return 1;
  }

  return 0;
}

void makeDbourMask(camera_fb_t* fb) {

  for (int i = 0; i < PIXELS; i++) {

    uint8_t hb = fb->buf[i * 2];
    uint8_t lb = fb->buf[i * 2 + 1];

    uint8_t r = hb & 0xF8;

    uint8_t g =
      ((hb & 0x07) << 5) |
      ((lb & 0xE0) >> 3);

    uint8_t b =
      (lb & 0x1F) << 3;

    classBuffer[i] = classifyInsectPixel(r, g, b);
  }
}

int detectDbour() {

  int dbourCount = 0;

  for (int start = 0; start < PIXELS; start++) {

    uint8_t startClass = classBuffer[start];

    if (startClass == 0) {
      continue;
    }

    int head = 0;
    int tail = 0;

    queueBuffer[tail++] = start;
    classBuffer[start] = 0;  

    int pixelCount = 1;
    int darkCount   = (startClass == 1) ? 1 : 0;
    int yellowCount = (startClass == 2) ? 1 : 0;

    int minX = start % IMG_W;
    int maxX = minX;

    int minY = start / IMG_W;
    int maxY = minY;

    while (head < tail) {

      uint16_t index =
        queueBuffer[head++];

      int x = index % IMG_W;
      int y = index / IMG_W;

      if (x < minX)
        minX = x;

      if (x > maxX)
        maxX = x;

      if (y < minY)
        minY = y;

      if (y > maxY)
        maxY = y;

      for (int dy = -1; dy <= 1; dy++) {

        for (int dx = -1; dx <= 1; dx++) {

          if (dx == 0 && dy == 0)
            continue;

          int nx = x + dx;
          int ny = y + dy;

          if (
            nx < 0 ||
            nx >= IMG_W ||
            ny < 0 ||
            ny >= IMG_H
          ) {
            continue;
          }

          int nextIndex =
            ny * IMG_W + nx;

          uint8_t nextClass = classBuffer[nextIndex];

          if (nextClass != 0) {

            if (nextClass == 1) {
              darkCount++;
            } else if (nextClass == 2) {
              yellowCount++;
            }

            pixelCount++;

            classBuffer[nextIndex] = 0;  

            queueBuffer[tail++] =
              nextIndex;
          }
        }
      }
    }

    if (
      pixelCount < MIN_DBOUR_PIXELS ||
      pixelCount > MAX_DBOUR_PIXELS
    ) {
      continue;
    }

    int width =
      maxX - minX + 1;

    int height =
      maxY - minY + 1;

    int longSide =
      max(width, height);

    int shortSide =
      min(width, height);

    if (
      longSide < MIN_LONG_SIDE ||
      longSide > MAX_LONG_SIDE
    ) {
      continue;
    }

    if (
      shortSide < MIN_SHORT_SIDE ||
      shortSide > MAX_SHORT_SIDE
    ) {
      continue;
    }

    float aspect =
      float(longSide) /
      float(shortSide);

    if (
      aspect < MIN_ASPECT ||
      aspect > MAX_ASPECT
    ) {
      continue;
    }

    int boxArea =
      width * height;

    float extent =
      float(pixelCount) /
      float(boxArea);

    if (extent < MIN_EXTENT) {
      continue;
    }

    if (yellowCount < MIN_YELLOW_PIXELS) {
      continue;
    }

    if (darkCount < MIN_DBOUR_PIXELS / 2) {
      continue;
    }

    float yellowRatio =
      float(yellowCount) /
      float(pixelCount);

    if (
      yellowRatio < MIN_YELLOW_RATIO ||
      yellowRatio > MAX_YELLOW_RATIO
    ) {
      continue;
    }

    if (dbourCount < MAX_DBOUR) {

      dbourBoxes[dbourCount].x = minX;
      dbourBoxes[dbourCount].y = minY;
      dbourBoxes[dbourCount].w = width;
      dbourBoxes[dbourCount].h = height;
    }

    dbourCount++;

    Serial.print("DBOUR -> pixels=");
    Serial.print(pixelCount);

    Serial.print(" dark=");
    Serial.print(darkCount);

    Serial.print(" yellow=");
    Serial.print(yellowCount);

    Serial.print(" w=");
    Serial.print(width);

    Serial.print(" h=");
    Serial.print(height);

    Serial.print(" aspect=");
    Serial.print(aspect, 2);

    Serial.print(" extent=");
    Serial.println(extent, 2);
  }

  return dbourCount;
}

void setPixelRed(
  camera_fb_t* fb,
  int x,
  int y
) {

  if (
    x < 0 ||
    x >= IMG_W ||
    y < 0 ||
    y >= IMG_H
  ) {
    return;
  }

  int index =
    (y * IMG_W + x) * 2;

  fb->buf[index] = 0xF8;
  fb->buf[index + 1] = 0x00;
}

void drawRedRectangle(
  camera_fb_t* fb,
  int x,
  int y,
  int w,
  int h
) {

  int x2 = x + w - 1;
  int y2 = y + h - 1;

  for (int t = 0; t < 2; t++) {

    for (int xx = x; xx <= x2; xx++) {

      setPixelRed(
        fb,
        xx,
        y + t
      );

      setPixelRed(
        fb,
        xx,
        y2 - t
      );
    }

    for (int yy = y; yy <= y2; yy++) {

      setPixelRed(
        fb,
        x + t,
        yy
      );

      setPixelRed(
        fb,
        x2 - t,
        yy
      );
    }
  }
}

void setupCamera() {

  camera_config_t config = {};

  config.ledc_channel =
    LEDC_CHANNEL_0;

  config.ledc_timer =
    LEDC_TIMER_0;

  config.pin_d0 =
    Y2_GPIO_NUM;

  config.pin_d1 =
    Y3_GPIO_NUM;

  config.pin_d2 =
    Y4_GPIO_NUM;

  config.pin_d3 =
    Y5_GPIO_NUM;

  config.pin_d4 =
    Y6_GPIO_NUM;

  config.pin_d5 =
    Y7_GPIO_NUM;

  config.pin_d6 =
    Y8_GPIO_NUM;

  config.pin_d7 =
    Y9_GPIO_NUM;

  config.pin_xclk =
    XCLK_GPIO_NUM;

  config.pin_pclk =
    PCLK_GPIO_NUM;

  config.pin_vsync =
    VSYNC_GPIO_NUM;

  config.pin_href =
    HREF_GPIO_NUM;

  config.pin_sccb_sda =
    SIOD_GPIO_NUM;

  config.pin_sccb_scl =
    SIOC_GPIO_NUM;

  config.pin_pwdn =
    PWDN_GPIO_NUM;

  config.pin_reset =
    RESET_GPIO_NUM;

  config.xclk_freq_hz =
    20000000;

  config.pixel_format =
    PIXFORMAT_RGB565;

  config.frame_size =
    FRAMESIZE_QQVGA;

  config.jpeg_quality =
    12;

  config.fb_count = 1;

  config.grab_mode =
    CAMERA_GRAB_WHEN_EMPTY;

  if (psramFound()) {

    config.fb_location =
      CAMERA_FB_IN_PSRAM;

    Serial.println(
      "PSRAM found"
    );

  } else {

    config.fb_location =
      CAMERA_FB_IN_DRAM;

    Serial.println(
      "PSRAM not found"
    );
  }

  esp_err_t err =
    esp_camera_init(&config);

  if (err != ESP_OK) {

    Serial.printf(
      "Camera init failed: 0x%x\n",
      err
    );

    while (true) {
      delay(1000);
    }
  }

  Serial.println(
    "Camera Ready!"
  );
}

static esp_err_t index_handler(
  httpd_req_t* req
) {

  const char html[] = R"rawliteral(

<!DOCTYPE html>

<html>

<head>

<meta charset="UTF-8">

<meta
  name="viewport"
  content="width=device-width, initial-scale=1"
>

<title>Hive Guard</title>

<style>

body {

  background: #08141d;
  color: white;
  font-family: Arial;
  text-align: center;
  margin: 0;
  padding: 20px;
}

h1 {
  margin-bottom: 5px;
}

.card {

  max-width: 600px;
  margin: auto;
  background: #102532;
  padding: 15px;
  border-radius: 18px;
}

#count {

  font-size: 34px;
  font-weight: bold;
  margin: 10px;
}

#status {

  font-size: 20px;
  margin-bottom: 15px;
}

#status.detected {
  color: #ff5c5c;
}

#status.clear {
  color: #5cff8f;
}

img {

  width: 100%;
  border-radius: 12px;
}

</style>

</head>

<body>

<div class="card">

<h1>Hive Guard</h1>

<div>
عدد الدبابير المكتشفة
</div>

<div id="count">
0
</div>

<div id="status" class="clear">
لا يوجد دبور
</div>

<img id="stream">

<div id="last">
آخر كشف: لا يوجد
</div>

</div>

<script>

const host =
  window.location.hostname;

document
  .getElementById("stream")
  .src =
  "http:
  host +
  "/";

function agoText(sec) {

  if (sec < 0)
    return "لا يوجد كشف بعد";

  if (sec < 60)
    return "قبل " + sec + " ثانية";

  if (sec < 3600)
    return "قبل " + Math.floor(sec / 60) + " دقيقة";

  if (sec < 86400)
    return "قبل " + Math.floor(sec / 3600) + " ساعة";

  return "قبل " + Math.floor(sec / 86400) + " يوم";
}

setInterval(
  function() {

    fetch("/status")

      .then(
        r => r.json()
      )

      .then(
        s => {

          document
            .getElementById("count")
            .innerText =
            s.count;

          const statusEl =
            document
              .getElementById("status");

          if (s.count > 0) {

            statusEl.innerText =
              "تحذير: دبور مكتشف!";

            statusEl.className =
              "detected";

          } else {

            statusEl.innerText =
              "لا يوجد دبور";

            statusEl.className =
              "clear";
          }

          document
            .getElementById("last")
            .innerText =
            "آخر كشف مؤكد: " +
            agoText(s.since_seconds) +
            " (المجموع: " +
            s.total_detections +
            ")";
        }
      );

  },
  1000
);

</script>

</body>

</html>

)rawliteral";

  httpd_resp_set_type(
    req,
    "text/html"
  );

  return httpd_resp_send(
    req,
    html,
    HTTPD_RESP_USE_STRLEN
  );
}

// =====================================================
// COUNT HANDLER
// =====================================================

static esp_err_t count_handler(
  httpd_req_t* req
) {

  char response[8];

  snprintf(
    response,
    sizeof(response),
    "%d",
    currentDbourCount
  );

  httpd_resp_set_type(
    req,
    "text/plain"
  );

  return httpd_resp_send(
    req,
    response,
    HTTPD_RESP_USE_STRLEN
  );
}

// =====================================================
// STATUS HANDLER  (JSON)
// =====================================================
// التطبيق بيقرأ من هون معلومات آخر كشف ليعرضها جنب الفيديو المباشر.
// since_seconds = كم ثانية صار من آخر كشف مؤكد، و -1 يعني ما صار كشف أبداً.

static esp_err_t status_handler(
  httpd_req_t* req
) {

  uint32_t since = secondsSinceLastDetection();

  char response[192];

  snprintf(
    response,
    sizeof(response),
    "{\"count\":%d,"
    "\"detected\":%s,"
    "\"ever_detected\":%s,"
    "\"since_seconds\":%ld,"
    "\"last_count\":%d,"
    "\"total_detections\":%lu,"
    "\"uptime_seconds\":%lu}",
    currentDbourCount,
    (currentDbourCount > 0) ? "true" : "false",
    everDetected ? "true" : "false",
    everDetected ? (long)since : -1L,
    lastDetectionCount,
    (unsigned long)totalDetections,
    (unsigned long)(millis() / 1000UL)
  );

  httpd_resp_set_type(
    req,
    "application/json"
  );

  httpd_resp_set_hdr(
    req,
    "Access-Control-Allow-Origin",
    "*"
  );

  return httpd_resp_send(
    req,
    response,
    HTTPD_RESP_USE_STRLEN
  );
}

int processDetection(camera_fb_t* fb) {

  makeDbourMask(fb);

  int dbourCount = detectDbour();

  currentDbourCount = dbourCount;

  bool hornetDetected = (dbourCount > 0);

  if (hornetDetected) {

    lastDetectionMs    = millis();
    lastDetectionCount = dbourCount;
    everDetected       = true;

    if (!lastHornetState) {
      totalDetections++;
    }
  }

  if (hornetDetected != lastHornetState) {

    sendHornetStatus(hornetDetected);

    lastHornetState = hornetDetected;
  }

  return dbourCount;
}

static esp_err_t stream_handler(
  httpd_req_t* req
) {

  camera_fb_t* fb = NULL;

  uint8_t* jpgBuffer = NULL;

  size_t jpgLength = 0;

  esp_err_t res = ESP_OK;

  char partBuffer[64];

  res =
    httpd_resp_set_type(
      req,
      STREAM_CONTENT_TYPE
    );

  if (res != ESP_OK)
    return res;

  streamClientActive = true;

  while (true) {

    fb =
      esp_camera_fb_get();

    if (!fb) {

      Serial.println(
        "Camera capture failed"
      );

      res = ESP_FAIL;

      break;
    }

    if (
      fb->width != IMG_W ||
      fb->height != IMG_H
    ) {

      esp_camera_fb_return(fb);

      fb = NULL;

      continue;
    }

    int dbourCount =
      processDetection(fb);

    int boxesToDraw =
      min(
        dbourCount,
        MAX_DBOUR
      );

    for (
      int i = 0;
      i < boxesToDraw;
      i++
    ) {

      drawRedRectangle(
        fb,
        dbourBoxes[i].x,
        dbourBoxes[i].y,
        dbourBoxes[i].w,
        dbourBoxes[i].h
      );
    }

    bool converted =
      frame2jpg(
        fb,
        70,
        &jpgBuffer,
        &jpgLength
      );

    esp_camera_fb_return(fb);

    fb = NULL;

    if (!converted) {

      Serial.println(
        "JPEG conversion failed"
      );

      res = ESP_FAIL;

      break;
    }

    res =
      httpd_resp_send_chunk(
        req,
        STREAM_BOUNDARY,
        strlen(STREAM_BOUNDARY)
      );

    if (res != ESP_OK) {

      free(jpgBuffer);

      jpgBuffer = NULL;

      break;
    }

    int hlen =
      snprintf(
        partBuffer,
        sizeof(partBuffer),
        STREAM_PART,
        jpgLength
      );

    res =
      httpd_resp_send_chunk(
        req,
        partBuffer,
        hlen
      );

    if (res != ESP_OK) {

      free(jpgBuffer);

      jpgBuffer = NULL;

      break;
    }

    res =
      httpd_resp_send_chunk(
        req,
        (const char*)jpgBuffer,
        jpgLength
      );

    free(jpgBuffer);

    jpgBuffer = NULL;

    if (res != ESP_OK)
      break;

    delay(30);
  }

  streamClientActive = false;

  return res;
}

void onEspNowSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.print("[ESP-NOW] delivery ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL (no ACK)");
}

void startCameraServer() {

  httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
  streamConfig.server_port      = 80;
  streamConfig.ctrl_port        = 32080;
  streamConfig.stack_size       = 10240;
  streamConfig.lru_purge_enable = true;
  streamConfig.max_open_sockets = 4;

  httpd_uri_t streamUri = {};
  streamUri.uri     = "/";
  streamUri.method  = HTTP_GET;
  streamUri.handler = stream_handler;

  if (httpd_start(&stream_httpd, &streamConfig) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &streamUri);
    Serial.println("Stream server started (port 80, root)");
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port      = 81;
  config.ctrl_port        = 32081;
  config.stack_size       = 10240;
  config.lru_purge_enable = true;
  config.max_open_sockets = 7;

  httpd_uri_t indexUri = {};
  indexUri.uri     = "/";
  indexUri.method  = HTTP_GET;
  indexUri.handler = index_handler;

  httpd_uri_t countUri = {};
  countUri.uri     = "/count";
  countUri.method  = HTTP_GET;
  countUri.handler = count_handler;

  httpd_uri_t statusUri = {};
  statusUri.uri     = "/status";
  statusUri.method  = HTTP_GET;
  statusUri.handler = status_handler;

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &indexUri);
    httpd_register_uri_handler(camera_httpd, &countUri);
    httpd_register_uri_handler(camera_httpd, &statusUri);
    Serial.println("Info server started (port 81)");
  }
}

void setup() {

  Serial.begin(115200);

  delay(1500);

  Serial.println();

  Serial.println(
    "======================"
  );

  Serial.println(
    "HIVE GUARD - DBOUR DETECTOR"
  );

  Serial.println(
    "======================"
  );

  setupCamera();

  WiFi.mode(WIFI_STA);

  WiFi.setSleep(false);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("[WIFI] Static IP config FAILED");
  }

  WiFi.begin(
    ssid,
    password
  );

  Serial.print(
    "Connecting to WiFi"
  );

  while (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  Serial.println(
    "WiFi connected!"
  );

  Serial.print(
    "Camera MAC: "
  );

  Serial.println(
    WiFi.macAddress()
  );

  if (
    esp_now_init() != ESP_OK
  ) {

    Serial.println(
      "[ESP-NOW] INIT FAILED!"
    );

    return;
  }

  esp_now_register_send_cb(onEspNowSent);

  esp_now_peer_info_t peerInfo = {};
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  memcpy(peerInfo.peer_addr, ESP32_MAIN_MAC, 6);
  esp_now_add_peer(&peerInfo);   

  memcpy(peerInfo.peer_addr, BROADCAST_MAC, 6);
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ESP-NOW] Failed to add broadcast peer");
    return;
  }

  Serial.println(
    "[ESP-NOW] READY"
  );

  Serial.println(
    "[ESP-NOW] Target ESP32 #1:"
  );

  Serial.println(
    "EC:E3:34:45:C6:94"
  );

  startCameraServer();

  Serial.println();

  Serial.print("Dashboard: http://");
  Serial.print(WiFi.localIP());
  Serial.println(":81/");

  Serial.print("Live stream: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/");
}

void loop() {

  unsigned long now = millis();

  if (
    !streamClientActive &&
    now - lastBackgroundScan >= BACKGROUND_SCAN_INTERVAL
  ) {

    lastBackgroundScan = now;

    camera_fb_t* fb =
      esp_camera_fb_get();

    if (fb) {

      if (
        fb->width == IMG_W &&
        fb->height == IMG_H
      ) {

        processDetection(fb);
      }

      esp_camera_fb_return(fb);
    }
  }

  if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {

    lastHeartbeat = now;

    sendHornetStatus(lastHornetState);
  }

  delay(20);
}
