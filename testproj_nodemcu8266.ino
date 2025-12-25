#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>

// ================== EXISTING LED PINS ==================
#define WHITE_LED_PIN D0
#define BLUE_LED_PIN D8

// ================== RFID + GREEN/RED LED PINS ==================
#define GREEN_LED_PIN D1
#define RED_LED_PIN D2
#define RST_PIN D3
#define SS_PIN D4

// ================== CONFIG ==================
#define HEARTBEAT_INTERVAL 30000
#define SCAN_INTERVAL 15000
#define FAIL_LIMIT 5
#define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

const char* DEVICE_UUID = "7b57f76d-4900-4f9d-9fa2-472e43c9e49c";
const char* DEVICE_SECRET = "25af328699ccd34ae06034feb7ee61ca";

// ================== GLOBAL VARIABLES ==================
unsigned long lastHeartbeatTime = 0;
unsigned long lastScanTime = 0;
unsigned long lastLedToggle = 0;
unsigned long lastConnectBlinkToggle = 0;

int heartbeatFailCount = 0;
int wifiFailCount = 0;

bool reprovisionMode = false;
bool serverUnreachable = false;
bool ledState = false;
bool connectBlinkState = false;
bool isConnecting = false;

// ================== OBJECTS ==================
WiFiManager wifiManager;
MFRC522 rfid(SS_PIN, RST_PIN);

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  Serial.println("Starting ESP8266 Attendance Device...");

  // Existing LEDs
  pinMode(WHITE_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  digitalWrite(WHITE_LED_PIN, HIGH);
  digitalWrite(BLUE_LED_PIN, LOW);

  // RFID + Green/Red LEDs
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);

  SPI.begin();
  rfid.PCD_Init();

  // WiFi Manager setup
  wifiManager.setDebugOutput(false);
  wifiManager.setShowInfoErase(false);
  wifiManager.setShowInfoUpdate(false);
  std::vector<const char*> menu = { "wifi", "exit" };
  wifiManager.setMenu(menu);
  wifiManager.setAPCallback(configModeCallback);

  if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
    Serial.println("WiFi failed, rebooting...");
    ESP.restart();
  }

  Serial.println("WiFi Connected");
  reprovisionMode = false;
}

// ================== LOOP ==================
void loop() {
  // ----------------- WHITE LED -----------------
  if (reprovisionMode) {
    digitalWrite(WHITE_LED_PIN, HIGH);
  } else if (WiFi.status() == WL_CONNECTED) {
    blinkWhiteLED();
  } else {
    digitalWrite(WHITE_LED_PIN, HIGH);
  }

  // ----------------- BLUE LED -----------------
  digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

  // ----------------- HEARTBEAT + WIFI SCAN -----------------
  if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
    wifiFailCount = 0;
    isConnecting = false;

    if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
      sendHeartbeat();
      lastHeartbeatTime = millis();
    }

    if (!serverUnreachable && millis() - lastScanTime >= SCAN_INTERVAL) {
      scanAndSendWifi();
      lastScanTime = millis();
    }

    // ----------------- RFID ATTENDANCE -----------------
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String cardUuid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        cardUuid += String(rfid.uid.uidByte[i], HEX);
      }
      cardUuid.toUpperCase();

      Serial.println("Card scanned: " + cardUuid);

      bool accepted = sendAttendance(cardUuid);
      if (accepted) blinkGreenOnce();
      else blinkRedTwice();

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      delay(1200);  // anti-double tap
    }

  } else {
    wifiFailCount++;
    checkReprovision();
  }

  delay(30);
}

// ================== HEARTBEAT ==================
void sendHeartbeat() {
  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(SERVER_URL) + "/api/v1/device/heartbeat");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_UUID);
  http.addHeader("x-device-secret", DEVICE_SECRET);

  DynamicJsonDocument doc(256);
  doc["status"] = "connected";
  doc["ip"] = WiFi.localIP().toString();
  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  if (code == 200) {
    heartbeatFailCount = 0;
    serverUnreachable = false;
    handleCommand(http.getString());
  } else {
    heartbeatFailCount++;
    checkServerUnreachable();
  }

  http.end();
}

// ================== COMMAND ==================
void handleCommand(String json) {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, json)) return;

  const char* command = doc["data"]["command"];
  if (command && strcmp(command, "CONNECT_WIFI") == 0) {
    connectToWifi(
      doc["data"]["payload"]["ssid"],
      doc["data"]["payload"]["password"]);
  }
}

// ================== CONNECT WIFI ==================
void connectToWifi(const char* ssid, const char* password) {
  if (!ssid || !password) return;

  Serial.print("Connecting to new WiFi: ");
  Serial.println(ssid);
  isConnecting = true;
  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 25) {
    fastBlinkWhiteLED();
    delay(200);
    yield();
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to new WiFi");
    isConnecting = false;
    return;
  }

  Serial.println("New WiFi failed — rebooting");
  delay(1000);
  ESP.restart();
}

