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
 * 15102026 - Smooth PEP display: peak-hold sampling on device, fwd/ref measured in pairs,
 *            time-based ballistics (attack/hold/release) and peak marker in browser,
 *            WiFi modem sleep off, non-blocking reconnect retries, HTML escaping,
 *            pages served from PROGMEM, diode offset calibration constant
 * 16102026 - SoftAP SSID is built from the chip ID (SWRmeter_XXXXXX), no per-device edits
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>

// Maximum power range: 200, 300, 1000, 2000
constexpr int MAX_POWER = 1000;
static_assert(MAX_POWER == 200 || MAX_POWER == 300 || MAX_POWER == 1000 || MAX_POWER == 2000,
              "MAX_POWER must be 200, 300, 1000 or 2000");

// Power calibration: P = (U + DIODE_OFFSET)^2 / POWER_K, U in ADC counts (0..1023)
constexpr float POWER_K = (MAX_POWER == 2000) ? 523 :
                          (MAX_POWER == 1000) ? 1046 :
                          (MAX_POWER == 300)  ? 3487 : 5230;
// Detector diode drop in ADC counts (1N5711 ~0.2-0.3 V = ~60-90 counts on NodeMCU A0).
// 0 keeps the classic U^2/K calibration. If you set it, recalibrate POWER_K.
constexpr int DIODE_OFFSET = 0;

constexpr unsigned int FWD_MIN = 50;      // Forward level below this is noise -> 0 W
constexpr unsigned int SWR_MIN_FWD = 100; // SWR is computed only above this forward level

// Peak-hold sampling: the reading is the maximum over the last 1-2 bins
constexpr unsigned long PEAK_BIN_MS = 100;
constexpr int PAIRS_PER_CALL = 4;         // fwd/ref sample pairs per loop() pass

// SoftAP SSID: prefix + chip ID in hex, set in setup() (e.g. SWRmeter_1A2B3C)
const char *softAP_prefix = "SWRmeter_";
char softAP_ssid[24] = "";
const char *softAP_password = "1234567890";

// Authentication for web pages (HTTP Basic Auth)
const bool AUTH_ENABLED = false;  // Set to false to build firmware without authentication
const char *auth_user = "admin";
const char *auth_pass = "swr12345";

char ssid[33] = "";      // Max SSID length is 32 chars + null terminator
char password[64] = "";  // WPA2 allows passwords up to 63 chars + null terminator
String StringSWR;

const int switch_pin = 5;
const int led_pin = 16;
int power = 0;
int power_per = 0;

// Peak-hold bins: max forward voltage and the reflected voltage measured right after it
unsigned int binFwd[2] = {0, 0};
unsigned int binRef[2] = {0, 0};
byte curBin = 0;
unsigned long binStart = 0;

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
const unsigned long RETRY_MS = 30000;
bool wrongPassword = false; // Set when a user-initiated connect was rejected
unsigned int status = WL_IDLE_STATUS;


String floatToString(float x, byte precision = 2) {
  char tmp[50];
  dtostrf(x, 0, precision, tmp);
  return String(tmp);
}

// Escape text for safe insertion into HTML (SSIDs come from the air)
String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += F("&amp;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;");  break;
      default:   out += c;
    }
  }
  return out;
}

// Returns true if the request is authorized (or auth is disabled)
bool requireAuth() {
  if (!AUTH_ENABLED) return true;
  if (server.authenticate(auth_user, auth_pass)) return true;
  server.requestAuthentication();
  return false;
}

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
}

// Start a chunked HTML response, page is then streamed with sendContent()/sendContent_P()
void beginPage() {
  sendNoCacheHeaders();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
}

void endPage() {
  server.sendContent(""); // Terminating chunk
}

// Average of two ADC reads (noise only, does not flatten envelope peaks)
unsigned int readAdc2() {
  unsigned int a = analogRead(A0);
  delayMicroseconds(50);
  return (a + analogRead(A0)) / 2;
}

// Sample forward/reflected in close pairs and keep the peak of the current bin
void samplePower() {
  unsigned long now = millis();
  if (now - binStart >= PEAK_BIN_MS) {
    if (now - binStart >= 2 * PEAK_BIN_MS) { // Loop was stalled: drop stale data
      binFwd[0] = binFwd[1] = 0;
      binRef[0] = binRef[1] = 0;
    }
    curBin ^= 1;
    binFwd[curBin] = 0;
    binRef[curBin] = 0;
    binStart = now;
  }
  for (int i = 0; i < PAIRS_PER_CALL; i++) {
    unsigned int f = readAdc2();
    digitalWrite(switch_pin, HIGH);
    delayMicroseconds(5);
    unsigned int r = readAdc2();
    digitalWrite(switch_pin, LOW);
    delayMicroseconds(5);
    if (f > binFwd[curBin]) {
      binFwd[curBin] = f;
      binRef[curBin] = r;
    }
    delayMicroseconds(200);
  }
}

