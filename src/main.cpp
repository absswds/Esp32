#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

#define BME680_I2C_ADDRESS 0x76
#define BME680_I2C_ADDRESS_ALT 0x77
#define WEB_PORT 80
#define DISPLAY_POINTS 30

#define FAN_PIN 25
#define FAN_CHANNEL 0
#define FAN_FREQ 25000
#define FAN_RESOLUTION 8

Adafruit_BME680 bme;
WebServer server(WEB_PORT);

int bmeAddress = BME680_I2C_ADDRESS;
bool bmeDetected = false;

float temperatureC = NAN;
float humidity = NAN;
float pressure = NAN;
float gasResistance = NAN;
int aqi = 0;
unsigned long lastRead = 0;

float runningMin[4] = {1000, 1000, 10000, 1e9};
float runningMax[4] = {-1000, -1000, -10000, -1};

// --- Fan control ---
bool fanAuto = true;
int fanSpeed = 0;
float targetTemp = 25.0;
float rampWidth = 3.0;

void setFanSpeed(int speed) {
  speed = constrain(speed, 0, 255);
  fanSpeed = speed;
  ledcWrite(FAN_CHANNEL, speed);
}

void updateFanAuto() {
  if (isnan(temperatureC)) return;
  if (!fanAuto) return;

  if (temperatureC < targetTemp - rampWidth) {
    setFanSpeed(255);
  } else if (temperatureC >= targetTemp) {
    setFanSpeed(0);
  } else {
    float ratio = (targetTemp - temperatureC) / rampWidth;
    setFanSpeed((int)(ratio * 255));
  }
}

String buildHtml() {
  return R"HTML(
<!DOCTYPE html>
<html lang="zh-Hant">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1.0"/>
<title>ESP32 BME688 Dashboard</title>
<style>
:root{color-scheme:light;--bg:#f1f5f9;--card:rgba(255,255,255,0.82);--border:rgba(0,0,0,0.06);--txt:#1e293b;--txt2:#64748b;--txt3:#94a3b8;--glass:rgba(255,255,255,0.6)}
*{box-sizing:border-box;margin:0;padding:0}
body{min-height:100vh;font-family:'Inter','Segoe UI',system-ui,-apple-system,sans-serif;background:var(--bg);color:var(--txt);overflow-x:hidden;padding:16px}
body::before{content:'';position:fixed;inset:-50%;width:200%;height:200%;background:conic-gradient(from 0deg at 50% 50%,rgba(241,245,249,0) 0deg,rgba(186,230,253,0.3) 60deg,rgba(196,181,253,0.2) 120deg,rgba(241,245,249,0) 180deg,rgba(186,230,253,0.2) 240deg,rgba(196,181,253,0.3) 300deg,rgba(241,245,249,0) 360deg);animation:bgSpin 30s linear infinite;pointer-events:none;z-index:0}
@keyframes bgSpin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.4;transform:scale(1.5)}}
@keyframes shimmer{0%{background-position:-200% 0}100%{background-position:200% 0}}
@keyframes fadeSlideIn{from{opacity:0;transform:translateY(-6px)}to{opacity:1;transform:translateY(0)}}
@keyframes fanSpin{from{transform:rotate(0deg)}to{transform:rotate(360deg)}}
.wrapper{max-width:1200px;margin:0 auto;position:relative;z-index:1}

