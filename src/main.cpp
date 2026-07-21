#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

#define FAN_PIN 25
#define FAN_CHANNEL 0
#define FAN_FREQ 25000
#define FAN_RES 8

Adafruit_BME680 bme;
WebServer server(80);

float t = NAN, h = NAN, p = NAN, g = NAN;
int aqi = 0;
bool fanAuto = true;
int fanSpeed = 0;
float target = 25.0;
int failCount = 0;
unsigned long lastRead = 0;
unsigned long lastReinit = 0;

void setFan(int s) {
  fanSpeed = constrain(s, 0, 255);
  ledcWrite(FAN_CHANNEL, fanSpeed);
}

void checkFan() {
  if (!fanAuto || isnan(t)) return;
  setFan(t > target ? 255 : 0);
}

int calcAqi(float gas) {
  if (gas >= 1000) return 0;
  if (gas >= 800)  return map((int)gas, 800, 1000, 50, 0);
  if (gas >= 600)  return map((int)gas, 600, 800, 100, 51);
  if (gas >= 400)  return map((int)gas, 400, 600, 200, 101);
  if (gas >= 200)  return map((int)gas, 200, 400, 300, 201);
  return map((int)max(0.0f,gas), 0, 200, 500, 301);
}

void readSensor() {
  if (!bme.begin(0x77)) {
    Serial.println("BME688 not found");
    return;
  }
  if (!bme.performReading()) {
    failCount++;
    Serial.printf("read fail %d/5\n", failCount);
    if (failCount >= 5) {
      t = h = p = g = NAN; aqi = 0;
      lastReinit = millis();
    }
    return;
  }
  failCount = 0;
  t = bme.temperature;
  h = bme.humidity;
  p = bme.pressure / 100.0;
  g = bme.gas_resistance / 1000.0;
  aqi = calcAqi(g);
  checkFan();
  Serial.printf("T:%.1f H:%.1f P:%.1f G:%.1f AQI:%d Fan:%d\n", t, h, p, g, aqi, fanSpeed);
}

void sendJson(const char* msg) {
  String j = "{\"ok\":false,\"message\":\"";
  j += msg;
  j += "\"}";
  server.send(200, "application/json", j);
}

void handleData() {
  if (isnan(t)) { sendJson("無感測資料"); return; }
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"gas\":%.2f,\"aqi\":%d,\"fanSpeed\":%d,\"auto\":%s,\"target\":%.1f}",
    t, h, p, g, aqi, fanSpeed, fanAuto ? "true" : "false", target);
  server.send(200, "application/json", buf);
}

void handleFan() {
  if (server.hasArg("speed")) {
    fanAuto = false;
    setFan(server.arg("speed").toInt() * 255 / 100);
  }
  if (server.hasArg("auto")) {
    fanAuto = server.arg("auto").toInt() == 1;
    checkFan();
  }
  if (server.hasArg("target")) {
    target = server.arg("target").toFloat();
    checkFan();
  }
  server.send(200, "text/plain", "OK");
}

