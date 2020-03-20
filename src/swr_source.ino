/*SWR and Power meter with wifi interface
 * ESP8266 based
 * RA0SMS 2019
 * latest version Aug 2019
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <Ticker.h>


#define MAX_POWER 1000 

const char *softAP_ssid = "SWRmeter_sn018";
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

void GetPower()
{
  digitalWrite(switch_pin, LOW);
  delayMicroseconds(1);
  forwU = readADC();
  digitalWrite(switch_pin, HIGH);
  delayMicroseconds(1);
  refrU = readADC();
  digitalWrite(switch_pin, LOW);
  if ((forwU > 70) || (forwU_prev > 70))
  {
    forwU_prev = forwU;
    if (forwU < 70)
    {
      forwU_prev = 0;
    }
    //power = ((((forwU * 3000)/1023)/10)*(((forwU * 3000)/1023)/10))/50;
    power = (forwU*forwU)/1046;
    power_per = (power / 10);
    sum = forwU + refrU;
    raz = forwU - refrU;
    if ((raz > 0) || (forwU > refrU))
    {
      swr = (float(sum)) / (float(raz));
      StringSWR = floatToString(swr, 2);
      delay(4);
    } else {
      StringSWR = "TOO HIGH";
      delay(1);
    }
  } else
  {
    power = 0;
    StringSWR = "NO DATA";
  }
}


void ProgressBar () {
  progress = "<style>\n";
  progress += "#myProgress {\n";
  progress += "width: 400px;\n";
  progress += "background-color: #ddd;\n";
  progress += "}\n";
  progress += "#myBar {\n";
  progress += "width:0%;\n";
  progress += "height: 30px;\n";
  progress += "background-color: #4CAF50;\n";
  progress += "text-align: center;\n";
  progress += "line-height: 30px;\n";
  progress += "color: white;\n";
  progress += "}\n";
  progress += "</style>\n";
  progress += "<div id=\"myProgress\">\n";
  progress += "<div id=\"myBar\"></div>\n";
  progress += "</div>\n";
}



void handleXML() {
  buildXML();
  server.send(200, "text/xml", XML);
}

void buildJavascript() {
  javaScript = "<SCRIPT>\n";
  javaScript += "var xmlHttp=createXmlHttpObject();\n";

  javaScript += "function createXmlHttpObject(){\n";
  javaScript += " if(window.XMLHttpRequest){\n";
  javaScript += "    xmlHttp=new XMLHttpRequest();\n";
  javaScript += " }else{\n";
  javaScript += "    xmlHttp=new ActiveXObject('Microsoft.XMLHTTP');\n";
  javaScript += " }\n";
  javaScript += " return xmlHttp;\n";
  javaScript += "}\n";

  javaScript += "function process(){\n";
  javaScript += " if(xmlHttp.readyState==0 || xmlHttp.readyState==4){\n";
  javaScript += "   xmlHttp.open('PUT','xml',true);\n";
  javaScript += "   xmlHttp.onreadystatechange=handleServerResponse;\n";
  javaScript += "   xmlHttp.send(null);\n";
  javaScript += " }\n";
  javaScript += " setTimeout('process()',10);\n";
  javaScript += "}\n";

  javaScript += "function handleServerResponse(){\n";
  javaScript += " if(xmlHttp.readyState==4 && xmlHttp.status==200){\n";
  javaScript += "   xmlResponse=xmlHttp.responseXML;\n";
  javaScript += "   xmldoc = xmlResponse.getElementsByTagName('response');\n";
  javaScript += "   message = xmldoc[0].firstChild.nodeValue;\n";
  javaScript += "   document.getElementById('runtime').innerHTML=message;\n";
  javaScript += "   var elem = document.getElementById('myBar');\n";
  javaScript += "   xmlResponse=xmlHttp.responseXML;\n";
  javaScript += "   xmldoc = xmlResponse.getElementsByTagName('adcpower');\n";
  javaScript += "   message = xmldoc[0].firstChild.nodeValue;\n";
  javaScript += "   elem.style.width=message;\n";
  javaScript += " }\n";
  javaScript += "}\n";

  javaScript += "</SCRIPT>\n";
}

void buildXML() {
  XML = "<?xml version='1.0'?>";
  XML += "<response>";
  XML += "Output power: " + String(power, DEC) + "&#160;&#160;&#160;&#160;&#160;&#160;&#160;&#160;&#160;&#160;" + " SWR: " + StringSWR;
  XML += "<adcpower>";
  XML += String(power_per, DEC) + "%";
  XML += "</adcpower>\n";
  XML += "</response>";
}



void handleSWR ()
{
  buildJavascript();
  ProgressBar();
  webPage = "<!DOCTYPE HTML><title>SWR/PWR Meter</title>\n";
  webPage += javaScript;
  webPage += "<BODY onload='process()'>\n";
  webPage += "<BR><h2>WI-FI SWR/PWR Meter by RA0SMS</h2><BR>\n";
  webPage += progress;
  webPage += "<h3><A id='runtime'></A></h3>\n";
  webPage += "</BODY>\n";
  webPage += "</HTML>\n"; 
  server.send(200, "text/html", webPage);
  }

void handleRoot() {
  String Page;
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  /*server.send(200, "text/html", ""); */
  Page =F(
    "<!DOCTYPE HTML> <html><head></head><body>"
    "<html><head></head><body>"
    "<h1>Start page WiFi SWR-PWR meter by RA0SMS</h1>"
  );
  if (server.client().localIP() == apIP) {
    Page +=(String("<p>You are connected through the soft AP: ") + softAP_ssid + "</p>");
  } else {
    Page +=(String("<p>You are connected through the wifi network: ") + ssid + "</p>");
  }
  Page += F(
    "<p><a href='/wifi'>Config the wifi connection</a>.</p>"
    "<p><a href='/swr'>SWR/PWR meter page</a>.</p>"
    "</body></html>"
  );
  server.send(200, "text/html", Page);
}

