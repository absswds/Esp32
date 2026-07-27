// ESP32-S3 AI Camera — FreeRTOS multi-client MJPEG broadcaster
// One task captures each JPEG once; another sends that same frame round-robin
// to connected viewers. Based on the proven architecture of
// arkhipenko/esp32-cam-mjpeg-multiclient, adapted for DFRobot DFR1154 pins.

#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

// === WiFi ===
const char *AP_SSID = "OPhone 12";
const char *AP_PASS = "qwer1234";

// === DFRobot DFR1154 FireBeetle 2 ESP32-S3 AI Camera ===
#define CAM_PIN_PWDN     -1
#define CAM_PIN_RESET    -1
#define CAM_PIN_XCLK      5
#define CAM_PIN_SIOD      8
#define CAM_PIN_SIOC      9
#define CAM_PIN_D7        4
#define CAM_PIN_D6        6
#define CAM_PIN_D5        7
#define CAM_PIN_D4       14
#define CAM_PIN_D3       17
#define CAM_PIN_D2       21
#define CAM_PIN_D1       18
#define CAM_PIN_D0       16
#define CAM_PIN_VSYNC     1
#define CAM_PIN_HREF      2
#define CAM_PIN_PCLK     15

#define PIN_IR  47
#define PIN_LED 3

// Keep this deliberately low for a phone hotspot. Each viewer receives the JPEG.
static constexpr uint8_t MAX_STREAM_CLIENTS = 4;
static constexpr uint8_t STREAM_FPS = 8;
static constexpr uint16_t HTTP_TASK_PERIOD_MS = 50;

static WebServer server(80);
static QueueHandle_t streamingClients = nullptr;
static SemaphoreHandle_t frameMutex = nullptr;
static portMUX_TYPE clientCountMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t cameraTaskHandle = nullptr;
static TaskHandle_t streamTaskHandle = nullptr;

// Double-buffered, copied JPEG frames. Only camera_capture_task calls esp_camera_fb_get().
static uint8_t *frameBuffers[2] = {nullptr, nullptr};
static size_t frameCapacities[2] = {0, 0};
static uint8_t *broadcastBuffer = nullptr;
static size_t broadcastCapacity = 0;
static const uint8_t *activeFrame = nullptr;
static size_t activeFrameSize = 0;
static uint8_t writeBufferIndex = 0;
static volatile uint8_t activeClientCount = 0;
static volatile bool cameraReady = false;

static const char STREAM_HEADER[] =
  "HTTP/1.1 200 OK\r\n"
  "Access-Control-Allow-Origin: *\r\n"
  "Cache-Control: no-store\r\n"
  "Connection: close\r\n"
  "Content-Type: multipart/x-mixed-replace; boundary=esp32cam\r\n\r\n";
static const char STREAM_BOUNDARY[] = "\r\n--esp32cam\r\n";
static const char STREAM_CONTENT_TYPE[] = "Content-Type: image/jpeg\r\nContent-Length: ";

static void camera_capture_task(void *parameter);
static void stream_broadcast_task(void *parameter);
static void http_server_task(void *parameter);
static void handleStream();
static void handleLight();
static void handleStatus();
static void handleHealth();
static void handleRoot();
static void connectWiFi();
static bool ensureFrameCapacity(uint8_t index, size_t required);
static bool ensureBroadcastCapacity(size_t required);
static bool reserveClientSlot();
static void releaseClientSlot();

static bool ensureFrameCapacity(uint8_t index, size_t required) {
  if (required <= frameCapacities[index]) return true;

  // Extra headroom avoids reallocating for small JPEG-size fluctuations.
  size_t nextCapacity = required + required / 4;
  uint8_t *next = static_cast<uint8_t *>(ps_malloc(nextCapacity));
  if (!next) {
    Serial.printf("[CAM] PSRAM allocation failed: %u bytes\n", static_cast<unsigned>(nextCapacity));
    return false;
  }
  free(frameBuffers[index]);
  frameBuffers[index] = next;
  frameCapacities[index] = nextCapacity;
  Serial.printf("[CAM] Frame buffer %u allocated: %u bytes\n", index, static_cast<unsigned>(nextCapacity));
  return true;
}