const char INDEX[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>BME688 環境監測</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#0f172a;color:#e2e8f0;padding:16px;max-width:600px;margin:0 auto}
h1{font-size:1.2rem;margin-bottom:12px;text-align:center}
.card{background:#1e293b;border-radius:12px;padding:16px;margin-bottom:12px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.val{font-size:1.8rem;font-weight:700;text-align:center}
.lbl{font-size:.8rem;color:#94a3b8;text-align:center;margin-top:4px}
.status{text-align:center;font-size:.9rem;color:#94a3b8;margin-bottom:12px}
.status.err{color:#f87171}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#22c55e;margin-right:6px;vertical-align:middle}
.dot.err{background:#f87171}
.fan{display:flex;gap:12px;align-items:center;flex-wrap:wrap}
input[type=range]{flex:1;min-width:120px}
button{background:#3b82f6;color:#fff;border:none;border-radius:8px;padding:8px 16px;cursor:pointer;font-size:.9rem}
button.on{background:#22c55e}
.hist-wrap{margin-top:8px;overflow-x:auto}
table{width:100%;border-collapse:collapse;font-size:.85rem}
th,td{padding:4px 8px;text-align:center;border-bottom:1px solid #334155;white-space:nowrap}
th{color:#94a3b8}
</style>
</head>
<body>
<h1>BME688 環境監測</h1>
<div class="status"><span class="dot" id="dot"></span><span id="st">等待連線...</span></div>
<div class="card grid">
  <div><div class="val" id="t">--</div><div class="lbl">溫度 °C</div></div>
  <div><div class="val" id="h">--</div><div class="lbl">濕度 %</div></div>
  <div><div class="val" id="p">--</div><div class="lbl">氣壓 hPa</div></div>
  <div><div class="val" id="g">--</div><div class="lbl">氣體 kΩ</div></div>
  <div><div class="val" id="a">--</div><div class="lbl">AQI</div></div>
  <div><div class="val" id="f">--</div><div class="lbl">風扇 %</div></div>
</div>
<div class="card">
  <div style="margin-bottom:8px;font-weight:600;">風扇控制</div>
  <div class="fan">
    <button id="autoBtn" class="on" onclick="toggleAuto()">自動</button>
    <input type="range" min="0" max="100" value="0" id="slider" oninput="setManual(this.value)">
    <span id="fanPct" style="min-width:40px;text-align:right">0%</span>
  </div>
</div>
<div class="card">
  <div style="margin-bottom:8px;font-weight:600;">歷史資料</div>
  <div class="hist-wrap"><table id="hist"><thead><tr><th>時間</th><th>溫度</th><th>濕度</th><th>氣壓</th><th>氣體</th><th>AQI</th></tr></thead><tbody></tbody></table></div>
  <button onclick="clearHist()" style="margin-top:8px;background:#475569">清除</button>
  <button onclick="exportCSV()" style="margin-top:8px;margin-left:4px;background:#475569">匯出CSV</button>
</div>
<script>
var hist=[];
var autoMode=true;

async function poll(){
  try{
    var r=await fetch('/data');
    var d=await r.json();
    if(!d.ok){document.getElementById('st').textContent=d.message;document.getElementById('dot').className='dot err';return;}
    document.getElementById('dot').className='dot';
    document.getElementById('st').textContent='最後更新 '+new Date().toLocaleTimeString();
    document.getElementById('t').textContent=d.temperature.toFixed(1);
    document.getElementById('h').textContent=d.humidity.toFixed(1);
    document.getElementById('p').textContent=d.pressure.toFixed(1);
    document.getElementById('g').textContent=d.gas.toFixed(1);
    document.getElementById('a').textContent=d.aqi;
    document.getElementById('f').textContent=Math.round(d.fanSpeed/255*100)+'%';
    autoMode=d.auto;
    document.getElementById('autoBtn').className=autoMode?'on':'';
    document.getElementById('slider').value=Math.round(d.fanSpeed/255*100);
    document.getElementById('fanPct').textContent=Math.round(d.fanSpeed/255*100)+'%';
    var now=new Date().toLocaleTimeString();
    hist.unshift({time:now,t:d.temperature,h:d.humidity,p:d.pressure,g:d.gas,a:d.aqi});
    if(hist.length>30)hist.pop();
    renderHist();
  }catch(e){
    document.getElementById('st').textContent='更新失敗';
    document.getElementById('dot').className='dot err';
  }
}

function renderHist(){
  var tb=document.querySelector('#hist tbody');
  tb.innerHTML=hist.map(function(r){
    return '<tr><td>'+r.time+'</td><td>'+r.t.toFixed(1)+'</td><td>'+r.h.toFixed(1)+'</td><td>'+r.p.toFixed(1)+'</td><td>'+r.g.toFixed(1)+'</td><td>'+r.a+'</td></tr>';
  }).join('');
}

function toggleAuto(){
  autoMode=!autoMode;
  fetch('/fan?auto='+(autoMode?1:0));
  document.getElementById('autoBtn').className=autoMode?'on':'';
}

function setManual(v){
  if(autoMode)return;
  document.getElementById('fanPct').textContent=v+'%';
  fetch('/fan?speed='+v);
}

function clearHist(){hist=[];renderHist();}

function exportCSV(){
  if(!hist.length){alert('尚無資料');return;}
  var csv='\uFEFF時間,溫度,濕度,氣壓,氣體,AQI\n';
  csv+=hist.map(function(r){return r.time+','+r.t+','+r.h+','+r.p+','+r.g+','+r.a;}).join('\n');
  var b=new Blob([csv],{type:'text/csv'});
  var a=document.createElement('a');a.href=URL.createObjectURL(b);
  a.download='BME688.csv';a.click();
}

poll();
setInterval(poll,2000);
</script>
</body>
</html>)HTML";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin();

  ledcSetup(FAN_CHANNEL, FAN_FREQ, FAN_RES);
  ledcAttachPin(FAN_PIN, FAN_CHANNEL);
  setFan(0);

  WiFi.softAP("ESP32-BME688", "12345678");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (bme.begin(0x77)) {
    bme.setTemperatureOversampling(BME680_OS_2X);
    bme.setHumidityOversampling(BME680_OS_1X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
    Serial.println("BME688 OK");
  } else {
    Serial.println("BME688 fail");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/fan", handleFan);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("Server started");
}

void loop() {
  server.handleClient();
  if (millis() - lastRead >= 2000) {
    lastRead = millis();
    if (failCount >= 5 && millis() - lastReinit < 30000) {
      // cooldown
    } else {
      readSensor();
    }
  }
}