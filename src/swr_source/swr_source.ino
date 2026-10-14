/* SWR and Power meter with wifi interface by RA0SMS
 * ESP8266 based
 * https://github.com/ra0sms/SWR_PWR_meter
 * 2019 - First version
 *
 * 12032021 - Add switching 2000/1000/200W maxpower (use define)
 * 22052025 - Add new CSS based web-interface
 * 24062026 - Code cleanup: removed unused vars/includes, constexpr, millis() timer
 * 13102026 - Reliability & security: reconnect on connection loss, no-cache headers,
 *            SWR clamp, dark /swr redesign, SSB/CW/FT8 mode selector, HTTP Basic Auth
 * 14102026 - Restored blocking waitForConnectResult (fixes first-connect WL_WRONG_PASSWORD)
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

// Maximum power range: 200, 300, 1000, 2000
constexpr int MAX_POWER = 1000;

const char *softAP_ssid = "SWRmeter_sn083";
const char *softAP_password = "1234567890";

// Authentication for web pages (HTTP Basic Auth)
const bool AUTH_ENABLED = false;  // Set to false to build firmware without authentication
const char *auth_user = "admin";
const char *auth_pass = "swr12345";

char ssid[33] = "";      // Max SSID length is 32 chars + null terminator
char password[64] = "";  // WPA2 allows passwords up to 63 chars + null terminator
String javaScript, XML, progress, StringSWR;

const int switch_pin = 5;
const int led_pin = 16;
unsigned int forwU = 0;
unsigned int forwU_prev = 0;
unsigned int refrU = 0;
int power = 0;
int power_per = 0;
float swr = 0;
unsigned int sum = 0;
unsigned int raz = 0;

// Uptime tracking using millis()
unsigned long previousMillis = 0;
const unsigned long interval = 1000; // 1 second
int sec = 0;
int minute = 0;
int hour = 0;
int day = 0;

int isWiFiConnected = 0;

byte mode = 0; // Display smoothing mode: 0=SSB, 1=CW, 2=FT8

const byte DNS_PORT = 53;
DNSServer dnsServer;

ESP8266WebServer server(80);

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);
boolean connect;
unsigned long lastConnectTry = 0;
unsigned int status = WL_IDLE_STATUS;


String floatToString(float x, byte precision = 2) {
  char tmp[50];
  dtostrf(x, 0, precision, tmp);
  return String(tmp);
}

// Returns true if the request is authorized (or auth is disabled)
bool requireAuth() {
  if (!AUTH_ENABLED) return true;
  if (server.authenticate(auth_user, auth_pass)) return true;
  server.requestAuthentication();
  return false;
}

int readMedian (){
  int samples = 12;
  int raw[samples];
  for (int i = 0; i < samples; i++){
    raw[i] = analogRead(A0);
    delayMicroseconds(200);
  }
  int temp = 0; 
  for (int i = 0; i < samples; i++){
    for (int j = 0; j < samples - 1; j++){
      if (raw[j] > raw[j + 1]){
        temp = raw[j];
        raw[j] = raw[j + 1];
        raw[j + 1] = temp;
      }
    }
  }
  return raw[samples/2];
}

void GetPower()
{
  forwU = readMedian();
  digitalWrite(switch_pin, HIGH);
  delayMicroseconds(5);
  refrU = readMedian();
  digitalWrite(switch_pin, LOW);
  delayMicroseconds(5);
  if ((forwU > 50) || (forwU_prev > 50))
  {
    forwU_prev = forwU;
    if (forwU < 50)
    {
      forwU_prev = 0;
    }
    int rawPower = 0;
    if (MAX_POWER == 1000)
    {
      rawPower = (forwU*forwU)/1046;   // MAX 1000w
    }
    if (MAX_POWER == 2000)
    {
      rawPower = (forwU*forwU)/523;     // MAX 2000w
    }
    if (MAX_POWER == 200)
    {
      rawPower = (forwU*forwU)/5230;     // MAX 200w
    }
    if (MAX_POWER == 300)
    {
      rawPower = (forwU*forwU)/3487;     // MAX 300w
    }
    // Send raw power value - smoothing is done on client side
    power = rawPower;
    
    // Calculate percentage based on max power
    if (MAX_POWER == 1000) power_per = (power / 10);
    else if (MAX_POWER == 2000) power_per = (power / 20);
    else if (MAX_POWER == 200) power_per = (power / 2);
    else if (MAX_POWER == 300) power_per = (power / 3);
    
    sum = forwU + refrU;
    if (forwU > refrU)
    {
      raz = forwU - refrU;
      swr = (float(sum)) / (float(raz));
      if (swr > 99.0) swr = 99.0; // Clamp noisy SWR at low power
      StringSWR = floatToString(swr, 2);
    } else {
      StringSWR = ">10";
    }
  } else
  {
    power = 0;
    StringSWR = " ";
  }
}


void handleXML() {
  if (!requireAuth()) return;
  buildXML();
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/xml", XML);
}

void ProgressBar() {
  progress = R"(
<div class="bar"><div id="myBar" class="bar-fill"></div></div>
)";
}

void buildXML() {
  XML = "<?xml version='1.0'?>";
  XML += "<response>";
  XML += "<pwr>";
  XML += String(power);
  XML += "</pwr>";
  XML += "<swr>";
  XML += StringSWR;
  XML += "</swr>";
  XML += "<adcpower>";
  XML += String(power_per, DEC);
  XML += "</adcpower>";
  XML += "</response>";
}

void buildJavascript() {
  javaScript = R"(
<script>
// Smoothing presets per mode (0.0 - 1.0, lower = smoother)
const MODES = {
  ssb: { smooth: 0.2, pwr: 0.15, swr: 0.1, decay: 0.5 },
  cw:  { smooth: 0.3, pwr: 0.4,  swr: 0.15, decay: 0.5 },
  ft8: { smooth: 0.2, pwr: 0.25, swr: 0.1, decay: 0.4 }
};

let currentMode = 'ssb'; // actual value is set by handleSWR
let JS_SMOOTH, JS_SMOOTH_PWR, JS_SMOOTH_SWR, JS_DECAY;
let smoothBar = -1;
let smoothPwr = -1;
let smoothSwr = 1.0;

// Apply smoothing coefficients for a mode and update button highlight
function applyMode(mode) {
  const m = MODES[mode];
  JS_SMOOTH = m.smooth;
  JS_SMOOTH_PWR = m.pwr;
  JS_SMOOTH_SWR = m.swr;
  JS_DECAY = m.decay;
  smoothBar = -1; smoothPwr = -1; smoothSwr = 1.0;
  document.querySelectorAll('.mode-btn').forEach(b =>
    b.classList.toggle('active', b.dataset.mode === mode));
}

// Switch mode: persist on device and apply coefficients
function setMode(mode) {
  if (!MODES[mode]) return;
  if (mode !== currentMode) {
    currentMode = mode;
    fetch('/mode?m=' + mode);
  }
  applyMode(mode);
}

const updateData = () => {
  fetch('/xml')
    .then(response => response.text())
    .then(str => (new window.DOMParser()).parseFromString(str, "text/xml"))
    .then(xml => {
      const rawPwr = parseFloat(xml.querySelector("pwr").textContent);
      const rawSwrText = xml.querySelector("swr").textContent.trim();
      const rawBar = parseInt(xml.querySelector("adcpower").textContent);
      
      // Initialize on first run (using -1 sentinel to handle zero values)
      if (smoothBar < 0) { smoothBar = rawBar; smoothPwr = rawPwr; }
      
      // --- Smooth power value ---
      const pwrDelta = rawPwr - smoothPwr;
      smoothPwr = smoothPwr + (pwrDelta >= 0 ? pwrDelta * JS_SMOOTH_PWR : pwrDelta * JS_DECAY);
      const displayPwr = Math.round(Math.max(smoothPwr, 0));
      
      // --- Smooth SWR value ---
      const rawSwr = parseFloat(rawSwrText);
      let displaySwr;
      if (isNaN(rawSwr) || rawSwr <= 0) {
        displaySwr = (rawSwrText === ">10") ? ">10" : "---";
        smoothSwr = 1.0;
      } else {
        smoothSwr = smoothSwr + (rawSwr - smoothSwr) * JS_SMOOTH_SWR;
        displaySwr = smoothSwr.toFixed(2);
      }
      
      // --- Smooth bar percentage ---
      // Instant reset to zero when no power
      if (rawBar === 0) {
        smoothBar = 0;
      } else {
        const barDelta = rawBar - smoothBar;
        smoothBar = smoothBar + (barDelta >= 0 ? barDelta * JS_SMOOTH : barDelta * JS_DECAY);
      }
      const displayBar = Math.round(Math.min(Math.max(smoothBar, 0), 100));
      
      // Update power display
      document.getElementById('pwrValue').textContent = displayPwr;
      document.getElementById('swrValue').textContent = displaySwr;
      
      // Update progress bar
      const bar = document.getElementById('myBar');
      bar.style.width = displayBar + '%';
      
      // Dynamic color: green < 50%, yellow 50-80%, red > 80%
      if (displayBar < 50) {
        bar.style.background = '#22c55e';
      } else if (displayBar < 80) {
        bar.style.background = '#eab308';
      } else {
        bar.style.background = '#ef4444';
      }
    })
    .catch(err => console.error('Error:', err));
  
  setTimeout(updateData, 100);
};

document.addEventListener('DOMContentLoaded', () => {
  applyMode(currentMode);
  updateData();
});
</script>
)";
}

void handleSWR() {
  if (!requireAuth()) return;
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  buildJavascript();
  ProgressBar();

  String webPage = R"(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SWR/PWR Meter</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      background: #0f172a;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 16px;
    }
    .card {
      width: 100%;
      max-width: 400px;
      background: #1e293b;
      border-radius: 12px;
      padding: 20px;
      box-shadow: 0 8px 30px rgba(0,0,0,.4);
    }
    .title {
      color: #cbd5e1;
      font-size: .75rem;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 2px;
      text-align: center;
      margin-bottom: 16px;
    }
    .row { display: flex; gap: 12px; }
    .cell {
      flex: 1;
      background: #0f172a;
      border-radius: 8px;
      padding: 14px 10px;
      text-align: center;
    }
    .label {
      color: #cbd5e1;
      font-size: .7rem;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 1.5px;
      margin-bottom: 4px;
    }
    .value {
      color: #f1f5f9;
      font-size: 1.9rem;
      font-weight: 700;
      font-variant-numeric: tabular-nums;
      line-height: 1.2;
    }
    .unit { color: #94a3b8; font-size: .8rem; font-weight: 400; }
    .bar {
      height: 6px;
      background: #0f172a;
      border-radius: 3px;
      margin-top: 12px;
      overflow: hidden;
    }
    .bar-fill {
      height: 100%;
      width: 0%;
      background: #22c55e;
      border-radius: 3px;
      transition: width .15s ease, background .3s ease;
    }
    .meta {
      display: flex;
      justify-content: space-between;
      align-items: center;
      color: #94a3b8;
      font-size: .75rem;
      margin-top: 10px;
    }
    .meta a { color: #60a5fa; text-decoration: none; }
    .modes {
      display: flex;
      gap: 6px;
      margin-top: 12px;
    }
    .mode-btn {
      flex: 1;
      padding: 6px 0;
      background: #0f172a;
      color: #94a3b8;
      border: 1px solid #334155;
      border-radius: 6px;
      font-size: .7rem;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 1px;
      cursor: pointer;
      transition: all .2s ease;
    }
    .mode-btn:hover { color: #e2e8f0; border-color: #475569; }
    .mode-btn.active {
      background: #3b82f6;
      color: #fff;
      border-color: #3b82f6;
    }
    .error { color: #f87171; text-align: center; padding: 20px 0; }
    .error a { color: #60a5fa; }
  </style>
</head>
<body>
  <div class="card">
    <div class="title">SWR / PWR Meter</div>
)";

  if (isWiFiConnected == 1) {
    webPage += R"(
    <div class="row">
      <div class="cell">
        <div class="label">Power</div>
        <div class="value"><span id="pwrValue">0</span> <span class="unit">W</span></div>
      </div>
      <div class="cell">
        <div class="label">SWR</div>
        <div class="value" id="swrValue">1.00</div>
      </div>
    </div>
)";
    webPage += progress;
    webPage += R"(
    <div class="modes">
      <button class="mode-btn" data-mode="ssb" onclick='setMode("ssb")'>SSB</button>
      <button class="mode-btn" data-mode="cw" onclick='setMode("cw")'>CW</button>
      <button class="mode-btn" data-mode="ft8" onclick='setMode("ft8")'>FT8</button>
    </div>
)";
  } else {
    webPage += R"(
    <div class="error">
      <strong>Connection required</strong><br>
      <a href='/wifi'>Connect to Wi-Fi</a>
    </div>
)";
  }

  webPage += R"(
    <div class="meta">
      <span>Max: )";
  if (MAX_POWER == 1000) webPage += "1000W";
  else if (MAX_POWER == 200) webPage += "200W";
  else if (MAX_POWER == 2000) webPage += "2000W";
  else if (MAX_POWER == 300) webPage += "300W";
  webPage += R"(</span>
      <a href="/">Home</a>
    </div>
  </div>
)";

  webPage += javaScript;
  webPage += "<script>currentMode = '";
  if (mode == 1) webPage += "cw";
  else if (mode == 2) webPage += "ft8";
  else webPage += "ssb";
  webPage += "';</script>";
  webPage += R"(
</body>
</html>
)";

  server.send(200, "text/html", webPage);
}

void handleRoot() {
  if (!requireAuth()) return;
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  String Page = R"(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>WiFi SWR-PWR Meter</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      background: #0f172a;
      min-height: 100vh;
      padding: 16px;
      color: #e2e8f0;
    }
    .card {
      max-width: 640px;
      margin: 0 auto;
      background: #1e293b;
      border-radius: 12px;
      padding: 24px;
      box-shadow: 0 8px 30px rgba(0,0,0,.4);
    }
    h1 {
      color: #f1f5f9;
      font-size: 1.2rem;
      text-align: center;
      margin-bottom: 20px;
    }
    .connection-info {
      background: #0f172a;
      border-radius: 8px;
      padding: 16px;
      margin-bottom: 20px;
      font-size: .9rem;
    }
    .connection-info strong { color: #f1f5f9; }
    .nav-links {
      display: flex;
      justify-content: center;
      gap: 12px;
      margin: 20px 0;
    }
    .btn {
      display: inline-block;
      padding: 10px 20px;
      background: #3b82f6;
      color: #fff;
      text-decoration: none;
      border-radius: 8px;
      font-size: .85rem;
      font-weight: 600;
      transition: background .2s;
    }
    .btn:hover { background: #2563eb; }
  </style>
</head>
<body>
  <div class="card">
    <h1>WiFi SWR-PWR Meter</h1>
    <div class="connection-info">
)";

  if (server.client().localIP() == apIP) {
    Page += "<p>Connected via <strong>Soft AP</strong>: ";
    Page += softAP_ssid;
    Page += "</p>";
  } else {
    Page += "<p>Connected to <strong>WiFi Network</strong>: ";
    Page += ssid;
    Page += "</p>";
  }

  Page += R"(
    </div>
    <div class="nav-links">
      <a class="btn" href='/wifi'>WiFi Configuration</a>
      <a class="btn" href='/swr'>SWR/PWR Meter</a>
    </div>
  </div>
</body>
</html>
)";

  server.send(200, "text/html", Page);
}

void handleWifi() {
  if (!requireAuth()) return;
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  String Page = String(R"(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>WiFi Configuration - SWR/PWR meter</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      background: #0f172a;
      min-height: 100vh;
      padding: 16px;
      color: #e2e8f0;
    }
    .card {
      max-width: 640px;
      margin: 0 auto;
      background: #1e293b;
      border-radius: 12px;
      padding: 24px;
      box-shadow: 0 8px 30px rgba(0,0,0,.4);
    }
    h1 {
      color: #f1f5f9;
      font-size: 1.2rem;
      text-align: center;
      margin-bottom: 20px;
    }
    h3 {
      color: #cbd5e1;
      font-size: .8rem;
      text-transform: uppercase;
      letter-spacing: 1.5px;
      margin-bottom: 12px;
    }
    .status-card {
      background: #0f172a;
      border-radius: 8px;
      padding: 16px;
      margin-bottom: 16px;
      font-size: .9rem;
    }
    .status-card strong { color: #f1f5f9; }
    table {
      width: 100%;
      border-collapse: collapse;
      margin: 10px 0;
    }
    th, td {
      padding: 10px 12px;
      text-align: left;
      border-bottom: 1px solid #334155;
      font-size: .85rem;
    }
    th {
      color: #94a3b8;
      text-transform: uppercase;
      font-size: .7rem;
      letter-spacing: 1px;
    }
    td { color: #e2e8f0; }
    tr:hover { background: #1e293b; }
    form {
      background: #0f172a;
      border-radius: 8px;
      padding: 16px;
      margin: 16px 0;
    }
    label {
      color: #94a3b8;
      font-size: .75rem;
      text-transform: uppercase;
      letter-spacing: 1px;
    }
    input[type="text"], input[type="password"] {
      width: 100%;
      padding: 10px;
      margin: 8px 0;
      background: #1e293b;
      color: #f1f5f9;
      border: 1px solid #334155;
      border-radius: 6px;
      box-sizing: border-box;
    }
    input[type="submit"] {
      width: 100%;
      padding: 12px;
      background: #3b82f6;
      color: #fff;
      border: none;
      border-radius: 8px;
      cursor: pointer;
      font-size: .9rem;
      font-weight: 600;
      margin-top: 8px;
    }
    input[type="submit"]:hover { background: #2563eb; }
    .error-card {
      background: rgba(248,113,113,.1);
      color: #f87171;
      border: 1px solid rgba(248,113,113,.3);
      border-radius: 8px;
      padding: 12px 16px;
      margin-bottom: 16px;
      font-size: .85rem;
    }
    .footer {
      text-align: center;
      margin-top: 20px;
    }
    .footer a {
      color: #60a5fa;
      text-decoration: none;
      font-size: .85rem;
    }
    .footer a:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <div class="card">
    <h1>WiFi Configuration</h1>
    <div class="status-card">
)");

  if (server.client().localIP() == apIP) {
    Page += F("<p>🔵 Connected via <strong>Soft AP</strong>: ");
    Page += softAP_ssid;
    Page += F("</p>");
  } else {
    Page += F("<p>🟢 Connected to <strong>WiFi Network</strong>: ");
    Page += ssid;
    Page += F("</p>");
  }

  Page += F("<p>Uptime: <strong>");
  Page += String(day) + "d " + String(hour) + "h " + String(minute) + "m " + String(sec) + "s";
  Page += F("</strong></p>");
  Page += F(R"(
  </div>

  <div class="status-card">
    <h3>SoftAP Configuration</h3>
    <table>
      <tr><th>Parameter</th><th>Value</th></tr>
      <tr><td>SSID</td><td>)");
  Page += softAP_ssid;
  Page += F(R"(</td></tr>
      <tr><td>IP Address</td><td>)");
  Page += WiFi.softAPIP().toString();
  Page += F(R"(</td></tr>
    </table>
  </div>

  <div class="status-card">
    <h3>WLAN Configuration</h3>
    <table>
      <tr><th>Parameter</th><th>Value</th></tr>
      <tr><td>SSID</td><td>)");
  Page += ssid;
  Page += F(R"(</td></tr>
      <tr><td>IP Address</td><td>)");
  Page += WiFi.localIP().toString();
  Page += F(R"(</td></tr>
    </table>
  </div>

  <div class="status-card">
    <h3>Available Networks</h3>
    <p>Click refresh if networks are missing</p>
    <table>
      <tr><th>Network Name</th><th>Signal</th></tr>
)");

  Serial.println("scan start");
  int n = WiFi.scanNetworks();
  Serial.println("scan done");

  if (n > 0) {
    for (int i = 0; i < n; i++) {
      Page += F("<tr><td>");
      Page += WiFi.SSID(i);
      if (WiFi.encryptionType(i) != ENC_TYPE_NONE) {
        Page += F(" <small>(secured)</small>");
      }
      Page += F("</td><td>");
      Page += WiFi.RSSI(i);
      Page += F(" dBm</td></tr>");
    }
  } else {
    Page += F("<tr><td colspan=\"2\">No WiFi networks found</td></tr>");
  }

  Page += F(R"(
    </table>
  </div>
)");

  // Show validation error if redirected from wifisave
  if (server.hasArg("error")) {
    String err = server.arg("error");
    Page += F("<div class='error-card'>");
    if (err == "ssid") {
      Page += F("<strong>Error:</strong> Invalid or missing network name (max 32 chars).");
    } else if (err == "pass") {
      Page += F("<strong>Error:</strong> Password too long (max 63 chars).");
    }
    Page += F("</div>");
  }

  Page += F(R"(
  <form method='POST' action='wifisave'>
    <h3>Connect to Network</h3>
    <label for="n">Network SSID:</label>
    <input type="text" id="n" name="n" placeholder="Enter network name" required>

    <label for="p">Password:</label>
    <input type="password" id="p" name="p" placeholder="Enter password">

    <input type="submit" value="Connect">
  </form>

  <div class="footer">
    <a href="/">← Return to Home Page</a>
  </div>
  </div>
</body>
</html>
)");

  server.send(200, "text/html", Page);
}


void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(404, "text/plain", message);
}

void handleWifiSave() {
  if (!requireAuth()) return;
  Serial.println("wifi save");
  String newSsid = server.arg("n");
  String newPass = server.arg("p");

  // Validate input before saving
  if (newSsid.length() == 0 || newSsid.length() > 32) {
    Serial.println("Invalid SSID, not saving");
    server.sendHeader("Location", "wifi?error=ssid", true);
  } else if (newPass.length() > 63) {
    Serial.println("Password too long, not saving");
    server.sendHeader("Location", "wifi?error=pass", true);
  } else {
    newSsid.toCharArray(ssid, sizeof(ssid));
    newPass.toCharArray(password, sizeof(password));
    server.sendHeader("Location", "wifi", true);
    saveCredentials();
    connect = strlen(ssid) > 0; // Request WLAN connect with new credentials if there is a SSID
  }

  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(302, "text/plain", "");    // Empty content inhibits Content-length header so we have to close the socket ourselves.
  server.client().stop(); // Stop is needed because we sent no content length
}

void loadCredentials() {
  EEPROM.begin(512);
  EEPROM.get(0, ssid);
  EEPROM.get(0 + sizeof(ssid), password);
  char ok[2 + 1];
  EEPROM.get(0 + sizeof(ssid) + sizeof(password), ok);
  EEPROM.end();
  if (String(ok) != String("OK")) {
    ssid[0] = 0;
    password[0] = 0;
  }
  Serial.println("Recovered credentials:");
  Serial.println(ssid);
  Serial.print("Password length: ");
  Serial.println(strlen(password));
}

/** Store WLAN credentials to EEPROM */
void saveCredentials() {
  EEPROM.begin(512);
  EEPROM.put(0, ssid);
  EEPROM.put(0 + sizeof(ssid), password);
  char ok[2 + 1] = "OK";
  EEPROM.put(0 + sizeof(ssid) + sizeof(password), ok);
  EEPROM.commit();
  EEPROM.end();
}