.hero{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:12px;padding:20px 28px;border-radius:20px;background:var(--card);backdrop-filter:blur(16px);border:1px solid var(--border);margin-bottom:20px;box-shadow:0 1px 3px rgba(0,0,0,0.04)}
.hero h1{font-size:clamp(1.4rem,3vw,2rem);font-weight:600;letter-spacing:-.01em;background:linear-gradient(135deg,#1e293b,#475569);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}
.hero-right{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.status-wrap{display:flex;align-items:center;gap:8px;font-size:.85rem;color:var(--txt2)}
.status-dot{width:8px;height:8px;border-radius:50%;background:#22c55e;animation:pulse 2s ease-in-out infinite}
.status-dot.err{background:#ef4444;animation:none}

.pill-group{display:flex;gap:2px;background:var(--glass);border:1px solid var(--border);border-radius:10px;padding:3px;backdrop-filter:blur(12px)}
.pill{padding:5px 14px;border-radius:8px;border:none;background:transparent;color:var(--txt2);font-size:.78rem;cursor:pointer;transition:all .2s;font-family:inherit}
.pill:hover{color:var(--txt);background:rgba(0,0,0,0.04)}
.pill.active{background:rgba(99,102,241,0.12);color:#6366f1;font-weight:500}
.btn{display:inline-flex;align-items:center;gap:6px;padding:8px 18px;border-radius:10px;border:1px solid var(--border);background:var(--glass);color:var(--txt2);font-size:.85rem;cursor:pointer;transition:all .2s;text-decoration:none;backdrop-filter:blur(12px);font-family:inherit}
.btn:hover{background:rgba(0,0,0,0.04);border-color:rgba(0,0,0,0.12);color:var(--txt)}
.btn svg{width:16px;height:16px;flex-shrink:0}

.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:14px;margin-bottom:20px}
.stat{position:relative;overflow:hidden;padding:18px 22px;border-radius:18px;background:var(--card);backdrop-filter:blur(12px);border:1px solid var(--border);transition:transform .2s,box-shadow .3s;box-shadow:0 1px 3px rgba(0,0,0,0.04)}
.stat:hover{transform:translateY(-2px);box-shadow:0 4px 20px rgba(0,0,0,0.08)}
.stat::before{content:'';position:absolute;inset:0;border-radius:18px;opacity:0;transition:opacity .3s;pointer-events:none;z-index:0}
.stat:nth-child(1):hover{box-shadow:0 0 24px rgba(251,146,60,0.12),0 4px 20px rgba(0,0,0,0.06)}.stat:nth-child(1):hover::before{opacity:1;background:radial-gradient(ellipse at 50% 0%,rgba(251,146,60,0.05),transparent 70%)}
.stat:nth-child(2):hover{box-shadow:0 0 24px rgba(56,189,248,0.12),0 4px 20px rgba(0,0,0,0.06)}.stat:nth-child(2):hover::before{opacity:1;background:radial-gradient(ellipse at 50% 0%,rgba(56,189,248,0.05),transparent 70%)}
.stat:nth-child(3):hover{box-shadow:0 0 24px rgba(167,139,250,0.12),0 4px 20px rgba(0,0,0,0.06)}.stat:nth-child(3):hover::before{opacity:1;background:radial-gradient(ellipse at 50% 0%,rgba(167,139,250,0.05),transparent 70%)}
.stat:nth-child(4):hover{box-shadow:0 0 24px rgba(52,211,153,0.12),0 4px 20px rgba(0,0,0,0.06)}.stat:nth-child(4):hover::before{opacity:1;background:radial-gradient(ellipse at 50% 0%,rgba(52,211,153,0.05),transparent 70%)}
.stat:nth-child(5):hover{box-shadow:0 0 24px rgba(251,191,36,0.12),0 4px 20px rgba(0,0,0,0.06)}.stat:nth-child(5):hover::before{opacity:1;background:radial-gradient(ellipse at 50% 0%,rgba(251,191,36,0.05),transparent 70%)}

.stat .accent{position:absolute;top:0;left:0;width:4px;height:100%;border-radius:0 2px 2px 0;z-index:1}
.stat .label{font-size:.8rem;font-weight:500;color:var(--txt3);text-transform:uppercase;letter-spacing:.05em;margin-bottom:6px;position:relative;z-index:1}
.stat .value{display:flex;align-items:baseline;font-size:clamp(1.8rem,3.5vw,2.8rem);font-weight:700;letter-spacing:-.02em;position:relative;z-index:1}
.stat .unit{margin-left:5px;font-size:.85rem;font-weight:400;color:var(--txt2)}
.stat .trend{margin-left:8px;font-size:.85rem;font-weight:500;opacity:.9}
.trend.up{color:#ef4444}.trend.down{color:#22c55e}.trend.flat{color:var(--txt3)}
.stat .mini{font-size:.75rem;color:var(--txt3);margin-top:4px;position:relative;z-index:1}
.stat .shimmer{position:absolute;inset:0;background:linear-gradient(90deg,transparent,rgba(148,163,184,0.04),transparent);background-size:200% 100%;animation:shimmer 1.8s ease-in-out infinite;border-radius:18px}
.stat .noise{position:absolute;inset:0;opacity:.3;border-radius:18px;pointer-events:none;background-image:radial-gradient(circle at 20% 80%,rgba(0,0,0,0.015) 1px,transparent 1px),radial-gradient(circle at 80% 20%,rgba(0,0,0,0.015) 1px,transparent 1px),radial-gradient(circle at 40% 40%,rgba(0,0,0,0.01) 1px,transparent 1px);background-size:5px 5px,7px 7px,11px 11px}
.card{border-radius:20px;padding:22px;background:var(--card);backdrop-filter:blur(12px);border:1px solid var(--border);margin-bottom:20px;box-shadow:0 1px 3px rgba(0,0,0,0.04)}
.card-header{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;margin-bottom:16px}
.card-title{font-size:1rem;font-weight:600;color:#1e293b}
.legend{display:flex;flex-wrap:wrap;gap:14px;font-size:.8rem;color:var(--txt2)}
.legend-item{display:inline-flex;align-items:center;gap:6px;cursor:pointer;transition:opacity .2s;user-select:none}
.legend-item:hover{opacity:1!important}
.legend-dot{width:10px;height:10px;border-radius:50%;transition:transform .2s}
.legend-item:hover .legend-dot{transform:scale(1.3)}
.chart-wrap{position:relative}
#chart{width:100%;height:320px;border-radius:14px;display:block}
.chart-tooltip{position:absolute;pointer-events:none;background:rgba(255,255,255,0.95);backdrop-filter:blur(8px);border:1px solid rgba(0,0,0,0.08);border-radius:10px;padding:10px 14px;font-size:.78rem;color:var(--txt);opacity:0;transition:opacity .15s;z-index:10;white-space:nowrap;box-shadow:0 4px 12px rgba(0,0,0,0.08)}
.chart-tooltip.show{opacity:1}
.tt-time{color:var(--txt3);margin-bottom:4px;font-size:.72rem}
.tt-row{display:flex;align-items:center;gap:6px;margin-top:2px}
.tt-dot{width:7px;height:7px;border-radius:50%;flex-shrink:0}

.table-wrap{max-height:220px;overflow-y:auto;border-radius:12px;border:1px solid rgba(0,0,0,0.06)}
.table-wrap::-webkit-scrollbar{width:5px}
.table-wrap::-webkit-scrollbar-track{background:transparent}
.table-wrap::-webkit-scrollbar-thumb{background:rgba(0,0,0,0.12);border-radius:4px}
table{width:100%;border-collapse:collapse;font-size:.82rem}
thead{position:sticky;top:0;z-index:1}
th{background:rgba(255,255,255,0.95);color:var(--txt3);font-weight:500;text-transform:uppercase;letter-spacing:.04em;padding:10px 12px;text-align:left;border-bottom:1px solid var(--border)}
td{padding:8px 12px;border-bottom:1px solid rgba(0,0,0,0.04);color:#334155}
tr:hover td{background:rgba(0,0,0,0.02)}
tr:nth-child(even) td{background:rgba(0,0,0,0.015)}
tr:nth-child(even):hover td{background:rgba(0,0,0,0.04)}
tr.new-row{animation:fadeSlideIn .4s ease-out}
.temp-c{color:#ea580c}.hum-c{color:#0284c7}.pres-c{color:#7c3aed}.gas-c{color:#059669}.aqi-c{color:#d97706}

/* Fan card */
.fan-card{position:relative}
.fan-ctrl{display:flex;align-items:center;gap:20px;flex-wrap:wrap}
.fan-icon{width:56px;height:56px;flex-shrink:0}
.fan-icon svg{width:100%;height:100%}
.fan-blades{transform-origin:28px 28px;transition:transform .3s}
.fan-blades.spinning{animation:fanSpin .8s linear infinite}
.fan-info{flex:1;min-width:180px}
.fan-speed-label{font-size:2rem;font-weight:700;color:#0ea5e9}
.fan-mode{display:inline-block;padding:3px 10px;border-radius:6px;font-size:.72rem;font-weight:600;text-transform:uppercase;letter-spacing:.04em;margin-left:8px}
.fan-mode.auto{background:rgba(34,197,94,0.12);color:#16a34a}
.fan-mode.manual{background:rgba(99,102,241,0.12);color:#6366f1}
.fan-controls{display:flex;flex-direction:column;gap:10px;min-width:260px}
.fan-slider-row{display:flex;align-items:center;gap:10px}
.fan-slider-row label{font-size:.82rem;color:var(--txt2);white-space:nowrap}
.fan-slider{flex:1;-webkit-appearance:none;height:6px;border-radius:3px;background:linear-gradient(90deg,#0ea5e9,#6366f1);outline:none;transition:opacity .2s}
.fan-slider::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:#fff;border:2px solid #0ea5e9;cursor:pointer;box-shadow:0 2px 6px rgba(0,0,0,0.15)}
.fan-slider::-moz-range-thumb{width:20px;height:20px;border-radius:50%;background:#fff;border:2px solid #0ea5e9;cursor:pointer}
.fan-btns{display:flex;gap:8px}
.fan-btn{padding:7px 16px;border-radius:8px;border:1px solid var(--border);background:var(--glass);color:var(--txt2);font-size:.8rem;cursor:pointer;transition:all .2s;font-family:inherit}
.fan-btn:hover{background:rgba(0,0,0,0.04);color:var(--txt)}
.fan-btn.active{background:rgba(14,165,233,0.12);color:#0ea5e9;border-color:rgba(14,165,233,0.3)}

.temp-target-row{display:flex;align-items:center;gap:10px;margin-top:6px}
.temp-target-row label{font-size:.82rem;color:var(--txt2)}
.temp-target-input{width:60px;padding:5px 8px;border-radius:8px;border:1px solid var(--border);font-size:.85rem;text-align:center;font-family:inherit}

@media(max-width:600px){
  body{padding:10px}
  .stat{padding:14px 16px}
  #chart{height:220px}
  .card{padding:14px}
  .hero{padding:14px 18px;flex-direction:column;align-items:stretch}
  .hero-right{justify-content:center}
  .fan-ctrl{flex-direction:column;align-items:stretch}
  .fan-controls{min-width:0}
}
</style>
</head>
<body>
<div class="wrapper">
  <section class="hero">
    <h1>ESP32 BME688 Dashboard</h1>
    <div class="hero-right">
      <div class="pill-group" id="refreshGroup">
        <button class="pill" data-ms="1000">1s</button>
        <button class="pill active" data-ms="2000">2s</button>
        <button class="pill" data-ms="5000">5s</button>
      </div>
      <div class="status-wrap">
        <span class="status-dot" id="statusDot"></span>
        <span id="status">等待連線...</span>
      </div>
      <button class="btn" id="exportBtn" onclick="exportCSV()">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
        匯出 CSV
      </button>
    </div>
  </section>

  <section class="stats">
    <div class="stat" id="cardTemp">
      <div class="accent" style="background:linear-gradient(180deg,#f97316,#ef4444)"></div>
      <div class="noise"></div><div class="shimmer"></div>
      <div class="label">溫度</div>
      <div class="value" id="temp">--<span class="unit">°C</span></div>
      <div class="mini" id="tempExtrema"></div>
    </div>
    <div class="stat" id="cardHum">
      <div class="accent" style="background:linear-gradient(180deg,#0ea5e9,#06b6d4)"></div>
      <div class="noise"></div><div class="shimmer"></div>
      <div class="label">濕度</div>
      <div class="value" id="hum">--<span class="unit">%</span></div>
      <div class="mini" id="humExtrema"></div>
    </div>
    <div class="stat" id="cardPres">
      <div class="accent" style="background:linear-gradient(180deg,#8b5cf6,#a78bfa)"></div>
      <div class="noise"></div><div class="shimmer"></div>
      <div class="label">氣壓</div>
      <div class="value" id="pres">--<span class="unit">hPa</span></div>
      <div class="mini" id="presExtrema"></div>
    </div>
    <div class="stat" id="cardGas">
      <div class="accent" style="background:linear-gradient(180deg,#10b981,#34d399)"></div>
      <div class="noise"></div><div class="shimmer"></div>
      <div class="label">氣體阻抗</div>
      <div class="value" id="gas">--<span class="unit">kΩ</span></div>
      <div class="mini" id="gasExtrema"></div>
    </div>
    <div class="stat" id="cardAqi">
      <div class="accent" style="background:linear-gradient(180deg,#eab308,#f59e0b)"></div>
      <div class="noise"></div><div class="shimmer"></div>
      <div class="label">空氣品質</div>
      <div class="value" id="aqiVal">--<span class="unit">AQI</span></div>
      <div class="mini" id="aqiLabel"></div>
    </div>
  </section>

  <section class="card fan-card">
    <div class="card-header">
      <span class="card-title">風扇控制</span>
      <span class="fan-mode auto" id="fanMode">自動</span>
    </div>
    <div class="fan-ctrl">
      <div class="fan-icon">
        <svg viewBox="0 0 56 56" fill="none">
          <circle cx="28" cy="28" r="27" stroke="#0ea5e9" stroke-width="2" opacity="0.2"/>
          <g class="fan-blades" id="fanBlades">
            <ellipse cx="28" cy="14" rx="5" ry="12" fill="#0ea5e9" opacity="0.7"/>
            <ellipse cx="28" cy="14" rx="5" ry="12" fill="#0ea5e9" opacity="0.7" transform="rotate(90 28 28)"/>
            <ellipse cx="28" cy="14" rx="5" ry="12" fill="#0ea5e9" opacity="0.7" transform="rotate(180 28 28)"/>
            <ellipse cx="28" cy="14" rx="5" ry="12" fill="#0ea5e9" opacity="0.7" transform="rotate(270 28 28)"/>
          </g>
          <circle cx="28" cy="28" r="4" fill="#0ea5e9"/>
        </svg>
      </div>
      <div class="fan-info">
        <div class="fan-speed-label" id="fanSpeedLabel">0%</div>
        <div class="mini" id="fanDetail">等待資料...</div>
      </div>
      <div class="fan-controls">
        <div class="fan-btns">
          <button class="fan-btn active" id="btnAuto" onclick="setFanMode(true)">自動</button>
          <button class="fan-btn" id="btnManual" onclick="setFanMode(false)">手動</button>
        </div>
        <div class="fan-slider-row" id="manualRow" style="display:none">
          <label>風速</label>
          <input type="range" class="fan-slider" id="fanSlider" min="0" max="100" value="0" oninput="manualFan(this.value)">
          <span id="fanSliderVal" style="font-size:.82rem;color:var(--txt2);min-width:36px;text-align:right">0%</span>
        </div>
        <div class="temp-target-row" id="autoRow">
          <label>目標溫度</label>
          <input type="number" class="temp-target-input" id="targetTempInput" value="25" min="20" max="40" step="0.5" onchange="setTargetTemp(this.value)">
          <label>°C</label>
        </div>
      </div>
    </div>
  </section>

  <section class="card">
    <div class="card-header">
      <span class="card-title">即時趨勢圖</span>
      <div class="legend" id="legend"></div>
    </div>
    <div class="chart-wrap">
      <canvas id="chart"></canvas>
      <div class="chart-tooltip" id="tooltip"></div>
    </div>
  </section>

  <section class="card">
    <div class="card-header">
      <span class="card-title">歷史紀錄 <span style="font-weight:400;color:var(--txt3);font-size:.8rem" id="rowCount"></span></span>
    </div>
    <div class="table-wrap">
      <table>
        <thead><tr>
          <th>時間</th><th>溫度 (°C)</th><th>濕度 (%)</th><th>氣壓 (hPa)</th><th>氣體 (kΩ)</th><th>AQI</th>
        </tr></thead>
        <tbody id="tableBody"></tbody>
      </table>
    </div>
  </section>
</div>

<script>
const COLORS=['#ea580c','#0284c7','#7c3aed','#059669'];
const KEYS=['temperature','humidity','pressure','gas'];
const LABELS=['溫度','濕度','氣壓','氣體'];
const UNITS=['°C','%','hPa','kΩ'];
const AQI_LABELS=['良好','普通','敏感族群不健康','不健康','非常不健康','危害'];

const $=id=>document.getElementById(id);
const tempEl=$('temp'),humEl=$('hum'),presEl=$('pres'),gasEl=$('gas'),aqiEl=$('aqiVal');
const tExtEl=$('tempExtrema'),hExtEl=$('humExtrema'),pExtEl=$('presExtrema'),gExtEl=$('gasExtrema'),aqiLblEl=$('aqiLabel');
const statusEl=$('status'),statusDot=$('statusDot');
const chartCanvas=$('chart'),ctx=chartCanvas.getContext('2d');
const legendEl=$('legend'),tbody=$('tableBody'),rowCount=$('rowCount');
const tooltipEl=$('tooltip');

const disp={labels:[],temperature:[],humidity:[],pressure:[],gas:[]};
const fullHistory=[];
const visible={temperature:true,humidity:false,pressure:false,gas:false};
let prevVals={temperature:NAN,humidity:NAN,pressure:NAN,gas:NAN};
let pollTimer=null;
let pollMs=2000;

// --- Fan UI ---
let fanAutoMode=true;

function setFanMode(auto){
  fanAutoMode=auto;
  $('btnAuto').className='fan-btn'+(auto?' active':'');
  $('btnManual').className='fan-btn'+(!auto?' active':'');
  $('manualRow').style.display=auto?'none':'flex';
  $('autoRow').style.display=auto?'flex':'none';
  $('fanMode').textContent=auto?'自動':'手動';
  $('fanMode').className='fan-mode '+(auto?'auto':'manual');
  fetch('/fan?auto='+(auto?1:0));
}

function manualFan(val){
  $('fanSliderVal').textContent=val+'%';
  const duty=Math.round(val*255/100);
  fetch('/fan?speed='+duty);
}

function setTargetTemp(val){
  fetch('/fan?target='+val);
}

function updateFanUI(data){
  if(data===undefined)return;
  const pct=data.fanSpeed!==undefined?Math.round(data.fanSpeed*100/255):0;
  $('fanSpeedLabel').textContent=pct+'%';
  $('fanDetail').textContent=data.auto?
    ('目標 '+data.targetTemp+'°C / 現在 '+(data.temperature!==undefined?data.temperature.toFixed(1):'--')+'°C'):
    ('手動設定 '+pct+'%');

  const blades=$('fanBlades');
  if(pct>0){blades.classList.add('spinning');blades.style.animationDuration=Math.max(0.15,1.5-pct/100*1.3)+'s';}
  else{blades.classList.remove('spinning');}

  if(data.auto!==undefined){
    fanAutoMode=data.auto;
    $('btnAuto').className='fan-btn'+(data.auto?' active':'');
    $('btnManual').className='fan-btn'+(!data.auto?' active':'');
    $('manualRow').style.display=data.auto?'none':'flex';
    $('autoRow').style.display=data.auto?'flex':'none';
    $('fanMode').textContent=data.auto?'自動':'手動';
    $('fanMode').className='fan-mode '+(data.auto?'auto':'manual');
  }
  if(data.targetTemp!==undefined)$('targetTempInput').value=data.targetTemp;
  if(!fanAutoMode&&data.fanSpeed!==undefined){
    const sliderPct=Math.round(data.fanSpeed*100/255);
    $('fanSlider').value=sliderPct;
    $('fanSliderVal').textContent=sliderPct+'%';
  }
}

function buildLegend(){
  legendEl.innerHTML=KEYS.map((k,i)=>
    `<span class="legend-item" data-key="${k}" style="opacity:${visible[k]?1:.4}">
      <span class="legend-dot" style="background:${COLORS[i]}"></span>${LABELS[i]}</span>`
  ).join('');
  legendEl.querySelectorAll('.legend-item').forEach(el=>{
    el.addEventListener('click',()=>{
      visible[el.dataset.key]=!visible[el.dataset.key];
      buildLegend();drawChart();
    });
  });
}

function drawRoundedRect(c,x,y,w,h,r){
  c.beginPath();c.moveTo(x+r,y);c.lineTo(x+w-r,y);
  c.quadraticCurveTo(x+w,y,x+w,y+r);c.lineTo(x+w,y+h-r);
  c.quadraticCurveTo(x+w,y+h,x+w-r,y+h);c.lineTo(x+r,y+h);
  c.quadraticCurveTo(x,y+h,x,y+h-r);c.lineTo(x,y+r);
  c.quadraticCurveTo(x,y,x+r,y);c.closePath();
}

function drawChart(){
  const dpr=window.devicePixelRatio||1;
  const W=chartCanvas.clientWidth,H=chartCanvas.clientHeight;
  ctx.clearRect(0,0,W,H);
  drawRoundedRect(ctx,0,0,W,H,14);
  ctx.fillStyle='rgba(255,255,255,0.85)';ctx.fill();
  const pad={top:20,bottom:28,left:50,right:16};
  const plotW=W-pad.left-pad.right,plotH=H-pad.top-pad.bottom;
  const active=KEYS.filter(k=>visible[k]);
  if(!active.length||disp.labels.length<2){
    ctx.fillStyle='#94a3b8';ctx.font='14px system-ui,sans-serif';ctx.textAlign='center';
    ctx.fillText('等待資料中...',W/2,H/2);return;
  }
  const ranges={};
  KEYS.forEach(k=>{
    const v=disp[k].filter(x=>!isNaN(x));
    if(v.length){let mn=Math.min(...v),mx=Math.max(...v);const p=(mx-mn)*.1||1;ranges[k]={min:mn-p,max:mx+p,range:mx-mn+p*2||1};}
  });
  ctx.setLineDash([4,4]);ctx.strokeStyle='rgba(0,0,0,0.06)';ctx.lineWidth=1;
  for(let i=0;i<=4;i++){const y=pad.top+(plotH*i)/4;ctx.beginPath();ctx.moveTo(pad.left,y);ctx.lineTo(W-pad.right,y);ctx.stroke();}
  ctx.setLineDash([]);

  const allPts={};
  KEYS.forEach((k,i)=>{
    if(!visible[k])return;
    const r=ranges[k];if(!r)return;
    const pts=[];
    disp[k].forEach((v,idx)=>{
      const x=pad.left+(plotW*idx)/Math.max(1,disp.labels.length-1);
      const y=pad.top+plotH*(1-(v-r.min)/r.range);
      pts.push({x,y,v});
    });
    allPts[k]=pts;
    const color=COLORS[i];
    const grad=ctx.createLinearGradient(0,pad.top,0,pad.top+plotH);
    grad.addColorStop(0,color+'25');grad.addColorStop(1,color+'03');
    ctx.beginPath();
    pts.forEach((p,idx)=>{idx===0?ctx.moveTo(p.x,p.y):ctx.lineTo(p.x,p.y);});
    ctx.lineTo(pts[pts.length-1].x,pad.top+plotH);ctx.lineTo(pts[0].x,pad.top+plotH);ctx.closePath();
    ctx.fillStyle=grad;ctx.fill();

    ctx.beginPath();
    pts.forEach((p,idx)=>{
      if(idx===0)ctx.moveTo(p.x,p.y);
      else{const cx=(pts[idx-1].x+p.x)/2,cy=(pts[idx-1].y+p.y)/2;ctx.quadraticCurveTo(pts[idx-1].x,pts[idx-1].y,cx,cy);}
    });
    ctx.strokeStyle=color;ctx.lineWidth=2.5;ctx.stroke();

    const last=pts[pts.length-1];
    ctx.beginPath();ctx.arc(last.x,last.y,4,0,Math.PI*2);
    ctx.fillStyle=color;ctx.fill();
    ctx.strokeStyle='rgba(255,255,255,0.8)';ctx.lineWidth=1.5;ctx.stroke();
    ctx.beginPath();ctx.arc(last.x,last.y,4,0,Math.PI*2);
    ctx.strokeStyle=color;ctx.lineWidth=1;ctx.globalAlpha=.5;ctx.stroke();ctx.globalAlpha=1;
    const pulseR=4+2.5*Math.sin(Date.now()/400);
    ctx.beginPath();ctx.arc(last.x,last.y,pulseR,0,Math.PI*2);
    ctx.strokeStyle=color+'40';ctx.lineWidth=1;ctx.stroke();
  });

  ctx.font='11px system-ui,sans-serif';ctx.textAlign='right';ctx.textBaseline='middle';
  KEYS.forEach(k=>{
    if(!visible[k]||!ranges[k])return;
    const r=ranges[k];
    ctx.fillStyle=COLORS[KEYS.indexOf(k)]+'aa';
    for(let i=0;i<=4;i++){
      const val=r.max-(r.range*i/4);
      const y=pad.top+(plotH*i)/4;
      ctx.fillText(val.toFixed(1),pad.left-6,y);
    }
  });
  if(disp.labels.length){
    ctx.fillStyle='#94a3b8';ctx.font='10px system-ui,sans-serif';ctx.textAlign='center';
    const step=Math.max(1,Math.floor(disp.labels.length/8));
    for(let i=0;i<disp.labels.length;i+=step){
      const x=pad.left+(plotW*i)/Math.max(1,disp.labels.length-1);
      ctx.fillText(disp.labels[i],x,H-6);
    }
  }
}

function resizeChart(){
  const r=chartCanvas.getBoundingClientRect();
  chartCanvas.width=r.width*window.devicePixelRatio;
  chartCanvas.height=r.height*window.devicePixelRatio;
  ctx.setTransform(window.devicePixelRatio,0,0,window.devicePixelRatio,0,0);
  drawChart();
}

function trendArrow(key){
  const cur=disp[key][disp[key].length-1];
  const prev=disp[key].length>1?disp[key][disp[key].length-2]:NAN;
  if(isNaN(cur)||isNaN(prev))return'';
  const d=cur-prev;
  if(Math.abs(d)<0.05)return'<span class="trend flat">&mdash;</span>';
  return d>0?'<span class="trend up">&#8593;</span>':'<span class="trend down">&#8595;</span>';
}

function removeShimmer(cardId){
  const card=$(cardId);
  if(card){const s=card.querySelector('.shimmer');if(s)s.remove();}
}

function addData(data){
  const now=new Date();
  const timeStr=now.toLocaleTimeString();
  disp.labels.push(timeStr);
  KEYS.forEach(k=>disp[k].push(data[k]));
  if(disp.labels.length>DISPLAY_POINTS){
    disp.labels.shift();KEYS.forEach(k=>disp[k].shift());
  }

  removeShimmer('cardTemp');removeShimmer('cardHum');
  removeShimmer('cardPres');removeShimmer('cardGas');removeShimmer('cardAqi');

  tempEl.innerHTML=data.temperature.toFixed(1)+'<span class="unit">°C</span>'+trendArrow('temperature');
  humEl.innerHTML=data.humidity.toFixed(1)+'<span class="unit">%</span>'+trendArrow('humidity');
  presEl.innerHTML=data.pressure.toFixed(1)+'<span class="unit">hPa</span>'+trendArrow('pressure');
  gasEl.innerHTML=data.gas.toFixed(1)+'<span class="unit">kΩ</span>'+trendArrow('gas');
  aqiEl.innerHTML=data.aqi+'<span class="unit">AQI</span>';
  const qi=data.aqi<=50?0:data.aqi<=100?1:data.aqi<=150?2:data.aqi<=200?3:data.aqi<=300?4:5;
  aqiLblEl.textContent=AQI_LABELS[qi];

  statusEl.textContent='最後更新：'+data.time;

  const row=document.createElement('tr');
  row.className='new-row';
  row.innerHTML=`<td>${timeStr}</td><td class="temp-c">${data.temperature.toFixed(1)}</td><td class="hum-c">${data.humidity.toFixed(1)}</td><td class="pres-c">${data.pressure.toFixed(1)}</td><td class="gas-c">${data.gas.toFixed(1)}</td><td class="aqi-c">${data.aqi}</td>`;
  tbody.insertBefore(row,tbody.firstChild);
  while(tbody.children.length>100)tbody.removeChild(tbody.lastChild);
  rowCount.textContent=`(${tbody.children.length} 筆)`;

  fullHistory.push({time:now.toISOString().replace('T',' ').slice(0,19),temperature:data.temperature,humidity:data.humidity,pressure:data.pressure,gas:data.gas,aqi:data.aqi});
  if(fullHistory.length>2000)fullHistory.splice(0,500);

  if(fullHistory.length>1){
    [tExtEl,hExtEl,pExtEl,gExtEl].forEach((el,i)=>{
      const k=KEYS[i];
      const mn=Math.min(...fullHistory.map(r=>r[k]));
      const mx=Math.max(...fullHistory.map(r=>r[k]));
      el.textContent=`最低 ${mn.toFixed(1)} / 最高 ${mx.toFixed(1)}`;
    });
  }
  drawChart();

  if(data.fanSpeed!==undefined)updateFanUI(data);
}

function handleTooltip(ev){
  const rect=chartCanvas.getBoundingClientRect();
  const mx=ev.clientX-rect.left,my=ev.clientY-rect.top;
  const W=rect.width,H=rect.height;
  const pad={top:20,bottom:28,left:50,right:16};
  const plotW=W-pad.left-pad.right;
  const active=KEYS.filter(k=>visible[k]);
  if(!active.length||disp.labels.length<2){tooltipEl.classList.remove('show');return;}
  const idx=Math.round((mx-pad.left)/plotW*Math.max(1,disp.labels.length-1));
  if(idx<0||idx>=disp.labels.length||mx<pad.left||mx>W-pad.right||my<pad.top||my>H-pad.bottom){tooltipEl.classList.remove('show');return;}
  let html=`<div class="tt-time">${disp.labels[idx]}</div>`;
  active.forEach(k=>{
    const i=KEYS.indexOf(k);
    const v=disp[k][idx];
    if(!isNaN(v))html+=`<div class="tt-row"><span class="tt-dot" style="background:${COLORS[i]}"></span>${LABELS[i]}: ${v.toFixed(1)} ${UNITS[i]}</div>`;
  });
  tooltipEl.innerHTML=html;tooltipEl.classList.add('show');
  const tx=ev.clientX-rect.left+16,ty=ev.clientY-rect.top-10;
  tooltipEl.style.left=(tx+tooltipEl.offsetWidth>W?tx-tooltipEl.offsetWidth-32:tx)+'px';
  tooltipEl.style.top=Math.max(4,Math.min(ty,H-tooltipEl.offsetHeight-4))+'px';
}

chartCanvas.addEventListener('mousemove',handleTooltip);
chartCanvas.addEventListener('mouseleave',()=>tooltipEl.classList.remove('show'));

async function loadData(){
  try{
    const res=await fetch('/data');
    const data=await res.json();
    if(data.ok){
      statusDot.className='status-dot';
      addData(data);
    }else{
      statusEl.textContent=data.message;
      statusDot.className='status-dot err';
    }
  }catch{statusEl.textContent='更新失敗';statusDot.className='status-dot err';}
}

function exportCSV(){
  if(!fullHistory.length){alert('尚無資料可匯出');return;}
  const head='時間,溫度 (°C),濕度 (%),氣壓 (hPa),氣體 (kΩ),AQI';
  const rows=fullHistory.map(r=>`${r.time},${r.temperature.toFixed(2)},${r.humidity.toFixed(2)},${r.pressure.toFixed(2)},${r.gas.toFixed(2)},${r.aqi}`);
  const csv='\uFEFF'+head+'\n'+rows.join('\n');
  const blob=new Blob([csv],{type:'text/csv;charset=utf-8;'});
  const url=URL.createObjectURL(blob);
  const a=document.createElement('a');a.href=url;
  a.download=`BME688_${new Date().toISOString().slice(0,19).replace(/[:-]/g,'')}.csv`;
  document.body.appendChild(a);a.click();document.body.removeChild(a);URL.revokeObjectURL(url);
}

function startPoll(){
  if(pollTimer)clearInterval(pollTimer);
  loadData();
  pollTimer=setInterval(loadData,pollMs);
}

document.querySelectorAll('.pill').forEach(btn=>{
  btn.addEventListener('click',()=>{
    document.querySelectorAll('.pill').forEach(b=>b.classList.remove('active'));
    btn.classList.add('active');
    pollMs=parseInt(btn.dataset.ms);
    startPoll();
  });
});

buildLegend();
window.addEventListener('resize',resizeChart);
resizeChart();
startPoll();
</script>
</body>
</html>
)HTML";
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", buildHtml());
}

void handleData() {
  String json;
  if (isnan(temperatureC)) {
    json = "{\"ok\":false,\"message\":\"暫無有效感測資料\"}";
  } else {
    char buffer[360];
    snprintf(buffer, sizeof(buffer),
             "{\"ok\":true,\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"gas\":%.2f,\"aqi\":%d,"
             "\"fanSpeed\":%d,\"auto\":%s,\"targetTemp\":%.1f,"
             "\"time\":\"%02u:%02u:%02u\"}",
             temperatureC, humidity, pressure, gasResistance, aqi,
             fanSpeed, fanAuto ? "true" : "false", targetTemp,
             (millis() / 1000 / 3600) % 24, (millis() / 1000 / 60) % 60, (millis() / 1000) % 60);
    json = buffer;
  }
  server.send(200, "application/json", json);
}

void handleFan() {
  if (server.hasArg("speed")) {
    fanAuto = false;
    int spd = server.arg("speed").toInt();
    setFanSpeed(spd);
    Serial.printf("手動風速: %d\n", spd);
  }
  if (server.hasArg("auto")) {
    fanAuto = (server.arg("auto") == "1");
    Serial.printf("自動模式: %s\n", fanAuto ? "開" : "關");
    if (fanAuto) updateFanAuto();
  }
  if (server.hasArg("target")) {
    targetTemp = server.arg("target").toFloat();
    targetTemp = constrain(targetTemp, 20.0, 40.0);
    Serial.printf("目標溫度: %.1f°C\n", targetTemp);
    if (fanAuto) updateFanAuto();
  }

  char buf[120];
  snprintf(buf, sizeof(buf), "{\"fanSpeed\":%d,\"auto\":%s,\"targetTemp\":%.1f}",
           fanSpeed, fanAuto ? "true" : "false", targetTemp);
  server.send(200, "application/json", buf);
}

bool scanI2C() {
  bool found = false;
  Serial.println("I2C 掃描中...");
  for (uint8_t address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.print("找到 I2C 裝置：0x");
      if (address < 16) Serial.print('0');
      Serial.println(address, HEX);
      if (address == BME680_I2C_ADDRESS || address == BME680_I2C_ADDRESS_ALT) {
        bmeAddress = address;
        found = true;
      }
    }
  }
  return found;
}

int calcAqi(float gasKOhm) {
  if (gasKOhm >= 1000) return 0;
  if (gasKOhm >= 800)  return map((int)gasKOhm, 800, 1000, 50, 0);
  if (gasKOhm >= 600)  return map((int)gasKOhm, 600, 800, 100, 51);
  if (gasKOhm >= 400)  return map((int)gasKOhm, 400, 600, 150, 101);
  if (gasKOhm >= 200)  return map((int)gasKOhm, 200, 400, 200, 151);
  if (gasKOhm >= 100)  return map((int)gasKOhm, 100, 200, 300, 201);
  return map((int)gasKOhm, 0, 100, 500, 301);
}

void readSensor() {
  if (!bmeDetected) return;
  if (!bme.performReading()) {
    Serial.println("BME688 讀取失敗");
    temperatureC = NAN; humidity = NAN; pressure = NAN; gasResistance = NAN; aqi = 0;
    return;
  }
  temperatureC = bme.temperature;
  humidity = bme.humidity;
  pressure = bme.pressure / 100.0;
  gasResistance = bme.gas_resistance / 1000.0;
  aqi = calcAqi(gasResistance);

  Serial.printf("T:%.1f°C H:%.1f%% P:%.1fhPa G:%.1fkΩ AQI:%d Fan:%d\n",
                temperatureC, humidity, pressure, gasResistance, aqi, fanSpeed);

  updateFanAuto();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin();

  // Fan PWM setup
  ledcSetup(FAN_CHANNEL, FAN_FREQ, FAN_RESOLUTION);
  ledcAttachPin(FAN_PIN, FAN_CHANNEL);
  ledcWrite(FAN_CHANNEL, 0);
  Serial.println("風扇 PWM 已初始化 (GPIO 25)");

  if (scanI2C()) {
    Serial.print("使用 BME688 地址：0x");
    if (bmeAddress < 16) Serial.print('0');
    Serial.println(bmeAddress, HEX);
    if (bme.begin(bmeAddress)) {
      bmeDetected = true;
      Serial.println("找到 BME688 感測器");
      bme.setTemperatureOversampling(BME680_OS_8X);
      bme.setHumidityOversampling(BME680_OS_2X);
      bme.setPressureOversampling(BME680_OS_4X);
      bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
      bme.setGasHeater(320, 150);
    } else {
      Serial.println("BME688 初始化失敗，請檢查硬體接線");
    }
  } else {
    Serial.println("未找到 BME688 I2C 地址，請檢查 I2C 連接");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-BME688", "12345678");
  Serial.println("AP 已啟動");
  Serial.print("SSID: ESP32-BME688  密碼: 12345678");
  Serial.print("AP IP：");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/fan", HTTP_GET, handleFan);
  server.begin();
  Serial.println("Web 伺服器已啟動");
}

void loop() {
  server.handleClient();
  unsigned long now = millis();
  if (now - lastRead >= 2000) {
    lastRead = now;
    readSensor();
  }
}
