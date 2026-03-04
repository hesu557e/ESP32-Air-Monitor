#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>

// ========= AP 配置 =========
const char* AP_SSID = "ESP32-AirMon";
const char* AP_PASS = "12345678";   // >= 8 chars

IPAddress AP_IP(192,168,4,1);
IPAddress AP_GW(192,168,4,1);
IPAddress AP_MASK(255,255,255,0);

// ========= 传感器 UART（SC-4M01A）=========
HardwareSerial SensorSerial(1);
#define RX_PIN 16   // ESP32 RX  <- 模块 TX
#define TX_PIN 17   // ESP32 TX  -> 模块 RX
#define SENSOR_BAUD 9600

#define FRAME_LEN 14
#define HEAD0 0x2C
#define HEAD1 0xE4

WebServer server(80);

// ========= 数据结构 =========
struct SensorData {
  float tvoc = NAN;      // mg/m^3
  float hcho = NAN;      // mg/m^3
  uint16_t co2 = 0;      // ppm
  uint8_t aqi = 0;       // 1..6
  float temp = NAN;      // C
  float rh = NAN;        // %
  bool valid = false;
  uint32_t ms = 0;
};
SensorData latest;

// ========= 高级 UI（HTTP 轮询 /data）=========
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP32 Air Monitor</title>
<style>
:root{
  --bg:#0b1220;
  --panel:#121b2f;
  --card:#16223a;
  --line:#223454;
  --text:#e6eefc;
  --muted:#8ea2c6;
  --good:#22c55e;
  --warn:#f59e0b;
  --bad:#ef4444;
  --accent:#60a5fa;
}
*{box-sizing:border-box}
body{
  margin:0;
  font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, Arial;
  background: radial-gradient(1200px 700px at 20% 10%, #14214a 0%, var(--bg) 45%, #060a14 100%);
  color:var(--text);
}
.container{max-width:900px;margin:0 auto;padding:18px 16px 28px;}
.header{display:flex;align-items:flex-end;justify-content:space-between;gap:12px;margin-bottom:14px;}
.title{display:flex;flex-direction:column;gap:6px;}
.title h1{margin:0;font-weight:800;letter-spacing:0.2px;font-size:22px;}
.title .sub{color:var(--muted);font-size:13px;}
.badge{
  display:inline-flex;align-items:center;gap:10px;
  padding:10px 12px;background: rgba(18,27,47,0.75);
  border: 1px solid rgba(34,52,84,0.55);border-radius: 14px;
}
.dot{width:10px;height:10px;border-radius:50%;background: var(--bad);box-shadow: 0 0 14px rgba(239,68,68,0.35);}
.badge span{font-size:13px;color:var(--muted);}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;}
.card{
  background: linear-gradient(180deg, rgba(22,34,58,0.96) 0%, rgba(18,27,47,0.96) 100%);
  border: 1px solid rgba(34,52,84,0.55);
  border-radius: 16px;padding: 14px 14px 12px;
  box-shadow: 0 12px 30px rgba(0,0,0,0.35);
}
.k{color:var(--muted);font-size:12px;display:flex;align-items:center;justify-content:space-between;gap:8px;}
.k .unit{color:rgba(142,162,198,0.85);font-size:11px;}
.v{margin-top:8px;font-size:28px;font-weight:900;letter-spacing:0.3px;display:flex;align-items:baseline;gap:8px;}
.v small{font-size:12px;color:var(--muted);font-weight:700;}
.pill{
  margin-top:10px;display:inline-flex;align-items:center;gap:8px;
  padding:6px 10px;border-radius: 999px;
  border:1px solid rgba(34,52,84,0.55);
  background: rgba(11,18,32,0.45);
  color: var(--muted);font-size: 12px;
}
.pill .level{font-weight:900;letter-spacing:0.2px;}
.panel{
  margin-top:12px;background: rgba(18,27,47,0.85);
  border: 1px solid rgba(34,52,84,0.55);
  border-radius: 16px;padding: 14px;
}
.panelTop{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:10px;}
.panelTitle{font-weight:900;letter-spacing:0.2px;}
.panelMeta{color:var(--muted);font-size:12px;}
.canvasWrap{
  position:relative;width:100%;height:220px;border-radius:12px;overflow:hidden;
  border:1px solid rgba(34,52,84,0.45);
  background: radial-gradient(900px 260px at 20% 10%, rgba(96,165,250,0.16) 0%, rgba(0,0,0,0) 55%);
}
#chart{width:100%;height:100%;}
.footer{
  margin-top: 12px;color: var(--muted);font-size: 12px;
  display:flex;justify-content:space-between;gap:10px;flex-wrap:wrap;
}
code{
  background: rgba(0,0,0,0.25);
  border: 1px solid rgba(34,52,84,0.45);
  padding: 2px 6px;border-radius: 8px;
  color: rgba(230,238,252,0.9);
}
@media(min-width:860px){.grid{grid-template-columns:repeat(3,minmax(0,1fr));}}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div class="title">
      <h1>Air Quality Dashboard</h1>
      <div class="sub">AP: <code>ESP32-AirMon</code> · 打开 <code>http://192.168.4.1/</code></div>
    </div>
    <div class="badge">
      <div id="dot" class="dot"></div>
      <span>LINK: <b id="linkState">WAIT</b> · 更新: <b id="lastUp">--</b></span>
    </div>
  </div>

  <div class="grid">
    <div class="card">
      <div class="k"><span>TVOC</span><span class="unit">mg/m³</span></div>
      <div class="v"><span id="tvoc">--</span><small id="tvocTag">--</small></div>
      <div class="pill">趋势 · <span class="level" id="tvocTrend">--</span></div>
    </div>

    <div class="card">
      <div class="k"><span>HCHO</span><span class="unit">mg/m³</span></div>
      <div class="v"><span id="hcho">--</span><small id="hchoTag">--</small></div>
      <div class="pill">提示 · <span class="level" id="hchoHint">--</span></div>
    </div>

    <div class="card">
      <div class="k"><span>CO₂</span><span class="unit">ppm</span></div>
      <div class="v"><span id="co2">--</span><small id="co2Tag">--</small></div>
      <div class="pill">通风 · <span class="level" id="co2Hint">--</span></div>
    </div>

    <div class="card">
      <div class="k"><span>AQI</span><span class="unit">1–6</span></div>
      <div class="v"><span id="aqi">--</span><small id="aqiTag">--</small></div>
      <div class="pill">等级 · <span class="level" id="aqiLevel">--</span></div>
    </div>

    <div class="card">
      <div class="k"><span>Temperature</span><span class="unit">°C</span></div>
      <div class="v"><span id="temp">--</span><small id="tempTag">--</small></div>
      <div class="pill">环境 · <span class="level" id="tempHint">--</span></div>
    </div>

    <div class="card">
      <div class="k"><span>Humidity</span><span class="unit">%</span></div>
      <div class="v"><span id="rh">--</span><small id="rhTag">--</small></div>
      <div class="pill">舒适 · <span class="level" id="rhHint">--</span></div>
    </div>
  </div>

  <div class="panel">
    <div class="panelTop">
      <div class="panelTitle">TVOC Trend (last 60 points)</div>
      <div class="panelMeta">Data API: <code>/data</code></div>
    </div>
    <div class="canvasWrap">
      <canvas id="chart"></canvas>
    </div>
  </div>

  <div class="footer">
    <div>提示：若手机提示“无互联网”，请选择“仍然连接”。</div>
    <div>更新卡住：刷新页面即可恢复。</div>
  </div>