// ================== REPROVISION ==================
void checkReprovision() {
  if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
    Serial.println("All WiFi failed — AP Mode");
    reprovisionMode = true;
    digitalWrite(WHITE_LED_PIN, HIGH);
    wifiManager.startConfigPortal("RFID_Device_001", "12345678");
    reprovisionMode = false;
    wifiFailCount = 0;
  }
}

// ================== SERVER FAIL ==================
void checkServerUnreachable() {
  if (heartbeatFailCount >= FAIL_LIMIT) serverUnreachable = true;
}

// ================== WIFI SCAN ==================
void scanAndSendWifi() {
  int n = WiFi.scanNetworks();
  if (n <= 0) return;

  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject net = arr.createNestedObject();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["channel"] = WiFi.channel(i);
    net["encryption"] = WiFi.encryptionType(i);
  }

  String json;
  serializeJson(doc, json);

  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(SERVER_URL) + "/api/v1/device/wifi-scan");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_UUID);
  http.addHeader("x-device-secret", DEVICE_SECRET);

  http.POST(json);
  http.end();
}

// ================== CONFIG CALLBACK ==================
void configModeCallback(WiFiManager* myWiFiManager) {
  reprovisionMode = true;
}

// ================== LED FUNCTIONS ==================
void blinkWhiteLED() {
  if (millis() - lastLedToggle >= 300) {
    lastLedToggle = millis();
    ledState = !ledState;
    digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
  }
}

void fastBlinkWhiteLED() {
  if (millis() - lastConnectBlinkToggle >= 100) {
    lastConnectBlinkToggle = millis();
    connectBlinkState = !connectBlinkState;
    digitalWrite(WHITE_LED_PIN, connectBlinkState ? HIGH : LOW);
  }
}

bool sendAttendance(String cardUuid) {
  HTTPClient http;
  WiFiClient client;
  http.begin(client, String(SERVER_URL) + "/api/v1/attendance/record");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_UUID);
  http.addHeader("x-device-secret", DEVICE_SECRET);

  DynamicJsonDocument doc(256);
  doc["cardUuid"] = cardUuid;
  doc["deviceUuid"] = DEVICE_UUID;
  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);

  // 🔹 Read server response
  if (code > 0) {
    String response = http.getString();
    Serial.println("📥 Server Response:");
    Serial.println(response);
  } else {
    Serial.print("❌ HTTP Error: ");
    Serial.println(http.errorToString(code));
  }

  http.end();
  return (code == 200);
}

void blinkGreenOnce() {
  digitalWrite(GREEN_LED_PIN, HIGH);
  delay(120);
  digitalWrite(GREEN_LED_PIN, LOW);
}

void blinkRedTwice() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(RED_LED_PIN, HIGH);
    delay(120);
    digitalWrite(RED_LED_PIN, LOW);
    delay(120);
  }
}




// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>

// // ================== PINS ==================
// #define WHITE_LED_PIN D0
// #define GREEN_LED_PIN D1
// #define RED_LED_PIN D2
// #define RST_PIN D3
// #define SS_PIN D4
// #define BLUE_LED_PIN D8

// // ================== CONFIG ==================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "7b57f76d-4900-4f9d-9fa2-472e43c9e49c";
// const char* DEVICE_SECRET = "25af328699ccd34ae06034feb7ee61ca";

// // ================== OBJECTS ==================
// WiFiManager wifiManager;
// MFRC522 rfid(SS_PIN, RST_PIN);

// // ================== GLOBALS ==================
// unsigned long lastHeartbeatTime = 0;
// unsigned long lastScanTime = 0;
// unsigned long lastLedToggle = 0;
// unsigned long lastConnectBlinkToggle = 0;

// int heartbeatFailCount = 0;
// int wifiFailCount = 0;

// bool reprovisionMode = false;
// bool serverUnreachable = false;
// bool ledState = false;
// bool connectBlinkState = false;
// bool isConnecting = false;

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   Serial.println("ESP8266 Attendance Device Starting...");

//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(GREEN_LED_PIN, OUTPUT);
//   pinMode(RED_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);

//   digitalWrite(WHITE_LED_PIN, HIGH);
//   digitalWrite(GREEN_LED_PIN, LOW);
//   digitalWrite(RED_LED_PIN, LOW);
//   digitalWrite(BLUE_LED_PIN, LOW);

//   SPI.begin();
//   rfid.PCD_Init();

//   wifiManager.setDebugOutput(false);
//   wifiManager.setAPCallback(configModeCallback);

//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     ESP.restart();
//   }

//   Serial.println("WiFi Connected");
// }

