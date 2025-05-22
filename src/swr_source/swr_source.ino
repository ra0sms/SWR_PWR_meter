/*SWR and Power meter with wifi interface by RA0SMS
 * ESP8266 based 
 * https://github.com/ra0sms/SWR_PWR_meter
 * 2019 - First version
 * 
 * 12032021 - Add switching 2000/1000/200W maxpower (use define)
 * 22052025 - Add new CSS based web-interface
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <Ticker.h>


#define MAX_POWER 2000       //1000, 2000, 200 

const char *softAP_ssid = "SWRmeter_sn080";
const char *softAP_password = "1234567890";
const char *myHostname = "esp8266";
 
char ssid[32] = "";
char password[32] = "";
String webPage = "";
String webpage = "";
String ourPage = "";
String Page3="";
String webSite, javaScript, XML, progress, StringSWR;
 
int switch_pin = 5;
int led_pin = 16;
unsigned int forwU = 0;
unsigned int forwU_prev = 0;
unsigned int refrU = 0;
int power = 0;
int power_per = 0;
float swr = 0;
unsigned int sum = 0;
unsigned int raz = 0;
int fl = 0;
int counter = 0;
unsigned int CntLoop = 0;
int tim = 0;
int sec = 0;
int minute = 0;
int hour = 0;
int day = 0;
int flagAP=0;
int flag_off=0;
int analogPin = A0;
int flagOn = 0;

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

int readADC ()
{
  unsigned int value = 0;
  unsigned int val_prev = 0;
  for (int k = 0; k < 20; k++)
  {
    val_prev = analogRead(A0);
    value = value + val_prev;
    delayMicroseconds(50);
  }
  unsigned int out = value / 20;
  return out;
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
    if (MAX_POWER == 1000) 
    {
      power = (forwU*forwU)/1046;   // MAX 1000w
      power_per = (power / 10); // MAX 1000w
    }
    if (MAX_POWER == 2000)
    {
      power = (forwU*forwU)/523;     // MAX 2000w
      power_per = (power / 20); // MAX 2000w
    } 
    if (MAX_POWER == 200)
    {
      power = (forwU*forwU)/5230;     // MAX 200w
      power_per = (power / 2); // MAX 200w
    }     
    sum = forwU + refrU;
    raz = forwU - refrU;
    if (forwU > refrU)
    {
      swr = (float(sum)) / (float(raz));
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
  buildXML();
  server.send(200, "text/xml", XML);
}

void ProgressBar() {
  progress = R"(
<style>
.progress-container {
  width: 100%;
  max-width: 400px;
  margin: 20px auto;
  background-color: #e0e0e0;
  border-radius: 15px;
  overflow: hidden;
  box-shadow: inset 0 1px 3px rgba(0,0,0,0.2);
}

.progress-bar {
  width: 0%;
  height: 30px;
  background: linear-gradient(90deg, #4CAF50, #2E7D32);
  transition: width 0.5s ease;
}
</style>

<div class="progress-container">
  <div id="myBar" class="progress-bar"></div>
</div>
)";
}

void buildXML() {
  XML = "<?xml version='1.0'?>";
  XML += "<response>";
  XML += "<pwr>";
  char pwrStr[20];
  sprintf(pwrStr, "PWR: %-20d", power);  
  XML += pwrStr; 
  char swrStr[20];
  sprintf(swrStr, "SWR: %-6s", StringSWR.c_str()); 
  XML += swrStr;
  XML += "</pwr>";
  XML += "<adcpower>";
  XML += String(power_per, DEC);
  XML += "</adcpower>";
  XML += "</response>";
}

void buildJavascript() {
  javaScript = R"(
<script>
const updateData = () => {
  fetch('/xml', { method: 'PUT' })
    .then(response => response.text())
    .then(str => (new window.DOMParser()).parseFromString(str, "text/xml"))
    .then(xml => {
      const pwr = xml.querySelector("pwr").textContent;
      const power = xml.querySelector("adcpower").textContent;
      
      document.getElementById('runtime').innerHTML = '<pre>' + pwr + '</pre>';
      document.getElementById('myBar').style.width = power + '%';
    })
    .catch(err => console.error('Error:', err));
  
  setTimeout(updateData, 100);
};

document.addEventListener('DOMContentLoaded', updateData);
</script>
)";
}

void handleSWR() {
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
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background-color: #f0f5ff;
      margin: 0;
      padding: 20px;
      text-align: center;
      color: #333;
    }
    
    .container {
      max-width: 600px;
      margin: 0 auto;
      background: white;
      border-radius: 10px;
      padding: 20px;
      box-shadow: 0 4px 12px rgba(0,0,0,0.1);
    }
    
    h3 {
      color: #2c3e50;
    }
    
    .status-display {
      font-family: 'Lucida Console', monospace;
      font-size: 1.4em; 
      font-weight: bold; 
      margin: 0;
      padding: 0 0.3em;
      background: #f8f9fa;
      border-radius: 3px;
      white-space: pre;
      line-height: 2em; 
      height: 2em; 
      text-shadow: 0.5px 0.5px 1px rgba(0,0,0,0.2); 
      display: inline-flex;   
      align-items: center;    
      box-sizing: border-box;    
      vertical-align: middle; 
      overflow: visible;     
    }
    
    .version-badge {
      display: inline-block;
      padding: 5px 10px;
      background: #3498db;
      color: white;
      border-radius: 20px;
      font-size: 0.9em;
      margin-top: 10px;
    }
    
    .error-message {
      color: #e74c3c;
      padding: 20px;
      background: #fdecea;
      border-radius: 5px;
      margin: 20px 0;
    }
    .footer {
      text-align: center;
      margin-top: 20px;
    }
    
    .footer a {
      color: var(--primary);
      text-decoration: none;
    }
    
    .footer a:hover {
      text-decoration: underline;
    }
  </style>
</head>
<body>
  <div class="container">
)";

  if (flagOn == 1) {
    webPage += R"(
    <h3>SWR/PWR Meter</h3>
    <div class="status-display">
      <span id="runtime"></span>
    </div>
)";
    webPage += progress;
  } else {
    webPage += R"(
    <div class="error-message">
      <h2>Connection Required</h2>
      <p><a href='/wifi'>Please connect to local Wi-Fi network</a></p>
    </div>
)";
  }
  webPage += "<div class='version-badge'>";
  if (MAX_POWER == 1000) webPage += "Maximum Power: 1000W";
  else if (MAX_POWER == 200) webPage += "Maximum Power: 200W";
  else if (MAX_POWER == 2000) webPage += "Maximum Power: 2000W";
  webPage += "</div>";

  webPage += R"(
  </div>
    <div class="footer">
      <a href="/">← Return to Home Page</a>
    </div>
)";

  webPage += javaScript;
  webPage += R"(
</body>
</html>
)";

  server.send(200, "text/html", webPage);
}

void handleRoot() {
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
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      background-color: #f0f5ff;
      margin: 0;
      padding: 20px;
      color: #333;
      line-height: 1.6;
    }
    
    .container {
      max-width: 800px;
      margin: 0 auto;
      background: white;
      border-radius: 10px;
      padding: 30px;
      box-shadow: 0 4px 12px rgba(0,0,0,0.1);
    }
    
    h1 {
      color: #2c3e50;
      text-align: center;
      margin-bottom: 30px;
    }
    
    .connection-info {
      background: #f8f9fa;
      padding: 15px;
      border-radius: 5px;
      margin-bottom: 20px;
    }
    
    .nav-links {
      display: flex;
      justify-content: center;
      gap: 15px;
      margin: 30px 0;
    }
    
    .nav-links a {
      display: inline-block;
      padding: 10px 20px;
      background: #3498db;
      color: white;
      text-decoration: none;
      border-radius: 5px;
      transition: background 0.3s;
    }
    
    .nav-links a:hover {
      background: #2980b9;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>WiFi SWR-PWR Meter by RA0SMS</h1>
    
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
      <a href='/wifi'>WiFi Configuration</a>
      <a href='/swr'>SWR/PWR Meter</a>
    </div>
    
)";
  Page += R"(
    </div>
  </div>
</body>
</html>
)";

  server.send(200, "text/html", Page);
}

void handleWifi() {
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
    :root {
      --primary: #3498db;
      --secondary: #2ecc71;
      --danger: #e74c3c;
      --dark: #2c3e50;
      --light: #ecf0f1;
    }
    
    body {
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      line-height: 1.6;
      color: var(--dark);
      max-width: 800px;
      margin: 0 auto;
      padding: 20px;
      background-color: #f5f7fa;
    }
    
    h1 {
      color: var(--primary);
      text-align: center;
      margin-bottom: 20px;
    }
    
    .status-card {
      background: white;
      border-radius: 8px;
      padding: 20px;
      margin-bottom: 20px;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
    }
    
    .btn {
      border: none;
      color: white;
      padding: 8px 16px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 14px;
      font-weight: 500;
      border-radius: 6px;
      cursor: pointer;
      transition: all 0.3s ease;
      min-width: 60px;
    }
    
    .btn-on {
      background-color: var(--secondary);
    }
    
    .btn-off {
      background-color: var(--danger);
    }
    
    .btn:hover {
      opacity: 0.9;
      transform: translateY(-1px);
    }
    
    table {
      width: 100%;
      border-collapse: collapse;
      margin: 15px 0;
      background: white;
      border-radius: 8px;
      overflow: hidden;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
    }
    
    th, td {
      padding: 12px 15px;
      text-align: left;
      border-bottom: 1px solid #ddd;
    }
    
    th {
      background-color: var(--primary);
      color: white;
    }
    
    tr:hover {
      background-color: #f5f5f5;
    }
    
    form {
      background: white;
      padding: 20px;
      border-radius: 8px;
      box-shadow: 0 2px 10px rgba(0,0,0,0.1);
      margin: 20px 0;
    }
    
    input[type="text"],
    input[type="password"] {
      width: 100%;
      padding: 10px;
      margin: 8px 0;
      display: inline-block;
      border: 1px solid #ccc;
      border-radius: 4px;
      box-sizing: border-box;
    }
    
    input[type="submit"] {
      width: 100%;
      background-color: var(--primary);
      color: white;
      padding: 12px 20px;
      margin: 8px 0;
      border: none;
      border-radius: 4px;
      cursor: pointer;
      font-size: 16px;
    }
    
    input[type="submit"]:hover {
      background-color: #2980b9;
    }
    
    .ap-status {
      display: inline-block;
      padding: 4px 8px;
      border-radius: 4px;
      font-weight: bold;
    }
    
    .ap-on {
      background-color: var(--secondary);
      color: white;
    }
    
    .ap-off {
      background-color: var(--danger);
      color: white;
    }
    
    .footer {
      text-align: center;
      margin-top: 20px;
    }
    
    .footer a {
      color: var(--primary);
      text-decoration: none;
    }
    
    .footer a:hover {
      text-decoration: underline;
    }
  </style>
</head>
<body>
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
  Page += F(R"(</span>
    </p>
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
  Serial.println("wifi save");
  server.arg("n").toCharArray(ssid, sizeof(ssid) - 1);
  server.arg("p").toCharArray(password, sizeof(password) - 1);
  server.sendHeader("Location", "wifi", true);
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.send(302, "text/plain", "");    // Empty content inhibits Content-length header so we have to close the socket ourselves.
  server.client().stop(); // Stop is needed because we sent no content length
  saveCredentials();
  connect = strlen(ssid) > 0; // Request WLAN connect with new credentials if there is a SSID
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
  Serial.println(strlen(password) > 0 ? "********" : "<no password>");
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


void connectWifi() {
  Serial.println("Connecting as wifi client...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int connRes = WiFi.waitForConnectResult();
  Serial.print("connRes: ");
  Serial.println(connRes);
}

void routineWIFI()
{
  if (connect) {
    Serial.println("Connect requested");
    connect = false;
    connectWifi();
    lastConnectTry = millis();
  }
  {
    unsigned int s = WiFi.status();
    if (s == 0 && millis() > (lastConnectTry + 60000)) {
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
        flagOn = 1;
        flagAP=1;
        WiFi.softAPdisconnect();
        delay(100);
      } else if (s == WL_NO_SSID_AVAIL) {
        digitalWrite(led_pin, HIGH);
        WiFi.disconnect();
        if (flagAP==1){
        flagAP=0;
        flagOn = 0;
        WiFi.softAPConfig(apIP, apIP, netMsk);
        WiFi.softAP(softAP_ssid, softAP_password);
        delay(500); // Without delay I've seen the IP address blank
        Serial.print("AP IP address: ");
        Serial.println(WiFi.softAPIP());
        }
      }
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
  server.onNotFound(handleNotFound);
  server.begin(); // Web server start
  Serial.println("HTTP server started");
  loadCredentials(); // Load WLAN credentials from network
  connect = strlen(ssid) > 0; // Request WLAN connect if there is a SSID
  dnsServer.processNextRequest();
  server.handleClient();
  
  server.begin();
  Serial.println("HTTP server started");
}
 
void loop(void)
{
  routineWIFI();
  if (flagAP==1) 
  {
    tim++; 
    if (tim==67) {sec++; tim=0;}
    if (sec==60) {minute++; sec=0;}
    if (minute==60) {hour++; minute=0;}
    if (hour==24) {day++; hour=0;}
  }
  if ((flagAP==0)&&(tim>0)) tim=sec=minute=hour=day=0;
  if (flagOn == 1) GetPower();
  server.handleClient();
  delay(7);
}
