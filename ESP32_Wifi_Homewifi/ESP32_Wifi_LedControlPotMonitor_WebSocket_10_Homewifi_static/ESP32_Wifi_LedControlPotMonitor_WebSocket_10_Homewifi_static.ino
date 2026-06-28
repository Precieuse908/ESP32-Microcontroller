/*=========================================================
  ESP32 Web Server + WebSocket (REAL-TIME)
  NOW USING HOME WIFI (STA MODE)

  ✔ ESP32 connects to your router
  ✔ Same WebSocket real-time dashboard
ESP32_Wifi_LedControlPotMonitor_WebSocket_10_Homewifi_static IP
after router configuration with ESP32 MAC number and static IP
=========================================================*/

#include <WiFi.h>                // Core WiFi library for ESP32
#include <WebSocketsServer.h>   // WebSocket server library
#include <ESPmDNS.h>   // Enables .local URL

// ---------------- WiFi (HOME ROUTER) ----------------
//const char* ssid = "U+NetA96C"; //"YOUR_WIFI_NAME";             // 🔴 CHANGE: your home WiFi name
//const char* password = "DEF2D66#1H"; //"YOUR_WIFI_PASSWORD";    // 🔴 CHANGE: your WiFi password
// ---------------- WiFi (Robotics Club ROUTER) ----------------
const char* ssid = "roboticsclub_iptime01"; //"YOUR_WIFI_NAME";   // 🔴 CHANGE: your lab WiFi name
const char* password = "robot1234"; //"YOUR_WIFI_PASSWORD";       // 🔴 CHANGE: your WiFi password

WiFiServer server(80);          // HTTP server runs on port 80
WebSocketsServer webSocket = WebSocketsServer(81); // WebSocket on port 81

// ---------------- Pins ----------------
const int ledPin = 2;           // GPIO 2 → LED output (PWM)
const int potPin = 34;          // GPIO 34 → Analog input (potentiometer)

// ---------------- Variables ----------------
int brightnessPercent = 0;      // Stores LED brightness (0–100%)
unsigned long lastSend = 0;     // Timer for periodic sensor sending

