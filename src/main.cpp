#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>

#define FAN_PIN 18
#define TEC_EN 19
#define TEC_LPWM 26
#define TEC_RPWM 25
#define PWM_FREQ 25000
#define PWM_RES 8
#define FAN_CH 0
#define TEC_L_CH 1
#define TEC_R_CH 2
#define DS18B20_PIN 4

OneWire ds(DS18B20_PIN);
DallasTemperature dt(&ds);
WebServer server(80);
U8G2_SSD1306_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, /* reset= */ U8X8_PIN_NONE);

// 三顆 DS18B20 — ROM 位址認人，不靠匯流排順序
DeviceAddress nestAddr = {0x28, 0xAE, 0xB0, 0xC9, 0x00, 0x00, 0x00, 0xB4};
DeviceAddress roomAddr = {0x28, 0x4F, 0x59, 0x22, 0x00, 0x00, 0x00, 0x5E};
DeviceAddress ventAddr = {0x28, 0x20, 0xD7, 0x20, 0x00, 0x00, 0x00, 0xD7};
bool nestOK = false, roomOK = false, ventOK = false;

float nestT = NAN, roomT = NAN, ventT = NAN;
float readTemps[3] = {NAN, NAN, NAN};

int fanSpeed = 0;
float coolTarget = 20.0;  // 你可從網頁調（實際最低約26°C）
float heatTarget = 28.0;  // 你可從網頁調
float hysteresis = 0.3;   // 窄滯回，配合實際窄工作區間
unsigned long lastRead = 0;
unsigned long lastScan = 0;

bool systemOn = false;
bool cooling = false;
bool heating = false;
bool dsOk = false;
bool fanManual = false;
bool tecManual = false;
bool manualMode = false;
bool convPending = false;
unsigned long convStart = 0;

// 安全保護閾值
float safeMin = 5.0;      // 巢穴最低溫（動物安全）
float safeMax = 35.0;     // 巢穴最高溫（動物安全）
float ventMax = 50.0;     // 出風口最高溫（硬體保護）

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
  fanManual = false;
  tecManual = false;
  Serial.println("[SYS] 停止");
}

void startAll() {
  digitalWrite(TEC_EN, HIGH);
  setFan(100);
  fanManual = false;
  tecManual = false;
  Serial.println("[SYS] 啟動");
}

void emergencyStop() {
  systemOn = false;
  stopAll();
  Serial.println("!!! [緊急] 出風口溫度過高，系統關閉 !!!");
}

void setTecPwm(float power, bool isCool) {
  // power: 0-1.0  (0=off, 1.0=full)
  int pwmVal = constrain((int)(power * 255), 0, 255);
  if (isCool) {
    cooling = power > 0.05;
    heating = false;
    ledcWrite(TEC_L_CH, pwmVal);
    ledcWrite(TEC_R_CH, 0);
  } else {
    heating = power > 0.05;
    cooling = false;
    ledcWrite(TEC_L_CH, 0);
    ledcWrite(TEC_R_CH, pwmVal);
  }
}

void controlTemp() {
  if (!systemOn || tecManual || manualMode) return;
  if (isnan(nestT)) return;

  // === 保護 1：出風口過熱（硬體安全，最高優先級）===
  if (!isnan(ventT) && ventT >= ventMax) {
    emergencyStop();
    return;
  }

  // === 保護 2：巢穴極端溫度（動物安全）===
  if (nestT < safeMin || nestT > safeMax) {
    setTecPwm(0, true);
    if (!fanManual) setFan(nestT > safeMax ? 255 : 60);
    Serial.printf("[SAFE] 巢穴極端溫度 %.1f°C\n", nestT);
    return;
  }

  // === 比例溫控 ===
  // 巢穴 > coolTarget+hysteresis → 製冷，PWM 隨偏差增大
  // 巢穴 < heatTarget-hysteresis → 加熱，PWM 隨偏差增大
  // 兩者之間 → 關 TEC，低風扇

  float coolDiff = nestT - coolTarget;  // +=太熱需製冷
  float heatDiff = heatTarget - nestT;  // +=太冷需加熱

  if (coolDiff > hysteresis) {
    // 需製冷 — 偏差每多1°C 增加~12% PWM
    float power = constrain(coolDiff / 5.0, 0.15, 1.0);
    setTecPwm(power, true);
    if (!fanManual) setFan(100 + (int)(155 * power));
  } else if (heatDiff > hysteresis) {
    // 需加熱 — 偏差每多1°C 增加~12% PWM
    float power = constrain(heatDiff / 5.0, 0.15, 1.0);
    setTecPwm(power, false);
    if (!fanManual) setFan(80 + (int)(175 * power));
  } else {
    // 在目標範圍內 → 關 TEC，低風扇循環
    setTecPwm(0, true);
    if (!fanManual) setFan(40);
  }
}