// Convert the peak of the last 100-200 ms into power, bar percent and SWR string
void computeReading() {
  byte b = (binFwd[0] >= binFwd[1]) ? 0 : 1;
  unsigned int forwU = binFwd[b];
  unsigned int refrU = binRef[b];

  if (forwU < FWD_MIN) {
    power = 0;
    power_per = 0;
    StringSWR = "";
    return;
  }

  float u = forwU + DIODE_OFFSET;
  power = lroundf(u * u / POWER_K);
  power_per = min(100, power * 100 / MAX_POWER);

  if (forwU < SWR_MIN_FWD) {
    StringSWR = ""; // Too little power for a meaningful SWR
  } else if (forwU > refrU) {
    float swr = float(forwU + refrU) / float(forwU - refrU);
    if (swr > 99.0) swr = 99.0;
    StringSWR = floatToString(swr, 2);
  } else {
    StringSWR = ">10";
  }
}

// Compact endpoint used by the /swr page
void handleData() {
  if (!requireAuth()) return;
  computeReading();
  String json = "{\"p\":";
  json += power;
  json += ",\"s\":\"";
  json += StringSWR;
  json += "\"}";
  sendNoCacheHeaders();
  server.send(200, "application/json", json);
}

// Legacy XML endpoint (kept for external integrations)
void handleXML() {
  if (!requireAuth()) return;
  computeReading();
  String XML = "<?xml version='1.0'?><response><pwr>";
  XML += power;
  XML += "</pwr><swr>";
  XML += StringSWR;
  XML += "</swr><adcpower>";
  XML += power_per;
  XML += "</adcpower></response>";
  sendNoCacheHeaders();
  server.send(200, "text/xml", XML);
}

static const char SWR_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
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
      transition: opacity .3s ease;
    }
    .stale .value { opacity: .35; }
    .unit { color: #94a3b8; font-size: .8rem; font-weight: 400; }
    .bar {
      position: relative;
      height: 8px;
      background: #0f172a;
      border-radius: 4px;
      margin-top: 12px;
      overflow: hidden;
    }
    .bar-fill {
      height: 100%;
      width: 0%;
      background: #22c55e;
      border-radius: 4px;
      transition: background .3s ease;
    }
    .peak {
      position: absolute;
      top: 0;
      bottom: 0;
      left: 0;
      width: 2px;
      margin-left: -2px;
      background: #f1f5f9;
      opacity: 0;
      transition: opacity .3s ease;
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
)rawliteral";

static const char SWR_METER[] PROGMEM = R"rawliteral(
    <div class="row">
      <div class="cell">
        <div class="label">Power</div>
        <div class="value"><span id="pwrValue">0</span> <span class="unit">W</span></div>
      </div>
      <div class="cell">
        <div class="label">SWR</div>
        <div class="value" id="swrValue">---</div>
      </div>
    </div>
    <div class="bar"><div id="myBar" class="bar-fill"></div><div id="peak" class="peak"></div></div>
    <div class="modes">
      <button class="mode-btn" data-mode="ssb" onclick='setMode("ssb")'>SSB</button>
      <button class="mode-btn" data-mode="cw" onclick='setMode("cw")'>CW</button>
      <button class="mode-btn" data-mode="ft8" onclick='setMode("ft8")'>FT8</button>
    </div>
)rawliteral";

static const char SWR_NO_WIFI[] PROGMEM = R"rawliteral(
    <div class="error">
      <strong>Connection required</strong><br>
      <a href='/wifi'>Connect to Wi-Fi</a>
    </div>
)rawliteral";

// Meter script. MAX_POWER and currentMode are defined by handleSWR before this block.
static const char SWR_SCRIPT[] PROGMEM = R"rawliteral(
<script>
// Display ballistics per mode: attack/release are time constants (s), hold is ms.
// The device already sends the peak of the last 100-200 ms (PEP).
const MODES = {
  ssb: { attack: 0.03, hold: 1000, release: 0.8 },  // peak reading, readable through speech
  cw:  { attack: 0.03, hold: 400,  release: 0.4 },  // bridges gaps between dots/dashes
  ft8: { attack: 0.3,  hold: 0,    release: 0.3 }   // steady carrier: plain averaging
};
const PEAK_HOLD = 2000, PEAK_RELEASE = 1.0;  // peak marker on the bar
const SWR_TAU = 0.3, SWR_HOLD = 3000;        // SWR smoothing, keep last value in pauses
const POLL_MS = 80, STALE_MS = 2000, TEXT_MS = 200;