void handleWifi() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");
  /*server.setContentLength(CONTENT_LENGTH_UNKNOWN);*/
   

  Page3 = F(
    "<html><head></head><body>"
    "<h1>Wifi config</h1>"
  );
  if (server.client().localIP() == apIP) {
    Page3 +=(String("<p>You are connected through the soft AP: ") + softAP_ssid + "</p>");
  } else {
    Page3 +=(String("<p>You are connected through the wifi network: ") + ssid + "</p>");
    
  }
  Page3 +=("<p>Uptime - " + String(day) + " day " + String(hour) + " h " + String(minute) + " m " + String(sec) + " s ""</p>");
  
  Page3 += F(
    "\r\n<br />"
    "<table><tr><th align='left'>SoftAP config</th></tr>"
  );
  Page3 +=(String() + "<tr><td>SSID " + String(softAP_ssid) + "</td></tr>");
  Page3 +=(String() + "<tr><td>IP " + WiFi.softAPIP().toString() + "</td></tr>");
 
  Page3 += F(
    "</table>"
    "\r\n<br />"
    "<table><tr><th align='left'>WLAN config</th></tr>"
  );
  Page3 +=(String() + "<tr><td>SSID " + String(ssid) + "</td></tr>");
  Page3 +=(String() + "<tr><td>IP " + WiFi.localIP().toString() + "</td></tr>");
  Page3 += F(
    "</table>"
    "\r\n<br />"
    "<table><tr><th align='left'>WLAN list (refresh if any missing)</th></tr>"
  );
  Serial.println("scan start");
  int n = WiFi.scanNetworks();
  Serial.println("scan done");
  if (n > 0) {
    for (int i = 0; i < n; i++) {
      Page3 +=(String() + "\r\n<tr><td>SSID " + WiFi.SSID(i) + String((WiFi.encryptionType(i) == ENC_TYPE_NONE) ? " " : " *") + " (" + WiFi.RSSI(i) + ")</td></tr>");
    }
  } else {
    Page3 +=(String() + "<tr><td>No WLAN found</td></tr>");
  }
  Page3 += F(
    "</table>"
    "\r\n<br /><form method='POST' action='wifisave'><h4>Connect to network:</h4>"
    "<input type='text' placeholder='network' name='n'/>"
    "<br /><input type='password' placeholder='password' name='p'/>"
    "<br /><input type='submit' value='Connect/Disconnect'/></form>"
    "<p><a href='/'>Return to the home page</a>.</p>"
    "</body></html>"
  );
  /*server.client().stop(); */
  server.send(200, "text/html", Page3);
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

void setup(void){
  delay(300);
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

void connectWifi() {
  Serial.println("Connecting as wifi client...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
  int connRes = WiFi.waitForConnectResult();
  Serial.print("connRes: ");
  Serial.println(connRes);
}


 
void loop(void){
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
