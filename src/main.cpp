#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

#define FAN_PIN 18
#define TEC_EN 19
#define TEC_LPWM 26  // 制冷（原先 25 接反了）
#define TEC_RPWM 25  // 加熱（原先 26 接反了）
#define PWM_FREQ 25000
#define PWM_RES 8
#define FAN_CH 0
#define TEC_L_CH 1
#define TEC_R_CH 2

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
  ledcWrite(FAN_CH, fanSpeed);
}

void setTec(int cool, int heat) {
  cooling = cool > 0;
  heating = heat > 0;
  ledcWrite(TEC_L_CH, cool);
  ledcWrite(TEC_R_CH, heat);
  Serial.printf("TEC: L=%d R=%d EN=%d\n", cool, heat, digitalRead(TEC_EN));
}

void stopAll() {
  setFan(0);
  setTec(0, 0);
  digitalWrite(TEC_EN, LOW);
  Serial.println("[SYS] 停止");
}

void startAll() {
  digitalWrite(TEC_EN, HIGH);
  setFan(100);
  Serial.println("[SYS] 啟動");
}

void controlTemp() {
  if (!systemOn || isnan(t)) return;
  if (t < SAFE_MIN || t > SAFE_MAX) {
    setTec(0, 0);
    setFan(t > SAFE_MAX ? 255 : 60);
    Serial.println("[SAFE] 極端溫度，TEC 停");
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
  if (!bmeOk) {
    Serial.printf("[BME688] 未連線 | SYS:%s | Fan:%d\n",
                  systemOn ? "ON" : "OFF", fanSpeed);
    return;
  }
  if (!bme.performReading()) {
    Serial.println("[BME688] 讀取失敗");
    bmeOk = false;
    return;
  }
  t = bme.temperature;
  h = bme.humidity;
  p = bme.pressure / 100.0;
  g = bme.gas_resistance / 1000.0;
  aqi = calcAqi(g);
  controlTemp();
  Serial.printf("[SENSOR] T:%.1f H:%.1f AQI:%d | SYS:%s %s | Fan:%d EN:%d\n",
                t, h, aqi,
                systemOn ? "ON" : "OFF",
                cooling ? "COOL" : heating ? "HEAT" : "IDLE",
                fanSpeed, digitalRead(TEC_EN));
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
  snprintf(buf, sizeof(buf), "{\"ok\":true,\"en\":%d,\"fan\":%d,\"cool\":%s,\"heat\":%s}",
    digitalRead(TEC_EN), fanSpeed, cooling ? "true" : "false", heating ? "true" : "false");
  server.send(200, "application/json", buf);
  Serial.printf("[TEST] EN=%d Fan=%d Cool=%s Heat=%s\n",
    digitalRead(TEC_EN), fanSpeed, cooling ? "ON" : "OFF", heating ? "ON" : "OFF");
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
.sts .pill{flex:1;text-align:center;padding:6px;border-radius:4px;background:var(--bg);border:1px solid var(--bd);cursor:pointer;user-select:none}
.sts .pill:active{opacity:.7}
.sts .pill.act{border-color:var(--c);color:var(--c)}
.sts .pill.hot{border-color:var(--r);color:var(--r)}
.sts .pill.cold{border-color:var(--b);color:var(--b)}
.sts .pill.hot.act{background:rgba(248,81,73,.15)}
.sts .pill.cold.act{background:rgba(88,166,255,.15)}
.fld{display:flex;align-items:center;gap:10px;margin-bottom:10px}
.fld label{font-size:.78rem;color:var(--t2);min-width:78px}
.fld input[type=range]{flex:1;height:4px;-webkit-appearance:none;appearance:none;background:var(--bd);border-radius:2px;outline:none}
.fld input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;border-radius:50%;background:var(--c);cursor:pointer}
.fld .val{font-size:.95rem;font-weight:700;color:var(--c);min-width:42px;text-align:right;font-variant-numeric:tabular-nums}
.note{font-size:.72rem;color:var(--t3);line-height:1.6}
.note b{color:var(--t2)}
.tst{display:flex;flex-wrap:wrap;gap:4px;margin-bottom:8px}
#chart{width:100%;height:160px;display:block;margin-bottom:8px}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);background:var(--c);color:#000;padding:6px 16px;border-radius:4px;font-size:.82rem;font-weight:600;opacity:0;transition:opacity .3s;pointer-events:none}
.toast.show{opacity:1}
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
  <h2>溫度趨勢</h2>
  <canvas id="chart"></canvas>
</div>
<div class="sec">
  <h2>系統開關</h2>
  <button class="btn off" id="sysBtn" onclick="toggleSys()">開啟系統</button>
  <div class="sts">
    <div class="pill" id="pSys">待機</div>
    <div class="pill cold" id="pCool" onclick="tTest('cool',200)">製冷 關</div>
    <div class="pill hot" id="pHeat" onclick="tTest('heat',200)">加熱 關</div>
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
    <p>• 點上方「製冷/加熱」狀態也可快速切換</p>
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
<div class="toast" id="toast"></div>
<script>
var hist=[],maxH=60;
var cv=document.getElementById('chart'),cx=cv.getContext('2d');
function rs(){var r=cv.getBoundingClientRect();cv.width=r.width*devicePixelRatio;cv.height=r.height*devicePixelRatio;cx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);drawChart();}
window.addEventListener('resize',rs);

