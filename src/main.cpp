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
:root{--bg:#0f1318;--sf:#161b22;--bd:#2d333b;--tx:#e6edf3;--t2:#8b949e;--t3:#484f58;--r:#f85149;--b:#58a6ff;--g:#3fb950;--a:#d29922;--c:#39d2c0}
body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--tx);padding:16px 20px;max-width:1040px;margin:0 auto;line-height:1.4}
.row{display:flex;align-items:center;justify-content:space-between;padding-bottom:12px;border-bottom:1px solid var(--bd);margin-bottom:14px}
.row h1{font-size:1.05rem;font-weight:600}
.row h1 b{color:var(--c)}
.st{display:flex;align-items:center;gap:6px;font-size:.75rem;color:var(--t2)}
.dot{width:6px;height:6px;border-radius:50%;background:var(--g);flex-shrink:0}
.dot.err{background:var(--r)}
.pw{text-align:center;margin-bottom:14px}
.pg{display:inline-flex;background:var(--sf);border:1px solid var(--bd);border-radius:4px;padding:2px;gap:1px}
.pg button{border:none;background:0 0;color:var(--t3);padding:4px 13px;border-radius:3px;cursor:pointer;font-size:.75rem;font-weight:500}
.pg button.on{background:var(--c);color:#000;font-weight:600}
.g3{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-bottom:14px}
.mc{background:var(--sf);border:1px solid var(--bd);border-radius:4px;padding:12px 10px 8px;text-align:center;position:relative;overflow:hidden}
.mc::after{content:'';position:absolute;top:0;left:0;right:0;height:2px}
.mc.a::after{background:var(--r)}.mc.b::after{background:var(--b)}.mc.c::after{background:var(--g)}.mc.d::after{background:var(--a)}
.mc .v{font-size:1.6rem;font-weight:700;font-variant-numeric:tabular-nums;line-height:1}
.mc .l{font-size:.62rem;color:var(--t3);margin-top:4px;text-transform:uppercase;letter-spacing:.5px}
.cT{color:var(--r)}.cH{color:var(--b)}.cP{color:var(--g)}.cG{color:var(--a)}
.tr{font-size:.55rem;margin-left:2px;vertical-align:super}
.up{color:var(--r)}.dn{color:var(--g)}.fl{color:var(--t3)}
.sec{background:var(--sf);border:1px solid var(--bd);border-radius:4px;padding:14px;margin-bottom:10px}
.sec h2{font-size:.68rem;color:var(--t3);font-weight:600;margin-bottom:10px;text-transform:uppercase;letter-spacing:.8px}
.lg{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:8px}
.lg label{display:flex;align-items:center;gap:5px;font-size:.73rem;color:var(--t2);cursor:pointer;user-select:none}
.lg label:hover{color:var(--tx)}
.lg span{width:12px;height:2px;border-radius:1px}
input[type=checkbox]{appearance:none;-webkit-appearance:none;width:12px;height:12px;border:1.5px solid var(--t3);border-radius:2px;cursor:pointer;position:relative;flex-shrink:0}
input[type=checkbox]:checked{border-color:var(--g);background:var(--g)}
input[type=checkbox]:checked::after{content:'';position:absolute;left:2px;top:-1px;width:3px;height:5px;border:solid #000;border-width:0 1.5px 1.5px 0;transform:rotate(45deg)}
canvas{width:100%;height:180px;display:block}
.fn{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.fb{padding:5px 16px;border-radius:3px;border:1px solid var(--bd);background:0 0;color:var(--t2);cursor:pointer;font-size:.78rem;font-weight:500}
.fb.on{background:var(--g);color:#000;border-color:var(--g)}
.fb:hover:not(.on){border-color:var(--t3)}
.fv{width:1px;height:18px;background:var(--bd)}
.fl{font-size:.75rem;color:var(--t3)}
input[type=range]{flex:1;min-width:130px;height:3px;-webkit-appearance:none;appearance:none;background:var(--bd);border-radius:2px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:12px;height:12px;border-radius:50%;background:var(--c);cursor:pointer}
input[type=number]{width:58px;background:var(--bg);border:1px solid var(--bd);color:var(--tx);border-radius:3px;padding:4px;font-size:.82rem;text-align:center;outline:none}
input[type=number]:focus{border-color:var(--c)}
.fp{font-size:.9rem;font-weight:700;color:var(--c);min-width:38px;text-align:right;font-variant-numeric:tabular-nums}
table{width:100%;border-collapse:collapse;font-size:.72rem;font-variant-numeric:tabular-nums}
th{color:var(--t3);font-weight:600;text-transform:uppercase;letter-spacing:.3px;font-size:.65rem;padding:5px 6px;border-bottom:1px solid var(--bd);position:sticky;top:0;background:var(--sf);z-index:1}
td{padding:4px 6px;text-align:center;border-bottom:1px solid rgba(45,51,59,.5);white-space:nowrap}
.hw{max-height:220px;overflow:auto;margin-bottom:8px;border:1px solid var(--bd);border-radius:3px}
.hw::-webkit-scrollbar{width:3px}
.hw::-webkit-scrollbar-thumb{background:var(--t3);border-radius:2px}
.gbtn{background:0 0;color:var(--t3);border:1px solid var(--bd);border-radius:3px;padding:5px 12px;cursor:pointer;font-size:.75rem;font-weight:500}
.gbtn:hover{border-color:var(--t3);color:var(--t2)}
@media(max-width:640px){.g3{grid-template-columns:1fr 1fr}.mc .v{font-size:1.3rem}}
</style>
</head>
<body>
<div class="row">
  <h1><b>BME688</b> 環境監測</h1>
  <div class="st"><span class="dot" id="dot"></span><span id="st">等待連線...</span></div>
</div>
<div class="pw"><div class="pg">
  <button onclick="setPoll(1000,this)">1s</button>
  <button class="on" onclick="setPoll(2000,this)">2s</button>
  <button onclick="setPoll(5000,this)">5s</button>
</div></div>
<div class="g3">
  <div class="mc a"><div class="v cT" id="mT">--</div><div class="l">Temp C <span class="tr" id="tT"></span></div></div>
  <div class="mc b"><div class="v cH" id="mH">--</div><div class="l">Humidity % <span class="tr" id="tH"></span></div></div>
  <div class="mc c"><div class="v cP" id="mP">--</div><div class="l">hPa <span class="tr" id="tP"></span></div></div>
  <div class="mc d"><div class="v cG" id="mG">--</div><div class="l">Gas kOhm <span class="tr" id="tG"></span></div></div>
  <div class="mc"><div class="v" id="mA" style="color:var(--a)">--</div><div class="l">AQI</div></div>
  <div class="mc"><div class="v" id="mF" style="color:var(--c)">--</div><div class="l">Fan</div></div>
</div>
<div class="sec">
  <h2>Trend</h2>
  <div class="lg" id="leg"></div>
  <canvas id="cvs" width="800" height="180"></canvas>
</div>
<div class="sec">
  <h2>Fan Control</h2>
  <div class="fn">
    <button class="fb on" id="ab" onclick="toggleAuto()">Auto</button>
    <div class="fv"></div>
    <span class="fl">Target</span>
    <input type="number" id="tgt" min="10" max="40" step="0.5" value="25" onchange="setTgt(this.value)">
    <span class="fl">C</span>
    <div class="fv"></div>
    <input type="range" min="0" max="100" value="0" id="sl" oninput="setMan(this.value)">
    <span class="fp" id="fp">0%</span>
  </div>
</div>
<div class="sec">
  <h2>History</h2>
  <div class="hw"><table id="ht"><thead><tr><th>Time</th><th>Temp</th><th>Hum</th><th>hPa</th><th>Gas</th><th>AQI</th></tr></thead><tbody></tbody></table></div>
  <div style="display:flex;gap:6px">
    <button class="gbtn" onclick="clearH()">Clear</button>
    <button class="gbtn" onclick="exportCSV()">導出 CSV</button>
  </div>
</div>
<script>
var hist=[],am=1,pm=2000,ti=null;
var K=['T','H','P','G'],CO={T:'#f85149',H:'#58a6ff',P:'#3fb950',G:'#d29922'},LA={T:'Temp',H:'Hum',P:'hPa',G:'Gas'};
var vs={T:1,H:1,P:0,G:0};
var cv=document.getElementById('cvs'),cx=cv.getContext('2d');
var pv={T:null,H:null,P:null,G:null};

function rs(){var r=cv.getBoundingClientRect();cv.width=r.width*devicePixelRatio;cv.height=r.height*devicePixelRatio;cx.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);dr();}
window.addEventListener('resize',rs);

function ti2(c,k){var p=pv[k];pv[k]=c;if(p===null||isNaN(c)||isNaN(p))return'';var d=c-p;if(Math.abs(d)<0.05)return'<span class="tr fl">&mdash;</span>';return d>0?'<span class="tr up">&#8593;</span>':'<span class="tr dn">&#8595;</span>';}

function dr(){
  var W=cv.getBoundingClientRect().width,H=cv.getBoundingClientRect().height;
  var pl=48,pr=14,pt=14,pb=24;
  cx.clearRect(0,0,W,H);
  if(hist.length<2){cx.fillStyle='#484f58';cx.font='12px system-ui';cx.textAlign='center';cx.fillText('Collecting data...',W/2,H/2);return;}
  var us=hist.slice().reverse();
  var ac=K.filter(function(k){return vs[k];});
  if(!ac.length){cx.fillStyle='#484f58';cx.font='12px system-ui';cx.textAlign='center';cx.fillText('Select a metric',W/2,H/2);return;}
  var mn=Infinity,mx=-Infinity;
  ac.forEach(function(k){us.forEach(function(r){if(r[k]<mn)mn=r[k];if(r[k]>mx)mx=r[k];});});
  var sp=mx-mn;if(sp===0){mn-=1;mx+=1;sp=2;}
  var pa=sp*.06;mn-=pa;mx+=pa;sp=mx-mn;
  var pw=W-pl-pr,ph=H-pt-pb;

  cx.strokeStyle='rgba(139,148,158,.08)';cx.lineWidth=1;
  cx.fillStyle='#484f58';cx.font='10px system-ui';cx.textAlign='right';
  for(var i=0;i<=4;i++){var v=mx-(sp*i/4);var y=pt+ph*i/4;cx.beginPath();cx.moveTo(pl,y);cx.lineTo(W-pr,y);cx.stroke();cx.fillText(v<10?v.toFixed(1):v<100?v.toFixed(1):Math.round(v),pl-6,y+3);}

  cx.textAlign='center';cx.fillStyle='#484f58';cx.font='9px system-ui';
  var st=pw/Math.max(1,us.length-1);
  var xt=Math.max(1,Math.ceil(us.length/7));
  us.forEach(function(r,i){if(i%xt===0||i===us.length-1){cx.fillText(r.time,pl+st*i,H-pb+14);}});

  ac.forEach(function(k){
    cx.beginPath();cx.strokeStyle=CO[k];cx.lineWidth=2;cx.lineJoin='round';
    us.forEach(function(r,i){var x=pl+st*i;var y=pt+(1-(r[k]-mn)/sp)*ph;i?cx.lineTo(x,y):cx.moveTo(x,y);});
    cx.stroke();
    var la=us[us.length-1];var lx=pl+st*(us.length-1);var ly=pt+(1-(la[k]-mn)/sp)*ph;
    cx.beginPath();cx.arc(lx,ly,3,0,Math.PI*2);cx.fillStyle=CO[k];cx.fill();
  });
}

function bld(){document.getElementById('leg').innerHTML=K.map(function(k){return'<label><input type="checkbox" '+(vs[k]?'checked':'')+' onchange="vs.'+k+'=this.checked?1:0;dr()"><span style="background:'+CO[k]+'"></span>'+LA[k]+'</label>';}).join('');}

function rH(){document.querySelector('#ht tbody').innerHTML=hist.map(function(r){return'<tr><td>'+r.time+'</td><td>'+r.T.toFixed(1)+'</td><td>'+r.H.toFixed(1)+'</td><td>'+r.P.toFixed(1)+'</td><td>'+r.G.toFixed(1)+'</td><td>'+r.A+'</td></tr>';}).join('');}

async function poll(){
  try{
    var r=await fetch('/data');var d=await r.json();
    if(!d.ok){document.getElementById('st').textContent=d.message;document.getElementById('dot').className='dot err';return;}
    document.getElementById('dot').className='dot';
    document.getElementById('st').textContent=new Date().toLocaleTimeString();
    document.getElementById('mT').innerHTML=d.temperature.toFixed(1);
    document.getElementById('tT').innerHTML=ti2(d.temperature,'T');
    document.getElementById('mH').innerHTML=d.humidity.toFixed(1);
    document.getElementById('tH').innerHTML=ti2(d.humidity,'H');
    document.getElementById('mP').innerHTML=d.pressure.toFixed(1);
    document.getElementById('tP').innerHTML=ti2(d.pressure,'P');
    document.getElementById('mG').innerHTML=d.gas.toFixed(1);
    document.getElementById('tG').innerHTML=ti2(d.gas,'G');
    document.getElementById('mA').textContent=d.aqi;
    var fp=Math.round(d.fanSpeed/255*100);
    document.getElementById('mF').textContent=fp+'%';
    am=d.auto;
    document.getElementById('ab').className=am?'fb on':'fb';
    document.getElementById('sl').value=fp;
    document.getElementById('fp').textContent=fp+'%';
    document.getElementById('tgt').value=d.target;
    hist.unshift({time:new Date().toLocaleTimeString(),T:d.temperature,H:d.humidity,P:d.pressure,G:d.gas,A:d.aqi});
    if(hist.length>60)hist.pop();
    rH();dr();
  }catch(e){
    document.getElementById('st').textContent='Error';
    document.getElementById('dot').className='dot err';
  }
}

function go(){if(ti)clearInterval(ti);poll();ti=setInterval(poll,pm);}
function setPoll(ms,b){document.querySelectorAll('.pg button').forEach(function(x){x.className='';});b.className='on';pm=ms;go();}
function toggleAuto(){am=!am;fetch('/fan?auto='+(am?1:0));document.getElementById('ab').className=am?'fb on':'fb';}
function setTgt(v){fetch('/fan?target='+v);}
function setMan(v){if(am)return;document.getElementById('fp').textContent=v+'%';fetch('/fan?speed='+v);}
function clearH(){hist=[];rH();dr();}
function exportCSV(){
  if(!hist.length)return;
  var c='\uFEFFTime,Temp,Hum,hPa,Gas,AQI\n'+hist.map(function(r){return r.time+','+r.T+','+r.H+','+r.P+','+r.G+','+r.A;}).join('\n');
  var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([c],{type:'text/csv'}));a.download='BME688.csv';a.click();
}
bld();rs();go();
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
