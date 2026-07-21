#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

#define FAN_PIN 18
#define TEC_EN 19
#define TEC_LPWM 25  // 制冷
#define TEC_RPWM 26  // 加热

#define PWM_FREQ 25000
#define PWM_RES 8

Adafruit_BME680 bme;
WebServer server(80);

float t = NAN, h = NAN, p = NAN, g = NAN;
int aqi = 0;
int fanSpeed = 0;
float coolStop = 26.0;
float heatStop = 28.0;
unsigned long lastRead = 0;

bool systemOn = false;
bool cooling = false;
bool heating = false;
bool bmeOk = false;

const float SAFE_MIN = 10.0;
const float SAFE_MAX = 40.0;

void setFan(int s) {
  fanSpeed = constrain(s, 0, 255);
  ledcWrite(FAN_PIN, fanSpeed);
}

void setTec(int cool, int heat) {
  cooling = cool > 0;
  heating = heat > 0;
  ledcWrite(TEC_LPWM, cool);
  ledcWrite(TEC_RPWM, heat);
  Serial.printf("TEC: L=%d R=%d EN=%d\n", cool, heat, digitalRead(TEC_EN));
}

void stopAll() {
  setFan(0);
  setTec(0, 0);
  digitalWrite(TEC_EN, LOW);
}

void startAll() {
  digitalWrite(TEC_EN, HIGH);
  setFan(100);
  Serial.println("System ON");
}

void controlTemp() {
  if (!systemOn || isnan(t)) return;

  if (t < SAFE_MIN || t > SAFE_MAX) {
    setTec(0, 0);
    setFan(t > SAFE_MAX ? 255 : 60);
    Serial.println("SAFE: extreme temp, TEC disabled");
    return;
  }

  if (t >= heatStop) {
    setTec(220, 0);
    setFan(255);
  } else if (t <= coolStop) {
    setTec(0, 200);
    setFan(150);
  } else {
    setTec(0, 0);
    setFan(100);
  }
}

int calcAqi(float gas) {
  if (gas >= 1000) return 0;
  if (gas >= 800)  return map((int)gas, 800, 1000, 50, 0);
  if (gas >= 600)  return map((int)gas, 600, 800, 100, 51);
  if (gas >= 400)  return map((int)gas, 400, 600, 200, 101);
  if (gas >= 200)  return map((int)gas, 200, 400, 300, 201);
  return map((int)max(0.0f, gas), 0, 200, 500, 301);
}

void readSensor() {
  if (!bmeOk) return;
  if (!bme.performReading()) {
    Serial.println("read fail");
    bmeOk = false;
    return;
  }
  t = bme.temperature;
  h = bme.humidity;
  p = bme.pressure / 100.0;
  g = bme.gas_resistance / 1000.0;
  aqi = calcAqi(g);
  controlTemp();
  Serial.printf("T:%.1f H:%.1f P:%.1f G:%.1f AQI:%d Fan:%d Cool:%d Heat:%d\n",
                t, h, p, g, aqi, fanSpeed, cooling, heating);
}

void sendJson(const char* msg) {
  String j = "{\"ok\":false,\"message\":\"";
  j += msg;
  j += "\"}";
  server.send(200, "application/json", j);
}

void handleData() {
  if (!bmeOk) { sendJson("BME688 未連線"); return; }
  if (isnan(t)) { sendJson("等待感測資料..."); return; }
  char buf[320];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"gas\":%.2f,\"aqi\":%d,\"fanSpeed\":%d,\"cooling\":%s,\"heating\":%s,\"systemOn\":%s,\"coolStop\":%.1f,\"heatStop\":%.1f}",
    t, h, p, g, aqi, fanSpeed, cooling ? "true" : "false", heating ? "true" : "false", systemOn ? "true" : "false", coolStop, heatStop);
  server.send(200, "application/json", buf);
}