// ---------------- HTML (stored in FLASH) ----------------
const char webpage[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<style>
body{font-family:Arial;text-align:center;background:#1a1a2e;color:white;}
.box{background:#222;padding:20px;border-radius:20px;margin:20px auto;width:90%;max-width:420px;}
.value{font-size:42px;color:#4CAF50;}
input[type=range]{width:100%;}
.gauge{position:relative;width:260px;height:260px;border:8px solid #888;border-radius:50%;margin:30px auto;background:#111;overflow:hidden;}
.blueArc{position:absolute;width:220px;height:110px;background:blue;left:20px;top:130px;border-radius:0 0 120px 120px;}
.needle{position:absolute;width:4px;height:95px;background:red;left:128px;top:35px;transform-origin:bottom center;transform:rotate(-90deg);transition:0.1s;}
.centerDot{position:absolute;width:18px;height:18px;background:white;border-radius:50%;top:121px;left:121px;}
.mark{position:absolute;font-weight:bold;}
.m0{left:25px;top:125px;}
.m25{left:55px;top:55px;}
.m50{left:118px;top:25px;}
.m75{left:185px;top:55px;}
.m100{left:205px;top:125px;}
</style>
</head>

<body>
<h1>ESP32 WebSocket Dashboard</h1>

<div class='box'>
<h2>LED Brightness</h2>
<div class='value' id='ledValue'>0%</div>

<input type='range' min='0' max='100' value='0'
oninput='sendLED(this.value)'>
</div>

<div class='box'>
<h2>Potentiometer Gauge</h2>
<div class='value' id='sensorValue'>0%</div>

<div class='gauge'>
<div class='blueArc'></div>

<div class='mark m0'>0</div>
<div class='mark m25'>25</div>
<div class='mark m50'>50</div>
<div class='mark m75'>75</div>
<div class='mark m100'>100</div>

<div class='needle' id='needle'></div>
<div class='centerDot'></div>
</div>
</div>

<script>

// Create WebSocket connection to ESP32
let socket = new WebSocket('ws://' + location.hostname + ':81/');

// When ESP32 sends data
socket.onmessage = function(event){

let data = parseInt(event.data); // Convert received string to number

// Update displayed percentage
document.getElementById('sensorValue').innerHTML = data + '%';

// Convert percentage → angle (-90 to +90)
let angle = -90 + (data * 1.8);

// Rotate needle
document.getElementById('needle').style.transform =
'rotate(' + angle + 'deg)';
};

// Send slider value to ESP32
function sendLED(val){
document.getElementById('ledValue').innerHTML = val + '%';
socket.send(val); // Send value via WebSocket
}

</script>
</body>
</html>
)rawliteral";


//======================================================
// SETUP
//======================================================


void setup(){

Serial.begin(115200);                 // Start serial monitor

pinMode(ledPin, OUTPUT);              // Set LED pin as output
analogWrite(ledPin, 0);               // Initialize LED OFF
analogSetAttenuation(ADC_11db);       // Expand ADC range (0–3.3V)

// ---------------- CONNECT TO WIFI ----------------
// IPAddress local_IP(192,168,0,11); // update depending on your ESP32-mac congfiguration in the router
// IPAddress gateway(192,168,0,1);
// IPAddress subnet(255,255,255,0);
// WiFi.config(local_IP,gateway,subnet);
WiFi.begin(ssid, password);           // Start connecting to router

Serial.print("Connecting to WiFi");

while(WiFi.status() != WL_CONNECTED){ // Wait until connected
  delay(500);                         // Small delay
  Serial.print(".");                  // Show progress
}

Serial.println("\nConnected!");
//ESP32 MAC
Serial.print("ESP32 MAC: ");
Serial.println(WiFi.macAddress());
// Example1: ESP32 MAC: 78:1C:3C:F5:C0:0C
// Print assigned IP (VERY IMPORTANT)


Serial.print("ESP32 IP Address: ");
Serial.println(WiFi.localIP());       // Use this IP in browser

// Example1: ESP32 IP Address: 192.168.219.151
// mDNS stands for Multicast Domain Name System. It is a protocol that allows devices on a local network to find and identify each other by name (like esp32.local) instead of having to remember or type in a numeric IP address (like 192.168.0.15).
// It is often referred to as "Zero Configuration Networking" (ZeroConf) because it works automatically without needing a central DNS server or a manual router setup.

// Start mDNS (this creates esp32.local)
if (MDNS.begin("esp32robot01")) {
  Serial.println("mDNS started → http://esp32robot01.local");
  // mDNS started → http://esp32.local
  //http://robotesp32.51.local
} else {
  Serial.println("Error starting mDNS");}

// ---------------- START SERVERS ----------------
server.begin();                       // Start HTTP server
webSocket.begin();                   // Start WebSocket server

webSocket.onEvent(webSocketEvent);   // Attach event handler
}


//======================================================
// LOOP
//======================================================
void loop(){

handleHTTP();         // Handle browser requests
webSocket.loop();     // Maintain WebSocket communication

// Send sensor data every 50 ms (~20 Hz)
if(millis() - lastSend > 50){

  sendSensorWS();     // Send potentiometer value
  lastSend = millis();

}

}


//======================================================
// HTTP HANDLER (SERVES WEBPAGE)
//======================================================
void handleHTTP(){

WiFiClient client = server.available(); // Check for new client

if(!client) return;                     // If none, exit

String request = client.readStringUntil('\r'); // Read request
client.flush();                         // Clear buffer

// Send HTTP response header
client.println("HTTP/1.1 200 OK");
client.println("Content-type:text/html");
client.println();

// Send webpage content
client.print(webpage);

client.stop();                          // Close connection

}


//======================================================
// WEBSOCKET EVENT HANDLER
//======================================================
void webSocketEvent(uint8_t num,
WStype_t type,
uint8_t * payload,
size_t length){

if(type == WStype_TEXT){                // If text message received

String msg = String((char*)payload);    // Convert payload to string
int val = msg.toInt();                  // Convert to integer

val = constrain(val,0,100);             // Limit between 0–100

brightnessPercent = val;                // Store value

// Convert percentage → PWM duty (0–255)
int duty = map(val,0,100,0,255);

// Apply PWM to LED
analogWrite(ledPin, duty);

}

}


//======================================================
// SEND SENSOR DATA VIA WEBSOCKET
//======================================================
void sendSensorWS(){

int raw = analogRead(potPin);           // Read analog (0–4095)

// Convert to percentage
int percent = map(raw,0,4095,0,100);

String sensorMsg = String(percent);     // Convert to string

webSocket.broadcastTXT(sensorMsg);      // Send to all clients

}