</div>

<script>
(function(){
  const $ = (id)=>document.getElementById(id);
  const dot = $("dot");
  const linkState = $("linkState");
  const lastUp = $("lastUp");

  function setLink(ok, text){
    linkState.textContent = text;
    if(ok){
      dot.style.background = "var(--good)";
      dot.style.boxShadow = "0 0 14px rgba(34,197,94,0.35)";
      linkState.style.color = "var(--good)";
    }else{
      dot.style.background = "var(--bad)";
      dot.style.boxShadow = "0 0 14px rgba(239,68,68,0.35)";
      linkState.style.color = "var(--bad)";
    }
  }

  function fmt(n, digits){
    if(typeof n !== "number" || isNaN(n)) return "--";
    return n.toFixed(digits);
  }
  function tagByCO2(co2){
    if(co2 < 800) return ["Good","var(--good)"];
    if(co2 < 1200) return ["Moderate","var(--warn)"];
    return ["High","var(--bad)"];
  }
  function tagByAQI(aqi){
    const map = {
      1:["Excellent","var(--good)"],
      2:["Good","var(--good)"],
      3:["Moderate","var(--warn)"],
      4:["Poor","var(--warn)"],
      5:["Bad","var(--bad)"],
      6:["Severe","var(--bad)"]
    };
    return map[aqi] || ["--","var(--muted)"];
  }

  // chart (no external libs)
  const canvas = document.getElementById("chart");
  const ctx = canvas.getContext("2d");
  let tvocSeries = [];
  const MAX_POINTS = 60;

  function resizeCanvas(){
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.floor(rect.width * dpr);
    canvas.height = Math.floor(rect.height * dpr);
    ctx.setTransform(dpr,0,0,dpr,0,0);
    drawChart();
  }
  window.addEventListener("resize", resizeCanvas);

  function drawGrid(w,h){
    ctx.save();
    ctx.globalAlpha = 0.35;
    ctx.strokeStyle = "rgba(142,162,198,0.35)";
    ctx.lineWidth = 1;
    const stepY = h/5;
    for(let i=1;i<5;i++){
      ctx.beginPath(); ctx.moveTo(0,i*stepY); ctx.lineTo(w,i*stepY); ctx.stroke();
    }
    ctx.restore();
  }

  function drawChart(){
    const w = canvas.clientWidth;
    const h = canvas.clientHeight;
    ctx.clearRect(0,0,w,h);
    drawGrid(w,h);

    if(tvocSeries.length < 2){
      ctx.fillStyle = "rgba(142,162,198,0.8)";
      ctx.font = "13px ui-sans-serif,system-ui";
      ctx.fillText("Waiting for data…", 14, 22);
      return;
    }

    let min = Math.min(...tvocSeries);
    let max = Math.max(...tvocSeries);
    if(min === max) max = min + 0.01;
    const pad = (max-min)*0.15;
    min = Math.max(0, min-pad);
    max = max + pad;

    ctx.save();
    ctx.lineWidth = 2.2;
    ctx.strokeStyle = "rgba(96,165,250,0.95)";
    ctx.shadowColor = "rgba(96,165,250,0.35)";
    ctx.shadowBlur = 10;

    const n = tvocSeries.length;
    const dx = w / (MAX_POINTS-1);

    ctx.beginPath();
    for(let i=0;i<n;i++){
      const x = i*dx;
      const y = h - ((tvocSeries[i]-min)/(max-min))*h;
      if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
    }
    ctx.stroke();
    ctx.restore();
  }

  // render
  function setSmallTag(elId, text, colorVar){
    const el = document.getElementById(elId);
    el.textContent = text;
    el.style.color = colorVar || "var(--muted)";
  }

  function render(d){
    $("tvoc").textContent = fmt(d.tvoc,3);
    $("hcho").textContent = fmt(d.hcho,3);
    $("co2").textContent  = (d.co2 ?? "--");
    $("aqi").textContent  = (d.aqi ?? "--");
    $("temp").textContent = fmt(d.temp,1);
    $("rh").textContent   = fmt(d.rh,1);

    lastUp.textContent = new Date().toLocaleTimeString();

    const [co2Tag, co2Color] = tagByCO2(d.co2||0);
    setSmallTag("co2Tag", co2Tag, co2Color);
    $("co2Hint").textContent = (d.co2 < 800) ? "OK" : (d.co2 < 1200) ? "Ventilate" : "Open window";

    const [aqiTag, aqiColor] = tagByAQI(d.aqi||0);
    setSmallTag("aqiTag", aqiTag, aqiColor);
    $("aqiLevel").textContent = aqiTag;

    $("hchoHint").textContent = (d.hcho < 0.08) ? "OK" : (d.hcho < 0.1) ? "Watch" : "Concern";
    setSmallTag("hchoTag", "mg/m³", "var(--muted)");
    setSmallTag("tvocTag", "mg/m³", "var(--muted)");

    $("tempHint").textContent = (d.temp < 18) ? "Cool" : (d.temp < 26) ? "Comfort" : "Warm";
    setSmallTag("tempTag", "°C", "var(--muted)");

    $("rhHint").textContent = (d.rh < 30) ? "Dry" : (d.rh < 60) ? "Comfort" : "Humid";
    setSmallTag("rhTag", "%", "var(--muted)");

    const tv = (typeof d.tvoc === "number") ? d.tvoc : NaN;
    if(!isNaN(tv)){
      tvocSeries.push(tv);
      if(tvocSeries.length > MAX_POINTS) tvocSeries.shift();
      drawChart();
      if(tvocSeries.length >= 6){
        const a = tvocSeries[tvocSeries.length-1];
        const b = tvocSeries[tvocSeries.length-6];
        const diff = a - b;
        $("tvocTrend").textContent = diff > 0.002 ? "Rising" : (diff < -0.002 ? "Falling" : "Stable");
      }else{
        $("tvocTrend").textContent = "Collecting";
      }
    }
  }

  // poll /data
  async function tick(){
    try{
      const r = await fetch("/data", {cache:"no-store"});
      if(!r.ok) throw new Error("HTTP "+r.status);
      const d = await r.json();
      setLink(true, "OK");
      if(d && d.valid) render(d);
    }catch(e){
      setLink(false, "ERR");
    }
  }

  resizeCanvas();
  tick();
  setInterval(tick, 1000);
})();
</script>
</body>
</html>
)HTML";

