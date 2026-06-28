/*
  SWARM ROBOT — ESP32-CAM
  ------------------------
  Streams MJPEG video over HTTP.
  Publishes stream URL to MQTT so dashboard can find it.
  Publishes detection results when target found.

  Board: AI Thinker ESP32-CAM
  In Arduino IDE: Tools → Board → ESP32 Wrover Module
                  Tools → Partition Scheme → Huge APP
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// ── CONFIG ───────────────────────────────────────────────
const char* WIFI_SSID  = "roboticsclub_iptime01";
const char* WIFI_PASS  = "robot1234";
const char* BROKER_IP  = "192.168.0.10";
const int   BROKER_PORT = 1883;
const int   ROBOT_ID   = 3;   // match the ESP32 on same robot
// ─────────────────────────────────────────────────────────

// ── CAMERA PINS (AI-Thinker) ─────────────────────────────
#define PWDN_GPIO  32
#define RESET_GPIO -1
#define XCLK_GPIO   0
#define SIOD_GPIO  26
#define SIOC_GPIO  27
#define Y9_GPIO    35
#define Y8_GPIO    34
#define Y7_GPIO    39
#define Y6_GPIO    36
#define Y5_GPIO    21
#define Y4_GPIO    19
#define Y3_GPIO    18
#define Y2_GPIO     5
#define VSYNC_GPIO 25
#define HREF_GPIO  23
#define PCLK_GPIO  22

// ── TOPICS ───────────────────────────────────────────────
char TOPIC_CAMERA[30];     // robot/1/camera    — stream URL
//char TOPIC_DETECTION[30];  // robot/1/detection — target found
char TOPIC_STATUS[30];     // robot/1/cam_status
char CLIENT_ID[20];        // cam_1

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
httpd_handle_t streamServer = NULL;

// ─────────────────────────────────────────────────────────

void initCamera() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO;   cfg.pin_d1 = Y3_GPIO;
  cfg.pin_d2 = Y4_GPIO;   cfg.pin_d3 = Y5_GPIO;
  cfg.pin_d4 = Y6_GPIO;   cfg.pin_d5 = Y7_GPIO;
  cfg.pin_d6 = Y8_GPIO;   cfg.pin_d7 = Y9_GPIO;
  cfg.pin_xclk     = XCLK_GPIO;
  cfg.pin_pclk     = PCLK_GPIO;
  cfg.pin_vsync    = VSYNC_GPIO;
  cfg.pin_href     = HREF_GPIO;
  cfg.pin_sscb_sda = SIOD_GPIO;
  cfg.pin_sscb_scl = SIOC_GPIO;
  cfg.pin_pwdn     = PWDN_GPIO;
  cfg.pin_reset    = RESET_GPIO;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = FRAMESIZE_VGA;   // 640x480
  cfg.jpeg_quality = 12;
  cfg.fb_count     = 2;

  if (esp_camera_init(&cfg) != ESP_OK) {
    Serial.println("Camera init failed.");
    while (true);
  }
  Serial.println("Camera ready.");
}

// ── MJPEG STREAM HANDLER ─────────────────────────────────
esp_err_t streamHandler(httpd_req_t* req) {
  esp_err_t res = httpd_resp_set_type(req,
    "multipart/x-mixed-replace; boundary=frame");
  if (res != ESP_OK) return res;

  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
      vTaskDelay(pdMS_TO_TICKS(10));  // brief pause on camera failure
      continue;
    }

    char header[80];
    int hlen = snprintf(header, sizeof(header),
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      fb->len);

    res = httpd_resp_send_chunk(req, header, hlen);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, "\r\n", 2);

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;  // client disconnected — exit cleanly

    vTaskDelay(pdMS_TO_TICKS(40));  // ~25fps, gives CPU room to breathe
  }

  return res;
}

void startStreamServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port    = 80;
  cfg.stack_size     = 8192;  
  cfg.max_open_sockets = 3;   // limit concurrent sockets; you only need 1 stream client

  if (httpd_start(&streamServer, &cfg) == ESP_OK) {
    httpd_uri_t uri = { "/stream", HTTP_GET, streamHandler, nullptr };
    httpd_register_uri_handler(streamServer, &uri);
    Serial.println("Stream server started.");
  }
}

// ── MQTT ─────────────────────────────────────────────────
void reconnect() {
  while (!mqtt.connected()) {
    Serial.print("CAM connecting to MQTT... ");
    if (mqtt.connect(CLIENT_ID)) {
      Serial.println("connected.");
      mqtt.publish(TOPIC_STATUS, "ONLINE");

      // Publish stream URL so dashboard knows where to find video
      char url[50];
      snprintf(url, sizeof(url), "http://%s/stream",
               WiFi.localIP().toString().c_str());
      mqtt.publish(TOPIC_CAMERA, url, true);  // true = retained message
      Serial.println(url);

    } else {
      Serial.print("failed rc=");
      Serial.print(mqtt.state());
      Serial.println(" retrying in 3s");
      delay(3000);
    }
  }
}

// ── SETUP ────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  snprintf(TOPIC_CAMERA,    sizeof(TOPIC_CAMERA),    "robot/%d/camera",    ROBOT_ID);
  //snprintf(TOPIC_DETECTION, sizeof(TOPIC_DETECTION), "robot/%d/detection", ROBOT_ID);
  snprintf(TOPIC_STATUS,    sizeof(TOPIC_STATUS),    "robot/%d/cam_status",ROBOT_ID);
  snprintf(CLIENT_ID,       sizeof(CLIENT_ID),       "cam_%d",             ROBOT_ID);

  initCamera();

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500); Serial.print(".");
  }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  startStreamServer();

  mqtt.setServer(BROKER_IP, BROKER_PORT);
}

// ── LOOP ─────────────────────────────────────────────────
void loop() {
  if (!mqtt.connected()) reconnect();
  mqtt.loop();

  // Heartbeat every 5 seconds
  static unsigned long last = 0;
  if (millis() - last > 5000) {
    mqtt.publish(TOPIC_STATUS, "ONLINE");
    last = millis();
  }
}