// Display smoothing mode is kept in RAM only (resets to SSB on reboot)
void handleMode() {
  if (!requireAuth()) return;
  String m = server.arg("m");
  byte newMode = 0; // ssb (default)
  if (m == "cw") newMode = 1;
  else if (m == "ft8") newMode = 2;
  mode = newMode; // RAM only, resets to SSB on reboot
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(200, "text/plain", "OK");
}


void connectWifi() {
  if (strlen(ssid) == 0) return; // No saved credentials
  Serial.println("Connecting as wifi client...");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("Password length: ");
  Serial.println(strlen(password));
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  // Block until connected or timeout (reliable first connect, matches original behavior)
  int connRes = WiFi.waitForConnectResult(10000);
  Serial.print("connRes: ");
  Serial.println(connRes);
  lastConnectTry = millis();
}

void routineWIFI()
{
  if (connect) {
    Serial.println("Connect requested");
    connect = false;
    connectWifi();
  }

  unsigned int s = WiFi.status();

  // Retry if not connected and enough time has passed (skip wrong password)
  if (s != WL_CONNECTED && s != WL_WRONG_PASSWORD && strlen(ssid) > 0 &&
      millis() > (lastConnectTry + 30000)) {
    Serial.println("Connection timeout, retrying...");
    connect = true;
  }

  if (status != s) { // WLAN status change
    Serial.print("Status: ");
    Serial.println(s);
    status = s;
    if (s == WL_CONNECTED) {
      /* Just connected to WLAN */
      Serial.println("");
      Serial.print("Connected to ");
      Serial.println(ssid);
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      digitalWrite(led_pin, LOW);
      isWiFiConnected = 1;
      WiFi.softAPdisconnect();
      delay(100);
    } else if (s == WL_NO_SSID_AVAIL || s == WL_CONNECTION_LOST ||
               (s == WL_DISCONNECTED && isWiFiConnected == 1)) {
      // Network unavailable or connection lost: fall back to AP mode.
      // NOTE: WL_DISCONNECTED is also a transient state right after
      // WiFi.begin(), so only treat it as a loss if we were connected.
      digitalWrite(led_pin, HIGH);
      WiFi.disconnect();
      if (isWiFiConnected == 1) {
        isWiFiConnected = 0;
        WiFi.softAPConfig(apIP, apIP, netMsk);
        WiFi.softAP(softAP_ssid, softAP_password);
        delay(500); // Without delay I've seen the IP address blank
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
      }
    } else if (s == WL_WRONG_PASSWORD) {
      Serial.println("Wrong password for the saved network. Re-enter credentials via /wifi.");
    }
  }
}