static bool ensureBroadcastCapacity(size_t required) {
  if (required <= broadcastCapacity) return true;

  size_t nextCapacity = required + required / 4;
  uint8_t *next = static_cast<uint8_t *>(ps_malloc(nextCapacity));
  if (!next) {
    Serial.printf("[CAM] Broadcast buffer allocation failed: %u bytes\n", static_cast<unsigned>(nextCapacity));
    return false;
  }
  free(broadcastBuffer);
  broadcastBuffer = next;
  broadcastCapacity = nextCapacity;
  Serial.printf("[CAM] Broadcast buffer allocated: %u bytes\n", static_cast<unsigned>(nextCapacity));
  return true;
}

static bool reserveClientSlot() {
  bool reserved = false;
  portENTER_CRITICAL(&clientCountMux);
  if (activeClientCount < MAX_STREAM_CLIENTS) {
    ++activeClientCount;
    reserved = true;
  }
  portEXIT_CRITICAL(&clientCountMux);
  return reserved;
}

static void releaseClientSlot() {
  portENTER_CRITICAL(&clientCountMux);
  if (activeClientCount > 0) --activeClientCount;
  portEXIT_CRITICAL(&clientCountMux);
}

// /stream only retains the client and returns. It never captures or sends frames itself.
static void handleStream() {
  if (!streamingClients || !reserveClientSlot()) {
    server.send(503, "text/plain", "Stream viewer limit reached");
    return;
  }

  WiFiClient *client = new WiFiClient(server.client());
  if (!client || !client->connected()) {
    delete client;
    releaseClientSlot();
    server.send(503, "text/plain", "Unable to open stream");
    return;
  }

  if (client->write(reinterpret_cast<const uint8_t *>(STREAM_HEADER), strlen(STREAM_HEADER)) != strlen(STREAM_HEADER) ||
      client->write(reinterpret_cast<const uint8_t *>(STREAM_BOUNDARY), strlen(STREAM_BOUNDARY)) != strlen(STREAM_BOUNDARY) ||
      xQueueSend(streamingClients, &client, 0) != pdPASS) {
    client->stop();
    delete client;
    releaseClientSlot();
    return;
  }

  Serial.printf("[STREAM] Client added; active=%u\n", activeClientCount);
}

static void sendLightJson() {
  char json[64];
  snprintf(json, sizeof(json), "{\"ir\":%d,\"led\":%d}", digitalRead(PIN_IR), digitalRead(PIN_LED));
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

static void handleLight() {
  if (server.hasArg("ir")) digitalWrite(PIN_IR, server.arg("ir").toInt() ? HIGH : LOW);
  if (server.hasArg("led")) digitalWrite(PIN_LED, server.arg("led").toInt() ? HIGH : LOW);
  sendLightJson();
}

static void handleStatus() { sendLightJson(); }

static void handleHealth() {
  char json[180];
  snprintf(json, sizeof(json),
    "{\"ok\":true,\"clients\":%u,\"frameBytes\":%u,\"freeHeap\":%u,\"freePsram\":%u,\"rssi\":%d}",
    activeClientCount, static_cast<unsigned>(activeFrameSize),
    static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getFreePsram()), WiFi.RSSI());
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

static void handleRoot() {
  server.send(200, "text/plain", "ESP32-S3 camera online. Use /stream, /health");
}

static void http_server_task(void *parameter) {
  streamingClients = xQueueCreate(MAX_STREAM_CLIENTS, sizeof(WiFiClient *));
  if (!streamingClients) {
    Serial.println("[HTTP] Client queue allocation failed");
    vTaskDelete(nullptr);
    return;
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/light", HTTP_GET, handleLight);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/health", HTTP_GET, handleHealth);
  server.onNotFound([]() { server.send(404, "text/plain", "This URI does not exist"); });
  server.begin();
  Serial.println("[HTTP] WebServer ready: /stream /light /status /health");

  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    server.handleClient();
    taskYIELD();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(HTTP_TASK_PERIOD_MS));
  }
}

static void camera_capture_task(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000 / STREAM_FPS);

  for (;;) {
    if (!streamingClients || uxQueueMessagesWaiting(streamingClients) == 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("[CAM] esp_camera_fb_get failed");
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    const uint8_t nextIndex = writeBufferIndex;
    if (ensureFrameCapacity(nextIndex, fb->len)) {
      memcpy(frameBuffers[nextIndex], fb->buf, fb->len);
      xSemaphoreTake(frameMutex, portMAX_DELAY);
      activeFrame = frameBuffers[nextIndex];
      activeFrameSize = fb->len;
      writeBufferIndex = nextIndex ^ 1;
      xSemaphoreGive(frameMutex);
      cameraReady = true;
    }
    esp_camera_fb_return(fb);

    vTaskDelayUntil(&lastWake, period);
  }
}