void readSensor() {
  if (!dsOk) return;
  if (!convPending) {
    dt.requestTemperatures();
    convPending = true;
    convStart = millis();
    return;
  }
  if (millis() - convStart < 750) return;
  convPending = false;

  readTemps[0] = nestOK ? dt.getTempC(nestAddr) : DEVICE_DISCONNECTED_F;
  readTemps[1] = roomOK ? dt.getTempC(roomAddr) : DEVICE_DISCONNECTED_F;
  readTemps[2] = ventOK ? dt.getTempC(ventAddr) : DEVICE_DISCONNECTED_F;

  for (int i = 0; i < 3; i++) {
    if (readTemps[i] == DEVICE_DISCONNECTED_F || isnan(readTemps[i])) {
      readTemps[i] = NAN;
    }
  }
  nestT = readTemps[0];
  roomT = readTemps[1];
  ventT = readTemps[2];

  if (isnan(nestT)) {
    Serial.println("[DS18B20] 巢穴感測器斷線");
    return;
  }
  controlTemp();
  Serial.printf("[巢穴:%.1f 活動:%.1f 出風:%.1f] %s %s Fan:%d\n",
                nestT, roomT, ventT,
                cooling ? "製冷" : heating ? "加熱" : "維持",
                systemOn ? "ON" : "OFF", fanSpeed);
}

void sendJson(const char* msg) {
  String j = "{\"ok\":false,\"message\":\"";
  j += msg;
  j += "\"}";
  server.send(200, "application/json", j);
}

void handleData() {
  if (!dsOk) { sendJson("DS18B20 未連線"); return; }
  if (isnan(nestT)) { sendJson("等待感測資料..."); return; }
  int n = (nestOK ? 1 : 0) + (roomOK ? 1 : 0) + (ventOK ? 1 : 0);
  char buf[700];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"nest\":%.2f,\"room\":%.2f,\"vent\":%.2f,\"temps\":[%.2f,%.2f,%.2f],\"sensorCount\":%d,\"fanSpeed\":%d,\"cooling\":%s,\"heating\":%s,\"systemOn\":%s,\"manualMode\":%s,\"coolTarget\":%.1f,\"heatTarget\":%.1f,\"safeMin\":%.1f,\"safeMax\":%.1f,\"ventMax\":%.1f}",
    nestT, roomT, ventT, readTemps[0], readTemps[1], readTemps[2], n,
    fanSpeed, cooling ? "true" : "false", heating ? "true" : "false", systemOn ? "true" : "false", manualMode ? "true" : "false", coolTarget, heatTarget, safeMin, safeMax, ventMax);
  server.send(200, "application/json", buf);
}

void handleControl() {
  if (server.hasArg("system")) {
    systemOn = server.arg("system").toInt() == 1;
    if (systemOn) startAll(); else stopAll();
  }
  if (server.hasArg("manual")) {
    manualMode = server.arg("manual").toInt() == 1;
    tecManual = manualMode;
    fanManual = manualMode;
    Serial.printf("[SYS] %s 模式\n", manualMode ? "手動" : "自動");
  }
  if (server.hasArg("coolTarget")) {
    coolTarget = constrain(server.arg("coolTarget").toFloat(), (float)10, (float)35);
  }
  if (server.hasArg("heatTarget")) {
    heatTarget = constrain(server.arg("heatTarget").toFloat(), (float)15, (float)40);
  }
  if (server.hasArg("safeMin")) {
    safeMin = constrain(server.arg("safeMin").toFloat(), 0, 20);
  }
  if (server.hasArg("safeMax")) {
    safeMax = constrain(server.arg("safeMax").toFloat(), 20, 50);
  }
  if (server.hasArg("ventMax")) {
    ventMax = constrain(server.arg("ventMax").toFloat(), 30, 80);
  }
  controlTemp();
  server.send(200, "text/plain", "OK");
}

