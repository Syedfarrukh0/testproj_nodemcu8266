#include <ESP8266WiFi.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>

const char* ssid = "Stom fiber Adam jee";
const char* password = "freefree";
const char* serverUrl = "http://192.168.1.7:3000/wifi-data";

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n📡 ESP8266 WiFi Scanner + Node.js Sender");
  Serial.println("======================================");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(1000);

  Serial.println("\nScanning...");
  int n = WiFi.scanNetworks();

  String foundSSID = "";
  String targetSSID = String(ssid);
  targetSSID.trim();  // Trim the target SSID once

  for (int i = 0; i < n; i++) {
    String scannedSSID = WiFi.SSID(i);

    // Remove trailing spaces for comparison
    scannedSSID.trim();

    Serial.print(i + 1);
    Serial.print(": '");
    Serial.print(scannedSSID);
    Serial.print("' | Ch: ");
    Serial.print(WiFi.channel(i));
    Serial.print(" | RSSI: ");
    Serial.println(WiFi.RSSI(i));

    // Compare with desired SSID
    if (scannedSSID.equals(targetSSID)) {
      foundSSID = WiFi.SSID(i);  // Store the actual SSID with original formatting
      Serial.println("✓ Found matching SSID!");
    }
  }

  if (foundSSID == "") {
    Serial.println("\n❌ SSID not found in scan!");
    return;
  }

  Serial.print("\nConnecting to: '");
  Serial.print(foundSSID);
  Serial.println("'");

  // Use the actual SSID found in scan (with spaces if any)
  WiFi.begin(foundSSID.c_str(), password);

  Serial.print("Connecting");
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    counter++;
    if (counter > 40) {
      Serial.println("\n❌ Failed! Status:");
      Serial.println(WiFi.status());

      // More detailed error information
      Serial.print("Error code meaning: ");
      switch (WiFi.status()) {
        case WL_IDLE_STATUS: Serial.println("Idle"); break;
        case WL_NO_SSID_AVAIL: Serial.println("SSID not found"); break;
        case WL_SCAN_COMPLETED: Serial.println("Scan completed"); break;
        case WL_CONNECT_FAILED: Serial.println("Connection failed"); break;
        case WL_CONNECTION_LOST: Serial.println("Connection lost"); break;
        case WL_DISCONNECTED: Serial.println("Disconnected"); break;
        default: Serial.println("Unknown"); break;
      }
      return;
    }
  }

  Serial.println("\n✅ Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  pinMode(D4, OUTPUT);
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(D4, HIGH);
    delay(1000);
    digitalWrite(D4, LOW);
    delay(1000);
  }

  // WiFi scan karo
  Serial.println("🔄 Scanning WiFi networks...");
  int n = WiFi.scanNetworks(false, true); // async scan
  
  if (n == 0) {
    Serial.println("❌ No networks found!");
  } else {
    Serial.print("📊 Found ");
    Serial.print(n);
    Serial.println(" networks");
    
    // JSON banaye
    DynamicJsonDocument doc(4096);
    JsonArray networks = doc.to<JsonArray>();
    
    for (int i = 0; i < n; i++) {
      JsonObject network = networks.createNestedObject();
      network["ssid"] = WiFi.SSID(i);
      network["rssi"] = WiFi.RSSI(i);
      network["channel"] = WiFi.channel(i);
      network["encryption"] = WiFi.encryptionType(i);
      network["bssid"] = WiFi.BSSIDstr(i);
      network["hidden"] = WiFi.isHidden(i);
      
      // Serial monitor pe bhi dikhao
      Serial.print(i + 1);
      Serial.print(". ");
      Serial.print(WiFi.SSID(i));
      Serial.print(" (");
      Serial.print(WiFi.RSSI(i));
      Serial.println(" dBm)");
    }
    
    // JSON stringify karo
    String jsonData;
    serializeJson(doc, jsonData);
    
    // Server ko send karo
    sendToServer(jsonData);
  }
  
  // 15 seconds ka wait
  Serial.println("\n⏳ Waiting 15 seconds...\n");
  delay(15000);
}

void sendToServer(String jsonData) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    
    http.begin(client, serverUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "ESP8266-WiFi-Scanner");
    
    Serial.println("📤 Sending data to server...");
    int httpCode = http.POST(jsonData);
    
    if (httpCode > 0) {
      Serial.print("✅ Data sent! HTTP Code: ");
      Serial.println(httpCode);
      
      String response = http.getString();
      Serial.print("Server Response: ");
      Serial.println(response);
    } else {
      Serial.print("❌ Error sending data: ");
      Serial.println(http.errorToString(httpCode).c_str());
    }
    
    http.end();
  } else {
    Serial.println("❌ WiFi disconnected!");
  }
}