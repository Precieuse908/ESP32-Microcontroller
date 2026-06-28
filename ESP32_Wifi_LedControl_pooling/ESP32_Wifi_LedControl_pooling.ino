/* The program:
 * ESP32 Web Server - PWM Brightness Control with Slider
 * Robotics Club Workshop 2
 * 
 * Creates a WiFi Access Point. Connect your phone to "ESP32_Robot"
 * Open browser to 192.168.4.1 and use the slider to dim/brighten the LED.
 */

#include <WiFi.h>   // Library to use WiFi features of ESP32

// Use Access Point mode (ESP32 creates its own WiFi network)
const char* ap_ssid = "ESP32_Robot0";     // Name of WiFi network created by ESP32
const char* ap_password = "12345678";    // Password (must be ≥ 8 chars for WPA2 security)

WiFiServer server(80);  
// Create a web server on port 80 (default HTTP port → browsers automatically use it)

const int ledPin = 2;  
// GPIO 2 is commonly connected to onboard LED on ESP32 dev boards

int brightnessPercent = 0;  
// Store brightness as percentage (user-friendly: 0–100 instead of 0–255)

void setup() {
  Serial.begin(115200);  
  // Start serial communication for debugging

  pinMode(ledPin, OUTPUT);  
  // Set LED pin as output

  analogWrite(ledPin, 0);  
  // Start with LED OFF (PWM duty = 0)

  // Start Access Point
  Serial.print("Setting up Access Point: ");
  Serial.println(ap_ssid);

  WiFi.softAP(ap_ssid, ap_password);  
  // ESP32 becomes a WiFi router (Access Point mode)

  IPAddress IP = WiFi.softAPIP();  
  // Get IP address of ESP32 in AP mode

  Serial.print("AP IP address: ");
  Serial.println(IP);  
  // Usually prints: 192.168.4.1
  // WHY 192.168.4.1?
  // - ESP32 AP mode uses default subnet: 192.168.4.x
  // - ESP32 itself acts as gateway → assigned .1
  // - So all connected devices access it via 192.168.4.1

  Serial.println("Connect your phone to WiFi: ESP32_Robot (password: 12345678)");
  Serial.println("Then open browser to: http://192.168.4.1");

  server.begin();  
  // Start listening for incoming HTTP requests
}

void loop() {
  WiFiClient client = server.available();  
  // Check if a client (browser) is connecting

  if (client) {
    Serial.println("New Client.");

    String header = "";      
    // Stores full HTTP request (e.g., "GET /set?value=50 HTTP/1.1")

    String currentLine = ""; 
    // Stores one line at a time while reading request

    while (client.connected()) {
      if (client.available()) {
        char c = client.read();  
        // Read incoming data byte-by-byte

        header += c;  
        // Append character to full request string

        if (c == '\n') {  
          // End of one line

          if (currentLine.length() == 0) {
            // Empty line → indicates END of HTTP request headers
            // Now we process the request

            // Check if user accessed URL like: /set?value=XX
            if (header.indexOf("GET /set?value=") >= 0) {

              // Example request:
              // GET /set?value=75 HTTP/1.1 

              int valueStart = header.indexOf("value=") + 6;
              // WHY +6?
              // "value=" has 6 characters
              // indexOf gives position of 'v'
              // +6 moves pointer to the FIRST digit after '='
              // Example:
              // "value=75"
              //        ^
              //        start reading here

              int valueEnd = header.indexOf(" ", valueStart);
              // Find first space AFTER the number
              // In HTTP: "GET /set?value=75 HTTP/1.1"
              //                            ^
              // Space marks END of the number

              String valueStr = header.substring(valueStart, valueEnd);
              // Extract substring → "75"

              brightnessPercent = valueStr.toInt();
              // Convert string to integer

              brightnessPercent = constrain(brightnessPercent, 0, 100);
              // Ensure safe range (avoid invalid values like 200 or -10)

              int duty = map(brightnessPercent, 0, 100, 0, 255);
              // Convert % → PWM scale
              // WHY?
              // - User thinks in %
              // - PWM needs 0–255

              analogWrite(ledPin, duty);  
              // Apply PWM signal to LED

              Serial.print("Brightness set to: ");
              Serial.print(brightnessPercent);
              Serial.print("% (PWM: ");
              Serial.print(duty);
              Serial.println(")");
            }

            // ---- Send HTTP Response ----
            client.println("HTTP/1.1 200 OK");  
            // Tell browser request was successful

            client.println("Content-type:text/html");  
            // Tell browser we are sending HTML page

            client.println("Connection: close");  
            // Close connection after response

            client.println();  
            // Blank line required before HTML body

            // ---- Build the Webpage ----
            client.println("<!DOCTYPE html><html>");
            client.println("<head>");

            client.println("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
            // Makes page responsive on mobile devices

            client.println("<title>ESP32 LED Dimmer</title>");

            client.println("<style>");
            // CSS styling for better UI
            client.println("body { font-family: Arial; text-align: center; margin-top: 50px; background-color: #1a1a2e; color: #eee; }");
            client.println(".slider-container { width: 80%; max-width: 400px; margin: 30px auto; }");
            client.println("input[type=range] { width: 100%; height: 20px; }");
            client.println(".value-display { font-size: 48px; font-weight: bold; margin: 20px; color: #4CAF50; }");
            client.println("</style>");

            client.println("</head>");
            client.println("<body>");

            client.println("<h1>ESP32 LED Dimmer</h1>");

            client.print("<div class=\"value-display\">");
            client.print(brightnessPercent);  
            // Show current brightness value
            client.println("%</div>");

            client.println("<div class=\"slider-container\">");

            client.print("<input type=\"range\" min=\"0\" max=\"100\" value=\"");
            client.print(brightnessPercent);
            client.println("\" class=\"slider\" id=\"brightnessSlider\" oninput=\"updateValue(this.value)\">");
            // Slider sends value continuously when moved

            client.println("</div>");

            // JavaScript
            client.println("<script>");
            client.println("function updateValue(val) {");
            client.println("  document.querySelector('.value-display').innerHTML = val + '%';");
            // Update number on screen instantly

            client.println("  fetch('/set?value=' + val);");
            // Send request to ESP32 WITHOUT reloading page
            // Example: /set?value=60

            client.println("}");
            client.println("</script>");

            client.println("</body></html>");
            client.println();

            break;  
            // Exit loop after sending response
          } else {
            currentLine = "";  
            // Reset line buffer
          }
        } else if (c != '\r') {
          currentLine += c;  
          // Build current line (ignore carriage return)
        }
      }
    }

    client.stop();  
    // Disconnect client

    Serial.println("Client disconnected.\n");
  }
}