void handleDiag() {
  char buf[512];
  int pinVal = digitalRead(DS18B20_PIN);
  int cnt = dt.getDeviceCount();
  snprintf(buf, sizeof(buf),
    "{\"pin\":%d,\"pinState\":%d,\"dsOk\":%s,\"deviceCount\":%d,\"nest\":%.2f,\"room\":%.2f,\"vent\":%.2f,\"conversionPending\":%s}",
    DS18B20_PIN, pinVal, dsOk ? "true" : "false", cnt, nestT, roomT, ventT, convPending ? "true" : "false");
  server.send(200, "application/json", buf);
}

void handleTest() {
  int fanVal = server.hasArg("fan") ? server.arg("fan").toInt() : -1;
  int coolVal = server.hasArg("cool") ? server.arg("cool").toInt() : -1;
  int heatVal = server.hasArg("heat") ? server.arg("heat").toInt() : -1;
  int enVal = server.hasArg("en") ? server.arg("en").toInt() : -1;

  if (enVal >= 0) digitalWrite(TEC_EN, enVal ? HIGH : LOW);
  if (fanVal >= 0) { setFan(fanVal); fanManual = true; }
  if (coolVal >= 0 || heatVal >= 0) { setTec(max(0, coolVal), max(0, heatVal)); tecManual = true; }

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
<title>TEC 實驗溫控</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0a0e14;--sf:#111820;--bd:#1e2a36;--tx:#e0e6ed;--t2:#7a8a9a;--t3:#4a5568;--r:#ef4444;--b:#3b82f6;--g:#22c55e;--a:#eab308;--c:#14b8a6;--o:#f97316}
body{font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;background:var(--bg);color:var(--tx);padding:12px 16px;max-width:600px;margin:0 auto}
.hdr{display:flex;align-items:center;justify-content:space-between;padding:12px 0 10px;border-bottom:1px solid var(--bd);margin-bottom:10px}
.hdr h1{font-size:1.05rem;font-weight:700}
.hdr h1 b{color:var(--c)}
.conn{display:flex;align-items:center;gap:6px;font-size:.7rem;color:var(--t2)}
.dot{width:6px;height:6px;border-radius:50%;background:var(--g);flex-shrink:0}
.dot.err{background:var(--r)}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:10px}
.card{background:var(--sf);border:1px solid var(--bd);border-radius:8px;padding:10px 4px;text-align:center;position:relative;overflow:hidden}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px}
.card.cr::before{background:var(--r)}.card.cb::before{background:var(--b)}.card.cg::before{background:var(--o)}
.card .val{font-size:1.15rem;font-weight:800;line-height:1;font-variant-numeric:tabular-nums}
.card .lbl{font-size:.5rem;color:var(--t3);margin-top:3px;letter-spacing:.3px}
.sec{background:var(--sf);border:1px solid var(--bd);border-radius:8px;padding:12px;margin-bottom:8px}
.sec h2{font-size:.62rem;color:var(--t3);font-weight:700;margin-bottom:8px;text-transform:uppercase;letter-spacing:1px}
.btn{width:100%;padding:11px;border-radius:6px;border:none;font-size:.85rem;font-weight:700;cursor:pointer;transition:all .15s}
.btn.on{background:var(--r);color:#fff}
.btn.off{background:var(--c);color:#000}
.btn:active{transform:scale(.98)}
.pills{display:flex;gap:5px;margin-top:8px}
.pill{flex:1;text-align:center;padding:6px 0;border-radius:6px;background:var(--bg);border:1px solid var(--bd);font-size:.7rem;font-weight:600;cursor:pointer;user-select:none;transition:all .15s}
.pill:active{transform:scale(.96)}
.pill.sys.act{border-color:var(--c);color:var(--c)}
.pill.cold{color:var(--t2)}.pill.cold.act{border-color:var(--b);color:var(--b)}
.pill.hot{color:var(--t2)}.pill.hot.act{border-color:var(--r);color:var(--r)}
.pill.ch-pill{color:var(--t3);font-size:.58rem}.pill.ch-pill.act{color:var(--tx);border-color:var(--c)}
.fld{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.fld label{font-size:.7rem;color:var(--t2);min-width:64px}
.fld input[type=range]{flex:1;height:3px;-webkit-appearance:none;appearance:none;background:var(--bd);border-radius:2px;outline:none}
.fld input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:16px;height:16px;border-radius:50%;background:var(--c);cursor:pointer}
.fld .rv{font-size:.85rem;font-weight:800;color:var(--c);min-width:40px;text-align:right;font-variant-numeric:tabular-nums}
.fld input.rv{background:0 0;border:1px solid transparent;outline:none;padding:2px 4px;border-radius:4px;width:60px;font-family:inherit;font-variant-numeric:tabular-nums;-moz-appearance:textfield}
.fld input.rv::-webkit-inner-spin-button,.fld input.rv::-webkit-outer-spin-button{-webkit-appearance:none;margin:0}
.fld input.rv:focus{border-color:var(--c);color:var(--tx)}
.info{font-size:.62rem;color:var(--t3);line-height:1.6;margin-top:4px}
.info b{color:var(--t2)}
.act-row{display:flex;gap:5px;margin-top:6px}
.act-btn{flex:1;padding:7px;border-radius:5px;border:1px solid var(--bd);background:0 0;color:var(--t2);cursor:pointer;font-size:.68rem;font-weight:600;transition:all .15s}
.act-btn:active{transform:scale(.95);background:rgba(20,184,166,.1);border-color:var(--c);color:var(--c)}
#chart{width:100%;height:130px;display:block}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(16px);background:var(--c);color:#000;padding:7px 18px;border-radius:8px;font-size:.78rem;font-weight:700;opacity:0;transition:all .25s;pointer-events:none;z-index:99}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
@media(max-width:400px){.grid{grid-template-columns:repeat(3,1fr)}.card .val{font-size:1rem}}
</style>
</head>
<body>
<div class="hdr">
  <h1><b>TEC</b> 蟄眠實驗</h1>
  <div class="conn"><span class="dot" id="dot"></span><span id="st">...</span></div>
</div>
<div class="grid">
  <div class="card cr"><div class="val" id="mNest">--</div><div class="lbl">巢穴</div></div>
  <div class="card cb"><div class="val" id="mRoom">--</div><div class="lbl">活動區</div></div>
  <div class="card cg"><div class="val" id="mVent">--</div><div class="lbl">出風口</div></div>
</div>
<div class="sec">
  <h2>溫度趨勢</h2>
  <canvas id="chart"></canvas>
  <div class="pills" style="margin-top:2px;margin-bottom:4px">
    <div class="pill ch-pill act" onclick="setChart(0)">巢穴</div>
    <div class="pill ch-pill" onclick="setChart(1)">活動區</div>
    <div class="pill ch-pill" onclick="setChart(2)">出風口</div>
  </div>
  <div class="act-row">
    <button class="act-btn" onclick="exportCSV()">導出 CSV</button>
    <button class="act-btn" onclick="clearHist()">清除記錄</button>
  </div>
</div>
<div class="sec">
  <h2>系統控制</h2>
  <button class="btn off" id="sysBtn" onclick="toggleSys()">開啟系統</button>
  <div class="pills">
    <div class="pill sys" id="pSys">待機</div>
    <div class="pill cold" id="pCool" onclick="toggleCool()">製冷</div>
    <div class="pill hot" id="pHeat" onclick="toggleHeat()">加熱</div>
  </div>
  <div class="pills" style="margin-top:6px">
    <button class="act-btn" id="modeBtn" onclick="toggleMode()">切換手動模式</button>
  </div>
</div>
<div class="sec">
  <h2>實驗目標溫度</h2>
  <div class="fld">
    <label>製冷目標</label>
    <input type="range" min="10" max="35" step="0.1" value="20" id="ct" oninput="setCT(this.value)">
    <input type="number" class="rv" id="ctV" value="20" step="0.1" min="10" max="35" onchange="setCT(this.value)">
  </div>
  <div class="fld">
    <label>加熱目標</label>
    <input type="range" min="15" max="40" step="0.1" value="28" id="ht" oninput="setHT(this.value)">
    <input type="number" class="rv" id="htV" value="28" step="0.1" min="15" max="40" onchange="setHT(this.value)">
  </div>
  <div class="info">巢穴 ＞<span id="nct">20</span>°C 需製冷 | 巢穴 ＜<span id="nht">28</span>°C 需加熱</div>
</div>
<div class="sec">
  <h2>安全保護</h2>
  <div class="fld">
    <label>巢穴最低</label>
    <input type="range" min="0" max="20" step="0.5" value="5" id="smin" oninput="setSMin(this.value)">
    <span class="rv" id="sminV">5</span>
  </div>
  <div class="fld">
    <label>巢穴最高</label>
    <input type="range" min="20" max="50" step="0.5" value="35" id="smax" oninput="setSMax(this.value)">
    <span class="rv" id="smaxV">35</span>
  </div>
  <div class="fld">
    <label>出風口上限</label>
    <input type="range" min="30" max="80" step="1" value="50" id="vmax" oninput="setVMax(this.value)">
    <span class="rv" id="vmaxV">50</span>
  </div>
  <div class="info">出風口超過上限 → 立即關閉系統（硬體保護）</div>
</div>
<div class="sec">
  <h2>風速控制</h2>
  <div class="fld">
    <label>手動風速</label>
    <input type="range" min="0" max="100" step="1" value="0" id="fanS" oninput="setFanM(this.value)">
    <span class="rv" id="fanV">0%</span>
  </div>
</div>
<div class="sec">
  <h2>更新頻率</h2>
  <div class="fld">
    <label>間隔秒數</label>
    <input type="range" min="1" max="10" step="1" value="2" id="pollS" oninput="setPoll(this.value)">
    <span class="rv" id="pollV">2s</span>
  </div>
</div>
<div class="toast" id="toast"></div>
<script>
var H=[],M=120,allData=[],ms=2000,pi=null,fm=false,chartMode=0;
var chartColors=[['#ef4444','rgba(239,68,68,'],['#3b82f6','rgba(59,130,246,'],['#f97316','rgba(249,115,22,']];
var chartLabels=['巢穴','活動區','出風口'];
var chartFields=['n','r','v'];
var cv=document.getElementById('chart'),cx=cv.getContext('2d');
function rs(){var r=cv.getBoundingClientRect();cv.width=r.width*devicePixelRatio;cv.height=r.height*devicePixelRatio;cx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);dC();}
window.addEventListener('resize',rs);
function dC(){
  var W=cv.getBoundingClientRect().width,HH=cv.getBoundingClientRect().height,pl=38,pr=8,pt=10,pb=18;
  var f=chartFields[chartMode],cl=chartColors[chartMode],lb=chartLabels[chartMode];
  cx.clearRect(0,0,W,HH);
  if(H.length<2){cx.fillStyle='#4a5568';cx.font='11px system-ui';cx.textAlign='center';cx.fillText('等待 '+lb+' 資料...',W/2,HH/2);return;}
  var mn=1e9,mx=-1e9;H.forEach(function(r){var v=r[f];if(v<mn)mn=v;if(v>mx)mx=v;});
  var sp=mx-mn;if(sp<1){mn-=1;mx+=1;sp=2;}
  var pa=sp*.12;mn-=pa;mx+=pa;sp=mx-mn;
  var pw=W-pl-pr,ph=HH-pt-pb;
  cx.strokeStyle='rgba(122,138,154,.06)';cx.lineWidth=1;cx.fillStyle='#4a5568';cx.font='9px system-ui';cx.textAlign='right';
  for(var i=0;i<=4;i++){var v=mx-(sp*i/4),y=pt+ph*i/4;cx.beginPath();cx.moveTo(pl,y);cx.lineTo(W-pr,y);cx.stroke();cx.fillText(v.toFixed(1),pl-4,y+3);}
  cx.textAlign='center';cx.font='8px system-ui';
  var st=pw/Math.max(1,H.length-1),xt=Math.max(1,Math.ceil(H.length/5));
  H.forEach(function(r,i){if(i%xt===0||i===H.length-1)cx.fillText(r.ti,pl+st*i,HH-pb+12);});
  var gd=cx.createLinearGradient(0,pt,0,HH-pb);gd.addColorStop(0,cl[1]+'.2)');gd.addColorStop(1,cl[1]+'0)');
  cx.beginPath();H.forEach(function(r,i){var x=pl+st*i,y=pt+(1-(r[f]-mn)/sp)*ph;i?cx.lineTo(x,y):cx.moveTo(x,y);});
  cx.lineTo(pl+st*(H.length-1),HH-pb);cx.lineTo(pl,HH-pb);cx.closePath();cx.fillStyle=gd;cx.fill();
  cx.beginPath();cx.strokeStyle=cl[0];cx.lineWidth=2;cx.lineJoin='round';
  H.forEach(function(r,i){var x=pl+st*i,y=pt+(1-(r[f]-mn)/sp)*ph;i?cx.lineTo(x,y):cx.moveTo(x,y);});cx.stroke();
  var la=H[H.length-1],lx=pl+st*(H.length-1),ly=pt+(1-(la[f]-mn)/sp)*ph;
  cx.beginPath();cx.arc(lx,ly,4,0,Math.PI*2);cx.fillStyle=cl[0];cx.fill();
  cx.beginPath();cx.arc(lx,ly,7,0,Math.PI*2);cx.strokeStyle=cl[1]+'.3)';cx.lineWidth=2;cx.stroke();
  cx.fillStyle=cl[0];cx.font='bold 10px system-ui';cx.textAlign='right';cx.fillText(la[f].toFixed(1)+'C',lx-10,ly-8);
}
rs();
function toast(m){var e=document.getElementById('toast');e.textContent=m;e.className='toast show';setTimeout(function(){e.className='toast';},1500);}
function exportCSV(){
  if(!allData.length){toast('無資料');return;}
  var c='﻿時間,巢穴,活動區,出風口,風扇(%),狀態\n'+allData.map(function(r){return r.ti+','+r.n.toFixed(1)+','+r.r.toFixed(1)+','+r.v.toFixed(1)+','+Math.round(r.f*100/255)+','+(r.c?'製冷':r.h?'加熱':'維持');}).join('\n');
  var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([c],{type:'text/csv'}));a.download='TEC_'+(new Date().toISOString().slice(0,19).replace('T','_').replace(/:/g,'-'))+'.csv';a.click();
  toast('已導出 '+allData.length+' 筆');
}
function clearHist(){H=[];allData=[];rs();toast('已清除');}
async function doPoll(){
  try{
    var r=await fetch('/data'),d=await r.json();
    if(!d.ok){document.getElementById('st').textContent=d.message;document.getElementById('dot').className='dot err';return;}
    document.getElementById('dot').className='dot';
    document.getElementById('st').textContent=new Date().toLocaleTimeString();
    document.getElementById('mNest').textContent=d.nest.toFixed(1);
    document.getElementById('mRoom').textContent=d.room.toFixed(1);
    document.getElementById('mVent').textContent=d.vent.toFixed(1);
    document.getElementById('sysBtn').className=d.systemOn?'btn on':'btn off';
    document.getElementById('sysBtn').textContent=d.systemOn?'停止系統':'開啟系統';
    var ps=d.systemOn?(d.cooling?'製冷中':d.heating?'加熱中':'維持中'):'待機';
    document.getElementById('pSys').textContent=ps;
    document.getElementById('pSys').className='pill sys'+(d.systemOn?' act':'');
    document.getElementById('pCool').className='pill cold'+(d.cooling?' act':'');
    document.getElementById('pHeat').className='pill hot'+(d.heating?' act':'');
    _cooling=d.cooling;_heating=d.heating;
    document.getElementById('ct').value=d.coolTarget;
    document.getElementById('ctV').value=d.coolTarget.toFixed(1);
    document.getElementById('ht').value=d.heatTarget;
    document.getElementById('htV').value=d.heatTarget.toFixed(1);
    document.getElementById('nct').textContent=d.coolTarget.toFixed(1);
    document.getElementById('nht').textContent=d.heatTarget.toFixed(1);
    document.getElementById('smin').value=d.safeMin;
    document.getElementById('sminV').textContent=d.safeMin.toFixed(1);
    document.getElementById('smax').value=d.safeMax;
    document.getElementById('smaxV').textContent=d.safeMax.toFixed(1);
    document.getElementById('vmax').value=d.ventMax;
    document.getElementById('vmaxV').textContent=d.ventMax.toFixed(0);
    document.getElementById('modeBtn').textContent=d.manualMode?'切換自動模式':'切換手動模式';
    document.getElementById('modeBtn').style.background=d.manualMode?'rgba(239,68,68,.15)':'rgba(20,184,166,.15)';
    document.getElementById('modeBtn').style.color=d.manualMode?'var(--r)':'var(--c)';
    if(!fm){
      document.getElementById('fanV').textContent=Math.round(d.fanSpeed*100/255)+'%';
      document.getElementById('fanS').value=Math.round(d.fanSpeed*100/255);
    }
    H.push({n:d.nest,r:d.room,v:d.vent,f:d.fanSpeed,ti:new Date().toLocaleTimeString()});
    if(H.length>M)H.shift();
    allData.push({n:d.nest,r:d.room,v:d.vent,f:d.fanSpeed,c:d.cooling,h:d.heating,ti:new Date().toLocaleString()});
    dC();
  }catch(e){
    document.getElementById('st').textContent='更新失敗';
    document.getElementById('dot').className='dot err';
  }
}
function toggleSys(){fm=false;var on=document.getElementById('sysBtn').classList.contains('off');fetch('/control?system='+(on?1:0));}
function toggleMode(){var m=document.getElementById('modeBtn').textContent.indexOf('手動')>=0?1:0;fetch('/control?manual='+m).then(function(){doPoll();});}
function setCT(v){
  v=parseFloat(v);
  if(isNaN(v))return;
  v=Math.round(v*10)/10;
  document.getElementById('ctV').value=v.toFixed(1);
  document.getElementById('ct').value=v;
  document.getElementById('nct').textContent=v.toFixed(1);
  fetch('/control?coolTarget='+v);
}
function setHT(v){
  v=parseFloat(v);
  if(isNaN(v))return;
  v=Math.round(v*10)/10;
  document.getElementById('htV').value=v.toFixed(1);
  document.getElementById('ht').value=v;
  document.getElementById('nht').textContent=v.toFixed(1);
  fetch('/control?heatTarget='+v);
}
function setSMin(v){document.getElementById('sminV').textContent=parseFloat(v).toFixed(1);fetch('/control?safeMin='+v);}
function setSMax(v){document.getElementById('smaxV').textContent=parseFloat(v).toFixed(1);fetch('/control?safeMax='+v);}
function setVMax(v){document.getElementById('vmaxV').textContent=parseFloat(v).toFixed(0);fetch('/control?ventMax='+v);}
function setFanM(v){fm=true;document.getElementById('fanV').textContent=v+'%';fetch('/test?fan='+Math.round(v*255/100));setTimeout(function(){fm=false;},3000);}
async function tTest(type,val){
  try{var url=type==='cool'?'/test?cool='+val+'&heat=0&en=1':'/test?heat='+val+'&cool=0&en=1';await fetch(url);}catch(e){}
}
var _cooling=false,_heating=false;
function toggleCool(){_cooling=!_cooling;if(_cooling){_heating=false;tTest('cool',200);}else{tTest('cool',0);}}
function toggleHeat(){_heating=!_heating;if(_heating){_cooling=false;tTest('heat',200);}else{tTest('heat',0);}}
function poll(){clearInterval(pi);pi=setInterval(doPoll,ms);}
function setPoll(v){ms=v*1000;document.getElementById('pollV').textContent=v+'s';poll();}
function setChart(v){
  chartMode=v;
  document.querySelectorAll('.ch-pill').forEach(function(e,i){e.className='pill ch-pill'+(i===v?' act':'');});
  rs();
}
document.querySelectorAll('input[type=range]').forEach(function(s){
  s.addEventListener('wheel',function(e){
    e.preventDefault();
    var step=parseFloat(s.step)||1;
    var v=parseFloat(s.value);
    if(e.deltaY<0)v+=step;else v-=step;
    v=Math.round(v/step)*step;
    v=Math.max(parseFloat(s.min),Math.min(parseFloat(s.max),v));
    s.value=v;
    s.dispatchEvent(new Event('input'));
  },{passive:false});
});
poll();
</script>
</body>
</html>)HTML";