static void stream_broadcast_task(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    if (!streamingClients || uxQueueMessagesWaiting(streamingClients) == 0 || !cameraReady) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    WiFiClient *client = nullptr;
    if (xQueueReceive(streamingClients, &client, 0) != pdPASS || !client) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    bool keep = client->connected();
    size_t size = 0;
    if (keep) {
      xSemaphoreTake(frameMutex, portMAX_DELAY);
      if (activeFrame && activeFrameSize > 0 && ensureBroadcastCapacity(activeFrameSize)) {
        size = activeFrameSize;
        memcpy(broadcastBuffer, activeFrame, size);
      }
      xSemaphoreGive(frameMutex);

      char lengthLine[24];
      snprintf(lengthLine, sizeof(lengthLine), "%u\r\n\r\n", static_cast<unsigned>(size));
      if (size == 0 ||
          client->write(reinterpret_cast<const uint8_t *>(STREAM_CONTENT_TYPE), strlen(STREAM_CONTENT_TYPE)) != strlen(STREAM_CONTENT_TYPE) ||
          client->write(reinterpret_cast<const uint8_t *>(lengthLine), strlen(lengthLine)) != strlen(lengthLine) ||
          client->write(broadcastBuffer, size) != size ||
          client->write(reinterpret_cast<const uint8_t *>(STREAM_BOUNDARY), strlen(STREAM_BOUNDARY)) != strlen(STREAM_BOUNDARY)) {
        keep = false;
      }
    }

    if (keep && client->connected() && xQueueSend(streamingClients, &client, 0) == pdPASS) {
      // Slot stays reserved while the client is queued or being sent.
    } else {
      client->stop();
      delete client;
      releaseClientSlot();
      Serial.printf("[STREAM] Client dropped; active=%u\n", activeClientCount);
    }

    const UBaseType_t clients = uxQueueMessagesWaiting(streamingClients);
    const TickType_t slice = clients ? pdMS_TO_TICKS(1000 / STREAM_FPS / clients) : pdMS_TO_TICKS(10);
    vTaskDelayUntil(&lastWake, slice > 0 ? slice : 1);
  }
}

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] Connecting to %s", AP_SSID);
  for (int tries = 0; tries < 40 && WiFi.status() != WL_CONNECTED; ++tries) {
    delay(500);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin("esp32-cam");
  } else {
    Serial.println("\n[WiFi] Failed; will retry");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[CAM] ESP32-S3 multi-client camera booting");

  if (!psramFound()) {
    Serial.println("[CAM] PSRAM is required for multi-client streaming; check the DFR1154 board definition");
    return;
  }
  Serial.printf("[CAM] PSRAM ready: %u bytes\n", static_cast<unsigned>(ESP.getPsramSize()));

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.fb_count = 2;
  config.frame_size = FRAMESIZE_QVGA;
  config.jpeg_quality = 20;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("[CAM] Camera init failed");
    return;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  sensor->set_framesize(sensor, FRAMESIZE_QVGA);
  sensor->set_vflip(sensor, 1);
  sensor->set_hmirror(sensor, 0);
  sensor->set_brightness(sensor, 1);
  sensor->set_contrast(sensor, 1);
  sensor->set_saturation(sensor, 0);
  sensor->set_exposure_ctrl(sensor, 1);
  sensor->set_aec2(sensor, 0);
  sensor->set_agc_gain(sensor, 0);
  sensor->set_gain_ctrl(sensor, 1);
  sensor->set_gainceiling(sensor, static_cast<gainceiling_t>(2));
  sensor->set_ae_level(sensor, 1);

  pinMode(PIN_IR, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_IR, LOW);
  digitalWrite(PIN_LED, LOW);

  WiFi.setHostname("esp32-cam");
  WiFi.setSleep(false);
  connectWiFi();

  frameMutex = xSemaphoreCreateMutex();
  if (!frameMutex) {
    Serial.println("[CAM] Frame mutex allocation failed");
    return;
  }

  xTaskCreatePinnedToCore(http_server_task, "cam_http", 4096, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(camera_capture_task, "cam_capture", 6144, nullptr, 2, &cameraTaskHandle, 1);
  xTaskCreatePinnedToCore(stream_broadcast_task, "cam_stream", 6144, nullptr, 2, &streamTaskHandle, 1);
  Serial.println("[CAM] Ready");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Connection lost; reconnecting");
    connectWiFi();
  }
  delay(5000);
}