// ========= 传感器读取/解析 =========
bool readFrame(uint8_t *f) {
  while (SensorSerial.available()) {
    uint8_t b = SensorSerial.read();
    if (b != HEAD0) continue;

    uint32_t t0 = millis();
    while (!SensorSerial.available() && millis() - t0 < 50) delay(1);
    if (!SensorSerial.available()) return false;

    uint8_t b2 = SensorSerial.read();
    if (b2 != HEAD1) continue;

    f[0] = HEAD0; f[1] = HEAD1;

    int idx = 2;
    t0 = millis();
    while (idx < FRAME_LEN && millis() - t0 < 200) {
      if (SensorSerial.available()) f[idx++] = SensorSerial.read();
      else delay(1);
    }
    return (idx == FRAME_LEN);
  }
  return false;
}

bool checksumOK(const uint8_t *f) {
  uint16_t sum = 0;
  for (int i = 0; i <= 12; i++) sum += f[i];
  return ((sum & 0xFF) == f[13]);
}

void parseFrame(const uint8_t *f) {
  uint16_t tvoc_raw = (uint16_t)f[3] * 256 + f[2];
  uint16_t hcho_raw = (uint16_t)f[5] * 256 + f[4];
  uint16_t co2_ppm  = (uint16_t)f[7] * 256 + f[6];
  uint8_t  aqi      = f[8];

  float temp_c = (float)f[10] + ((float)f[9] / 10.0f);
  float rh_pct = (float)f[12] + ((float)f[11] / 10.0f);

  latest.tvoc = tvoc_raw * 0.001f;
  latest.hcho = hcho_raw * 0.001f;
  latest.co2  = co2_ppm;
  latest.aqi  = aqi;
  latest.temp = temp_c;
  latest.rh   = rh_pct;
  latest.valid = true;
  latest.ms = millis();
}