let m = MODES.ssb;
let target = 0, shown = 0, holdT = 0;
let peak = 0, peakT = 0;
let swrState = 'none', swrTarget = 1, swrShown = 1, swrT = 0;
let lastData = 0, lastFrame = 0, lastText = 0;
let pwrEl, swrEl, barEl, peakEl;

// Apply ballistics for a mode and update button highlight
function applyMode(mode) {
  m = MODES[mode];
  document.querySelectorAll('.mode-btn').forEach(b =>
    b.classList.toggle('active', b.dataset.mode === mode));
}

// Switch mode: persist on device and apply ballistics
function setMode(mode) {
  if (!MODES[mode]) return;
  if (mode !== currentMode) {
    currentMode = mode;
    fetch('/mode?m=' + mode).catch(() => {});
  }
  applyMode(mode);
}

// Exponential approach towards `to` with time constant tau (s) over dt (s)
const approach = (from, to, dt, tau) =>
  tau > 0 ? from + (to - from) * (1 - Math.exp(-dt / tau)) : to;

// Coarser steps at high power so the last digit does not flicker
const quantize = w => {
  const step = w >= 1000 ? 10 : w >= 100 ? 5 : 1;
  return Math.round(w / step) * step;
};

// Poll the device; the next request is sent only after the previous one finished
async function poll() {
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 2000);
  try {
    const r = await fetch('/data', { signal: ctl.signal, cache: 'no-store' });
    const d = await r.json();
    const now = performance.now();
    target = d.p;
    lastData = now;
    if (d.s === '>10') {
      swrState = 'high'; swrT = now;
    } else if (d.s !== '') {
      const s = parseFloat(d.s);
      if (!isNaN(s)) {
        if (swrState !== 'val') swrShown = s;
        swrState = 'val'; swrTarget = s; swrT = now;
      }
    }
    // Empty SWR (pause, low power): keep the last value until SWR_HOLD expires
  } catch (e) {}
  clearTimeout(timer);
  setTimeout(poll, POLL_MS);
}

// Render at display frame rate, independent of network timing
function frame(t) {
  const dt = lastFrame ? Math.min((t - lastFrame) / 1000, 0.25) : 0;
  lastFrame = t;

  const stale = t - lastData > STALE_MS;
  if (stale) target = 0;
  document.body.classList.toggle('stale', stale);

  // Power: fast attack, hold, then release
  if (target >= shown) {
    shown = approach(shown, target, dt, m.attack);
    holdT = t;
  } else if (t - holdT > m.hold) {
    shown = approach(shown, target, dt, m.release);
  }

  // Peak marker
  if (shown >= peak) {
    peak = shown; peakT = t;
  } else if (t - peakT > PEAK_HOLD) {
    peak = approach(peak, shown, dt, PEAK_RELEASE);
  }

  // SWR
  if (swrState === 'val') swrShown = approach(swrShown, swrTarget, dt, SWR_TAU);
  if (swrState !== 'none' && t - swrT > SWR_HOLD) swrState = 'none';

  const pct = Math.min(shown / MAX_POWER * 100, 100);
  barEl.style.width = pct + '%';
  barEl.style.background = pct < 50 ? '#22c55e' : pct < 80 ? '#eab308' : '#ef4444';
  peakEl.style.left = Math.min(peak / MAX_POWER * 100, 100) + '%';
  peakEl.style.opacity = peak >= 0.5 ? 1 : 0;

  if (t - lastText >= TEXT_MS) {
    lastText = t;
    pwrEl.textContent = quantize(shown);
    swrEl.textContent = swrState === 'val' ? swrShown.toFixed(2) :
                        swrState === 'high' ? '>10' : '---';
  }
  requestAnimationFrame(frame);
}

document.addEventListener('DOMContentLoaded', () => {
  pwrEl = document.getElementById('pwrValue');
  swrEl = document.getElementById('swrValue');
  barEl = document.getElementById('myBar');
  peakEl = document.getElementById('peak');
  applyMode(currentMode);
  poll();
  requestAnimationFrame(frame);
});
</script>
)rawliteral";

void handleSWR() {
  if (!requireAuth()) return;
  beginPage();
  server.sendContent_P(SWR_HEAD);
  server.sendContent_P(isWiFiConnected == 1 ? SWR_METER : SWR_NO_WIFI);

  String meta = R"(
    <div class="meta">
      <span>Max: )";
  meta += MAX_POWER;
  meta += R"(W</span>
      <a href="/">Home</a>
    </div>
  </div>
)";
  server.sendContent(meta);

  if (isWiFiConnected == 1) {
    String cfg = "<script>const MAX_POWER = ";
    cfg += MAX_POWER;
    cfg += "; let currentMode = '";
    cfg += (mode == 1) ? "cw" : (mode == 2) ? "ft8" : "ssb";
    cfg += "';</script>";
    server.sendContent(cfg);
    server.sendContent_P(SWR_SCRIPT);
  }
  server.sendContent(F("\n</body>\n</html>\n"));
  endPage();
}

