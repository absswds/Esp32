// ESP32-S3 AI Camera → MJPEG stream server
// Connects to user's WiFi AP, serves MJPEG stream + single-frame capture + light control
// Board: DFRobot FireBeetle 2 ESP32-S3 AI Camera v1.1

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// === WiFi ===
const char* AP_SSID = "OPhone 12";
const char* AP_PASS = "qwer1234";

// === DFRobot DFR1154 ESP32-S3 AI Camera pinout (OV3660) ===
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

WebServer server(80);

// Stream task runs on core 0 — does NOT block WebServer on core 1
volatile bool streamActive = false;

void streamTask(void *arg) {
  WiFiClient client = *(WiFiClient*)arg;
  client.setTimeout(5);
  client.print("HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\nAccess-Control-Allow-Origin: *\r\n\r\n");

  streamActive = true;
  Serial.println("[STREAM] Client connected");

  while (client.connected() && streamActive) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { delay(5); continue; }

    client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
    size_t written = client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);

    if (written != fb->len) {
      Serial.println("[STREAM] Write mismatch, closing");
      break;
    }

    // ~15 fps max, yield to other tasks
    vTaskDelay(pdMS_TO_TICKS(33));
  }

  client.stop();
  streamActive = false;
  Serial.println("[STREAM] Client disconnected");
  vTaskDelete(NULL);
}

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

  // CIF 400x296 — 比 VGA 小 4 倍，每幀 ~8-15KB，編碼 ~15ms
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_CIF;   // 400x296
    config.jpeg_quality = 15;              // 0=best 63=worst, 15=good balance
    Serial.println("[CAM] PSRAM found, CIF mode (400x296)");
  } else {
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240
    config.jpeg_quality = 18;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    Serial.println("[CAM] No PSRAM, QVGA mode (320x240)");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init FAILED: 0x%x\n", err);
    return;
  }
  Serial.println("[CAM] Init OK");

  // Sensor tuning — 穩定自動曝光，減少運動時抖動
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);
  // 固定白平衡模式 0=auto，但可試 1=sunny 減少切換延遲
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);  // 0=auto
  // 自動曝光穩定
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);      // DSP auto exposure
  s->set_gain_ctrl(s, 1);  // auto gain
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, (gainceiling_t)6);

  // WiFi
  WiFi.setHostname("esp32-cam");
  WiFi.begin(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] Connecting to %s", AP_SSID);
  for (int tries = 0; tries < 40; tries++) {
    if (WiFi.status() == WL_CONNECTED) break;
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] FAILED");
    return;
  }
  Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());

  if (MDNS.begin("esp32-cam")) {
    Serial.println("[MDNS] esp32-cam.local ready");
  }

  // HTTP routes

  // Single-frame capture (always works, even when stream is active)
  server.on("/capture", HTTP_GET, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { server.send(500, "text/plain", "Capture failed"); return; }
    server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
    server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
  });

  // MJPEG stream — spawns FreeRTOS task on core 0
  server.on("/stream", HTTP_GET, []() {
    if (streamActive) {
      server.send(503, "text/plain", "Stream busy");
      return;
    }
    WiFiClient client = server.client();
    // Detach from WebServer — task will own this client
    xTaskCreatePinnedToCore(streamTask, "stream", 4096, new WiFiClient(client), 1, NULL, 0);
    // Don't send response — streamTask does it
  });

  // IR + LED
  pinMode(PIN_IR, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_IR, LOW);
  digitalWrite(PIN_LED, LOW);

  server.on("/light", HTTP_GET, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    if (server.hasArg("ir"))   digitalWrite(PIN_IR,   server.arg("ir").toInt() ? HIGH : LOW);
    if (server.hasArg("led"))  digitalWrite(PIN_LED,  server.arg("led").toInt() ? HIGH : LOW);
    String json = "{\"ir\":" + String(digitalRead(PIN_IR)) + ",\"led\":" + String(digitalRead(PIN_LED)) + "}";
    server.send(200, "application/json", json);
  });

  server.on("/status", HTTP_GET, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String json = "{\"ir\":" + String(digitalRead(PIN_IR)) + ",\"led\":" + String(digitalRead(PIN_LED)) + "}";
    server.send(200, "application/json", json);
  });

  server.begin();
  Serial.println("[CAM] HTTP server started");
}

void loop() {
  server.handleClient();
  delay(1);
}