String dataJSON() {
  String s;
  s.reserve(180);
  s += "{";
  s += "\"valid\":"; s += (latest.valid ? "true" : "false"); s += ",";
  s += "\"tvoc\":";  s += String(latest.tvoc, 3); s += ",";
  s += "\"hcho\":";  s += String(latest.hcho, 3); s += ",";
  s += "\"co2\":";   s += String(latest.co2); s += ",";
  s += "\"aqi\":";   s += String(latest.aqi); s += ",";
  s += "\"temp\":";  s += String(latest.temp, 1); s += ",";
  s += "\"rh\":";    s += String(latest.rh, 1);
  s += "}";
  return s;
}

// ========= HTTP handlers =========
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleData() {
  String s = dataJSON();
  server.send(200, "application/json", s);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // UART
  SensorSerial.begin(SENSOR_BAUD, SERIAL_8N1, RX_PIN, TX_PIN);
  Serial.println("Sensor UART started (9600, RX=16, TX=17).");

  // AP
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("AP start: %s\n", ok ? "OK" : "FAIL");
  Serial.print("SSID: "); Serial.println(AP_SSID);
  Serial.print("PASS: "); Serial.println(AP_PASS);
  Serial.print("IP  : "); Serial.println(WiFi.softAPIP());

  // HTTP
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  Serial.println("HTTP: http://192.168.4.1/  (data: /data)");
}

void loop() {
  server.handleClient();

  static uint8_t f[FRAME_LEN];
  static uint32_t lastDbg = 0;

  if (readFrame(f) && checksumOK(f)) {
    parseFrame(f);
  }

  if (millis() - lastDbg >= 5000) {
    lastDbg = millis();
    Serial.printf("[DBG] valid=%d tvoc=%.3f co2=%u\n", latest.valid, latest.tvoc, latest.co2);
  }
}