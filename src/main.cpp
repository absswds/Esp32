#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>

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

float t = NAN;
int fanSpeed = 0;
float coolStop = 26.0;
float heatStop = 28.0;
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

float safeMin = 10.0;
float safeMax = 40.0;

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

void controlTemp() {
  if (!systemOn || isnan(t) || tecManual || manualMode) return;
  if (t < safeMin || t > safeMax) {
    setTec(0, 0);
    if (!fanManual) setFan(t > safeMax ? 255 : 60);
    Serial.println("[SAFE] 極端溫度");
    return;
  }
  if (t >= heatStop) {
    setTec(220, 0);
    if (!fanManual) setFan(255);
  } else if (t <= coolStop) {
    setTec(0, 200);
    if (!fanManual) setFan(200);
  } else {
    setTec(0, 0);
    if (!fanManual) setFan(100);
  }
}

float readTemps[3] = {NAN, NAN, NAN};

void doScan() {
  Serial.println("[DS18B20] 開始掃描匯流排...");
  pinMode(DS18B20_PIN, INPUT_PULLUP);
  delay(10);
  int val = digitalRead(DS18B20_PIN);
  Serial.printf("[DS18B20] GPIO%d 電位: %d (1=HIGH=上拉OK, 0=LOW=可能短路)\n", DS18B20_PIN, val);
  dt.begin();
  int cnt = dt.getDeviceCount();
  Serial.printf("[DS18B20] getDeviceCount=%d\n", cnt);
  if (cnt > 0) {
    dsOk = true;
    dt.setWaitForConversion(false);
    dt.setResolution(11);
    DeviceAddress addr;
    for (int i = 0; i < cnt && i < 3; i++) {
      if (dt.getAddress(addr, i)) {
        char buf[24];
        sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X%02X",
          addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
        Serial.printf("[DS18B20]   [%d] %s\n", i, buf);
      }
    }
    Serial.printf("[DS18B20] 成功, count=%d\n", cnt);
  } else {
    dsOk = false;
    t = NAN;

    // Raw OneWire 重置脈衝檢測
    ds.reset_search();
    delay(10);
    bool present = !ds.reset();   // reset() returns 1 if no device (pulled low fails)
    Serial.printf("[DS18B20] raw reset pulse -> present=%d (1=有裝置應答, 0=無裝置)\n", present);

    // 嘗試 raw search
    uint8_t rawAddr[8];
    bool foundOne = ds.search(rawAddr);
    if (foundOne) {
      char buf[24];
      sprintf(buf, "%02X%02X%02X%02X%02X%02X%02X%02X",
        rawAddr[0], rawAddr[1], rawAddr[2], rawAddr[3],
        rawAddr[4], rawAddr[5], rawAddr[6], rawAddr[7]);
      Serial.printf("[DS18B20] raw search 找到: %s （DallasTemperature 卻回報0，可能是庫相容問題）\n", buf);
    } else {
      Serial.println("[DS18B20] raw search 也沒找到裝置（純硬體問題）");
    }

    Serial.println("[DS18B20] 可能原因:");
    if (!present) {
      Serial.println("[DS18B20]   *** 首要懷疑: DATA腳和VCC間缺少4.7kΩ上拉電阻 ***");
    }
    Serial.println("[DS18B20]   1. 缺少4.7kΩ上拉電阻(gpio到3.3V)");
    Serial.println("[DS18B20]   2. DS18B20的VCC/GND沒接");
    Serial.println("[DS18B20]   3. GPIO腳選錯(板子印字 vs GPIO編號)");
    Serial.println("[DS18B20]   4. DS18B20本身故障");
    Serial.println("[DS18B20] 每10秒將重試...");
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

  int n = dt.getDeviceCount();
  float sum = 0;
  int valid = 0;
  for (int i = 0; i < n && i < 3; i++) {
    float v = dt.getTempCByIndex(i);
    if (v == DEVICE_DISCONNECTED_F || isnan(v)) {
      Serial.printf("[DS18BOT%d] 斷線\n", i);
      readTemps[i] = NAN;
      continue;
    }
    readTemps[i] = v;
    sum += v;
    valid++;
  }
  if (valid == 0) {
    Serial.println("[DS18B20] 全數斷線");
    t = NAN;
    return;
  }
  t = sum / valid;
  controlTemp();
  Serial.printf("[T:%.1f (avg %d/%d)] %s %s Fan:%d\n",
                t, valid, n,
                cooling ? "COOL" : heating ? "HEAT" : "IDLE",
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
  if (isnan(t)) { sendJson("等待感測資料..."); return; }
  int n = dt.getDeviceCount();
  char buf[640];
  snprintf(buf, sizeof(buf),
    "{\"ok\":true,\"temperature\":%.2f,\"temps\":[%.2f,%.2f,%.2f],\"sensorCount\":%d,\"fanSpeed\":%d,\"cooling\":%s,\"heating\":%s,\"systemOn\":%s,\"manualMode\":%s,\"coolStop\":%.1f,\"heatStop\":%.1f,\"safeMin\":%.1f,\"safeMax\":%.1f}",
    t, readTemps[0], readTemps[1], readTemps[2], n,
    fanSpeed, cooling ? "true" : "false", heating ? "true" : "false", systemOn ? "true" : "false", manualMode ? "true" : "false", coolStop, heatStop, safeMin, safeMax);
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
  if (server.hasArg("coolStop")) {
    float v = server.arg("coolStop").toFloat();
    coolStop = constrain(v, 0, 50);
  }
  if (server.hasArg("heatStop")) {
    float v = server.arg("heatStop").toFloat();
    heatStop = constrain(v, 0, 50);
  }
  if (server.hasArg("safeMin")) {
    float v = server.arg("safeMin").toFloat();
    safeMin = constrain(v, 0, 50);
  }
  if (server.hasArg("safeMax")) {
    float v = server.arg("safeMax").toFloat();
    safeMax = constrain(v, 0, 50);
  }
  controlTemp();
  server.send(200, "text/plain", "OK");
}

void handleDiag() {
  char buf[512];
  int pinVal = digitalRead(DS18B20_PIN);
  int cnt = dt.getDeviceCount();
  snprintf(buf, sizeof(buf),
    "{\"pin\":%d,\"pinState\":%d,\"dsOk\":%s,\"deviceCount\":%d,\"temp\":%.2f,\"conversionPending\":%s}",
    DS18B20_PIN, pinVal, dsOk ? "true" : "false", cnt, t, convPending ? "true" : "false");
  server.send(200, "application/json", buf);
}

void handleTest() {
  int fanVal = server.hasArg("fan") ? server.arg("fan").toInt() : -1;
  int coolVal = server.hasArg("cool") ? server.arg("cool").toInt() : -1;
  int heatVal = server.hasArg("heat") ? server.arg("heat").toInt() : -1;
  int enVal = server.hasArg("en") ? server.arg("en").toInt() : -1;

  if (enVal >= 0) digitalWrite(TEC_EN, enVal ? HIGH : LOW);
  if (fanVal >= 0) {
    setFan(fanVal);
    fanManual = true;
  }
  if (coolVal >= 0 || heatVal >= 0) {
    setTec(max(0, coolVal), max(0, heatVal));
    tecManual = true;
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
<title>TEC</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{--bg:#0a0e14;--sf:#111820;--bd:#1e2a36;--tx:#e0e6ed;--t2:#7a8a9a;--t3:#4a5568;--r:#ef4444;--b:#3b82f6;--g:#22c55e;--a:#eab308;--c:#14b8a6}
body{font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;background:var(--bg);color:var(--tx);padding:12px 16px;max-width:600px;margin:0 auto}
.hdr{display:flex;align-items:center;justify-content:space-between;padding:12px 0 10px;border-bottom:1px solid var(--bd);margin-bottom:10px}
.hdr h1{font-size:1.05rem;font-weight:700}
.hdr h1 b{color:var(--c)}
.conn{display:flex;align-items:center;gap:6px;font-size:.7rem;color:var(--t2)}
.dot{width:6px;height:6px;border-radius:50%;background:var(--g);flex-shrink:0}
.dot.err{background:var(--r)}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:6px;margin-bottom:10px}
.card{background:var(--sf);border:1px solid var(--bd);border-radius:8px;padding:10px 4px;text-align:center;position:relative;overflow:hidden}
.card::before{content:'';position:absolute;top:0;left:0;right:0;height:2px}
.card.cr::before{background:var(--r)}.card.cb::before{background:var(--b)}.card.cg::before{background:var(--g)}.card.ca::before{background:var(--a)}
.card .val{font-size:1.4rem;font-weight:800;line-height:1;font-variant-numeric:tabular-nums}
.card .lbl{font-size:.55rem;color:var(--t3);margin-top:3px;text-transform:uppercase;letter-spacing:.5px}
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
.fld{display:flex;align-items:center;gap:8px;margin-bottom:8px}
.fld label{font-size:.7rem;color:var(--t2);min-width:60px}
.fld input[type=range]{flex:1;height:3px;-webkit-appearance:none;appearance:none;background:var(--bd);border-radius:2px;outline:none}
.fld input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:16px;height:16px;border-radius:50%;background:var(--c);cursor:pointer}
.fld .rv{font-size:.85rem;font-weight:800;color:var(--c);min-width:40px;text-align:right;font-variant-numeric:tabular-nums}
.info{font-size:.62rem;color:var(--t3);line-height:1.6;margin-top:4px}
.info b{color:var(--t2)}
.act-row{display:flex;gap:5px;margin-top:6px}
.act-btn{flex:1;padding:7px;border-radius:5px;border:1px solid var(--bd);background:0 0;color:var(--t2);cursor:pointer;font-size:.68rem;font-weight:600;transition:all .15s}
.act-btn:active{transform:scale(.95);background:rgba(20,184,166,.1);border-color:var(--c);color:var(--c)}
#chart{width:100%;height:130px;display:block}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(16px);background:var(--c);color:#000;padding:7px 18px;border-radius:8px;font-size:.78rem;font-weight:700;opacity:0;transition:all .25s;pointer-events:none;z-index:99}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
@media(max-width:400px){.grid{grid-template-columns:repeat(2,1fr)}.card .val{font-size:1.1rem}}
</style>
</head>
<body>
<div class="hdr">
  <h1><b>TEC</b> 溫控系統</h1>
  <div class="conn"><span class="dot" id="dot"></span><span id="st">...</span></div>
</div>
<div class="grid">
  <div class="card cr"><div class="val" id="mT">--</div><div class="lbl">溫度 °C</div></div>
</div>
<div class="sec">
  <h2>溫度趨勢</h2>
  <canvas id="chart"></canvas>
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
  <h2>溫度閾值</h2>
  <div class="fld">
    <label>高溫製冷</label>
    <input type="range" min="15" max="35" step="0.1" value="28" id="hs" oninput="setHS(this.value)">
    <span class="rv" id="hsv">28</span>
  </div>
  <div class="fld">
    <label>低溫加熱</label>
    <input type="range" min="10" max="30" step="0.1" value="26" id="cs" oninput="setCS(this.value)">
    <span class="rv" id="csvv">26</span>
  </div>
  <div class="info"><b>製冷</b>: ＞<span id="nhs">28</span>°C | <b>加熱</b>: ＜<span id="ncs">26</span>°C | 安全: ＜<span id="nmin">10</span>°C / ＞<span id="nmax">40</span>°C</div>
</div>
<div class="sec">
  <h2>安全溫度</h2>
  <div class="fld">
    <label>最低溫</label>
    <input type="range" min="0" max="30" step="0.5" value="10" id="smin" oninput="setSMin(this.value)">
    <span class="rv" id="sminV">10</span>
  </div>
  <div class="fld">
    <label>最高溫</label>
    <input type="range" min="20" max="50" step="0.5" value="40" id="smax" oninput="setSMax(this.value)">
    <span class="rv" id="smaxV">40</span>
  </div>
  <div class="info">低於最低或超過最高溫，TEC 自動關閉保護</div>
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
var H=[],M=120,ms=2000,pi=null,fm=false;
var cv=document.getElementById('chart'),cx=cv.getContext('2d');
function rs(){var r=cv.getBoundingClientRect();cv.width=r.width*devicePixelRatio;cv.height=r.height*devicePixelRatio;cx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);dC();}
window.addEventListener('resize',rs);
function dC(){
  var W=cv.getBoundingClientRect().width,HH=cv.getBoundingClientRect().height,pl=38,pr=8,pt=10,pb=18;
  cx.clearRect(0,0,W,HH);
  if(H.length<2){cx.fillStyle='#4a5568';cx.font='11px system-ui';cx.textAlign='center';cx.fillText('等待資料...',W/2,HH/2);return;}
  var mn=1e9,mx=-1e9;H.forEach(function(r){if(r.t<mn)mn=r.t;if(r.t>mx)mx=r.t;});
  var sp=mx-mn;if(sp<1){mn-=1;mx+=1;sp=2;}
  var pa=sp*.12;mn-=pa;mx+=pa;sp=mx-mn;
  var pw=W-pl-pr,ph=HH-pt-pb;
  cx.strokeStyle='rgba(122,138,154,.06)';cx.lineWidth=1;cx.fillStyle='#4a5568';cx.font='9px system-ui';cx.textAlign='right';
  for(var i=0;i<=4;i++){var v=mx-(sp*i/4),y=pt+ph*i/4;cx.beginPath();cx.moveTo(pl,y);cx.lineTo(W-pr,y);cx.stroke();cx.fillText(v.toFixed(1),pl-4,y+3);}
  cx.textAlign='center';cx.font='8px system-ui';
  var st=pw/Math.max(1,H.length-1),xt=Math.max(1,Math.ceil(H.length/5));
  H.forEach(function(r,i){if(i%xt===0||i===H.length-1)cx.fillText(r.ti,pl+st*i,HH-pb+12);});
  var gd=cx.createLinearGradient(0,pt,0,HH-pb);gd.addColorStop(0,'rgba(239,68,68,.2)');gd.addColorStop(1,'rgba(239,68,68,0)');
  cx.beginPath();H.forEach(function(r,i){var x=pl+st*i,y=pt+(1-(r.t-mn)/sp)*ph;i?cx.lineTo(x,y):cx.moveTo(x,y);});
  cx.lineTo(pl+st*(H.length-1),HH-pb);cx.lineTo(pl,HH-pb);cx.closePath();cx.fillStyle=gd;cx.fill();
  cx.beginPath();cx.strokeStyle='#ef4444';cx.lineWidth=2;cx.lineJoin='round';
  H.forEach(function(r,i){var x=pl+st*i,y=pt+(1-(r.t-mn)/sp)*ph;i?cx.lineTo(x,y):cx.moveTo(x,y);});cx.stroke();
  var la=H[H.length-1],lx=pl+st*(H.length-1),ly=pt+(1-(la.t-mn)/sp)*ph;
  cx.beginPath();cx.arc(lx,ly,4,0,Math.PI*2);cx.fillStyle='#ef4444';cx.fill();
  cx.beginPath();cx.arc(lx,ly,7,0,Math.PI*2);cx.strokeStyle='rgba(239,68,68,.3)';cx.lineWidth=2;cx.stroke();
  cx.fillStyle='#ef4444';cx.font='bold 10px system-ui';cx.textAlign='right';cx.fillText(la.t.toFixed(1)+'C',lx-10,ly-8);
}
rs();
function toast(m){var e=document.getElementById('toast');e.textContent=m;e.className='toast show';setTimeout(function(){e.className='toast';},1500);}
function exportCSV(){
  if(!H.length){toast('無資料');return;}
  var c='﻿時間,溫度(°C)\n'+H.map(function(r){return r.ti+','+r.t.toFixed(2);}).join('\n');
  var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([c],{type:'text/csv'}));a.download='TEC_'+new Date().toISOString().slice(0,10)+'.csv';a.click();
  toast('已導出 '+H.length+' 筆資料');
}
function clearHist(){H=[];rs();toast('記錄已清除');}
async function doPoll(){
  try{
    var r=await fetch('/data'),d=await r.json();
    if(!d.ok){document.getElementById('st').textContent=d.message;document.getElementById('dot').className='dot err';return;}
    document.getElementById('dot').className='dot';
    document.getElementById('st').textContent=new Date().toLocaleTimeString();
    document.getElementById('mT').textContent=d.temperature.toFixed(1);
    document.getElementById('sysBtn').className=d.systemOn?'btn on':'btn off';
    document.getElementById('sysBtn').textContent=d.systemOn?'停止系統':'開啟系統';
    var ps=d.systemOn?(d.cooling?'製冷中':d.heating?'加熱中':'運轉中'):'待機';
    document.getElementById('pSys').textContent=ps;
    document.getElementById('pSys').className='pill sys'+(d.systemOn?' act':'');
    document.getElementById('pCool').className='pill cold'+(d.cooling?' act':'');
    document.getElementById('pHeat').className='pill hot'+(d.heating?' act':'');
    _cooling=d.cooling;_heating=d.heating;
    document.getElementById('hs').value=d.heatStop;
    document.getElementById('hsv').textContent=d.heatStop.toFixed(1);
    document.getElementById('cs').value=d.coolStop;
    document.getElementById('csvv').textContent=d.coolStop.toFixed(1);
    document.getElementById('nhs').textContent=d.heatStop.toFixed(1);
    document.getElementById('ncs').textContent=d.coolStop.toFixed(1);
    document.getElementById('smin').value=d.safeMin;
    document.getElementById('sminV').textContent=d.safeMin.toFixed(1);
    document.getElementById('smax').value=d.safeMax;
    document.getElementById('smaxV').textContent=d.safeMax.toFixed(1);
    document.getElementById('nmin').textContent=d.safeMin.toFixed(1);
    document.getElementById('nmax').textContent=d.safeMax.toFixed(1);
    document.getElementById('modeBtn').textContent=d.manualMode?'切換自動模式':'切換手動模式';
    document.getElementById('modeBtn').style.background=d.manualMode?'rgba(239,68,68,.15)':'rgba(20,184,166,.15)';
    document.getElementById('modeBtn').style.color=d.manualMode?'var(--r)':'var(--c)';
    if(!fm){
      document.getElementById('fanV').textContent=Math.round(d.fanSpeed*100/255)+'%';
      document.getElementById('fanS').value=Math.round(d.fanSpeed*100/255);
    }
    H.push({t:d.temperature,ti:new Date().toLocaleTimeString()});
    if(H.length>M)H.shift();
    dC();
  }catch(e){
    document.getElementById('st').textContent='更新失敗';
    document.getElementById('dot').className='dot err';
  }
}
function toggleSys(){fm=false;var on=document.getElementById('sysBtn').classList.contains('off');fetch('/control?system='+(on?1:0));}
function toggleMode(){var m=document.getElementById('modeBtn').textContent.indexOf('手動')>=0?1:0;fetch('/control?manual='+m).then(function(){doPoll();});}
function setHS(v){document.getElementById('hsv').textContent=parseFloat(v).toFixed(1);fetch('/control?heatStop='+v);}
function setCS(v){document.getElementById('csvv').textContent=parseFloat(v).toFixed(1);fetch('/control?coolStop='+v);}
function setSMin(v){document.getElementById('sminV').textContent=parseFloat(v).toFixed(1);document.getElementById('nmin').textContent=parseFloat(v).toFixed(1);fetch('/control?safeMin='+v);}
function setSMax(v){document.getElementById('smaxV').textContent=parseFloat(v).toFixed(1);document.getElementById('nmax').textContent=parseFloat(v).toFixed(1);fetch('/control?safeMax='+v);}
function setFanM(v){fm=true;document.getElementById('fanV').textContent=v+'%';fetch('/test?fan='+Math.round(v*255/100));setTimeout(function(){fm=false;},3000);}
async function tTest(type,val){
  try{
    var url=type==='cool'?'/test?cool='+val+'&heat=0&en=1':'/test?heat='+val+'&cool=0&en=1';
    await fetch(url);
  }catch(e){}
}
var _cooling=false,_heating=false;
function toggleCool(){
  _cooling=!_cooling;
  if(_cooling){_heating=false;tTest('cool',220);}
  else{tTest('cool',0);}
}
function toggleHeat(){
  _heating=!_heating;
  if(_heating){_cooling=false;tTest('heat',220);}
  else{tTest('heat',0);}
}
function poll(){clearInterval(pi);pi=setInterval(doPoll,ms);}
function setPoll(v){ms=v*1000;document.getElementById('pollV').textContent=v+'s';poll();}
poll();
</script>
</body>
</html>)HTML";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n========== TEC 啟動 ==========");
  Serial.printf("晶片: ESP32, 編譯: %s %s\n", __DATE__, __TIME__);
  Serial.printf("DS18B20 接腳: GPIO%d\n", DS18B20_PIN);

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

  WiFi.softAP("ESP32-TEMP", "12345678");
  Serial.print("[WiFi] AP IP: ");
  Serial.println(WiFi.softAPIP());

  doScan();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/diag", handleDiag);
  server.on("/control", handleControl);
  server.on("/test", handleTest);
  server.onNotFound([]() { server.send(404, "text/plain", "404"); });
  server.begin();
  Serial.println("[Server] 啟動完成");
  Serial.println("==============================\n");
}

void loop() {
  server.handleClient();

  if (!dsOk && millis() - lastScan >= 10000) {
    lastScan = millis();
    doScan();
  }

  if (millis() - lastRead >= 2000) {
    lastRead = millis();
    readSensor();
  }
}