unsigned long lastOled = 0;

void updateOLED() {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_6x12_tr);
    // 溫度
    u8g2.setCursor(0, 10);
    u8g2.print("Nest:");
    u8g2.setCursor(50, 10);
    if (isnan(nestT)) u8g2.print("---"); else u8g2.print(nestT, 1);
    u8g2.print("C");

    u8g2.setCursor(0, 22);
    u8g2.print("Room:");
    u8g2.setCursor(50, 22);
    if (isnan(roomT)) u8g2.print("---"); else u8g2.print(roomT, 1);
    u8g2.print("C");

    u8g2.setCursor(0, 34);
    u8g2.print("Vent:");
    u8g2.setCursor(50, 34);
    if (isnan(ventT)) u8g2.print("---"); else u8g2.print(ventT, 1);
    u8g2.print("C");

    // 分隔線
    u8g2.drawHLine(0, 38, 128);

    // 狀態行
    u8g2.setCursor(0, 50);
    u8g2.print(systemOn ? (cooling ? "Cooling" : heating ? "Heating" : "Idle") : "OFF");
    u8g2.setCursor(64, 50);
    u8g2.print("Fan:");
    u8g2.print((int)(fanSpeed * 100.0 / 255));
    u8g2.print("%");

    // 目標溫度
    u8g2.setCursor(0, 62);
    u8g2.print("Tgt:");
    u8g2.print(cooling ? coolTarget : heatTarget, 1);
    u8g2.print("C");
    u8g2.setCursor(64, 62);
    u8g2.print(manualMode ? "Manual" : "Auto");
  } while (u8g2.nextPage());
}