void setup(void){
  delay(100);
  pinMode(switch_pin, OUTPUT);
  pinMode(led_pin, OUTPUT);
  Serial.begin(115200);
  digitalWrite(led_pin, HIGH);
  Serial.println();
  Serial.print("Configuring access point...");
  /* You can remove the password parameter if you want the AP to be open. */
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(softAP_ssid, softAP_password);
  delay(500); // Without delay I've seen the IP address blank
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);
  server.on("/", handleRoot);
  server.on("/wifi", handleWifi);
  server.on("/swr", handleSWR);
  server.on("/wifisave", handleWifiSave);
  server.on("/generate_204", handleRoot);
  server.on("/fwlink", handleRoot);
  server.on("/xml", handleXML);
  server.on("/mode", handleMode);
  server.onNotFound(handleNotFound);
  server.begin(); // Web server start
  Serial.println("HTTP server started");
  loadCredentials(); // Load WLAN credentials from network
  connect = strlen(ssid) > 0; // Request WLAN connect if there is a SSID
}
 
void loop(void)
{
  routineWIFI();
  dnsServer.processNextRequest();
  
  // Uptime tracking using millis() - independent of loop frequency
  if (isWiFiConnected == 1) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= interval) {
      previousMillis = currentMillis;
      sec++;
      if (sec == 60) { minute++; sec = 0; }
      if (minute == 60) { hour++; minute = 0; }
      if (hour == 24) { day++; hour = 0; }
    }
  } else {
    // Reset uptime when not connected to WiFi
    previousMillis = millis();
    sec = minute = hour = day = 0;
  }
  
  if (isWiFiConnected == 1) GetPower();
  server.handleClient();
  delay(7); // Required to prevent WiFi instability with frequent ADC reads
}