// // ================== LOOP ==================
// void loop() {

//   // -------- WHITE LED (WiFi status) --------
//   if (reprovisionMode) {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     blinkWhiteLED();
//   } else {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // -------- BLUE LED (server fail) --------
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   // -------- HEARTBEAT --------
//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//     wifiFailCount = 0;

//     if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
//       sendHeartbeat();
//       lastHeartbeatTime = millis();
//     }

//     if (!serverUnreachable) {
//       handleRFID();  // 👈 MAIN ATTENDANCE POINT
//     }
//   } else {
//     wifiFailCount++;
//     checkReprovision();
//   }

//   delay(30);
// }

// // ================== RFID ==================
// void handleRFID() {
//   if (!rfid.PICC_IsNewCardPresent()) return;
//   if (!rfid.PICC_ReadCardSerial()) return;

//   String cardUuid = "";
//   for (byte i = 0; i < rfid.uid.size; i++) {
//     cardUuid += String(rfid.uid.uidByte[i], HEX);
//   }
//   cardUuid.toUpperCase();

//   Serial.println("Card: " + cardUuid);

//   bool accepted = sendAttendance(cardUuid);

//   if (accepted) {
//     blinkGreenOnce();
//   } else {
//     blinkRedTwice();
//   }

//   rfid.PICC_HaltA();
//   rfid.PCD_StopCrypto1();

//   delay(1500);  // double tap protection
// }

// // ================== ATTENDANCE API ==================
// bool sendAttendance(String cardUuid) {
//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/attendance/record");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   DynamicJsonDocument doc(256);
//   doc["cardUuid"] = cardUuid;
//   doc["deviceUuid"] = DEVICE_UUID;

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);

//   // 🔹 Read server response
//   if (code > 0) {
//     String response = http.getString();
//     Serial.println("📥 Server Response:");
//     Serial.println(response);
//   } else {
//     Serial.print("❌ HTTP Error: ");
//     Serial.println(http.errorToString(code));
//   }

//   http.end();

//   return (code == 200);
// }

// // ================== HEARTBEAT ==================
// void sendHeartbeat() {
//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/heartbeat");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   DynamicJsonDocument doc(256);
//   doc["status"] = "connected";
//   doc["ip"] = WiFi.localIP().toString();

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);
//   http.end();

//   if (code == 200) {
//     heartbeatFailCount = 0;
//     serverUnreachable = false;
//   } else {
//     heartbeatFailCount++;
//     if (heartbeatFailCount >= FAIL_LIMIT) {
//       serverUnreachable = true;
//     }
//   }
// }

// // ================== REPROVISION ==================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode) {
//     reprovisionMode = true;
//     wifiManager.startConfigPortal("RFID_Device_001", "12345678");
//     reprovisionMode = false;
//     wifiFailCount = 0;
//   }
// }

// void configModeCallback(WiFiManager*) {
//   reprovisionMode = true;
// }

// // ================== LED EFFECTS ==================
// void blinkGreenOnce() {
//   digitalWrite(GREEN_LED_PIN, HIGH);
//   delay(120);
//   digitalWrite(GREEN_LED_PIN, LOW);
// }

// void blinkRedTwice() {
//   for (int i = 0; i < 2; i++) {
//     digitalWrite(RED_LED_PIN, HIGH);
//     delay(120);
//     digitalWrite(RED_LED_PIN, LOW);
//     delay(120);
//   }
// }

// void blinkWhiteLED() {
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }







// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <SPI.h>
// #include <MFRC522.h>
// #include <ArduinoJson.h>

// // ================= WIFI =================
// #define WIFI_SSID "YOUR_WIFI_NAME"
// #define WIFI_PASS "YOUR_WIFI_PASS"

// // ================= SERVER =================
// #define SERVER_URL "http://your-server/api/v1/attendance/record"
// #define DEVICE_UUID "ESP-001"

// // ================= PINS =================
// #define WHITE_LED D0
// #define GREEN_LED D1
// #define RED_LED D2
// #define RST_PIN D3
// #define SS_PIN D4
// #define BLUE_LED D8

// MFRC522 mfrc522(SS_PIN, RST_PIN);

// // ================= STATES =================
// bool reprovisionMode = false;
// unsigned long whiteLedTimer = 0;
// bool whiteLedState = false;
// unsigned long lastServerCheck = 0;

// // ================= WHITE LED HANDLER =================
// void handleWhiteLED() {

//   // AP MODE
//   if (reprovisionMode) {
//     digitalWrite(WHITE_LED, HIGH);
//     return;
//   }

//   // WIFI CONNECTING → FAST BLINK
//   if (WiFi.status() != WL_CONNECTED) {
//     if (millis() - whiteLedTimer > 120) {
//       whiteLedTimer = millis();
//       whiteLedState = !whiteLedState;
//       digitalWrite(WHITE_LED, whiteLedState);
//     }
//     return;
//   }