static const char ROOT_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
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
)rawliteral";

static const char ROOT_TAIL[] PROGMEM = R"rawliteral(
    </div>
    <div class="nav-links">
      <a class="btn" href='/wifi'>WiFi Configuration</a>
      <a class="btn" href='/swr'>SWR/PWR Meter</a>
    </div>
  </div>
</body>
</html>
)rawliteral";

void handleRoot() {
  if (!requireAuth()) return;
  beginPage();
  server.sendContent_P(ROOT_HEAD);

  String info;
  if (server.client().localIP() == apIP) {
    info = "<p>Connected via <strong>Soft AP</strong>: ";
    info += htmlEscape(softAP_ssid);
  } else {
    info = "<p>Connected to <strong>WiFi Network</strong>: ";
    info += htmlEscape(ssid);
  }
  info += "</p>";
  server.sendContent(info);

  server.sendContent_P(ROOT_TAIL);
  endPage();
}

static const char WIFI_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
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
)rawliteral";

static const char WIFI_TAIL[] PROGMEM = R"rawliteral(
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
)rawliteral";

void handleWifi() {
  if (!requireAuth()) return;
  beginPage();
  server.sendContent_P(WIFI_HEAD);

  String Page;
  if (server.client().localIP() == apIP) {
    Page += F("<p>🔵 Connected via <strong>Soft AP</strong>: ");
    Page += htmlEscape(softAP_ssid);
  } else {
    Page += F("<p>🟢 Connected to <strong>WiFi Network</strong>: ");
    Page += htmlEscape(ssid);
  }
  Page += F("</p>");

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
  Page += htmlEscape(softAP_ssid);
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
  Page += htmlEscape(ssid);
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
  server.sendContent(Page); // Send status part before the slow network scan
  Page = "";

  Serial.println("scan start");
  int n = WiFi.scanNetworks();
  Serial.println("scan done");

  if (n > 0) {
    for (int i = 0; i < n; i++) {
      Page += F("<tr><td>");
      Page += htmlEscape(WiFi.SSID(i));
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
  WiFi.scanDelete();

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
  server.sendContent(Page);

  server.sendContent_P(WIFI_TAIL);
  endPage();
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
  sendNoCacheHeaders();
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
    wrongPassword = false;
    connect = strlen(ssid) > 0; // Request WLAN connect with new credentials if there is a SSID
  }

  sendNoCacheHeaders();
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
  sendNoCacheHeaders();
  server.send(200, "text/plain", "OK");
}


// blocking = true for user-initiated connects (boot, new credentials): waits for the
// result, which is reliable on first connect. Automatic retries are non-blocking so
// the web server and captive portal stay responsive.
void connectWifi(bool blocking) {
  if (strlen(ssid) == 0) return; // No saved credentials
  Serial.println(blocking ? "Connecting as wifi client..." : "Retrying wifi connection...");
  Serial.print("SSID: ");
  Serial.println(ssid);
  Serial.print("Password length: ");
  Serial.println(strlen(password));
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  if (blocking) {
    int connRes = WiFi.waitForConnectResult(10000);
    Serial.print("connRes: ");
    Serial.println(connRes);
    wrongPassword = (connRes == WL_WRONG_PASSWORD);
  }
  lastConnectTry = millis();
}

void routineWIFI()
{
  if (connect) {
    Serial.println("Connect requested");
    connect = false;
    connectWifi(true);
  }

  unsigned int s = WiFi.status();

  // Retry if not connected and enough time has passed (skip confirmed wrong password)
  if (s != WL_CONNECTED && !wrongPassword && strlen(ssid) > 0 &&
      millis() - lastConnectTry > RETRY_MS) {
    Serial.println("Connection timeout, retrying...");
    connectWifi(false);
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
  snprintf(softAP_ssid, sizeof(softAP_ssid), "%s%06X", softAP_prefix, ESP.getChipId());
  // Modem sleep adds 100-300 ms random latency to every response -> jerky meter
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  Serial.println();
  Serial.print("Configuring access point...");
  /* You can remove the password parameter if you want the AP to be open. */
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(softAP_ssid, softAP_password);
  delay(500); // Without delay I've seen the IP address blank
  Serial.print("AP SSID: ");
  Serial.println(softAP_ssid);
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
  server.on("/data", handleData);
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

  if (isWiFiConnected == 1) samplePower();
  server.handleClient();
  delay(7); // Required to prevent WiFi instability with frequent ADC reads
}
