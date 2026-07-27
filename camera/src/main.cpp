// ESP32-S3 AI Camera → MJPEG stream server using esp_http_server (non-blocking)
// Board: DFRobot FireBeetle 2 ESP32-S3 AI Camera v1.1

#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_http_server.h>

// === WiFi ===
const char* AP_SSID = "OPhone 12";
const char* AP_PASS = "qwer1234";

// === Pinout (DFRobot DFR1154 ESP32-S3 AI Camera / OV3660) ===
#define CAM_PIN_pwdn    -1
#define CAM_PIN_reset   -1
#define CAM_PIN_xclk     5
#define CAM_PIN_sccb_sda  8
#define CAM_PIN_sccb_scl  9
#define CAM_PIN_d7       4
#define CAM_PIN_d6       6
#define CAM_PIN_d5       7
#define CAM_PIN_d4      14
#define CAM_PIN_d3      17
#define CAM_PIN_d2      21
#define CAM_PIN_d1      18
#define CAM_PIN_d0      16
#define CAM_PIN_vsync    1
#define CAM_PIN_href     2
#define CAM_PIN_pclk    15

#define PIN_IR   47
#define PIN_LED   3

static httpd_handle_t server = NULL;

// ================== MJPEG Stream Handler ==================

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  Serial.println("[STREAM] Client connected");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      delay(5);
      continue;
    }

    char part_buf[64];
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);

    // Non-blocking chunked send — returns error on disconnect
    if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len) != ESP_OK) break;

    esp_camera_fb_return(fb);
  }

  Serial.println("[STREAM] Client disconnected");
  return ESP_OK;
}

// ================== Capture Handler ==================

static esp_err_t capture_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_sendstr(req, "Capture failed");
    return ESP_FAIL;
  }
  httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return ESP_OK;
}

// ================== IR / LED Handlers ==================

static esp_err_t light_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char query[100];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char param[16];
    if (httpd_query_key_value(query, "ir", param, sizeof(param)) == ESP_OK) {
      digitalWrite(PIN_IR, atoi(param) ? HIGH : LOW);
    }
    if (httpd_query_key_value(query, "led", param, sizeof(param)) == ESP_OK) {
      digitalWrite(PIN_LED, atoi(param) ? HIGH : LOW);
    }
  }

  char json[64];
  snprintf(json, sizeof(json), "{\"ir\":%d,\"led\":%d}", digitalRead(PIN_IR), digitalRead(PIN_LED));
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char json[64];
  snprintf(json, sizeof(json), "{\"ir\":%d,\"led\":%d}", digitalRead(PIN_IR), digitalRead(PIN_LED));
  httpd_resp_sendstr(req, json);
  return ESP_OK;
}

// ================== Start HTTP Server ==================

static void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = 10;     // 支援多客戶端同時串流
  config.max_uri_handlers = 10;
  config.lru_purge_enable = true;

  // Stream URI (priority)
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t capture_uri = {
    .uri       = "/capture",
    .method    = HTTP_GET,
    .handler   = capture_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t light_uri = {
    .uri       = "/light",
    .method    = HTTP_GET,
    .handler   = light_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t status_uri = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &stream_uri);
    httpd_register_uri_handler(server, &capture_uri);
    httpd_register_uri_handler(server, &light_uri);
    httpd_register_uri_handler(server, &status_uri);
    Serial.println("[HTTP] Server started on port 80");
  } else {
    Serial.println("[HTTP] Failed to start server!");
  }
}

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.begin(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] Connecting to %s", AP_SSID);
  for (int tries = 0; tries < 40; tries++) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    if (server) {
      httpd_stop(server);
      server = NULL;
    }
    startCameraServer();
    MDNS.begin("esp32-cam");
  } else {
    Serial.println("\n[WiFi] FAILED — will retry in loop()");
  }
}

// ================== Setup ==================

void setup() {
  Serial.begin(115200);
  Serial.println("\n[CAM] ESP32-S3 AI Camera booting...");

  // Camera config
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = CAM_PIN_d0;
  config.pin_d1       = CAM_PIN_d1;
  config.pin_d2       = CAM_PIN_d2;
  config.pin_d3       = CAM_PIN_d3;
  config.pin_d4       = CAM_PIN_d4;
  config.pin_d5       = CAM_PIN_d5;
  config.pin_d6       = CAM_PIN_d6;
  config.pin_d7       = CAM_PIN_d7;
  config.pin_xclk     = CAM_PIN_xclk;
  config.pin_pclk     = CAM_PIN_pclk;
  config.pin_vsync    = CAM_PIN_vsync;
  config.pin_href     = CAM_PIN_href;
  config.pin_sccb_sda = CAM_PIN_sccb_sda;
  config.pin_sccb_scl = CAM_PIN_sccb_scl;
  config.pin_pwdn     = CAM_PIN_pwdn;
  config.pin_reset    = CAM_PIN_reset;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.fb_count     = 2;
  config.frame_size   = FRAMESIZE_UXGA;     // 大 buffer 預分配
  config.jpeg_quality = 20;

  if (!psramFound()) {
    config.frame_size  = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count    = 1;
    Serial.println("[CAM] No PSRAM, QVGA DRAM mode");
  } else {
    Serial.println("[CAM] PSRAM found, UXGA buffer → QVGA stream");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init FAILED: 0x%x\n", err);
    return;
  }
  Serial.println("[CAM] Init OK");

  // Sensor: 啟動後降到 QVGA 提高幀率
  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);  // 320x240
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);
  // AEC/AGC 穩定：防止運動時曝光突跳導致卡頓
  s->set_exposure_ctrl(s, 1);     // 開 auto exposure
  s->set_aec2(s, 0);              // 關閉 DSP auto exposure (減少抖動)
  s->set_agc_gain(s, 0);          // 固定增益
  s->set_gain_ctrl(s, 1);         // auto gain
  s->set_gainceiling(s, (gainceiling_t)2); // gain ceiling 2x
  s->set_ae_level(s, 1);          // 曝光補償 +1

  // WiFi
  WiFi.setHostname("esp32-cam");
  WiFi.setSleep(false);
  connectWiFi();

  // IR / LED 初始關閉
  pinMode(PIN_IR, OUTPUT); digitalWrite(PIN_IR, LOW);
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);

  startCameraServer();
}

// loop — 檢查 WiFi 狀態，離線時自動重連
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[LOOP] WiFi lost, reconnecting...");
    connectWiFi();
  }
  delay(5000);
}
