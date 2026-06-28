/*
  SWARM ROBOT — ESP32 MQTT Receiver
  ----------------------------------
  Connects to WiFi + MQTT broker.
  Subscribes to its own command topic AND robot/all/cmd.
  Forwards commands to Arduino Uno via Serial.

  Libraries needed (install via Arduino Library Manager):
    - PubSubClient  by Nick O'Leary
    - ArduinoJson   by Benoit Blanchon (optional, for status messages)
*/

#include <WiFi.h>
#include <PubSubClient.h>

// ── CONFIG ─────────────────────────────────────────────
const char* WIFI_SSID   = "roboticsclub_iptime01";
const char* WIFI_PASS   = "robot1234";

const char* BROKER_IP   = "192.168.0.28";  // your laptop/Pi IP
const int   BROKER_PORT = 1883;

const int   ROBOT_ID    = 3;               // change per robot: 1, 2, 3...
// ────────────────────────────────────────────────────────

// Topics built from ROBOT_ID
char TOPIC_CMD[30];      // robot/1/cmd
char TOPIC_ALL[20];      // robot/all/cmd
char TOPIC_STATUS[30];   // robot/1/status
char CLIENT_ID[20];      // robot_1

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ── CALLED WHEN A MESSAGE ARRIVES ──────────────────────
void onMessage(char* topic, byte* payload, unsigned int length) {
  // Convert payload bytes to string
  char cmd[64] = {0};
  for (int i = 0; i < length && i < 63; i++) cmd[i] = (char)payload[i];

  Serial.print("[MQTT] ");
  Serial.print(topic);
  Serial.print(" → ");
  Serial.println(cmd);

  // Forward command to Arduino Uno via Serial
  // Arduino Uno is connected: ESP32 TX2(17) → Arduino RX
  //                           ESP32 RX2(16) → Arduino TX
  Serial2.println(cmd);

  // Handle locally too (optional LEDs, logging, etc.)
  handleCommand(cmd);
}

void handleCommand(const char* cmd) {
  if      (strcmp(cmd, "FORWARD")  == 0) { /* nothing extra needed */ }
  else if (strcmp(cmd, "BACKWARD") == 0) { }
  else if (strcmp(cmd, "LEFT")     == 0) { }
  else if (strcmp(cmd, "RIGHT")    == 0) { }
  else if (strcmp(cmd, "STOP")     == 0) { }
  else {
    Serial.print("[WARN] Unknown command: ");
    Serial.println(cmd);
  }
}

// ── MQTT RECONNECT LOOP ─────────────────────────────────
void reconnect() {
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker... ");
    if (mqtt.connect(CLIENT_ID)) {
      Serial.println("connected.");
      mqtt.subscribe(TOPIC_CMD);   // robot/1/cmd
      mqtt.subscribe(TOPIC_ALL);   // robot/all/cmd
      mqtt.publish(TOPIC_STATUS, "ONLINE");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" — retrying in 3s");
      delay(3000);
    }
  }
}

// ── SETUP ───────────────────────────────────────────────
void setup() {
  Serial.begin(115200);    // USB debug
  Serial2.begin(9600, SERIAL_8N1, 16, 17);  // to Arduino Uno

  // Build topic strings
  snprintf(TOPIC_CMD,    sizeof(TOPIC_CMD),    "robot/%d/cmd",    ROBOT_ID);
  snprintf(TOPIC_ALL,    sizeof(TOPIC_ALL),    "robot/all/cmd");
  snprintf(TOPIC_STATUS, sizeof(TOPIC_STATUS), "robot/%d/status", ROBOT_ID);
  snprintf(CLIENT_ID,    sizeof(CLIENT_ID),    "robot_%d",        ROBOT_ID);

  // WiFi
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nWiFi connected. IP: %s\n", WiFi.localIP().toString().c_str());

  // MQTT
  mqtt.setServer(BROKER_IP, BROKER_PORT);
  mqtt.setCallback(onMessage);
}

// ── LOOP ────────────────────────────────────────────────
void loop() {
  if (!mqtt.connected()) reconnect();
  mqtt.loop();  // keeps connection alive and fires onMessage callbacks

  // Optional: publish status every 5 seconds
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 5000) {
    mqtt.publish(TOPIC_STATUS, "ONLINE");
    lastStatus = millis();
  }
}
