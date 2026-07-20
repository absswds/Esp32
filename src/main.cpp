#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

#define BME680_I2C_ADDRESS 0x76
#define BME680_I2C_ADDRESS_ALT 0x77
#define WIFI_SSID "Mi 11i"
#define WIFI_PASSWORD "31415926"

Adafruit_BME680 bme;
WebServer server(80);

int bmeAddress = BME680_I2C_ADDRESS;
bool bmeDetected = false;

float temperatureC = NAN;
float humidity = NAN;
float pressure = NAN;
float gasResistance = NAN;
unsigned long lastRead = 0;

String buildHtml() {
  return R"HTML(
<!DOCTYPE html>
<html lang="zh-Hant">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1.0" />
  <title>ESP32 BME688 即時監控</title>
  <style>
    :root { color-scheme: dark; }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      min-height: 100vh;
      font-family: 'Inter','Segoe UI',system-ui,-apple-system,sans-serif;
      background: radial-gradient(ellipse at 50% 0%, #0f172a 0%, #020617 100%);
      color: #e2e8f0;
      overflow-x: hidden;
      padding: 16px;
    }
    .wrapper { max-width: 1200px; margin: 0 auto; }
    .hero {
      display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 12px;
      padding: 20px 28px;
      border-radius: 20px;
      background: rgba(15,23,42,0.75);
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      border: 1px solid rgba(148,163,184,0.1);
      margin-bottom: 20px;
    }
    .hero h1 {
      font-size: clamp(1.4rem,3vw,2rem);
      font-weight: 600;
      letter-spacing: -0.01em;
      background: linear-gradient(135deg,#f8fafc,#94a3b8);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
    }
    .hero-right { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
    #status { color: #94a3b8; font-size: 0.85rem; }
    .btn {
      display: inline-flex; align-items: center; gap: 6px;
      padding: 8px 18px; border-radius: 10px; border: 1px solid rgba(148,163,184,0.15);
      background: rgba(30,41,59,0.8);
      color: #cbd5e1; font-size: 0.85rem; cursor: pointer;
      transition: all .2s; text-decoration: none;
    }
    .btn:hover { background: rgba(51,65,85,0.9); border-color: rgba(148,163,184,0.3); color: #f8fafc; }
    .btn svg { width: 16px; height: 16px; flex-shrink: 0; }
    .stats {
      display: grid;
      grid-template-columns: repeat(auto-fit,minmax(210px,1fr));
      gap: 14px;
      margin-bottom: 20px;
    }
    .stat {
      position: relative; overflow: hidden;
      padding: 18px 22px;
      border-radius: 18px;
      background: rgba(15,23,42,0.7);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid rgba(148,163,184,0.08);
      transition: transform .2s, border-color .2s;
    }
    .stat:hover { transform: translateY(-2px); }
    .stat .accent {
      position: absolute; top: 0; left: 0; width: 4px; height: 100%;
      border-radius: 0 2px 2px 0;
    }
    .stat .label {
      font-size: 0.8rem; font-weight: 500; color: #64748b;
      text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 6px;
    }
    .stat .value {
      display: flex; align-items: baseline;
      font-size: clamp(1.8rem,3.5vw,2.8rem); font-weight: 700;
      letter-spacing: -0.02em;
    }
    .stat .unit { margin-left: 5px; font-size: 0.85rem; font-weight: 400; color: #94a3b8; }
    .stat .mini { font-size: 0.75rem; color: #64748b; margin-top: 4px; }
    .card {
      border-radius: 20px;
      padding: 22px;
      background: rgba(15,23,42,0.7);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border: 1px solid rgba(148,163,184,0.08);
      margin-bottom: 20px;
    }
    .card-header {
      display: flex; align-items: center; justify-content: space-between; flex-wrap: wrap; gap: 10px;
      margin-bottom: 16px;
    }
    .card-title { font-size: 1rem; font-weight: 600; color: #f1f5f9; }
    .legend {
      display: flex; flex-wrap: wrap; gap: 14px;
      font-size: 0.8rem; color: #94a3b8;
    }
    .legend-item { display: inline-flex; align-items: center; gap: 6px; cursor: pointer; }
    .legend-dot { width: 10px; height: 10px; border-radius: 50%; }
    #chart {
      width: 100%; height: 320px;
      border-radius: 14px;
      display: block;
    }
    .table-wrap {
      max-height: 220px; overflow-y: auto;
      border-radius: 12px;
      border: 1px solid rgba(148,163,184,0.06);
    }
    .table-wrap::-webkit-scrollbar { width: 5px; }
    .table-wrap::-webkit-scrollbar-track { background: transparent; }
    .table-wrap::-webkit-scrollbar-thumb { background: rgba(148,163,184,0.2); border-radius: 4px; }
    table { width: 100%; border-collapse: collapse; font-size: 0.82rem; }
    thead { position: sticky; top: 0; z-index: 1; }
    th {
      background: rgba(15,23,42,0.95);
      color: #64748b; font-weight: 500; text-transform: uppercase; letter-spacing: 0.04em;
      padding: 10px 12px; text-align: left;
      border-bottom: 1px solid rgba(148,163,184,0.08);
    }
    td {
      padding: 8px 12px;
      border-bottom: 1px solid rgba(148,163,184,0.04);
      color: #cbd5e1;
    }
    tr:hover td { background: rgba(51,65,85,0.3); }
    .temp-c { color: #fb923c; }
    .hum-c  { color: #38bdf8; }
    .pres-c { color: #a78bfa; }
    .gas-c  { color: #34d399; }
    @media (max-width: 600px) {
      body { padding: 10px; }
      .stat { padding: 14px 16px; }
      #chart { height: 220px; }
      .card { padding: 14px; }
      .hero { padding: 14px 18px; flex-direction: column; align-items: stretch; }
    }
  </style>
</head>
<body>
  <div class="wrapper">
    <section class="hero">
      <h1>ESP32 BME688 即時監控</h1>
      <div class="hero-right">
        <span id="status">正在讀取資料...</span>
        <button class="btn" id="exportBtn" onclick="exportCSV()">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
          匯出 CSV
        </button>
      </div>
    </section>

    <section class="stats">
      <div class="stat">
        <div class="accent" style="background:linear-gradient(180deg,#f97316,#ef4444)"></div>
        <div class="label">溫度</div>
        <div class="value" id="temp">--<span class="unit">°C</span></div>
        <div class="mini" id="tempExtrema"></div>
      </div>
      <div class="stat">
        <div class="accent" style="background:linear-gradient(180deg,#0ea5e9,#06b6d4)"></div>
        <div class="label">濕度</div>
        <div class="value" id="hum">--<span class="unit">%</span></div>
        <div class="mini" id="humExtrema"></div>
      </div>
      <div class="stat">
        <div class="accent" style="background:linear-gradient(180deg,#8b5cf6,#a78bfa)"></div>
        <div class="label">氣壓</div>
        <div class="value" id="pres">--<span class="unit">hPa</span></div>
        <div class="mini" id="presExtrema"></div>
      </div>
      <div class="stat">
        <div class="accent" style="background:linear-gradient(180deg,#10b981,#34d399)"></div>
        <div class="label">氣體阻抗</div>
        <div class="value" id="gas">--<span class="unit">kΩ</span></div>
        <div class="mini" id="gasExtrema"></div>
      </div>
    </section>

    <section class="card">
      <div class="card-header">
        <span class="card-title">即時趨勢圖</span>
        <div class="legend" id="legend"></div>
      </div>
      <canvas id="chart"></canvas>
    </section>

    <section class="card">
      <div class="card-header">
        <span class="card-title">歷史紀錄 <span style="font-weight:400;color:#64748b;font-size:0.8rem" id="rowCount"></span></span>
      </div>
      <div class="table-wrap">
        <table>
          <thead><tr>
            <th>時間</th>
            <th>溫度 (°C)</th>
            <th>濕度 (%)</th>
            <th>氣壓 (hPa)</th>
            <th>氣體 (kΩ)</th>
          </tr></thead>
          <tbody id="tableBody"></tbody>
        </table>
      </div>
    </section>
  </div>

  <script>
    const COLORS = ['#fb923c','#38bdf8','#a78bfa','#34d399'];
    const KEYS = ['temperature','humidity','pressure','gas'];
    const LABELS = ['溫度','濕度','氣壓','氣體'];
    const UNITS = ['°C','%','hPa','kΩ'];

    const tempEl = document.getElementById('temp');
    const humEl = document.getElementById('hum');
    const presEl = document.getElementById('pres');
    const gasEl = document.getElementById('gas');
    const tExtEl = document.getElementById('tempExtrema');
    const hExtEl = document.getElementById('humExtrema');
    const pExtEl = document.getElementById('presExtrema');
    const gExtEl = document.getElementById('gasExtrema');
    const statusEl = document.getElementById('status');
    const chartCanvas = document.getElementById('chart');
    const ctx = chartCanvas.getContext('2d');
    const legendEl = document.getElementById('legend');
    const tbody = document.getElementById('tableBody');
    const rowCount = document.getElementById('rowCount');

    const displayPoints = 30;
    const disp = { labels:[], temperature:[], humidity:[], pressure:[], gas:[] };
    const fullHistory = [];

    const visible = { temperature:true, humidity:false, pressure:false, gas:false };

    function buildLegend() {
      legendEl.innerHTML = KEYS.map((k,i) =>
        `<span class="legend-item" data-key="${k}" style="opacity:${visible[k]?1:0.4}">
          <span class="legend-dot" style="background:${COLORS[i]}"></span>${LABELS[i]}</span>`
      ).join('');
      legendEl.querySelectorAll('.legend-item').forEach(el => {
        el.addEventListener('click',()=>{
          const k = el.dataset.key;
          visible[k] = !visible[k];
          buildLegend();
          drawChart();
        });
      });
    }

    function drawRoundedRect(ctx,x,y,w,h,r) {
      ctx.beginPath();
      ctx.moveTo(x+r,y); ctx.lineTo(x+w-r,y);
      ctx.quadraticCurveTo(x+w,y,x+w,y+r);
      ctx.lineTo(x+w,y+h-r);
      ctx.quadraticCurveTo(x+w,y+h,x+w-r,y+h);
      ctx.lineTo(x+r,y+h);
      ctx.quadraticCurveTo(x,y+h,x,y+h-r);
      ctx.lineTo(x,y+r);
      ctx.quadraticCurveTo(x,y,x+r,y);
      ctx.closePath();
    }

    function drawChart() {
      const W = chartCanvas.clientWidth, H = chartCanvas.clientHeight;
      ctx.clearRect(0,0,W,H);
      drawRoundedRect(ctx,0,0,W,H,14);
      ctx.fillStyle = 'rgba(15,23,42,0.85)';
      ctx.fill();

      const pad = { top:20, bottom:28, left:50, right:16 };
      const plotW = W - pad.left - pad.right;
      const plotH = H - pad.top - pad.bottom;

      const active = KEYS.filter(k => visible[k]);
      if (!active.length || disp.labels.length<2) {
        ctx.fillStyle = '#475569';
        ctx.font = '14px system-ui,sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('等待資料中...', W/2, H/2);
        return;
      }

      const ranges = {};
      KEYS.forEach(k => {
        const vals = disp[k].filter(v => !isNaN(v));
        if (vals.length) {
          let mn = Math.min(...vals), mx = Math.max(...vals);
          const padR = (mx-mn)*0.1 || 1;
          ranges[k] = { min: mn-padR, max: mx+padR, range: mx-mn+padR*2 || 1 };
        }
      });

      ctx.strokeStyle = 'rgba(148,163,184,0.08)';
      ctx.lineWidth = 1;
      for (let i=0;i<=4;i++) {
        const y = pad.top + (plotH*i)/4;
        ctx.beginPath(); ctx.moveTo(pad.left,y); ctx.lineTo(W-pad.right,y); ctx.stroke();
      }

      KEYS.forEach((k,i)=>{
        if (!visible[k]) return;
        const vals = disp[k];
        const r = ranges[k];
        if (!r) return;
        const color = COLORS[i];
        const pts = [];
        vals.forEach((v,idx)=>{
          const x = pad.left + (plotW*idx)/Math.max(1,disp.labels.length-1);
          const y = pad.top + plotH * (1 - (v-r.min)/r.range);
          pts.push({x,y});
        });

        const grad = ctx.createLinearGradient(0,pad.top,0,pad.top+plotH);
        grad.addColorStop(0,color+'40');
        grad.addColorStop(1,color+'05');
        ctx.beginPath();
        pts.forEach((p,idx)=>{ idx===0 ? ctx.moveTo(p.x,p.y) : ctx.lineTo(p.x,p.y); });
        ctx.lineTo(pts[pts.length-1].x, pad.top+plotH);
        ctx.lineTo(pts[0].x, pad.top+plotH);
        ctx.closePath();
        ctx.fillStyle = grad;
        ctx.fill();

        ctx.beginPath();
        pts.forEach((p,idx)=>{
          if (idx===0) ctx.moveTo(p.x,p.y);
          else {
            const cpx = (pts[idx-1].x+p.x)/2;
            const cpy = (pts[idx-1].y+p.y)/2;
            ctx.quadraticCurveTo(pts[idx-1].x,pts[idx-1].y,cpx,cpy);
          }
        });
        ctx.strokeStyle = color;
        ctx.lineWidth = 2.5;
        ctx.stroke();

        pts.forEach((p,idx)=>{
          if (idx===pts.length-1) {
            ctx.beginPath();
            ctx.arc(p.x,p.y,4,0,Math.PI*2);
            ctx.fillStyle = color;
            ctx.fill();
            ctx.strokeStyle = 'rgba(15,23,42,0.8)';
            ctx.lineWidth = 1.5;
            ctx.stroke();
          }
        });
      });

      const lastV = {};
      KEYS.forEach(k=>{
        if (!visible[k]) return;
        const vals = disp[k].filter(v=>!isNaN(v));
        if (vals.length) {
          lastV[k] = { min: Math.min(...vals).toFixed(1), max: Math.max(...vals).toFixed(1) };
        }
      });
      ctx.fillStyle = '#475569';
      ctx.font = '11px system-ui,sans-serif';
      ctx.textAlign = 'right';
      ctx.textBaseline = 'middle';
      const yLabels = ['最高','','','','最低'];
      KEYS.forEach(k=>{
        if (!visible[k]||!lastV[k]) return;
        ctx.fillStyle = '#475569';
        ctx.textAlign = 'right';
        ctx.fillText(lastV[k].max, pad.left-6, pad.top+6);
        ctx.fillText(lastV[k].min, pad.left-6, pad.top+plotH-2);
        ctx.fillStyle = COLORS[KEYS.indexOf(k)]+'60';
        ctx.textAlign = 'left';
        ctx.font = '10px system-ui,sans-serif';
        ctx.fillText(LABELS[KEYS.indexOf(k)], pad.left+4, pad.top+plotH+16);
      });

      if (disp.labels.length) {
        ctx.fillStyle = '#475569';
        ctx.font = '10px system-ui,sans-serif';
        ctx.textAlign = 'center';
        const step = Math.max(1,Math.floor(disp.labels.length/8));
        for (let i=0;i<disp.labels.length;i+=step) {
          const x = pad.left + (plotW*i)/Math.max(1,disp.labels.length-1);
          ctx.fillText(disp.labels[i], x, H-6);
        }
      }
    }

    function resizeChart() {
      const rect = chartCanvas.getBoundingClientRect();
      chartCanvas.width = rect.width * window.devicePixelRatio;
      chartCanvas.height = rect.height * window.devicePixelRatio;
      ctx.setTransform(window.devicePixelRatio,0,0,window.devicePixelRatio,0,0);
      drawChart();
    }

    function addData(data) {
      const now = new Date();
      const timeStr = now.toLocaleTimeString();
      disp.labels.push(timeStr);
      KEYS.forEach(k => disp[k].push(data[k]));
      if (disp.labels.length > displayPoints) {
        disp.labels.shift();
        KEYS.forEach(k => disp[k].shift());
      }

      const row = document.createElement('tr');
      row.innerHTML = `<td>${timeStr}</td>
        <td class="temp-c">${data.temperature.toFixed(1)}</td>
        <td class="hum-c">${data.humidity.toFixed(1)}</td>
        <td class="pres-c">${data.pressure.toFixed(1)}</td>
        <td class="gas-c">${data.gas.toFixed(1)}</td>`;
      tbody.insertBefore(row, tbody.firstChild);
      while (tbody.children.length > 100) tbody.removeChild(tbody.lastChild);
      rowCount.textContent = `(${tbody.children.length} 筆)`;

      fullHistory.push({
        time: now.toISOString().replace('T',' ').slice(0,19),
        temperature: data.temperature,
        humidity: data.humidity,
        pressure: data.pressure,
        gas: data.gas
      });
      if (fullHistory.length > 2000) fullHistory.splice(0, 500);

      KEYS.forEach((k,i)=>{
        const vals = fullHistory.map(r=>r[k]).filter(v=>!isNaN(v));
        if (vals.length>1) {
          const mn = Math.min(...vals), mx = Math.max(...vals);
          const el = [tExtEl,hExtEl,pExtEl,gExtEl][i];
          el.textContent = `最低 ${mn.toFixed(1)} / 最高 ${mx.toFixed(1)}`;
        }
      });

      drawChart();
    }

    async function loadData() {
      try {
        const res = await fetch('/data');
        const data = await res.json();
        if (data.ok) {
          tempEl.innerHTML = data.temperature.toFixed(1) + '<span class="unit">°C</span>';
          humEl.innerHTML  = data.humidity.toFixed(1)  + '<span class="unit">%</span>';
          presEl.innerHTML = data.pressure.toFixed(1)  + '<span class="unit">hPa</span>';
          gasEl.innerHTML  = data.gas.toFixed(1)       + '<span class="unit">kΩ</span>';
          statusEl.textContent = '最後更新：' + data.time;
          addData(data);
        } else {
          statusEl.textContent = data.message;
        }
      } catch { statusEl.textContent = '更新失敗'; }
    }

    function exportCSV() {
      if (!fullHistory.length) { alert('尚無資料可匯出'); return; }
      const head = '時間,溫度 (°C),濕度 (%),氣壓 (hPa),氣體 (kΩ)';
      const rows = fullHistory.map(r =>
        `${r.time},${r.temperature.toFixed(2)},${r.humidity.toFixed(2)},${r.pressure.toFixed(2)},${r.gas.toFixed(2)}`
      );
      const bom = '\uFEFF';
      const csv = bom + head + '\n' + rows.join('\n');
      const blob = new Blob([csv], { type:'text/csv;charset=utf-8;' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `BME688_${new Date().toISOString().slice(0,19).replace(/[:-]/g,'')}.csv`;
      document.body.appendChild(a); a.click();
      document.body.removeChild(a); URL.revokeObjectURL(url);
    }

    buildLegend();
    window.addEventListener('resize', resizeChart);
    resizeChart();
    setInterval(loadData, 2000);
    loadData();
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
    char buffer[200];
    snprintf(buffer, sizeof(buffer),
             "{\"ok\":true,\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"gas\":%.2f,\"time\":\"%02u:%02u:%02u\"}",
             temperatureC, humidity, pressure, gasResistance, (millis() / 1000 / 3600) % 24, (millis() / 1000 / 60) % 60, (millis() / 1000) % 60);
    json = buffer;
  }
  server.send(200, "application/json", json);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("正在連線到 Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print('.');
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Wi-Fi 已連線，IP 地址：");
    Serial.println(WiFi.localIP());
    return;
  }

  Serial.println();
  Serial.println("Wi-Fi 連線失敗，啟動 AP 模式");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-BME688", "12345678");
  Serial.print("AP 已啟動，請連線到 SSID: ESP32-BME688");
  Serial.println(" 密碼: 12345678");
  Serial.print("AP IP 地址：");
  Serial.println(WiFi.softAPIP());
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

void readSensor() {
  if (!bmeDetected) {
    return;
  }

  if (!bme.performReading()) {
    Serial.println("BME688 讀取失敗");
    temperatureC = NAN;
    humidity = NAN;
    pressure = NAN;
    gasResistance = NAN;
    return;
  }

  temperatureC = bme.temperature;
  humidity = bme.humidity;
  pressure = bme.pressure / 100.0;
  gasResistance = bme.gas_resistance / 1000.0;

  Serial.print("Temp: "); Serial.print(temperatureC); Serial.println(" °C");
  Serial.print("Hum:  "); Serial.print(humidity); Serial.println(" %");
  Serial.print("Pres: "); Serial.print(pressure); Serial.println(" hPa");
  Serial.print("Gas:  "); Serial.print(gasResistance); Serial.println(" kΩ");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin();

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

  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.begin();

  Serial.println("Web 伺服器已啟動");
  Serial.println("請在瀏覽器打開：http://" + WiFi.localIP().toString());
}

void loop() {
  server.handleClient();

  unsigned long now = millis();
  if (now - lastRead >= 2000) {
    lastRead = now;
    readSensor();
  }
}