//   // WIFI CONNECTED (STA) → SLOW BLINK
//   if (millis() - whiteLedTimer > 600) {
//     whiteLedTimer = millis();
//     whiteLedState = !whiteLedState;
//     digitalWrite(WHITE_LED, whiteLedState);
//   }
// }

// // ================= SERVER CHECK =================
// bool checkServer() {
//   WiFiClient client;
//   HTTPClient http;

//   http.begin(client, SERVER_URL);
//   int code = http.GET();
//   http.end();

//   Serial.print("[SERVER] ping code: ");
//   Serial.println(code);

//   return (code > 0 && code < 500);
// }

// // ================= SETUP =================
// void setup() {
//   Serial.begin(115200);
//   Serial.println("\nBOOTING...");

//   pinMode(WHITE_LED, OUTPUT);
//   pinMode(GREEN_LED, OUTPUT);
//   pinMode(RED_LED, OUTPUT);
//   pinMode(BLUE_LED, OUTPUT);

//   digitalWrite(GREEN_LED, LOW);
//   digitalWrite(RED_LED, LOW);
//   digitalWrite(BLUE_LED, LOW);
//   digitalWrite(WHITE_LED, HIGH);  // AP default

//   // WIFI
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(WIFI_SSID, WIFI_PASS);

//   Serial.println("[WIFI] connecting...");

//   // RFID
//   SPI.begin();
//   mfrc522.PCD_Init();
//   Serial.println("[RC522] READY");
// }

// // ================= LOOP =================
// void loop() {

//   handleWhiteLED();

//   // -------- SERVER STATUS --------
//   if (WiFi.status() == WL_CONNECTED) {
//     if (millis() - lastServerCheck > 5000) {
//       lastServerCheck = millis();

//       if (!checkServer()) {
//         digitalWrite(BLUE_LED, HIGH);
//         Serial.println("[STATUS] SERVER UNREACHABLE");
//       } else {
//         digitalWrite(BLUE_LED, LOW);
//       }
//     }
//   }

//   // -------- RFID --------
//   if (!mfrc522.PICC_IsNewCardPresent()) return;
//   if (!mfrc522.PICC_ReadCardSerial()) return;

//   String uid = "";
//   for (byte i = 0; i < mfrc522.uid.size; i++) {
//     uid += String(mfrc522.uid.uidByte[i], HEX);
//   }

//   Serial.print("[RFID] UID: ");
//   Serial.println(uid);

//   // -------- SEND TO SERVER --------
//   WiFiClient client;
//   HTTPClient http;

//   http.begin(client, SERVER_URL);
//   http.addHeader("Content-Type", "application/json");

//   DynamicJsonDocument doc(256);
//   doc["cardUuid"] = uid;
//   doc["deviceUuid"] = DEVICE_UUID;

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);
//   http.end();

//   Serial.print("[SERVER] response: ");
//   Serial.println(code);

//   if (code == 200) {
//     // SUCCESS → GREEN 1 BLINK (buzzer auto)
//     digitalWrite(GREEN_LED, HIGH);
//     delay(200);
//     digitalWrite(GREEN_LED, LOW);
//   } else {
//     // DENIED → RED 2 BLINK (buzzer auto)
//     for (int i = 0; i < 2; i++) {
//       digitalWrite(RED_LED, HIGH);
//       delay(200);
//       digitalWrite(RED_LED, LOW);
//       delay(200);
//     }
//   }

//   mfrc522.PICC_HaltA();
//   delay(1500);
// }




// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>

// #include <Wire.h>
// #include <SPI.h>
// #include <MFRC522.h>
// #include <PCF8574.h>

// // ===================== ESP GPIO =====================
// #define WHITE_LED_PIN D4
// #define BLUE_LED_PIN D1

// // ===================== RFID =====================
// #define SS_PIN D8
// #define RST_PIN D0
// MFRC522 mfrc522(SS_PIN, RST_PIN);

// // ===================== PCF8574 =====================
// PCF8574 pcf(0x20);
// #define LED_YELLOW 1
// #define LED_GREEN 0
// #define LED_RED 2
// #define BUZZER 3

// // ===================== CONSTANTS =====================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define FAIL_LIMIT 5
// #define CARD_COOLDOWN 3000

// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "7b57f76d-4900-4f9d-9fa2-472e43c9e49c";
// const char* DEVICE_SECRET = "25af328699ccd34ae06034feb7ee61ca";

// // ===================== GLOBALS =====================
// unsigned long lastHeartbeatTime = 0;
// unsigned long lastScanTime = 0;
// unsigned long lastLedToggle = 0;
// unsigned long lastCardScan = 0;

// int heartbeatFailCount = 0;
// int wifiFailCount = 0;