void handleControl() {
  if (server.hasArg("system")) {
    systemOn = server.arg("system").toInt() == 1;
    if (systemOn) startAll(); else stopAll();
  }
  if (server.hasArg("coolStop")) {
    float v = server.arg("coolStop").toFloat();
    coolStop = constrain(v, SAFE_MIN, heatStop - 0.5);
  }
  if (server.hasArg("heatStop")) {
    float v = server.arg("heatStop").toFloat();
    heatStop = constrain(v, coolStop + 0.5, SAFE_MAX);
  }
  controlTemp();
  server.send(200, "text/plain", "OK");
}

// 硬件测试端点: /test?fan=128&cool=200&heat=0&en=1
void handleTest() {
  int fanVal = server.hasArg("fan") ? server.arg("fan").toInt() : -1;
  int coolVal = server.hasArg("cool") ? server.arg("cool").toInt() : -1;
  int heatVal = server.hasArg("heat") ? server.arg("heat").toInt() : -1;
  int enVal = server.hasArg("en") ? server.arg("en").toInt() : -1;

  if (enVal >= 0) digitalWrite(TEC_EN, enVal ? HIGH : LOW);
  if (fanVal >= 0) setFan(fanVal);
  if (coolVal >= 0 || heatVal >= 0) {
    setTec(max(0, coolVal), max(0, heatVal));
  }

  char buf[128];
  snprintf(buf, sizeof(buf), "{\"en\":%d,\"fan\":%d,\"cool\":%d,\"heat\":%d}",
    digitalRead(TEC_EN), fanSpeed, cooling ? 220 : 0, heating ? 200 : 0);
  server.send(200, "application/json", buf);
  Serial.printf("TEST: EN=%d Fan=%d Cool=%d Heat=%d\n",
    digitalRead(TEC_EN), fanSpeed, cooling, heating);
}

