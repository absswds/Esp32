// ESP32-S3 AI Camera → MJPEG stream server using esp_http_server
// Each client connection gets its own task (free-threaded mode)
// Board: DFRobot FireBeetle 2 ESP32-S3 AI Camera v1.1

#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_http_server.h>

// === WiFi ===
const char* AP_SSID = "OPhone 12";
const char* AP_PASS = "qwer1234";

#define PIN_IR   47
#define PIN_LED   3

static httpd_handle_t server = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ================== Stream Handler ==================

static esp_err_t stream_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  Serial.printf("[STREAM] Client connected (fd=%d)\n", httpd_req_to_sockfd(req));

  unsigned long lastSend = millis();

  while (true) {
    // Timeout: disconnect after 30s of no frame sent
    if (millis() - lastSend > 30000) {
      Serial.printf("[STREAM] Client %d timeout\n", httpd_req_to_sockfd(req));
      break;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      delay(5);
      continue;
    }

    char part_buf[64];
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);

    if (httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY)) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK) break;
    if (httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len) != ESP_OK) break;

    esp_camera_fb_return(fb);
    lastSend = millis();

    // Yield to other tasks between frames
    delay(1);
  }

  Serial.printf("[STREAM] Client %d disconnected\n", httpd_req_to_sockfd(req));
  return ESP_OK;
}

// ================== Capture Handler ==================

static esp_err_t capture_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_sendstr(req, "fail"); return ESP_FAIL; }
  httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return ESP_OK;
}

// ================== IR / LED ==================

static esp_err_t light_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  char query[100];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char param[16];
    if (httpd_query_key_value(query, "ir", param, sizeof(param)) == ESP_OK)
      digitalWrite(PIN_IR, atoi(param) ? HIGH : LOW);
    if (httpd_query_key_value(query, "led", param, sizeof(param)) == ESP_OK)
      digitalWrite(PIN_LED, atoi(param) ? HIGH : LOW);
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

// ================== Start Server ==================

static void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_open_sockets = 10;
  config.max_uri_handlers = 10;
  config.lru_purge_enable = true;

  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler };
  httpd_uri_t light_uri = { .uri = "/light", .method = HTTP_GET, .handler = light_handler };
  httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler };

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
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    if (server) { httpd_stop(server); server = NULL; }
    startCameraServer();
    MDNS.begin("esp32-cam");
  } else {
    Serial.println("\n[WiFi] FAILED — will retry in loop()");
  }
}

// ================== Setup ==================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[CAM] ESP32-S3 AI Camera booting...");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = 16;
  config.pin_d1       = 18;
  config.pin_d2       = 21;
  config.pin_d3       = 17;
  config.pin_d4       = 14;
  config.pin_d5       = 7;
  config.pin_d6       = 6;
  config.pin_d7       = 4;
  config.pin_xclk     = 5;
  config.pin_pclk     = 15;
  config.pin_vsync    = 1;
  config.pin_href     = 2;
  config.pin_sccb_sda = 8;
  config.pin_sccb_scl = 9;
  config.pin_pwdn     = -1;
  config.pin_reset    = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.fb_count     = 2;
  config.frame_size   = FRAMESIZE_UXGA;
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
  if (err != ESP_OK) { Serial.printf("[CAM] Init FAILED: 0x%x\n", err); return; }
  Serial.println("[CAM] Init OK");

  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 0);
  s->set_agc_gain(s, 0);
  s->set_gain_ctrl(s, 1);
  s->set_gainceiling(s, (gainceiling_t)2);
  s->set_ae_level(s, 1);

  pinMode(PIN_IR, OUTPUT); digitalWrite(PIN_IR, LOW);
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);

  WiFi.setHostname("esp32-cam");
  WiFi.setSleep(false);
  connectWiFi();

  Serial.println("[CAM] Ready");
}

// ================== Loop ==================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[LOOP] WiFi lost, reconnecting...");
    connectWiFi();
  }
  delay(5000);
}