// bool reprovisionMode = false;
// bool serverUnreachable = false;
// bool ledState = false;

// WiFiManager wifiManager;

// // ===================== SETUP =====================
// void setup() {
//   Serial.begin(115200);
//   Serial.println("BOOTING...");

//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);

//   digitalWrite(WHITE_LED_PIN, HIGH);
//   digitalWrite(BLUE_LED_PIN, LOW);

//   // I2C
//   Wire.begin(D2, D3);

//   // PCF init (ACTIVE LOW)
//   pcf.begin();
//   pcf.pinMode(LED_YELLOW, OUTPUT);
//   pcf.pinMode(LED_GREEN, OUTPUT);
//   pcf.pinMode(LED_RED, OUTPUT);
//   pcf.pinMode(BUZZER, OUTPUT);

//   // ALL OFF
//   pcf.digitalWrite(LED_GREEN, HIGH);
//   pcf.digitalWrite(LED_RED, HIGH);
//   pcf.digitalWrite(BUZZER, HIGH);

//   // RFID
//   SPI.begin();
//   mfrc522.PCD_Init();

//   // RC522 READY INDICATOR
//   pcf.digitalWrite(LED_YELLOW, LOW);  // ✅ ON
//   Serial.println("RC522 READY");

//   // WiFi
//   wifiManager.setDebugOutput(false);
//   wifiManager.setAPCallback(configModeCallback);

//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     ESP.restart();
//   }

//   Serial.println("WIFI CONNECTED");
// }

// // ===================== LOOP =====================
// void loop() {

//   // White LED
//   if (reprovisionMode) {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     blinkWhiteLED();
//   } else {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // Blue LED (server down)
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {

//     if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
//       sendHeartbeat();
//       lastHeartbeatTime = millis();
//     }

//     if (!serverUnreachable && millis() - lastScanTime >= SCAN_INTERVAL) {
//       scanAndSendWifi();
//       lastScanTime = millis();
//     }

//     handleRFID();
//   }

//   delay(20);
// }

// // ===================== RFID =====================
// void handleRFID() {
//   if (millis() - lastCardScan < CARD_COOLDOWN) return;
//   if (!mfrc522.PICC_IsNewCardPresent()) return;
//   if (!mfrc522.PICC_ReadCardSerial()) return;

//   lastCardScan = millis();

//   String uid = "";
//   for (byte i = 0; i < mfrc522.uid.size; i++) {
//     uid += String(mfrc522.uid.uidByte[i], HEX);
//   }
//   uid.toUpperCase();

//   Serial.println("CARD: " + uid);
//   sendAttendance(uid);

//   mfrc522.PICC_HaltA();
// }

// // ===================== ATTENDANCE =====================
// void sendAttendance(String cardUid) {
//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/attendance/record");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   DynamicJsonDocument doc(256);
//   doc["cardUuid"] = cardUid;
//   doc["deviceUuid"] = DEVICE_UUID;

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);

//   if (code == 200) successSignal();
//   else deniedSignal();

//   http.end();
// }

// // ===================== SIGNALS =====================
// void successSignal() {
//   pcf.digitalWrite(LED_GREEN, LOW);
//   pcf.digitalWrite(BUZZER, LOW);
//   delay(150);
//   pcf.digitalWrite(LED_GREEN, HIGH);
//   pcf.digitalWrite(BUZZER, HIGH);
// }

// void deniedSignal() {
//   for (int i = 0; i < 2; i++) {
//     pcf.digitalWrite(LED_RED, LOW);
//     pcf.digitalWrite(BUZZER, LOW);
//     delay(150);
//     pcf.digitalWrite(LED_RED, HIGH);
//     pcf.digitalWrite(BUZZER, HIGH);
//     delay(150);
//   }
// }

// // ===================== HEARTBEAT =====================
// void sendHeartbeat() {
//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/heartbeat");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   DynamicJsonDocument doc(256);
//   doc["status"] = "connected";
//   doc["ip"] = WiFi.localIP().toString();

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);

//   if (code == 200) {
//     heartbeatFailCount = 0;
//     serverUnreachable = false;
//   } else {
//     heartbeatFailCount++;
//     if (heartbeatFailCount >= FAIL_LIMIT) serverUnreachable = true;
//   }

//   http.end();
// }

// // ===================== WIFI SCAN =====================
// void scanAndSendWifi() {
//   int n = WiFi.scanNetworks();
//   if (n <= 0) return;

//   DynamicJsonDocument doc(2048);
//   JsonArray arr = doc.to<JsonArray>();

//   for (int i = 0; i < n; i++) {
//     JsonObject net = arr.createNestedObject();
//     net["ssid"] = WiFi.SSID(i);
//     net["rssi"] = WiFi.RSSI(i);
//   }