void handleRoot() { server.send_P(200, "text/html; charset=utf-8", INDEX); }

bool sameAddr(DeviceAddress a, DeviceAddress b) {
  for (int i = 0; i < 8; i++) { if (a[i] != b[i]) return false; }
  return true;
}

void doScan() {
  Serial.println("[DS18B20] 掃描匯流排...");
  dt.begin();
  delay(10);
  int val = digitalRead(DS18B20_PIN);
  Serial.printf("[DS18B20] GPIO%d: %d\n", DS18B20_PIN, val);
  int cnt = dt.getDeviceCount();
  Serial.printf("[DS18B20] count=%d\n", cnt);
  if (cnt > 0) {
    dsOk = true;
    dt.setWaitForConversion(false);
    dt.setResolution(11);
    nestOK = false; roomOK = false; ventOK = false;
    DeviceAddress addr;
    for (int i = 0; i < cnt && i < 3; i++) {
      if (dt.getAddress(addr, i)) {
        // ROM 位址認人
        if (sameAddr(addr, nestAddr)) { nestOK = true; Serial.print("[巢穴] "); }
        else if (sameAddr(addr, roomAddr)) { roomOK = true; Serial.print("[活動] "); }
        else if (sameAddr(addr, ventAddr)) { ventOK = true; Serial.print("[出風] "); }
        else { Serial.print("[?] "); }
        char buf[24];
        sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X%02X",
          addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
        Serial.println(buf);
      }
    }
    Serial.printf("[DS18B20] 就緒 巢穴=%d 活動=%d 出風=%d\n", nestOK, roomOK, ventOK);
  } else {
    dsOk = false;
    nestT = NAN; roomT = NAN; ventT = NAN;
    ds.reset_search(); delay(10);
    bool present = !ds.reset();
    Serial.printf("[DS18B20] present=%d\n", present);
    uint8_t rawAddr[8];
    if (ds.search(rawAddr)) {
      char buf[24];
      sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X%02X",
        rawAddr[0], rawAddr[1], rawAddr[2], rawAddr[3],
        rawAddr[4], rawAddr[5], rawAddr[6], rawAddr[7]);
      Serial.printf("[DS18B20] raw=%s\n", buf);
    } else {
      Serial.println("[DS18B20] 無裝置");
    }
  }
}

