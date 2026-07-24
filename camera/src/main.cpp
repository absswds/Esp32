// ESP32-S3 AI Camera → JPEG capture server (non-blocking, multi-client)
// Connects to ESP32-TEMP WiFi AP, serves camera stream
// Board: DFRobot FireBeetle 2 ESP32-S3 AI Camera v1.1

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>

// === WiFi ===
const char* AP_SSID = "ESP32-TEMP";
const char* AP_PASS = "12345678";

// === DFRobot DFR1154 ESP32-S3 AI Camera pinout (OV3660) ===
// Source: https://github.com/DFRobot/DFR1154_Examples
#define CAM_PIN_pwdn    -1
#define CAM_PIN_reset   -1
#define CAM_PIN_xclk     5
#define CAM_PIN_sccb_sda  8
#define CAM_PIN_sccb_scl  9
#define CAM_PIN_d7       4   // Y9
#define CAM_PIN_d6       6   // Y8
#define CAM_PIN_d5       7   // Y7
#define CAM_PIN_d4      14   // Y6
#define CAM_PIN_d3      17   // Y5
#define CAM_PIN_d2      21   // Y4
#define CAM_PIN_d1      18   // Y3
#define CAM_PIN_d0      16   // Y2
#define CAM_PIN_vsync    1
#define CAM_PIN_href     2
#define CAM_PIN_pclk    15

// On-board peripherals
#define PIN_IR   47   // IR fill light (active HIGH)
#define PIN_LED   3   // White LED (active HIGH)

WebServer server(80);

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

  // PSRAM → higher resolution; no PSRAM → QVGA
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;  // 640x480
    config.jpeg_quality = 12;
    Serial.println("[CAM] PSRAM found, VGA mode");
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    Serial.println("[CAM] No PSRAM, QVGA mode");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init FAILED: 0x%x — 检查 pinout!\n", err);
    Serial.println("[CAM] 对照 DFRobot 板子絲印修改 CAM_PIN_* 定义");
    return;
  }
  Serial.println("[CAM] Init OK");

  // Sensor tuning
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);

  // WiFi — join ESP32-TEMP AP
  // Static IP so dashboard knows where to find us
  IPAddress camIP(192, 168, 4, 2);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.config(camIP, gateway, subnet);

  WiFi.begin(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] Connecting to %s", AP_SSID);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[WiFi] FAILED — check AP is running");
    return;
  }
  Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());

  // HTTP routes
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>ESP32-S3 Camera</title>"
      "<style>body{margin:0;background:#1a1a2e;color:#e0e0e0;font-family:system-ui;display:flex;flex-direction:column;align-items:center;padding:10px}"
      "img{max-width:100%;border-radius:8px;border:2px solid #334155}"
      "h2{margin:10px 0}</style></head><body>"
      "<h2>ESP32-S3 AI Camera</h2>"
      "<img id='cam' src='/capture' onload=\"setTimeout(function(){document.getElementById('cam').src='/capture?r='+Date.now()},300)\"></body></html>";
    server.send(200, "text/html", html);
  });

  // Single-frame capture (non-blocking, supports multiple clients)
  server.on("/capture", HTTP_GET, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      server.send(500, "text/plain", "Capture failed");
      return;
    }
    server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
    server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);
  });

  // IR + LED control (CORS headers for cross-origin from dashboard)
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