function drawChart(){
  var W=cv.getBoundingClientRect().width,H=cv.getBoundingClientRect().height;
  var pl=42,pr=8,pt=10,pb=20;
  cx.clearRect(0,0,W,H);
  if(hist.length<2){cx.fillStyle='#484f58';cx.font='12px system-ui';cx.textAlign='center';cx.fillText('等待資料...',W/2,H/2);return;}
  var mn=Infinity,mx=-Infinity;
  hist.forEach(function(r){if(r.t<mn)mn=r.t;if(r.t>mx)mx=r.t;});
  var sp=mx-mn;if(sp<1){mn-=1;mx+=1;sp=2;}
  var pa=sp*.1;mn-=pa;mx+=pa;sp=mx-mn;
  var pw=W-pl-pr,ph=H-pt-pb;

  cx.strokeStyle='rgba(139,148,158,.1)';cx.lineWidth=1;
  cx.fillStyle='#484f58';cx.font='10px system-ui';cx.textAlign='right';
  for(var i=0;i<=4;i++){var v=mx-(sp*i/4);var y=pt+ph*i/4;cx.beginPath();cx.moveTo(pl,y);cx.lineTo(W-pr,y);cx.stroke();cx.fillText(v.toFixed(1),pl-4,y+3);}

  cx.textAlign='center';cx.fillStyle='#484f58';cx.font='9px system-ui';
  var st=pw/Math.max(1,hist.length-1);
  var xt=Math.max(1,Math.ceil(hist.length/6));
  hist.forEach(function(r,i){if(i%xt===0||i===hist.length-1)cx.fillText(r.ti,pl+st*i,H-pb+14);});

  cx.beginPath();cx.strokeStyle='#f85149';cx.lineWidth=2;cx.lineJoin='round';
  hist.forEach(function(r,i){var x=pl+st*i;var y=pt+(1-(r.t-mn)/sp)*ph;i?cx.lineTo(x,y):cx.moveTo(x,y);});
  cx.stroke();
  var la=hist[hist.length-1];var lx=pl+st*(hist.length-1);var ly=pt+(1-(la.t-mn)/sp)*ph;
  cx.beginPath();cx.arc(lx,ly,4,0,Math.PI*2);cx.fillStyle='#f85149';cx.fill();

  cx.fillStyle='#f85149';cx.font='bold 11px system-ui';cx.textAlign='left';
  cx.fillText(la.t.toFixed(1)+'°C',lx-36,ly-8);
}
rs();

function toast(msg){var e=document.getElementById('toast');e.textContent=msg;e.className='toast show';setTimeout(function(){e.className='toast';},1500);}

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
    hist.push({t:d.temperature,ti:new Date().toLocaleTimeString()});
    if(hist.length>maxH)hist.shift();
    drawChart();
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
async function tTest(type,val){
  try{
    var url;
    if(type==='fan')url='/test?fan='+val;
    else if(type==='cool')url='/test?cool='+val+'&heat=0&en=1';
    else if(type==='heat')url='/test?heat='+val+'&cool=0&en=1';
    else url='/test?fan=0&cool=0&heat=0&en=0';
    var r=await fetch(url);var d=await r.json();
    if(d.ok){
      if(type==='off')toast('全部停止');
      else if(type==='fan')toast('風扇: '+val);
      else if(type==='cool')toast(d.cooling==='true'?'製冷已開':'製冷已關');
      else if(type==='heat')toast(d.heating==='true'?'加熱已開':'加熱已關');
    }
  }catch(e){toast('操作失敗');}
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

  ledcSetup(FAN_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(FAN_PIN, FAN_CH);
  ledcSetup(TEC_L_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(TEC_LPWM, TEC_L_CH);
  ledcSetup(TEC_R_CH, PWM_FREQ, PWM_RES);
  ledcAttachPin(TEC_RPWM, TEC_R_CH);

  setFan(0);
  setTec(0, 0);
  Serial.println("[PWM] OK");

  Wire.begin();
  Wire.setClock(100000);

  Serial.println("[I2C] 掃描中...");
  int found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    byte err = Wire.endTransmission();
    if (err == 0) {
      Serial.printf("[I2C] 發現裝置: 0x%02X\n", addr);
      found++;
    }
  }
  if (found == 0) Serial.println("[I2C] 無裝置！檢查 SDA/SCL 接線");

  WiFi.softAP("ESP32-TEMP", "12345678");
  Serial.print("[WiFi] AP IP: ");
  Serial.println(WiFi.softAPIP());

  uint8_t bmeAddr = 0;
  if (bme.begin(0x77)) { bmeAddr = 0x77; }
  else if (bme.begin(0x76)) { bmeAddr = 0x76; }

  if (bmeAddr) {
    bme.setTemperatureOversampling(BME680_OS_2X);
    bme.setHumidityOversampling(BME680_OS_1X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150);
    bmeOk = true;
    Serial.printf("[BME688] 連線成功 (addr: 0x%02X)\n", bmeAddr);
  } else {
    Serial.println("[BME688] 未找到！嘗試 0x77/0x76 均失敗");
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/control", handleControl);
  server.on("/test", handleTest);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("[Server] 啟動完成");
}

void loop() {
  server.handleClient();
  if (millis() - lastRead >= 2000) {
    lastRead = millis();
    readSensor();
  }
}
