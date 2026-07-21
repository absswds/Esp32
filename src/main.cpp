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
:root{--bg:#0b1220;--card:rgba(30,41,59,.6);--border:rgba(148,163,184,.12);--txt:#e2e8f0;--txt2:#94a3b8;--acc:#60a5fa;--good:#22c55e;--bad:#f87171;}
*{box-sizing:border-box;margin:0;padding:0}
body{min-height:100vh;font-family:system-ui,-apple-system,sans-serif;background:radial-gradient(1200px 600px at 80% -10%,rgba(96,165,250,.10),transparent),var(--bg);color:var(--txt);padding:16px;max-width:680px;margin:0 auto}
.hero{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px;padding:18px 22px;border-radius:18px;background:var(--card);backdrop-filter:blur(16px);border:1px solid var(--border);margin-bottom:16px}
.hero h1{font-size:1.15rem;font-weight:700;letter-spacing:.3px}
.hero h1 span{color:var(--acc)}
.status-wrap{display:flex;align-items:center;gap:8px;font-size:.82rem;color:var(--txt2)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--good);box-shadow:0 0 8px var(--good)}
.dot.err{background:var(--bad);box-shadow:0 0 8px var(--bad)}
.pills{display:flex;gap:2px;background:rgba(15,23,42,.5);border:1px solid var(--border);border-radius:10px;padding:3px}
.pill{border:none;background:transparent;color:var(--txt2);padding:5px 12px;border-radius:8px;cursor:pointer;font-size:.8rem}
.pill.active{background:var(--acc);color:#fff;font-weight:600}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:16px}
.m{background:var(--card);backdrop-filter:blur(12px);border:1px solid var(--border);border-radius:14px;padding:14px 10px;text-align:center;transition:transform .15s}
.m:hover{transform:translateY(-2px)}
.m .v{font-size:1.55rem;font-weight:700;line-height:1.1}
.m .l{font-size:.72rem;color:var(--txt2);margin-top:4px}
.trend{font-size:.7rem;margin-left:4px}
.up{color:var(--bad)}.down{color:var(--good)}.flat{color:var(--txt2)}
.t{color:#f87171}.hh{color:#60a5fa}.p{color:#22c55e}.g{color:#fbbf24}
.card{background:var(--card);backdrop-filter:blur(14px);border:1px solid var(--border);border-radius:16px;padding:16px;margin-bottom:16px}
.card h2{font-size:.85rem;color:var(--txt2);font-weight:600;margin-bottom:12px;text-transform:uppercase;letter-spacing:.5px}
.legend{display:flex;gap:14px;flex-wrap:wrap;margin-bottom:10px}
.legend label{display:flex;align-items:center;gap:5px;font-size:.78rem;cursor:pointer;user-select:none}
.sw{width:14px;height:3px;border-radius:2px}
canvas{width:100%;height:200px;display:block}
.fan-row{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
button{background:var(--acc);color:#fff;border:none;border-radius:9px;padding:8px 16px;cursor:pointer;font-size:.85rem;font-weight:600;transition:filter .15s}
button:hover{filter:brightness(1.08)}
button.ghost{background:rgba(148,163,184,.15);color:var(--txt)}
button.on{background:var(--good)}
input[type=range]{flex:1;min-width:130px;accent-color:var(--acc)}
input[type=number]{width:72px;background:rgba(15,23,42,.6);border:1px solid var(--border);color:var(--txt);border-radius:8px;padding:6px 8px;font-size:.9rem}
table{width:100%;border-collapse:collapse;font-size:.78rem}
th,td{padding:5px 6px;text-align:center;border-bottom:1px solid var(--border);white-space:nowrap}
th{color:var(--txt2);font-weight:600}
.hist-wrap{max-height:260px;overflow:auto;margin-bottom:10px;border-radius:8px}
@media(max-width:480px){.grid{grid-template-columns:1fr 1fr}.m .v{font-size:1.35rem}}
</style>
</head>
<body>
<div class="hero">
  <h1>BME688 <span>環境監測</span></h1>
  <div class="status-wrap"><span class="dot" id="dot"></span><span id="st">等待連線...</span></div>
</div>
<div class="pills" style="justify-self:center;margin:0 auto 16px;display:flex">
  <button class="pill" data-ms="1000">1s</button>
  <button class="pill active" data-ms="2000">2s</button>
  <button class="pill" data-ms="5000">5s</button>
</div>
<div class="grid">
  <div class="m"><div class="v t" id="mT">--<span class="trend" id="tT"></span></div><div class="l">溫度 °C</div></div>
  <div class="m"><div class="v hh" id="mH">--<span class="trend" id="tH"></span></div><div class="l">濕度 %</div></div>
  <div class="m"><div class="v p" id="mP">--<span class="trend" id="tP"></span></div><div class="l">氣壓 hPa</div></div>
  <div class="m"><div class="v g" id="mG">--<span class="trend" id="tG"></span></div><div class="l">氣體 kΩ</div></div>
  <div class="m"><div class="v" id="mA">--</div><div class="l">AQI</div></div>
  <div class="m"><div class="v" id="mF">--</div><div class="l">風扇 %</div></div>
</div>
<div class="card">
  <h2>趨勢圖</h2>
  <div class="legend" id="legend"></div>
  <canvas id="chart" width="600" height="200"></canvas>
</div>
<div class="card">
  <h2>風扇控制</h2>
  <div class="fan-row">
    <button id="autoBtn" class="on" onclick="toggleAuto()">自動</button>
    <label style="font-size:.82rem;color:var(--txt2)">目標</label>
    <input type="number" id="tgt" min="10" max="40" step="0.5" value="25" onchange="setTarget(this.value)">
    <span style="font-size:.82rem;color:var(--txt2)">°C</span>
    <input type="range" min="0" max="100" value="0" id="slider" oninput="setManual(this.value)">
    <span id="fanPct" style="min-width:42px;text-align:right;font-weight:600">0%</span>
  </div>
</div>
<div class="card">
  <h2>歷史資料</h2>
  <div class="hist-wrap"><table id="hist"><thead><tr><th>時間</th><th>溫度</th><th>濕度</th><th>氣壓</th><th>氣體</th><th>AQI</th></tr></thead><tbody></tbody></table></div>
  <button class="ghost" onclick="clearHist()">清除</button>
  <button class="ghost" style="margin-left:6px" onclick="exportCSV()">匯出 CSV</button>
</div>
<script>
var hist=[],autoMode=true,pollMs=2000,tid=null;
var KEYS=['T','H','P','G'],COLORS={T:'#f87171',H:'#60a5fa',P:'#22c55e',G:'#fbbf24'},LABELS={T:'溫度',H:'濕度',P:'氣壓',G:'氣體'};
var vis={T:true,H:true,P:false,G:false};
var cvs=document.getElementById('chart'),ctx=cvs.getContext('2d');
var prev={T:null,H:null,P:null,G:null};

function resize(){var r=cvs.getBoundingClientRect();cvs.width=r.width*devicePixelRatio;cvs.height=r.height*devicePixelRatio;ctx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);draw();}
window.addEventListener('resize',resize);

function trendIcon(cur,k){
  var p=prev[k];prev[k]=cur;
  if(p===null||isNaN(cur)||isNaN(p))return '';
  var d=cur-p;
  if(Math.abs(d)<0.05)return '<span class="trend flat">—</span>';
  return d>0?'<span class="trend up">↑</span>':'<span class="trend down">↓</span>';
}

function draw(){
  var W=cvs.getBoundingClientRect().width,H=cvs.getBoundingClientRect().height;
  var pad={l:40,r:12,t:12,b:22};
  ctx.clearRect(0,0,W,H);
  if(hist.length<2){ctx.fillStyle='#64748b';ctx.font='13px system-ui';ctx.textAlign='center';ctx.fillText('等待資料累積中...',W/2,H/2);return;}
  var usable=hist.slice().reverse();
  var act=KEYS.filter(k=>vis[k]);
  if(!act.length){ctx.fillStyle='#64748b';ctx.textAlign='center';ctx.fillText('請勾選要顯示的項目',W/2,H/2);return;}
  var ranges={};
  act.forEach(k=>{var arr=usable.map(r=>r[k]);ranges[k]={min:Math.min.apply(null,arr),max:Math.max.apply(null,arr)};if(ranges[k].min===ranges[k].max){ranges[k].min-=1;ranges[k].max+=1;}});
  var plotW=W-pad.l-pad.r,plotH=H-pad.t-pad.b;
  ctx.strokeStyle='rgba(148,163,184,.1)';ctx.lineWidth=1;ctx.fillStyle='#64748b';ctx.font='10px system-ui';ctx.textAlign='right';
  for(var i=0;i<=4;i++){var y=pad.t+plotH*i/4;ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(W-pad.r,y);ctx.stroke();ctx.fillText(String(i*25),pad.l-6,y+3);}
  ctx.textAlign='center';
  var step=plotW/Math.max(1,usable.length-1);
  var xtick=Math.ceil(usable.length/6);
  usable.forEach((r,i)=>{if(i%xtick===0||i===usable.length-1){var x=pad.l+step*i;ctx.fillText(r.time,x,H-pad.b+14);}});
  act.forEach(k=>{
    var rg=ranges[k];ctx.beginPath();ctx.strokeStyle=COLORS[k];ctx.lineWidth=2;ctx.lineJoin='round';
    usable.forEach((r,i)=>{var x=pad.l+step*i;var y=pad.t+(1-(r[k]-rg.min)/(rg.max-rg.min))*plotH;i?ctx.lineTo(x,y):ctx.moveTo(x,y);});
    ctx.stroke();
  });
}

function buildLegend(){
  document.getElementById('legend').innerHTML=KEYS.map(k=>'<label><input type="checkbox" '+(vis[k]?'checked':'')+' onchange="vis.'+k+'=this.checked;draw()"><span class="sw" style="background:'+COLORS[k]+'"></span>'+LABELS[k]+'</label>').join('');
}

function renderHist(){
  document.querySelector('#hist tbody').innerHTML=hist.map(r=>'<tr><td>'+r.time+'</td><td>'+r.T.toFixed(1)+'</td><td>'+r.H.toFixed(1)+'</td><td>'+r.P.toFixed(1)+'</td><td>'+r.G.toFixed(1)+'</td><td>'+r.A+'</td></tr>').join('');
}

async function poll(){
  try{
    var r=await fetch('/data');
    var d=await r.json();
    if(!d.ok){document.getElementById('st').textContent=d.message;document.getElementById('dot').className='dot err';return;}
    document.getElementById('dot').className='dot';
    document.getElementById('st').textContent='最後更新 '+new Date().toLocaleTimeString();
    document.getElementById('mT').innerHTML=d.temperature.toFixed(1)+trendIcon(d.temperature,'T');
    document.getElementById('mH').innerHTML=d.humidity.toFixed(1)+trendIcon(d.humidity,'H');
    document.getElementById('mP').innerHTML=d.pressure.toFixed(1)+trendIcon(d.pressure,'P');
    document.getElementById('mG').innerHTML=d.gas.toFixed(1)+trendIcon(d.gas,'G');
    document.getElementById('mA').textContent=d.aqi;
    var fp=Math.round(d.fanSpeed/255*100);
    document.getElementById('mF').textContent=fp+'%';
    autoMode=d.auto;
    document.getElementById('autoBtn').className=autoMode?'on':'ghost';
    document.getElementById('slider').value=fp;
    document.getElementById('fanPct').textContent=fp+'%';
    document.getElementById('tgt').value=d.target;
    hist.unshift({time:new Date().toLocaleTimeString(),T:d.temperature,H:d.humidity,P:d.pressure,G:d.gas,A:d.aqi});
    if(hist.length>60)hist.pop();
    renderHist();draw();
  }catch(e){
    document.getElementById('st').textContent='更新失敗';
    document.getElementById('dot').className='dot err';
  }
}

function startPoll(){if(tid)clearInterval(tid);poll();tid=setInterval(poll,pollMs);}
function toggleAuto(){autoMode=!autoMode;fetch('/fan?auto='+(autoMode?1:0));document.getElementById('autoBtn').className=autoMode?'on':'ghost';}
function setTarget(v){fetch('/fan?target='+v);}
function setManual(v){if(autoMode)return;document.getElementById('fanPct').textContent=v+'%';fetch('/fan?speed='+v);}
function clearHist(){hist=[];renderHist();draw();}
function exportCSV(){
  if(!hist.length){alert('尚無資料');return;}
  var csv='\uFEFF時間,溫度,濕度,氣壓,氣體,AQI\n'+hist.map(r=>r.time+','+r.T+','+r.H+','+r.P+','+r.G+','+r.A).join('\n');
  var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([csv],{type:'text/csv'}));a.download='BME688.csv';a.click();
}
document.querySelectorAll('.pill').forEach(b=>b.onclick=()=>{document.querySelectorAll('.pill').forEach(x=>x.classList.remove('active'));b.classList.add('active');pollMs=parseInt(b.dataset.ms);startPoll();});
buildLegend();resize();startPoll();
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