//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/wifi-scan");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   String json;
//   serializeJson(doc, json);
//   http.POST(json);
//   http.end();
// }

// // ===================== CALLBACK =====================
// void configModeCallback(WiFiManager* myWiFiManager) {
//   reprovisionMode = true;
// }

// // ===================== LED =====================
// void blinkWhiteLED() {
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }





















// working code =================

// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <MFRC522.h>
// #include <SPI.h>

// #define WHITE_LED_PIN D4
// #define BLUE_LED_PIN D1
// #define YELLOW_LED_PIN D2  // RC522 ready
// #define GREEN_LED_PIN D6   // Access granted
// #define RED_LED_PIN D7

// #define RFID_SS_PIN D8
// #define RFID_RST_PIN D3

// #define RFID_DEBOUNCE 3000
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "7b57f76d-4900-4f9d-9fa2-472e43c9e49c";
// const char* DEVICE_SECRET = "25af328699ccd34ae06034feb7ee61ca";

// unsigned long lastHeartbeatTime = 0;
// unsigned long lastScanTime = 0;
// unsigned long lastLedToggle = 0;
// unsigned long lastConnectBlinkToggle = 0;
// unsigned long lastRFIDTime = 0;

// int heartbeatFailCount = 0;
// int wifiFailCount = 0;

// bool reprovisionMode = false;
// bool serverUnreachable = false;
// bool ledState = false;
// bool connectBlinkState = false;
// bool isConnecting = false;

// String lastUID = "";

// WiFiManager wifiManager;

// MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);

// // ===================== SETUP =====================
// void setup() {
//   Serial.begin(115200);
//   Serial.println("Starting ESP8266...");

//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);

//   digitalWrite(WHITE_LED_PIN, HIGH);
//   digitalWrite(BLUE_LED_PIN, LOW);

//   wifiManager.setDebugOutput(false);
//   wifiManager.setShowInfoErase(false);
//   wifiManager.setShowInfoUpdate(false);

//   std::vector<const char*> menu = { "wifi", "exit" };
//   wifiManager.setMenu(menu);

//   wifiManager.setAPCallback(configModeCallback);

//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     Serial.println("WiFi failed, rebooting...");
//     ESP.restart();
//   }

//   Serial.println("WiFi Connected");
//   reprovisionMode = false;

//   // RFID LEDs
//   pinMode(YELLOW_LED_PIN, OUTPUT);
//   pinMode(GREEN_LED_PIN, OUTPUT);
//   pinMode(RED_LED_PIN, OUTPUT);

//   digitalWrite(GREEN_LED_PIN, LOW);
//   digitalWrite(RED_LED_PIN, LOW);

//   // RFID init
//   SPI.begin();
//   mfrc522.PCD_Init();

//   // RC522 READY → YELLOW SOLID
//   digitalWrite(YELLOW_LED_PIN, HIGH);
//   Serial.println("RC522 READY");
// }

// // ===================== LOOP =====================
// void loop() {

//   // White LED handling
//   if (reprovisionMode) {
//     digitalWrite(WHITE_LED_PIN, HIGH);  // solid in AP
//   } else if (WiFi.status() == WL_CONNECTED) {
//     blinkWhiteLED();
//   } else {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // Blue LED (server unreachable)
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//     wifiFailCount = 0;
//     isConnecting = false;

//     if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
//       sendHeartbeat();
//       lastHeartbeatTime = millis();
//     }

//     if (!serverUnreachable && millis() - lastScanTime >= SCAN_INTERVAL) {
//       scanAndSendWifi();
//       lastScanTime = millis();
//     }
//   } else {
//     wifiFailCount++;
//     checkReprovision();
//   }

//   checkRFID();

//   delay(30);
// }

// // ===================== HEARTBEAT =====================
// void sendHeartbeat() {
//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/heartbeat");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   DynamicJsonDocument doc(256);
//   doc["status"] = "connected";
//   doc["ip"] = WiFi.localIP().toString();

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);

//   if (code == 200) {
//     heartbeatFailCount = 0;
//     serverUnreachable = false;
//     handleCommand(http.getString());
//   } else {
//     heartbeatFailCount++;
//     checkServerUnreachable();
//   }

//   http.end();
// }

// // ===================== COMMAND =====================
// void handleCommand(String json) {
//   DynamicJsonDocument doc(512);
//   if (deserializeJson(doc, json)) return;

//   const char* command = doc["data"]["command"];
//   if (command && strcmp(command, "CONNECT_WIFI") == 0) {
//     connectToWifi(
//       doc["data"]["payload"]["ssid"],
//       doc["data"]["payload"]["password"]);
//   }
// }

// // ===================== CONNECT WIFI =====================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);

//   isConnecting = true;

//   WiFi.disconnect(true);  // FULL reset of STA
//   delay(500);

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);