void setup() {
  Serial.begin(115200); delay(500);
  Serial.printf("\n========== TEC 蟄眠溫控 ==========\n%s %s\n", __DATE__, __TIME__);
  Serial.printf("GPIO%d, 3× DS18B20: 巢穴/活動/出風\n", DS18B20_PIN);

  pinMode(TEC_EN, OUTPUT); digitalWrite(TEC_EN, LOW);
  ledcSetup(FAN_CH, PWM_FREQ, PWM_RES); ledcAttachPin(FAN_PIN, FAN_CH);
  ledcSetup(TEC_L_CH, PWM_FREQ, PWM_RES); ledcAttachPin(TEC_LPWM, TEC_L_CH);
  ledcSetup(TEC_R_CH, PWM_FREQ, PWM_RES); ledcAttachPin(TEC_RPWM, TEC_R_CH);
  setFan(0); setTec(0, 0);

  doScan();

  u8g2.begin();
  u8g2.setContrast(128);
  Serial.println("[OLED] 就緒");

  WiFi.softAP("ESP32-TEMP", "12345678");
  Serial.printf("[WiFi] %s\n", WiFi.softAPIP().toString().c_str());

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/diag", handleDiag);
  server.on("/control", handleControl);
  server.on("/test", handleTest);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("[Server] OK\n");
}

void loop() {
  server.handleClient();
  if (!dsOk && millis() - lastScan >= 10000) { lastScan = millis(); doScan(); }
  if (millis() - lastRead >= 2000) { lastRead = millis(); readSensor(); }
  if (millis() - lastOled >= 2000) { lastOled = millis(); updateOLED(); }
}