const char INDEX[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>溫控系統</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0f1318;--sf:#161b22;--bd:#2d333b;--tx:#e6edf3;--t2:#8b949e;--t3:#484f58;--r:#f85149;--b:#58a6ff;--g:#3fb950;--a:#d29922;--c:#39d2c0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--tx);padding:16px 20px;max-width:560px;margin:0 auto;line-height:1.5}
.row{display:flex;align-items:center;justify-content:space-between;padding-bottom:12px;border-bottom:1px solid var(--bd);margin-bottom:14px}
.row h1{font-size:1.1rem;font-weight:600}
.row h1 b{color:var(--c)}
.st{display:flex;align-items:center;gap:6px;font-size:.75rem;color:var(--t2)}
.dot{width:7px;height:7px;border-radius:50%;background:var(--g);flex-shrink:0}
.dot.err{background:var(--r)}
.g2{display:grid;grid-template-columns:repeat(2,1fr);gap:8px;margin-bottom:14px}
.mc{background:var(--sf);border:1px solid var(--bd);border-radius:5px;padding:14px 10px;text-align:center}
.mc .v{font-size:1.9rem;font-weight:700;line-height:1}
.mc .l{font-size:.68rem;color:var(--t3);margin-top:5px;text-transform:uppercase;letter-spacing:.5px}
.sec{background:var(--sf);border:1px solid var(--bd);border-radius:5px;padding:16px;margin-bottom:10px}
.sec h2{font-size:.72rem;color:var(--t3);font-weight:600;margin-bottom:12px;text-transform:uppercase;letter-spacing:.8px}
.btn{width:100%;padding:14px;border-radius:5px;border:none;font-size:.95rem;font-weight:700;cursor:pointer}
.btn.on{background:var(--r);color:#fff}
.btn.off{background:var(--g);color:#000}
.btn:active{opacity:.7}
.btn-t{padding:8px 14px;border-radius:4px;border:1px solid var(--bd);background:0 0;color:var(--t2);cursor:pointer;font-size:.8rem;font-weight:500;margin-right:6px;margin-bottom:6px}
.btn-t.on{background:var(--c);color:#000;border-color:var(--c)}
.btn-t:active{opacity:.7}
.sts{display:flex;gap:8px;margin-top:12px;font-size:.78rem}
.sts .pill{flex:1;text-align:center;padding:6px;border-radius:4px;background:var(--bg);border:1px solid var(--bd)}
.sts .pill.act{border-color:var(--c);color:var(--c)}
.sts .pill.hot{border-color:var(--r);color:var(--r)}
.sts .pill.cold{border-color:var(--b);color:var(--b)}
.fld{display:flex;align-items:center;gap:10px;margin-bottom:10px}
.fld label{font-size:.78rem;color:var(--t2);min-width:78px}
.fld input[type=range]{flex:1;height:4px;-webkit-appearance:none;appearance:none;background:var(--bd);border-radius:2px;outline:none}
.fld input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;border-radius:50%;background:var(--c);cursor:pointer}
.fld .val{font-size:.95rem;font-weight:700;color:var(--c);min-width:42px;text-align:right;font-variant-numeric:tabular-nums}
.note{font-size:.72rem;color:var(--t3);line-height:1.6}
.note b{color:var(--t2)}
.tst{display:flex;flex-wrap:wrap;gap:4px;margin-bottom:8px}
</style>
</head>
<body>
<div class="row">
  <h1><b>TEC</b> 溫控系統</h1>
  <div class="st"><span class="dot" id="dot"></span><span id="st">等待連線...</span></div>
</div>
<div class="g2">
  <div class="mc"><div class="v" style="color:var(--r)" id="mT">--</div><div class="l">溫度 °C</div></div>
  <div class="mc"><div class="v" style="color:var(--b)" id="mH">--</div><div class="l">濕度 %</div></div>
  <div class="mc"><div class="v" style="color:var(--g)" id="mP">--</div><div class="l">氣壓 hPa</div></div>
  <div class="mc"><div class="v" style="color:var(--a)" id="mA">--</div><div class="l">AQI</div></div>
</div>
<div class="sec">
  <h2>系統開關</h2>
  <button class="btn off" id="sysBtn" onclick="toggleSys()">開啟系統</button>
  <div class="sts">
    <div class="pill" id="pSys">待機</div>
    <div class="pill cold" id="pCool">製冷 關</div>
    <div class="pill hot" id="pHeat">加熱 關</div>
  </div>
</div>
<div class="sec">
  <h2>溫度設定</h2>
  <div class="fld">
    <label>高溫→製冷</label>
    <input type="range" min="15" max="35" step="0.5" value="28" id="hs" oninput="setHS(this.value)">
    <span class="val" id="hsv">28°C</span>
  </div>
  <div class="fld">
    <label>低溫→加熱</label>
    <input type="range" min="10" max="30" step="0.5" value="26" id="cs" oninput="setCS(this.value)">
    <span class="val" id="csv">26°C</span>
  </div>
</div>
<div class="sec">
  <h2>硬件測試</h2>
  <div class="tst">
    <button class="btn-t" onclick="tTest('fan',150)">風扇 150</button>
    <button class="btn-t" onclick="tTest('fan',255)">風扇 255</button>
    <button class="btn-t" onclick="tTest('fan',0)">風扇 停</button>
    <button class="btn-t" onclick="tTest('cool',200)">製冷 200</button>
    <button class="btn-t" onclick="tTest('heat',200)">加熱 200</button>
    <button class="btn-t" onclick="tTest('off',0)">全部停</button>
  </div>
  <div class="note">
    <p>• 點按鈕直接驅動硬件，跳過溫控邏輯</p>
    <p>• <b>製冷/加熱不會同時開</b>，點了會先關另一邊</p>
  </div>
</div>
<div class="sec">
  <h2>運行說明</h2>
  <div class="note">
    <p>• <b>溫度＞<span id="nhs">28</span>°C</b> → 製冷啟動，風扇全速</p>
    <p>• <b>溫度＜<span id="ncs">26</span>°C</b> → 加熱啟動，風扇中速</p>
    <p>• 之間為恆溫區，TEC 關閉，風扇低速循環</p>
    <p>• 安全保護 ＜10°C 或 ＞40°C 強制停 TEC</p>
  </div>
</div>
<script>
async function poll(){
  try{
    var r=await fetch('/data');var d=await r.json();
    if(!d.ok){document.getElementById('st').textContent=d.message;document.getElementById('dot').className='dot err';return;}
    document.getElementById('dot').className='dot';
    document.getElementById('st').textContent=new Date().toLocaleTimeString();
    document.getElementById('mT').textContent=d.temperature.toFixed(1);
    document.getElementById('mH').textContent=d.humidity.toFixed(1);
    document.getElementById('mP').textContent=d.pressure.toFixed(1);
    document.getElementById('mA').textContent=d.aqi;
    document.getElementById('sysBtn').className=d.systemOn?'btn on':'btn off';
    document.getElementById('sysBtn').textContent=d.systemOn?'停止系統':'開啟系統';
    var ps=d.systemOn?(d.cooling?'製冷中':d.heating?'加熱中':'運轉中'):'待機';
    document.getElementById('pSys').textContent=ps;
    document.getElementById('pSys').className='pill'+(d.systemOn?' act':'');
    document.getElementById('pCool').textContent='製冷 '+(d.cooling?'開':'關');
    document.getElementById('pCool').className='pill cold'+(d.cooling?' act':'');
    document.getElementById('pHeat').textContent='加熱 '+(d.heating?'開':'關');
    document.getElementById('pHeat').className='pill hot'+(d.heating?' act':'');
    document.getElementById('hs').value=d.heatStop;
    document.getElementById('hsv').textContent=d.heatStop.toFixed(1)+'°C';
    document.getElementById('cs').value=d.coolStop;
    document.getElementById('csv').textContent=d.coolStop.toFixed(1)+'°C';
    document.getElementById('nhs').textContent=d.heatStop.toFixed(1);
    document.getElementById('ncs').textContent=d.coolStop.toFixed(1);
  }catch(e){
    document.getElementById('st').textContent='更新失敗';
    document.getElementById('dot').className='dot err';
  }
}
function toggleSys(){
  var on=document.getElementById('sysBtn').classList.contains('off');
  fetch('/control?system='+(on?1:0));
}
function setHS(v){document.getElementById('hsv').textContent=parseFloat(v).toFixed(1)+'°C';fetch('/control?heatStop='+v);}
function setCS(v){document.getElementById('csv').textContent=parseFloat(v).toFixed(1)+'°C';fetch('/control?coolStop='+v);}
function tTest(type,val){
  if(type==='fan')fetch('/test?fan='+val);
  else if(type==='cool')fetch('/test?cool='+val+'&heat=0&en=1');
  else if(type==='heat')fetch('/test?heat='+val+'&cool=0&en=1');
  else fetch('/test?fan=0&cool=0&heat=0&en=0');
}
poll();setInterval(poll,2000);
</script>
</body>
</html>)HTML";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TEC_EN, OUTPUT);
  digitalWrite(TEC_EN, LOW);

  // ESP32 Arduino Core 3.x API: ledcAttach(pin, freq, resolution)
  ledcAttach(FAN_PIN, PWM_FREQ, PWM_RES);
  ledcAttach(TEC_LPWM, PWM_FREQ, PWM_RES);
  ledcAttach(TEC_RPWM, PWM_FREQ, PWM_RES);

  setFan(0);
  setTec(0, 0);
  Serial.println("PWM OK");

  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeOut(50);

  WiFi.softAP("ESP32-TEMP", "12345678");
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (bme.begin(0x77) || bme.begin(0x76)) {
    bme.setTemperatureOversampling(BME680_OS_2X);
    bme.setHumidityOversampling(BME680_OS_1X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
    bmeOk = true;
    Serial.println("BME688 OK");
  } else {
    Serial.println("BME688 fail");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/control", handleControl);
  server.on("/test", handleTest);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("Server started");
}

void loop() {
  server.handleClient();
  if (millis() - lastRead >= 2000) {
    lastRead = millis();
    readSensor();
  }
}