//   int tries = 0;
//   while (WiFi.status() != WL_CONNECTED && tries < 25) {
//     fastBlinkWhiteLED();
//     delay(200);
//     yield();
//     tries++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Connected to new WiFi");
//     isConnecting = false;
//     return;
//   }

//   Serial.println("New WiFi failed — rebooting to restore saved WiFi");
//   delay(1000);

//   ESP.restart();  // 🔥 THIS IS THE REAL FIX
// }

// // ===================== REPROVISION =====================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
//     Serial.println("All WiFi failed — AP Mode");
//     reprovisionMode = true;
//     digitalWrite(WHITE_LED_PIN, HIGH);
//     wifiManager.startConfigPortal("RFID_Device_001", "12345678");
//     reprovisionMode = false;
//     wifiFailCount = 0;
//   }
// }

// // ===================== SERVER FAIL =====================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) {
//     serverUnreachable = true;
//   }
// }

// // ===================== WIFI SCAN =====================
// void scanAndSendWifi() {
//   int n = WiFi.scanNetworks();
//   if (n <= 0) return;

//   DynamicJsonDocument doc(4096);
//   JsonArray arr = doc.to<JsonArray>();

//   for (int i = 0; i < n; i++) {
//     JsonObject net = arr.createNestedObject();
//     net["ssid"] = WiFi.SSID(i);
//     net["rssi"] = WiFi.RSSI(i);
//     net["channel"] = WiFi.channel(i);
//     net["encryption"] = WiFi.encryptionType(i);
//   }

//   String json;
//   serializeJson(doc, json);

//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/wifi-scan");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   http.POST(json);
//   http.end();
// }

// // ===================== CALLBACK =====================
// void configModeCallback(WiFiManager* myWiFiManager) {
//   reprovisionMode = true;
// }

// // ===================== LED FUNCTIONS =====================
// void blinkWhiteLED() {
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }

// void fastBlinkWhiteLED() {
//   if (millis() - lastConnectBlinkToggle >= 100) {
//     lastConnectBlinkToggle = millis();
//     connectBlinkState = !connectBlinkState;
//     digitalWrite(WHITE_LED_PIN, connectBlinkState ? HIGH : LOW);
//   }
// }



// String readUID() {
//   String uid = "";
//   for (byte i = 0; i < mfrc522.uid.size; i++) {
//     if (mfrc522.uid.uidByte[i] < 0x10) uid += "0";
//     uid += String(mfrc522.uid.uidByte[i], HEX);
//   }
//   uid.toUpperCase();
//   return uid;
// }

// void sendRFID(String uid) {
//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/rfid-attendance");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   DynamicJsonDocument doc(256);
//   doc["uid"] = uid;

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);

//   if (code == 200) {
//     DynamicJsonDocument res(256);
//     deserializeJson(res, http.getString());

//     bool allowed = res["data"]["allowed"];
//     if (allowed) {
//       digitalWrite(GREEN_LED_PIN, HIGH);
//       delay(800);
//       digitalWrite(GREEN_LED_PIN, LOW);
//     } else {
//       digitalWrite(RED_LED_PIN, HIGH);
//       delay(800);
//       digitalWrite(RED_LED_PIN, LOW);
//     }
//   }

//   http.end();
// }


// void checkRFID() {
//   if (!mfrc522.PICC_IsNewCardPresent()) return;
//   if (!mfrc522.PICC_ReadCardSerial()) return;

//   String uid = readUID();

//   if (uid == lastUID && millis() - lastRFIDTime < RFID_DEBOUNCE) return;
//   lastUID = uid;
//   lastRFIDTime = millis();

//   Serial.println("Card UID: " + uid);

//   sendRFID(uid);

//   mfrc522.PICC_HaltA();
//   mfrc522.PCD_StopCrypto1();
// }

























// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>

// #define WHITE_LED_PIN D4
// #define BLUE_LED_PIN D1

// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "7b57f76d-4900-4f9d-9fa2-472e43c9e49c";
// const char* DEVICE_SECRET = "25af328699ccd34ae06034feb7ee61ca";

// unsigned long lastHeartbeatTime = 0;
// unsigned long lastScanTime = 0;
// unsigned long lastLedToggle = 0;
// unsigned long lastConnectBlinkToggle = 0;

// int heartbeatFailCount = 0;
// int wifiFailCount = 0;

// bool reprovisionMode = false;
// bool serverUnreachable = false;
// bool ledState = false;
// bool connectBlinkState = false;
// bool isConnecting = false;

// WiFiManager wifiManager;

// // ===================== SETUP =====================
// void setup() {
//   Serial.begin(115200);
//   Serial.println("Starting ESP8266...");

//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);

