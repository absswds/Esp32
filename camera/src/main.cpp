// ESP32-S3 AI Camera — reliable JPEG polling + direct IR/LED control
// Board: DFRobot DFR1154 ESP32-S3 AI Camera (OV3660)
// Design: one short /capture request per frame. No never-ending stream handler,
// so /light always gets a turn in WebServer::handleClient().

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// === WiFi ===
const char* AP_SSID = "OPhone 12";
const char* AP_PASS = "qwer1234";

// === DFRobot DFR1154 camera pinout ===
#define CAM_PIN_pwdn    -1
#define CAM_PIN_reset   -1
#define CAM_PIN_xclk     5
#define CAM_PIN_sccb_sda 8
#define CAM_PIN_sccb_scl 9
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

// On-board fill lights — active HIGH (DFRobot DFR1154 pinout)
#define PIN_IR  47
#define PIN_LED  3

WebServer server(80);

static void sendLightState() {
  String json = "{\"ir\":" + String(digitalRead(PIN_IR)) +
                ",\"led\":" + String(digitalRead(PIN_LED)) + "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

static void startCameraServer() {
  server.on("/capture", HTTP_GET, []() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      server.send(503, "text/plain", "Capture failed");
      return;
    }
    server.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
    server.send_P(200, "image/jpeg", reinterpret_cast<const char*>(fb->buf), fb->len);
    esp_camera_fb_return(fb);
  });

  // Light controls are intentionally on the same short-request server.
  // No long-lived stream handler can starve this route.
  server.on("/light", HTTP_GET, []() {
    if (server.hasArg("ir")) {
      digitalWrite(PIN_IR, server.arg("ir").toInt() ? HIGH : LOW);
    }
    if (server.hasArg("led")) {
      digitalWrite(PIN_LED, server.arg("led").toInt() ? HIGH : LOW);
    }
    sendLightState();
  });

  server.on("/status", HTTP_GET, []() { sendLightState(); });

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "DFRobot ESP32-S3 Camera: /capture /light /status");
  });

  server.begin();
  Serial.println("[HTTP] JPEG polling/control server started on port 80");
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] Connecting to %s", AP_SSID);
  for (int tries = 0; tries < 40 && WiFi.status() != WL_CONNECTED; ++tries) {
    delay(500);
    Serial.print('.');
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
    if (MDNS.begin("esp32-cam")) Serial.println("[mDNS] esp32-cam.local ready");
  } else {
    Serial.println("\n[WiFi] FAILED — retrying from loop");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[CAM] DFR1154 reliable polling camera booting...");

  // Start the light pins before camera/WiFi, so their default state is known.
  pinMode(PIN_IR, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_IR, LOW);
  digitalWrite(PIN_LED, LOW);

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_d0;
  config.pin_d1 = CAM_PIN_d1;
  config.pin_d2 = CAM_PIN_d2;
  config.pin_d3 = CAM_PIN_d3;
  config.pin_d4 = CAM_PIN_d4;
  config.pin_d5 = CAM_PIN_d5;
  config.pin_d6 = CAM_PIN_d6;
  config.pin_d7 = CAM_PIN_d7;
  config.pin_xclk = CAM_PIN_xclk;
  config.pin_pclk = CAM_PIN_pclk;
  config.pin_vsync = CAM_PIN_vsync;
  config.pin_href = CAM_PIN_href;
  config.pin_sccb_sda = CAM_PIN_sccb_sda;
  config.pin_sccb_scl = CAM_PIN_sccb_scl;
  config.pin_pwdn = CAM_PIN_pwdn;
  config.pin_reset = CAM_PIN_reset;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 20;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    Serial.printf("[CAM] PSRAM OK: %u bytes, QVGA/JPEG20/2 buffers\n", ESP.getPsramSize());
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 25;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("[CAM] WARNING: no PSRAM; QVGA/JPEG25/1 DRAM buffer");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init FAILED: 0x%x\n", err);
    return;
  }
  Serial.println("[CAM] Init OK");

  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  s->set_hmirror(s, 0);
  s->set_brightness(s, 1);
  s->set_contrast(s, 1);
  s->set_saturation(s, 0);

  WiFi.setHostname("esp32-cam");
  WiFi.setSleep(false);
  connectWiFi();
  startCameraServer();
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect >= 5000) {
      lastReconnect = millis();
      Serial.println("[WiFi] Lost; reconnecting...");
      WiFi.disconnect();
      connectWiFi();
    }
  }
  delay(1);
}