//   digitalWrite(WHITE_LED_PIN, HIGH);
//   digitalWrite(BLUE_LED_PIN, LOW);

//   wifiManager.setDebugOutput(false);
//   wifiManager.setShowInfoErase(false);
//   wifiManager.setShowInfoUpdate(false);

//   std::vector<const char*> menu = { "wifi", "exit" };
//   wifiManager.setMenu(menu);

//   wifiManager.setAPCallback(configModeCallback);

//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     Serial.println("WiFi failed, rebooting...");
//     ESP.restart();
//   }

//   Serial.println("WiFi Connected");
//   reprovisionMode = false;
// }

// // ===================== LOOP =====================
// void loop() {

//   // White LED handling
//   if (reprovisionMode) {
//     digitalWrite(WHITE_LED_PIN, HIGH);  // solid in AP
//   } else if (WiFi.status() == WL_CONNECTED) {
//     blinkWhiteLED();
//   } else {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // Blue LED (server unreachable)
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//     wifiFailCount = 0;
//     isConnecting = false;

//     if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
//       sendHeartbeat();
//       lastHeartbeatTime = millis();
//     }

//     if (!serverUnreachable && millis() - lastScanTime >= SCAN_INTERVAL) {
//       scanAndSendWifi();
//       lastScanTime = millis();
//     }
//   } else {
//     wifiFailCount++;
//     checkReprovision();
//   }

//   delay(30);
// }

// // ===================== HEARTBEAT =====================
// void sendHeartbeat() {
//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/heartbeat");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   DynamicJsonDocument doc(256);
//   doc["status"] = "connected";
//   doc["ip"] = WiFi.localIP().toString();

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);

//   if (code == 200) {
//     heartbeatFailCount = 0;
//     serverUnreachable = false;
//     handleCommand(http.getString());
//   } else {
//     heartbeatFailCount++;
//     checkServerUnreachable();
//   }

//   http.end();
// }

// // ===================== COMMAND =====================
// void handleCommand(String json) {
//   DynamicJsonDocument doc(512);
//   if (deserializeJson(doc, json)) return;

//   const char* command = doc["data"]["command"];
//   if (command && strcmp(command, "CONNECT_WIFI") == 0) {
//     connectToWifi(
//       doc["data"]["payload"]["ssid"],
//       doc["data"]["payload"]["password"]);
//   }
// }

// // ===================== CONNECT WIFI =====================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);

//   isConnecting = true;

//   WiFi.disconnect(true);  // FULL reset of STA
//   delay(500);

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);

//   int tries = 0;
//   while (WiFi.status() != WL_CONNECTED && tries < 25) {
//     fastBlinkWhiteLED();
//     delay(200);
//     yield();
//     tries++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Connected to new WiFi");
//     isConnecting = false;
//     return;
//   }

//   Serial.println("New WiFi failed — rebooting to restore saved WiFi");
//   delay(1000);

//   ESP.restart();  // 🔥 THIS IS THE REAL FIX
// }

// // ===================== REPROVISION =====================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
//     Serial.println("All WiFi failed — AP Mode");
//     reprovisionMode = true;
//     digitalWrite(WHITE_LED_PIN, HIGH);
//     wifiManager.startConfigPortal("RFID_Device_001", "12345678");
//     reprovisionMode = false;
//     wifiFailCount = 0;
//   }
// }

// // ===================== SERVER FAIL =====================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) {
//     serverUnreachable = true;
//   }
// }

// // ===================== WIFI SCAN =====================
// void scanAndSendWifi() {
//   int n = WiFi.scanNetworks();
//   if (n <= 0) return;

//   DynamicJsonDocument doc(4096);
//   JsonArray arr = doc.to<JsonArray>();

//   for (int i = 0; i < n; i++) {
//     JsonObject net = arr.createNestedObject();
//     net["ssid"] = WiFi.SSID(i);
//     net["rssi"] = WiFi.RSSI(i);
//     net["channel"] = WiFi.channel(i);
//     net["encryption"] = WiFi.encryptionType(i);
//   }

//   String json;
//   serializeJson(doc, json);

//   HTTPClient http;
//   WiFiClient client;

//   http.begin(client, String(SERVER_URL) + "/api/v1/device/wifi-scan");
//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   http.POST(json);
//   http.end();
// }

// // ===================== CALLBACK =====================
// void configModeCallback(WiFiManager* myWiFiManager) {
//   reprovisionMode = true;
// }

// // ===================== LED FUNCTIONS =====================
// void blinkWhiteLED() {
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }

// void fastBlinkWhiteLED() {
//   if (millis() - lastConnectBlinkToggle >= 100) {
//     lastConnectBlinkToggle = millis();
//     connectBlinkState = !connectBlinkState;
//     digitalWrite(WHITE_LED_PIN, connectBlinkState ? HIGH : LOW);
//   }
// }