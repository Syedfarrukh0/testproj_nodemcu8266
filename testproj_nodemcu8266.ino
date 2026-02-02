#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <PCF8574.h>
#include <SD.h>
#include <FS.h>

// ================== PCF8574 CONFIG ==================
#define PCF_ADDRESS 0x20  // A0,A1,A2 pins according to wiring
PCF8574 pcf(PCF_ADDRESS);

// ================== SD CARD CONFIG ==================
#define SD_CS_PIN D8  // SD card CS pin
#define SCHEDULE_FILE "/schedule.json"

// ================== LED PINS on PCF8574 ==================
#define WHITE_LED_PIN 0  // Main Status LED
#define BLUE_LED_PIN 1   // Server Status

// ================== RFID + GREEN/RED LED PINS ==================
#define GREEN_LED_PIN 2
#define RED_LED_PIN 3
#define RST_PIN D3
#define SS_PIN D4

// ================== CONFIG ==================
#define HEARTBEAT_INTERVAL 30000
#define SCAN_INTERVAL 15000
#define SCHEDULE_UPDATE_INTERVAL 86400000
#define FAIL_LIMIT 5
#define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// ================== EEPROM CONFIG ==================
// #define EEPROM_SIZE 4096
#define SCHEDULE_START_ADDR 0
#define SCHEDULE_CRC_ADDR 1000
#define SCHEDULE_COUNT_ADDR 1004
#define LAST_UPDATE_ADDR 1008

// ================== GLOBAL VARIABLES ==================
unsigned long lastHeartbeatTime = 0;
unsigned long lastScanTime = 0;
unsigned long lastLedToggle = 0;
unsigned long lastConnectBlinkToggle = 0;
unsigned long lastScheduleUpdate = 0;
unsigned long scheduleUpdateInterval = 3600000;  // Default 1 hour

int heartbeatFailCount = 0;
int wifiFailCount = 0;

bool reprovisionMode = false;
bool serverUnreachable = false;
bool ledState = false;
bool connectBlinkState = false;
bool isConnecting = false;
bool isInitialScheduleLoaded = false;
bool sdMounted = false;

// ================== STRUCTURES ==================
struct UserSchedule {
  int userId;
  String cardUuid;
  String userName;
  int dayOfWeek;
  String checkInFrom;
  String checkInTo;
  String checkOutFrom;
  String checkOutTo;
};

struct StoredData {
  char lastUpdated[25];
  int totalUsers;
  unsigned long nextUpdateTime;
};

// ================== GLOBAL VECTORS ==================
std::vector<UserSchedule> userSchedules;
StoredData storedData;

// ================== OBJECTS ==================
WiFiManager wifiManager;
MFRC522 rfid(SS_PIN, RST_PIN);

// ================== CRC32 FUNCTION ==================
uint32_t calculateCRC32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc >>= 1;
      }
    }
  }
  return ~crc;
}

// ================== SCHEDULE MANAGEMENT ==================
void fetchAndStoreSchedules() {
  Serial.println("Fetching schedules from server...");

  HTTPClient http;
  WiFiClient client;

  // Increase timeout and buffer size
  client.setTimeout(15000);
  http.setTimeout(15000);
  http.setReuse(true);

  String url = String(SERVER_URL) + "/api/v1/device/schedules";
  Serial.println("URL: " + url);

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed!");
    blinkRedTwice();
    return;
  }

  http.addHeader("x-device-id", DEVICE_UUID);
  http.addHeader("x-device-secret", DEVICE_SECRET);
  http.addHeader("Connection", "close");

  // Get current time for timeout
  unsigned long startTime = millis();
  int httpCode = http.GET();
  unsigned long elapsed = millis() - startTime;

  Serial.print("HTTP GET took ");
  Serial.print(elapsed);
  Serial.println(" ms");

  if (httpCode > 0) {
    Serial.printf("HTTP Response code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.println("Response length: " + String(response.length()));

      // Print first 500 chars for debugging
      if (response.length() > 500) {
        Serial.println("First 500 chars: " + response.substring(0, 500));
      } else {
        Serial.println("Response: " + response);
      }

      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, response);

      if (!error) {
        // Check if response is successful
        bool success = doc["success"];
        if (!success) {
          Serial.println("Server returned error: " + String(doc["message"].as<String>()));
          blinkRedTwice();
          http.end();
          return;
        }

        // Get the data object
        JsonObject data = doc["data"];
        if (data.isNull()) {
          Serial.println("Error: No 'data' object in response");
          blinkRedTwice();
          http.end();
          return;
        }

        // Check if data is different from current
        bool dataChanged = true;

        // Get lastUpdated from data object
        String newHash = String(data["lastUpdated"].as<String>());
        String oldHash = String(storedData.lastUpdated);
        dataChanged = (newHash != oldHash);
        Serial.println("Data changed: " + String(dataChanged ? "YES" : "NO"));
        Serial.println("New hash: " + newHash);
        Serial.println("Old hash: " + oldHash);

        if (dataChanged) {
          // Clear existing schedules
          userSchedules.clear();

          // Parse users from data object
          JsonArray users = data["users"];
          Serial.println("Total users in response: " + String(users.size()));

          int scheduleCount = 0;
          int userCount = 0;

          for (JsonObject user : users) {
            String cardUuid = user["cardUuid"].as<String>();
            String userName = user["name"].as<String>();
            int userId = user["id"];

            userCount++;
            Serial.println("\nUser #" + String(userCount) + ": " + userName);
            Serial.println("  ID: " + String(userId));
            Serial.println("  Card UUID: " + cardUuid);

            // Only process users with card UUID
            if (cardUuid != "null" && cardUuid.length() > 0) {
              JsonArray schedules = user["schedules"];
              Serial.println("  Schedules count: " + String(schedules.size()));

              for (JsonObject schedule : schedules) {
                UserSchedule userSchedule;
                userSchedule.userId = userId;
                userSchedule.cardUuid = cardUuid;
                userSchedule.userName = userName;
                userSchedule.dayOfWeek = schedule["day"];
                userSchedule.checkInFrom = schedule["checkInFrom"].as<String>();
                userSchedule.checkInTo = schedule["checkInTo"].as<String>();
                userSchedule.checkOutFrom = schedule["checkOutFrom"].as<String>();
                userSchedule.checkOutTo = schedule["checkOutTo"].as<String>();

                userSchedules.push_back(userSchedule);
                scheduleCount++;

                Serial.println("    Day " + String(userSchedule.dayOfWeek) + ": " + userSchedule.checkInFrom + " to " + userSchedule.checkInTo);
              }
            } else {
              Serial.println("  Skipped - no card UUID");
            }
          }

          Serial.println("\n=== SUMMARY ===");
          Serial.println("Total users processed: " + String(userCount));
          Serial.println("Total schedules added: " + String(scheduleCount));

          // Save to sd card
          saveScheduleToSD();

          Serial.println("Schedules updated. Total schedules in memory: " + String(userSchedules.size()));

          // Show success indication based on count
          int blinkCount = min(scheduleCount, 5);  // Max 5 blinks
          for (int i = 0; i < blinkCount; i++) {
            blinkGreenOnce();
            delay(150);
          }

        } else {
          Serial.println("Schedules unchanged - no update needed");
          // Update stored time
          storedData.nextUpdateTime = millis() + scheduleUpdateInterval;
          blinkGreenOnce();  // Single blink for success but no change
        }

        serverUnreachable = false;

      } else {
        Serial.print("Failed to parse schedule JSON: ");
        Serial.println(error.c_str());
        blinkRedTwice();
      }

    } else {
      Serial.println("Server returned error: " + String(httpCode));
      blinkRedTwice();
    }

  } else {
    Serial.print("HTTP GET failed: ");
    Serial.println(http.errorToString(httpCode));
    blinkRedTwice();
  }

  http.end();
  Serial.println("HTTP connection closed\n");
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);  // Serial stable hone ka wait karo

  Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

  // ================== SD CARD INIT ==================
  sdMounted = SD.begin(SD_CS_PIN);
  if (!sdMounted) {
    Serial.println("❌ SD Card mount failed");
  } else {
    Serial.println("✅ SD Card mounted successfully");
  }

  // ================== LOAD SCHEDULE FROM SD ==================
  if (loadScheduleFromSD()) {
    Serial.println("Using stored schedules from SD card");
    isInitialScheduleLoaded = true;
  }

  // ================== PCF8574 INIT ==================
  Wire.begin(D1, D2);  // SDA, SCL
  pcf.begin();

  // INITIAL LED STATES - SAB OFF KARO
  pcf.write(WHITE_LED_PIN, LOW);
  pcf.write(BLUE_LED_PIN, LOW);
  pcf.write(GREEN_LED_PIN, LOW);
  pcf.write(RED_LED_PIN, LOW);

  // WHITE LED ON (Booting status)
  pcf.write(WHITE_LED_PIN, HIGH);

  // CLEAN WIFI STATE
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(100);

  // RFID INITIALIZE
  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID Module Initialized");

  // WIFI MANAGER SETUP
  wifiManager.setDebugOutput(false);
  wifiManager.setShowInfoErase(false);
  wifiManager.setShowInfoUpdate(false);

  std::vector<const char*> menu = { "wifi", "exit" };
  wifiManager.setMenu(menu);

  wifiManager.setAPCallback([](WiFiManager* wm) {
    Serial.println("Config Portal Started");
    reprovisionMode = true;
    // Config portal mein white LED still ON
    pcf.write(WHITE_LED_PIN, HIGH);
  });

  wifiManager.setConnectTimeout(30);        // 30 seconds connection try
  wifiManager.setConfigPortalTimeout(180);  // 3 minutes portal timeout

  Serial.println("Attempting WiFi connection...");

  // CONNECT TO WIFI
  if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
    Serial.println("WiFi connection failed! Auto-restarting...");

    // 5 seconds wait aur phir restart
    delay(5000);
    ESP.restart();  // AUTO RESTART - NO BUTTON NEEDED
    delay(5000);
  }

  // SUCCESS - WiFi connected
  Serial.println("WiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // Fetch schedules immediately after WiFi connection
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Fetching fresh schedules from server...");
    fetchAndStoreSchedules();
  } else {
    Serial.println("WiFi not connected, using EEPROM schedules");
  }

  // Ab white LED blink karegi (connected status)
  reprovisionMode = false;
  lastHeartbeatTime = millis();
  lastScanTime = millis();
  lastScheduleUpdate = millis();

  Serial.println("Setup complete!");
}

// ================== LOOP ==================
void loop() {
  // ----------------- WHITE LED STATUS -----------------
  if (reprovisionMode) {
    // Config portal mode - White LED ON (still)
    pcf.write(WHITE_LED_PIN, HIGH);
  } else if (WiFi.status() == WL_CONNECTED) {
    // WiFi connected - White LED BLINK (slow)
    blinkWhiteLED();
  } else {
    // WiFi not connected - White LED ON (still)
    pcf.write(WHITE_LED_PIN, HIGH);
  }

  // ----------------- BLUE LED (SERVER STATUS) -----------------
  pcf.write(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

  // ----------------- PERIODIC SCHEDULE UPDATE -----------------
  if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
    if (millis() - lastScheduleUpdate >= scheduleUpdateInterval) {
      fetchAndStoreSchedules();
      lastScheduleUpdate = millis();
    }
  }

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

      // First check in local schedule (if available)
      if (userSchedules.size() > 0) {
        bool foundInSchedule = false;
        for (const auto& schedule : userSchedules) {
          if (schedule.cardUuid == cardUuid) {
            foundInSchedule = true;
            Serial.println("User found in local schedule: " + schedule.userName);
            break;
          }
        }

        if (!foundInSchedule) {
          Serial.println("Card not found in local schedule");
          blinkRedTwice();
          rfid.PICC_HaltA();
          rfid.PCD_StopCrypto1();
          delay(1200);
          return;
        }
      }

      bool accepted = sendAttendance(cardUuid);
      if (accepted) blinkGreenOnce();  // RFID Success - Green blink
      else blinkRedTwice();            // RFID Fail - Red blink

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();

      delay(1200);  // anti-double tap
    }

  } else {
    // WiFi disconnected
    wifiFailCount++;

    // Try reconnecting after some failures
    if (wifiFailCount > 10) {
      Serial.println("WiFi disconnected. Attempting reconnect...");
      WiFi.reconnect();
      delay(5000);

      if (WiFi.status() != WL_CONNECTED) {
        checkReprovision();
      } else {
        wifiFailCount = 0;
      }
    }
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
  doc["schedulesLoaded"] = userSchedules.size();

  // ================== SD CARD STATUS ==================
  JsonObject sd = doc.createNestedObject("sd");
  sd["mounted"] = sdMounted;
  sd["scheduleFileExists"] = SD.exists(SCHEDULE_FILE);

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
  } else if (command && strcmp(command, "UPDATE_SCHEDULES") == 0) {
    // Force schedule update from server
    Serial.println("Force schedule update requested from server");
    if (WiFi.status() == WL_CONNECTED) {
      fetchAndStoreSchedules();
    }
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

  // Connecting ke dauraan white LED fast blink
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 25) {
    // Fast blink while connecting
    pcf.write(WHITE_LED_PIN, HIGH);
    delay(100);
    pcf.write(WHITE_LED_PIN, LOW);
    delay(100);
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to new WiFi");
    // Fetch schedules after new WiFi connection
    fetchAndStoreSchedules();
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
    pcf.write(WHITE_LED_PIN, HIGH);  // White LED ON (still)

    // Reset WiFi fail count
    wifiFailCount = 0;

    // Start config portal
    if (!wifiManager.startConfigPortal("RFID_Device_001", "12345678")) {
      Serial.println("Portal timeout, restarting...");
      delay(3000);
      ESP.restart();
    }

    reprovisionMode = false;
  }
}

// ================== SERVER FAIL ==================
void checkServerUnreachable() {
  if (heartbeatFailCount >= FAIL_LIMIT) {
    serverUnreachable = true;
    Serial.println("Server unreachable!");
  }
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

// ================== SEND ATTENDANCE FUNCTIONS ==================
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

  // Read server response
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

// ================== SAVE SCHEDULE FUNCTIONS ==================
void saveScheduleToSD() {
  DynamicJsonDocument doc(4096);
  JsonArray users = doc.createNestedArray("users");

  for (auto& u : userSchedules) {
    JsonObject o = users.createNestedObject();
    o["id"] = u.userId;
    o["cardUuid"] = u.cardUuid;
    o["name"] = u.userName;
    o["dayOfWeek"] = u.dayOfWeek;
    o["checkInFrom"] = u.checkInFrom;
    o["checkInTo"] = u.checkInTo;
    o["checkOutFrom"] = u.checkOutFrom;
    o["checkOutTo"] = u.checkOutTo;
  }

  File file = SD.open(SCHEDULE_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Failed to open SD file for writing");
    return;
  }

  serializeJson(doc, file);
  file.close();

  Serial.println("✅ Schedule saved to SD card");
}

// ================== LOAD SCHEDULE FUNCTIONS ==================
bool loadScheduleFromSD() {
  if (!SD.exists(SCHEDULE_FILE)) {
    Serial.println("⚠️ No schedule file on SD");
    return false;
  }

  File file = SD.open(SCHEDULE_FILE);
  if (!file) return false;

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, file)) {
    file.close();
    return false;
  }
  file.close();

  userSchedules.clear();

  for (JsonObject u : doc["users"].as<JsonArray>()) {
    UserSchedule s;
    s.userId = u["id"];
    s.cardUuid = u["cardUuid"].as<String>();
    s.userName = u["name"].as<String>();
    s.dayOfWeek = u["dayOfWeek"];
    s.checkInFrom = u["checkInFrom"].as<String>();
    s.checkInTo = u["checkInTo"].as<String>();
    s.checkOutFrom = u["checkOutFrom"].as<String>();
    s.checkOutTo = u["checkOutTo"].as<String>();
    userSchedules.push_back(s);
  }

  Serial.println("✅ Loaded schedules from SD: " + String(userSchedules.size()));
  return true;
}

// ================== LED FUNCTIONS ==================
void blinkWhiteLED() {
  // WiFi connected - Slow blink (300ms ON, 300ms OFF)
  if (millis() - lastLedToggle >= 300) {
    lastLedToggle = millis();
    ledState = !ledState;
    pcf.write(WHITE_LED_PIN, ledState ? HIGH : LOW);
  }
}

void blinkGreenOnce() {
  // RFID Success - Green blink once
  pcf.write(GREEN_LED_PIN, HIGH);
  delay(120);
  pcf.write(GREEN_LED_PIN, LOW);
}

void blinkRedTwice() {
  // RFID Fail - Red blink twice
  for (int i = 0; i < 2; i++) {
    pcf.write(RED_LED_PIN, HIGH);
    delay(120);
    pcf.write(RED_LED_PIN, LOW);
    delay(120);
  }
}





// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>
// #include <EEPROM.h>
// #include <Wire.h>
// #include <PCF8574.h>

// // ================== PCF8574 CONFIG ==================
// #define PCF_ADDRESS 0x20  // A0,A1,A2 pins according to wiring
// PCF8574 pcf(PCF_ADDRESS);

// // ================== LED PINS on PCF8574 ==================
// #define WHITE_LED_PIN 0
// #define BLUE_LED_PIN 1

// // ================== EXISTING LED PINS ==================
// // #define WHITE_LED_PIN D0  // Main Status LED
// // #define BLUE_LED_PIN D8   // Server Status

// // ================== RFID + GREEN/RED LED PINS ==================
// // #define GREEN_LED_PIN D1  // RFID Success
// // #define RED_LED_PIN D2    // RFID Fail
// #define GREEN_LED_PIN 2
// #define RED_LED_PIN 3
// #define RST_PIN D3
// #define SS_PIN D4

// // ================== CONFIG ==================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define SCHEDULE_UPDATE_INTERVAL 86400000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
// const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// // ================== EEPROM CONFIG ==================
// #define EEPROM_SIZE 4096
// #define SCHEDULE_START_ADDR 0
// #define SCHEDULE_CRC_ADDR 1000
// #define SCHEDULE_COUNT_ADDR 1004
// #define LAST_UPDATE_ADDR 1008

// // ================== GLOBAL VARIABLES ==================
// unsigned long lastHeartbeatTime = 0;
// unsigned long lastScanTime = 0;
// unsigned long lastLedToggle = 0;
// unsigned long lastConnectBlinkToggle = 0;
// unsigned long lastScheduleUpdate = 0;
// unsigned long scheduleUpdateInterval = 3600000;  // Default 1 hour

// int heartbeatFailCount = 0;
// int wifiFailCount = 0;

// bool reprovisionMode = false;
// bool serverUnreachable = false;
// bool ledState = false;
// bool connectBlinkState = false;
// bool isConnecting = false;
// bool isInitialScheduleLoaded = false;

// // ================== STRUCTURES ==================
// struct UserSchedule {
//   int userId;
//   String cardUuid;
//   String userName;
//   int dayOfWeek;
//   String checkInFrom;
//   String checkInTo;
//   String checkOutFrom;
//   String checkOutTo;
// };

// struct StoredData {
//   char lastUpdated[25];
//   int totalUsers;
//   unsigned long nextUpdateTime;
// };

// // ================== GLOBAL VECTORS ==================
// std::vector<UserSchedule> userSchedules;
// StoredData storedData;

// // ================== OBJECTS ==================
// WiFiManager wifiManager;
// MFRC522 rfid(SS_PIN, RST_PIN);

// // ================== CRC32 FUNCTION ==================
// uint32_t calculateCRC32(const uint8_t* data, size_t length) {
//   uint32_t crc = 0xFFFFFFFF;
//   for (size_t i = 0; i < length; i++) {
//     crc ^= data[i];
//     for (int j = 0; j < 8; j++) {
//       if (crc & 1) {
//         crc = (crc >> 1) ^ 0xEDB88320;
//       } else {
//         crc >>= 1;
//       }
//     }
//   }
//   return ~crc;
// }

// // ================== EEPROM FUNCTIONS ==================
// void saveScheduleToEEPROM() {
//   Serial.println("Saving schedules to EEPROM...");

//   DynamicJsonDocument doc(3072);  // 3KB for schedule data
//   JsonArray users = doc.createNestedArray("users");

//   Serial.println("Saving " + String(userSchedules.size()) + " schedules");

//   for (size_t i = 0; i < userSchedules.size(); i++) {
//     const auto& userSched = userSchedules[i];
//     JsonObject user = users.createNestedObject();
//     user["id"] = userSched.userId;
//     user["cardUuid"] = userSched.cardUuid;
//     user["name"] = userSched.userName;
//     user["dayOfWeek"] = userSched.dayOfWeek;
//     user["checkInFrom"] = userSched.checkInFrom;
//     user["checkInTo"] = userSched.checkInTo;
//     user["checkOutFrom"] = userSched.checkOutFrom;
//     user["checkOutTo"] = userSched.checkOutTo;

//     if (i < 3) {  // Log first 3 for debugging
//       Serial.println("  Saving: " + userSched.userName + " (Card: " + userSched.cardUuid + ", Day: " + String(userSched.dayOfWeek) + ")");
//     }
//   }

//   String jsonData;
//   serializeJson(doc, jsonData);

//   // Save JSON data
//   for (size_t i = 0; i < jsonData.length(); i++) {
//     EEPROM.write(SCHEDULE_START_ADDR + i, jsonData[i]);
//   }
//   EEPROM.write(SCHEDULE_START_ADDR + jsonData.length(), '\0');

//   // Save metadata
//   storedData.nextUpdateTime = millis() + scheduleUpdateInterval;

//   // Use current time as last updated since we don't have data variable here
//   String lastUpdatedStr = String(millis());
//   strncpy(storedData.lastUpdated, lastUpdatedStr.c_str(), sizeof(storedData.lastUpdated) - 1);

//   storedData.totalUsers = userSchedules.size();

//   EEPROM.put(LAST_UPDATE_ADDR, storedData);
//   EEPROM.put(SCHEDULE_COUNT_ADDR, userSchedules.size());

//   // Calculate and save CRC
//   uint32_t crc = calculateCRC32((uint8_t*)jsonData.c_str(), jsonData.length());
//   EEPROM.put(SCHEDULE_CRC_ADDR, crc);

//   EEPROM.commit();
//   Serial.println("Schedules saved to EEPROM successfully");
//   Serial.println("Total schedules saved: " + String(userSchedules.size()));
// }

// bool loadScheduleFromEEPROM() {
//   Serial.println("Loading schedules from EEPROM...");

//   // Read metadata first
//   EEPROM.get(LAST_UPDATE_ADDR, storedData);
//   int storedCount = 0;
//   EEPROM.get(SCHEDULE_COUNT_ADDR, storedCount);

//   if (storedCount == 0) {
//     Serial.println("No schedules found in EEPROM");
//     return false;
//   }

//   // Read JSON data
//   String jsonData = "";
//   for (int i = 0; i < 3000; i++) {
//     char c = EEPROM.read(SCHEDULE_START_ADDR + i);
//     if (c == '\0') break;
//     jsonData += c;
//   }

//   if (jsonData.length() == 0) {
//     Serial.println("Empty schedule data in EEPROM");
//     return false;
//   }

//   // Verify CRC
//   uint32_t storedCRC;
//   EEPROM.get(SCHEDULE_CRC_ADDR, storedCRC);
//   uint32_t calculatedCRC = calculateCRC32((uint8_t*)jsonData.c_str(), jsonData.length());

//   if (storedCRC != calculatedCRC) {
//     Serial.println("EEPROM CRC mismatch - data corrupted");
//     return false;
//   }

//   // Parse JSON
//   DynamicJsonDocument doc(3072);
//   DeserializationError error = deserializeJson(doc, jsonData);

//   if (error) {
//     Serial.print("Failed to parse EEPROM JSON: ");
//     Serial.println(error.c_str());
//     return false;
//   }

//   // Load into vector
//   userSchedules.clear();
//   JsonArray users = doc["users"];

//   for (JsonObject user : users) {
//     UserSchedule schedule;
//     schedule.userId = user["id"];
//     schedule.cardUuid = user["cardUuid"].as<String>();
//     schedule.userName = user["name"].as<String>();
//     schedule.dayOfWeek = user["dayOfWeek"];
//     schedule.checkInFrom = user["checkInFrom"].as<String>();
//     schedule.checkInTo = user["checkInTo"].as<String>();
//     schedule.checkOutFrom = user["checkOutFrom"].as<String>();
//     schedule.checkOutTo = user["checkOutTo"].as<String>();

//     userSchedules.push_back(schedule);
//   }

//   Serial.println("Loaded " + String(userSchedules.size()) + " schedules from EEPROM");
//   Serial.println("Last update: " + String(storedData.lastUpdated));

//   return true;
// }

// // ================== SCHEDULE MANAGEMENT ==================
// void fetchAndStoreSchedules() {
//   Serial.println("Fetching schedules from server...");

//   HTTPClient http;
//   WiFiClient client;

//   // Increase timeout and buffer size
//   client.setTimeout(15000);
//   http.setTimeout(15000);
//   http.setReuse(true);

//   String url = String(SERVER_URL) + "/api/v1/device/schedules";
//   Serial.println("URL: " + url);

//   if (!http.begin(client, url)) {
//     Serial.println("HTTP begin failed!");
//     blinkRedTwice();
//     return;
//   }

//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);
//   http.addHeader("Connection", "close");

//   // Get current time for timeout
//   unsigned long startTime = millis();
//   int httpCode = http.GET();
//   unsigned long elapsed = millis() - startTime;

//   Serial.print("HTTP GET took ");
//   Serial.print(elapsed);
//   Serial.println(" ms");

//   if (httpCode > 0) {
//     Serial.printf("HTTP Response code: %d\n", httpCode);

//     if (httpCode == HTTP_CODE_OK) {
//       String response = http.getString();
//       Serial.println("Response length: " + String(response.length()));

//       // Print first 500 chars for debugging
//       if (response.length() > 500) {
//         Serial.println("First 500 chars: " + response.substring(0, 500));
//       } else {
//         Serial.println("Response: " + response);
//       }

//       DynamicJsonDocument doc(4096);
//       DeserializationError error = deserializeJson(doc, response);

//       if (!error) {
//         // Check if response is successful
//         bool success = doc["success"];
//         if (!success) {
//           Serial.println("Server returned error: " + String(doc["message"].as<String>()));
//           blinkRedTwice();
//           http.end();
//           return;
//         }

//         // Get the data object
//         JsonObject data = doc["data"];
//         if (data.isNull()) {
//           Serial.println("Error: No 'data' object in response");
//           blinkRedTwice();
//           http.end();
//           return;
//         }

//         // Check if data is different from current
//         bool dataChanged = true;

//         // Get lastUpdated from data object
//         String newHash = String(data["lastUpdated"].as<String>());
//         String oldHash = String(storedData.lastUpdated);
//         dataChanged = (newHash != oldHash);
//         Serial.println("Data changed: " + String(dataChanged ? "YES" : "NO"));
//         Serial.println("New hash: " + newHash);
//         Serial.println("Old hash: " + oldHash);

//         if (dataChanged) {
//           // Clear existing schedules
//           userSchedules.clear();

//           // Parse users from data object
//           JsonArray users = data["users"];
//           Serial.println("Total users in response: " + String(users.size()));

//           int scheduleCount = 0;
//           int userCount = 0;

//           for (JsonObject user : users) {
//             String cardUuid = user["cardUuid"].as<String>();
//             String userName = user["name"].as<String>();
//             int userId = user["id"];

//             userCount++;
//             Serial.println("\nUser #" + String(userCount) + ": " + userName);
//             Serial.println("  ID: " + String(userId));
//             Serial.println("  Card UUID: " + cardUuid);

//             // Only process users with card UUID
//             if (cardUuid != "null" && cardUuid.length() > 0) {
//               JsonArray schedules = user["schedules"];
//               Serial.println("  Schedules count: " + String(schedules.size()));

//               for (JsonObject schedule : schedules) {
//                 UserSchedule userSchedule;
//                 userSchedule.userId = userId;
//                 userSchedule.cardUuid = cardUuid;
//                 userSchedule.userName = userName;
//                 userSchedule.dayOfWeek = schedule["day"];
//                 userSchedule.checkInFrom = schedule["checkInFrom"].as<String>();
//                 userSchedule.checkInTo = schedule["checkInTo"].as<String>();
//                 userSchedule.checkOutFrom = schedule["checkOutFrom"].as<String>();
//                 userSchedule.checkOutTo = schedule["checkOutTo"].as<String>();

//                 userSchedules.push_back(userSchedule);
//                 scheduleCount++;

//                 Serial.println("    Day " + String(userSchedule.dayOfWeek) + ": " + userSchedule.checkInFrom + " to " + userSchedule.checkInTo);
//               }
//             } else {
//               Serial.println("  Skipped - no card UUID");
//             }
//           }

//           Serial.println("\n=== SUMMARY ===");
//           Serial.println("Total users processed: " + String(userCount));
//           Serial.println("Total schedules added: " + String(scheduleCount));

//           // Save to EEPROM
//           saveScheduleToEEPROM();

//           Serial.println("Schedules updated. Total schedules in memory: " + String(userSchedules.size()));

//           // Show success indication based on count
//           int blinkCount = min(scheduleCount, 5);  // Max 5 blinks
//           for (int i = 0; i < blinkCount; i++) {
//             blinkGreenOnce();
//             delay(150);
//           }

//         } else {
//           Serial.println("Schedules unchanged - no update needed");

//           // Update stored time
//           storedData.nextUpdateTime = millis() + scheduleUpdateInterval;
//           EEPROM.put(LAST_UPDATE_ADDR, storedData);
//           EEPROM.commit();

//           blinkGreenOnce();  // Single blink for success but no change
//         }

//         serverUnreachable = false;

//       } else {
//         Serial.print("Failed to parse schedule JSON: ");
//         Serial.println(error.c_str());
//         blinkRedTwice();
//       }

//     } else {
//       Serial.println("Server returned error: " + String(httpCode));
//       blinkRedTwice();
//     }

//   } else {
//     Serial.print("HTTP GET failed: ");
//     Serial.println(http.errorToString(httpCode));
//     blinkRedTwice();
//   }

//   http.end();
//   Serial.println("HTTP connection closed\n");
// }

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   delay(1000);  // Serial stable hone ka wait karo

//   Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

//   // EEPROM INITIALIZE
//   EEPROM.begin(EEPROM_SIZE);

//   // Load existing schedules from EEPROM first
//   if (loadScheduleFromEEPROM()) {
//     Serial.println("Using stored schedules from EEPROM");
//     isInitialScheduleLoaded = true;
//   }

//   // ================== PCF8574 INIT ==================
//   Wire.begin(D1, D2);  // SDA, SCL
//   pcf.begin();
//   // LED PINS SETUP
//   // pcf.pinMode(WHITE_LED_PIN, OUTPUT);
//   // pcf.pinMode(BLUE_LED_PIN, OUTPUT);
//   // pcf.pinMode(GREEN_LED_PIN, OUTPUT);
//   // pcf.pinMode(RED_LED_PIN, OUTPUT);

//   // INITIAL LED STATES - SAB OFF KARO
//   pcf.write(WHITE_LED_PIN, LOW);
//   pcf.write(BLUE_LED_PIN, LOW);
//   pcf.write(GREEN_LED_PIN, LOW);
//   pcf.write(RED_LED_PIN, LOW);

//   // WHITE LED ON (Booting status)
//   pcf.write(WHITE_LED_PIN, HIGH);

//   // CLEAN WIFI STATE
//   WiFi.disconnect(true);
//   delay(100);
//   WiFi.mode(WIFI_STA);
//   delay(100);

//   // RFID INITIALIZE
//   SPI.begin();
//   rfid.PCD_Init();
//   Serial.println("RFID Module Initialized");

//   // WIFI MANAGER SETUP
//   wifiManager.setDebugOutput(false);
//   wifiManager.setShowInfoErase(false);
//   wifiManager.setShowInfoUpdate(false);

//   std::vector<const char*> menu = { "wifi", "exit" };
//   wifiManager.setMenu(menu);

//   wifiManager.setAPCallback([](WiFiManager* wm) {
//     Serial.println("Config Portal Started");
//     reprovisionMode = true;
//     // Config portal mein white LED still ON
//     pcf.write(WHITE_LED_PIN, HIGH);
//   });

//   wifiManager.setConnectTimeout(30);        // 30 seconds connection try
//   wifiManager.setConfigPortalTimeout(180);  // 3 minutes portal timeout

//   Serial.println("Attempting WiFi connection...");

//   // CONNECT TO WIFI
//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     Serial.println("WiFi connection failed! Auto-restarting...");

//     // 5 seconds wait aur phir restart
//     delay(5000);
//     ESP.restart();  // AUTO RESTART - NO BUTTON NEEDED
//     delay(5000);
//   }

//   // SUCCESS - WiFi connected
//   Serial.println("WiFi Connected!");
//   Serial.print("IP Address: ");
//   Serial.println(WiFi.localIP());

//   // IMPORTANT: Fetch schedules immediately after WiFi connection
//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Fetching fresh schedules from server...");
//     fetchAndStoreSchedules();
//   } else {
//     Serial.println("WiFi not connected, using EEPROM schedules");
//   }

//   // Ab white LED blink karegi (connected status)
//   reprovisionMode = false;
//   lastHeartbeatTime = millis();
//   lastScanTime = millis();
//   lastScheduleUpdate = millis();

//   Serial.println("Setup complete!");
// }

// // ================== LOOP ==================
// void loop() {
//   // ----------------- WHITE LED STATUS -----------------
//   if (reprovisionMode) {
//     // Config portal mode - White LED ON (still)
//     pcf.write(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     // WiFi connected - White LED BLINK (slow)
//     blinkWhiteLED();
//   } else {
//     // WiFi not connected - White LED ON (still)
//     pcf.write(WHITE_LED_PIN, HIGH);
//   }

//   // ----------------- BLUE LED (SERVER STATUS) -----------------
//   pcf.write(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   // ----------------- PERIODIC SCHEDULE UPDATE -----------------
//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//     if (millis() - lastScheduleUpdate >= scheduleUpdateInterval) {
//       fetchAndStoreSchedules();
//       lastScheduleUpdate = millis();
//     }
//   }

//   // ----------------- HEARTBEAT + WIFI SCAN -----------------
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

//     // ----------------- RFID ATTENDANCE -----------------
//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       String cardUuid = "";
//       for (byte i = 0; i < rfid.uid.size; i++) {
//         cardUuid += String(rfid.uid.uidByte[i], HEX);
//       }
//       cardUuid.toUpperCase();

//       Serial.println("Card scanned: " + cardUuid);

//       // First check in local schedule (if available)
//       if (userSchedules.size() > 0) {
//         bool foundInSchedule = false;
//         for (const auto& schedule : userSchedules) {
//           if (schedule.cardUuid == cardUuid) {
//             foundInSchedule = true;
//             Serial.println("User found in local schedule: " + schedule.userName);
//             break;
//           }
//         }

//         if (!foundInSchedule) {
//           Serial.println("Card not found in local schedule");
//           blinkRedTwice();
//           rfid.PICC_HaltA();
//           rfid.PCD_StopCrypto1();
//           delay(1200);
//           return;
//         }
//       }

//       bool accepted = sendAttendance(cardUuid);
//       if (accepted) blinkGreenOnce();  // RFID Success - Green blink
//       else blinkRedTwice();            // RFID Fail - Red blink

//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();

//       delay(1200);  // anti-double tap
//     }

//   } else {
//     // WiFi disconnected
//     wifiFailCount++;

//     // Try reconnecting after some failures
//     if (wifiFailCount > 10) {
//       Serial.println("WiFi disconnected. Attempting reconnect...");
//       WiFi.reconnect();
//       delay(5000);

//       if (WiFi.status() != WL_CONNECTED) {
//         checkReprovision();
//       } else {
//         wifiFailCount = 0;
//       }
//     }
//   }

//   delay(30);
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
//   doc["schedulesLoaded"] = userSchedules.size();
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

// // ================== COMMAND ==================
// void handleCommand(String json) {
//   DynamicJsonDocument doc(512);
//   if (deserializeJson(doc, json)) return;

//   const char* command = doc["data"]["command"];
//   if (command && strcmp(command, "CONNECT_WIFI") == 0) {
//     connectToWifi(
//       doc["data"]["payload"]["ssid"],
//       doc["data"]["payload"]["password"]);
//   } else if (command && strcmp(command, "UPDATE_SCHEDULES") == 0) {
//     // Force schedule update from server
//     Serial.println("Force schedule update requested from server");
//     if (WiFi.status() == WL_CONNECTED) {
//       fetchAndStoreSchedules();
//     }
//   }
// }

// // ================== CONNECT WIFI ==================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);
//   isConnecting = true;
//   WiFi.disconnect(true);
//   delay(500);

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);

//   // Connecting ke dauraan white LED fast blink
//   int tries = 0;
//   while (WiFi.status() != WL_CONNECTED && tries < 25) {
//     // Fast blink while connecting
//     pcf.write(WHITE_LED_PIN, HIGH);
//     delay(100);
//     pcf.write(WHITE_LED_PIN, LOW);
//     delay(100);
//     tries++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Connected to new WiFi");
//     // Fetch schedules after new WiFi connection
//     fetchAndStoreSchedules();
//     isConnecting = false;
//     return;
//   }

//   Serial.println("New WiFi failed — rebooting");
//   delay(1000);
//   ESP.restart();
// }

// // ================== REPROVISION ==================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
//     Serial.println("All WiFi failed — AP Mode");
//     reprovisionMode = true;
//     pcf.write(WHITE_LED_PIN, HIGH);  // White LED ON (still)

//     // Reset WiFi fail count
//     wifiFailCount = 0;

//     // Start config portal
//     if (!wifiManager.startConfigPortal("RFID_Device_001", "12345678")) {
//       Serial.println("Portal timeout, restarting...");
//       delay(3000);
//       ESP.restart();
//     }

//     reprovisionMode = false;
//   }
// }

// // ================== SERVER FAIL ==================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) {
//     serverUnreachable = true;
//     Serial.println("Server unreachable!");
//   }
// }

// // ================== WIFI SCAN ==================
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

// // ================== LED FUNCTIONS ==================
// void blinkWhiteLED() {
//   // WiFi connected - Slow blink (300ms ON, 300ms OFF)
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     pcf.write(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }

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

//   // Read server response
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

// void blinkGreenOnce() {
//   // RFID Success - Green blink once
//   pcf.write(GREEN_LED_PIN, HIGH);
//   delay(120);
//   pcf.write(GREEN_LED_PIN, LOW);
// }

// void blinkRedTwice() {
//   // RFID Fail - Red blink twice
//   for (int i = 0; i < 2; i++) {
//     pcf.write(RED_LED_PIN, HIGH);
//     delay(120);
//     pcf.write(RED_LED_PIN, LOW);
//     delay(120);
//   }
// }





// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>
// #include <EEPROM.h>

// // ================== EXISTING LED PINS ==================
// #define WHITE_LED_PIN D0  // Main Status LED
// #define BLUE_LED_PIN D8   // Server Status

// // ================== RFID + GREEN/RED LED PINS ==================
// #define GREEN_LED_PIN D1  // RFID Success
// #define RED_LED_PIN D2    // RFID Fail
// #define RST_PIN D3
// #define SS_PIN D4

// // ================== CONFIG ==================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define SCHEDULE_UPDATE_INTERVAL 86400000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
// const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// // ================== EEPROM CONFIG ==================
// #define EEPROM_SIZE 4096
// #define SCHEDULE_START_ADDR 0
// #define SCHEDULE_CRC_ADDR 1000
// #define SCHEDULE_COUNT_ADDR 1004
// #define LAST_UPDATE_ADDR 1008

// // ================== GLOBAL VARIABLES ==================
// unsigned long lastHeartbeatTime = 0;
// unsigned long lastScanTime = 0;
// unsigned long lastLedToggle = 0;
// unsigned long lastConnectBlinkToggle = 0;
// unsigned long lastScheduleUpdate = 0;
// unsigned long scheduleUpdateInterval = 3600000;  // Default 1 hour

// int heartbeatFailCount = 0;
// int wifiFailCount = 0;

// bool reprovisionMode = false;
// bool serverUnreachable = false;
// bool ledState = false;
// bool connectBlinkState = false;
// bool isConnecting = false;
// bool isInitialScheduleLoaded = false;

// // ================== STRUCTURES ==================
// struct UserSchedule {
//   int id;
//   String cardUuid;
//   String name;
//   int dayOfWeek;
//   String checkInFrom;
//   String checkInTo;
//   String checkOutFrom;
//   String checkOutTo;
// };

// struct StoredData {
//   char lastUpdated[25];
//   int totalEmployees;
//   unsigned long nextUpdateTime;
// };

// // ================== GLOBAL VECTORS ==================
// std::vector<UserSchedule> userSchedules;
// StoredData storedData;

// // ================== OBJECTS ==================
// WiFiManager wifiManager;
// MFRC522 rfid(SS_PIN, RST_PIN);

// // ================== CRC32 FUNCTION ==================
// uint32_t calculateCRC32(const uint8_t* data, size_t length) {
//   uint32_t crc = 0xFFFFFFFF;
//   for (size_t i = 0; i < length; i++) {
//     crc ^= data[i];
//     for (int j = 0; j < 8; j++) {
//       if (crc & 1) {
//         crc = (crc >> 1) ^ 0xEDB88320;
//       } else {
//         crc >>= 1;
//       }
//     }
//   }
//   return ~crc;
// }

// // ================== EEPROM FUNCTIONS ==================
// void saveScheduleToEEPROM() {
//   Serial.println("Saving user schedules to EEPROM...");

//   DynamicJsonDocument doc(3072);
//   JsonArray users = doc.createNestedArray("users");

//   Serial.println("Saving " + String(userSchedules.size()) + " user schedules");

//   for (size_t i = 0; i < userSchedules.size(); i++) {
//     const auto& user = userSchedules[i];
//     JsonObject userObj = users.createNestedObject();
//     userObj["id"] = user.id;
//     userObj["cardUuid"] = user.cardUuid;
//     userObj["name"] = user.name;
//     userObj["dayOfWeek"] = user.dayOfWeek;
//     userObj["checkInFrom"] = user.checkInFrom;
//     userObj["checkInTo"] = user.checkInTo;
//     userObj["checkOutFrom"] = user.checkOutFrom;
//     userObj["checkOutTo"] = user.checkOutTo;

//     if (i < 3) {
//       Serial.println("  Saving user: " + user.name + " (Card: " + user.cardUuid + ", Day: " + String(user.dayOfWeek) + ")");
//     }
//   }

//   String jsonData;
//   serializeJson(doc, jsonData);

//   // Save JSON data
//   for (size_t i = 0; i < jsonData.length(); i++) {
//     EEPROM.write(SCHEDULE_START_ADDR + i, jsonData[i]);
//   }
//   EEPROM.write(SCHEDULE_START_ADDR + jsonData.length(), '\0');

//   // Save metadata - YAHAN CHANGE HAI
//   storedData.nextUpdateTime = millis() + scheduleUpdateInterval;

//   // Use current time as last updated
//   String lastUpdatedStr = String(millis());
//   strncpy(storedData.lastUpdated, lastUpdatedStr.c_str(), sizeof(storedData.lastUpdated) - 1);

//   // YAHAN CHANGE HAI: employeeSchedules se userSchedules
//   storedData.totalEmployees = userSchedules.size();

//   EEPROM.put(LAST_UPDATE_ADDR, storedData);
//   EEPROM.put(SCHEDULE_COUNT_ADDR, userSchedules.size());  // Yahan bhi change

//   // Calculate and save CRC
//   uint32_t crc = calculateCRC32((uint8_t*)jsonData.c_str(), jsonData.length());
//   EEPROM.put(SCHEDULE_CRC_ADDR, crc);

//   EEPROM.commit();
//   Serial.println("User schedules saved to EEPROM successfully");
//   Serial.println("Total users saved: " + String(userSchedules.size()));
// }

// bool loadScheduleFromEEPROM() {
//   Serial.println("Loading user schedules from EEPROM...");

//   // Read metadata
//   EEPROM.get(LAST_UPDATE_ADDR, storedData);
//   int storedCount = 0;
//   EEPROM.get(SCHEDULE_COUNT_ADDR, storedCount);

//   if (storedCount == 0) {
//     Serial.println("No user schedules found in EEPROM");
//     return false;
//   }

//   // Read JSON data
//   String jsonData = "";
//   for (int i = 0; i < 3000; i++) {
//     char c = EEPROM.read(SCHEDULE_START_ADDR + i);
//     if (c == '\0') break;
//     jsonData += c;
//   }

//   if (jsonData.length() == 0) {
//     Serial.println("Empty user schedule data in EEPROM");
//     return false;
//   }

//   // Verify CRC
//   uint32_t storedCRC;
//   EEPROM.get(SCHEDULE_CRC_ADDR, storedCRC);
//   uint32_t calculatedCRC = calculateCRC32((uint8_t*)jsonData.c_str(), jsonData.length());

//   if (storedCRC != calculatedCRC) {
//     Serial.println("EEPROM CRC mismatch - data corrupted");
//     return false;
//   }

//   // Parse JSON
//   DynamicJsonDocument doc(3072);
//   DeserializationError error = deserializeJson(doc, jsonData);

//   if (error) {
//     Serial.print("Failed to parse EEPROM JSON: ");
//     Serial.println(error.c_str());
//     return false;
//   }

//   // Load into vector
//   userSchedules.clear();
//   JsonArray users = doc["users"];  // "employees" se "users"

//   for (JsonObject user : users) {  // employee se user
//     UserSchedule schedule;         // EmployeeSchedule se UserSchedule
//     schedule.id = user["id"];
//     schedule.cardUuid = user["cardUuid"].as<String>();
//     schedule.name = user["name"].as<String>();
//     schedule.dayOfWeek = user["dayOfWeek"];
//     schedule.checkInFrom = user["checkInFrom"].as<String>();
//     schedule.checkInTo = user["checkInTo"].as<String>();
//     schedule.checkOutFrom = user["checkOutFrom"].as<String>();
//     schedule.checkOutTo = user["checkOutTo"].as<String>();

//     userSchedules.push_back(schedule);
//   }

//   Serial.println("Loaded " + String(userSchedules.size()) + " user schedules from EEPROM");
//   Serial.println("Last update: " + String(storedData.lastUpdated));

//   return true;
// }

// // ================== SCHEDULE MANAGEMENT ==================
// void fetchAndStoreSchedules() {
//   Serial.println("Fetching user schedules from server...");

//   HTTPClient http;
//   WiFiClient client;

//   client.setTimeout(15000);
//   http.setTimeout(15000);
//   http.setReuse(true);

//   String url = String(SERVER_URL) + "/api/v1/device/schedules";
//   Serial.println("URL: " + url);

//   if (!http.begin(client, url)) {
//     Serial.println("HTTP begin failed!");
//     blinkRedTwice();
//     return;
//   }

//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);
//   http.addHeader("Connection", "close");

//   unsigned long startTime = millis();
//   int httpCode = http.GET();
//   unsigned long elapsed = millis() - startTime;

//   Serial.print("HTTP GET took ");
//   Serial.print(elapsed);
//   Serial.println(" ms");

//   if (httpCode > 0) {
//     Serial.printf("HTTP Response code: %d\n", httpCode);

//     if (httpCode == HTTP_CODE_OK) {
//       String response = http.getString();
//       Serial.println("Response length: " + String(response.length()));

//       // Print for debugging
//       if (response.length() > 500) {
//         Serial.println("First 500 chars: " + response.substring(0, 500));
//       }

//       DynamicJsonDocument doc(4096);
//       DeserializationError error = deserializeJson(doc, response);

//       if (!error) {
//         bool success = doc["success"];
//         if (!success) {
//           Serial.println("Server error: " + String(doc["message"].as<String>()));
//           blinkRedTwice();
//           http.end();
//           return;
//         }

//         JsonObject data = doc["data"];
//         if (data.isNull()) {
//           Serial.println("Error: No 'data' object");
//           blinkRedTwice();
//           http.end();
//           return;
//         }

//         // Check if data changed
//         String newHash = String(data["lastUpdated"].as<String>());
//         String oldHash = String(storedData.lastUpdated);
//         bool dataChanged = (newHash != oldHash);

//         Serial.println("Data changed: " + String(dataChanged ? "YES" : "NO"));
//         Serial.println("New: " + newHash + " | Old: " + oldHash);

//         if (dataChanged) {
//           // Clear existing
//           userSchedules.clear();

//           // Parse users array
//           JsonArray users = data["users"];
//           Serial.println("Total users in response: " + String(users.size()));

//           int scheduleCount = 0;
//           int userCount = 0;

//           for (JsonObject user : users) {
//             // Get card UUID from cards array
//             String cardUuid = "";
//             if (user.containsKey("cards") && user["cards"].is<JsonArray>()) {
//               JsonArray cards = user["cards"];
//               if (cards.size() > 0 && cards[0].containsKey("uuid")) {
//                 cardUuid = cards[0]["uuid"].as<String>();
//               }
//             }

//             String name = user["name"].as<String>();
//             int userId = user["id"];

//             userCount++;
//             Serial.println("\nUser #" + String(userCount) + ": " + name);
//             Serial.println("  ID: " + String(userId));
//             Serial.println("  Card: " + cardUuid);

//             // Process only users with card
//             if (cardUuid != "null" && cardUuid.length() > 0) {
//               JsonArray schedules = user["schedules"];
//               Serial.println("  Schedules: " + String(schedules.size()));

//               for (JsonObject schedule : schedules) {
//                 UserSchedule userSchedule;
//                 userSchedule.id = userId;
//                 userSchedule.cardUuid = cardUuid;
//                 userSchedule.name = name;
//                 userSchedule.dayOfWeek = schedule["day"];
//                 userSchedule.checkInFrom = schedule["checkInFrom"].as<String>();
//                 userSchedule.checkInTo = schedule["checkInTo"].as<String>();
//                 userSchedule.checkOutFrom = schedule["checkOutFrom"].as<String>();
//                 userSchedule.checkOutTo = schedule["checkOutTo"].as<String>();

//                 userSchedules.push_back(userSchedule);
//                 scheduleCount++;

//                 Serial.println("    Day " + String(userSchedule.dayOfWeek) + ": " + userSchedule.checkInFrom + "-" + userSchedule.checkInTo);
//               }
//             } else {
//               Serial.println("  Skipped - no card");
//             }
//           }

//           Serial.println("\n=== SUMMARY ===");
//           Serial.println("Users processed: " + String(userCount));
//           Serial.println("Schedules added: " + String(scheduleCount));

//           // Save to EEPROM
//           saveScheduleToEEPROM();

//           Serial.println("Total user schedules: " + String(userSchedules.size()));

//           // Success blink
//           int blinkCount = min(scheduleCount, 5);
//           for (int i = 0; i < blinkCount; i++) {
//             blinkGreenOnce();
//             delay(150);
//           }

//         } else {
//           Serial.println("Schedules unchanged");

//           storedData.nextUpdateTime = millis() + scheduleUpdateInterval;
//           EEPROM.put(LAST_UPDATE_ADDR, storedData);
//           EEPROM.commit();

//           blinkGreenOnce();
//         }

//         serverUnreachable = false;

//       } else {
//         Serial.print("JSON parse error: ");
//         Serial.println(error.c_str());
//         blinkRedTwice();
//       }

//     } else {
//       Serial.println("HTTP error: " + String(httpCode));
//       blinkRedTwice();
//     }

//   } else {
//     Serial.print("HTTP GET failed: ");
//     Serial.println(http.errorToString(httpCode));
//     blinkRedTwice();
//   }

//   http.end();
//   Serial.println("HTTP connection closed\n");
// }

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   delay(1000);  // Serial stable hone ka wait karo

//   Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

//   // EEPROM INITIALIZE
//   EEPROM.begin(EEPROM_SIZE);

//   // Load existing schedules from EEPROM first
//   if (loadScheduleFromEEPROM()) {
//     Serial.println("Using stored schedules from EEPROM");
//     isInitialScheduleLoaded = true;
//   }

//   // LED PINS SETUP
//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);
//   pinMode(GREEN_LED_PIN, OUTPUT);
//   pinMode(RED_LED_PIN, OUTPUT);

//   // INITIAL LED STATES - SAB OFF KARO
//   digitalWrite(WHITE_LED_PIN, LOW);
//   digitalWrite(BLUE_LED_PIN, LOW);
//   digitalWrite(GREEN_LED_PIN, LOW);
//   digitalWrite(RED_LED_PIN, LOW);

//   // WHITE LED ON (Booting status)
//   digitalWrite(WHITE_LED_PIN, HIGH);

//   // CLEAN WIFI STATE
//   WiFi.disconnect(true);
//   delay(100);
//   WiFi.mode(WIFI_STA);
//   delay(100);

//   // RFID INITIALIZE
//   SPI.begin();
//   rfid.PCD_Init();
//   Serial.println("RFID Module Initialized");

//   // WIFI MANAGER SETUP
//   wifiManager.setDebugOutput(false);
//   wifiManager.setShowInfoErase(false);
//   wifiManager.setShowInfoUpdate(false);

//   std::vector<const char*> menu = { "wifi", "exit" };
//   wifiManager.setMenu(menu);

//   wifiManager.setAPCallback([](WiFiManager* wm) {
//     Serial.println("Config Portal Started");
//     reprovisionMode = true;
//     // Config portal mein white LED still ON
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   });

//   wifiManager.setConnectTimeout(30);        // 30 seconds connection try
//   wifiManager.setConfigPortalTimeout(180);  // 3 minutes portal timeout

//   Serial.println("Attempting WiFi connection...");

//   // CONNECT TO WIFI
//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     Serial.println("WiFi connection failed! Auto-restarting...");

//     // 5 seconds wait aur phir restart
//     delay(5000);
//     ESP.restart();  // AUTO RESTART - NO BUTTON NEEDED
//     delay(5000);
//   }

//   // SUCCESS - WiFi connected
//   Serial.println("WiFi Connected!");
//   Serial.print("IP Address: ");
//   Serial.println(WiFi.localIP());

//   // IMPORTANT: Fetch schedules immediately after WiFi connection
//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Fetching fresh schedules from server...");
//     fetchAndStoreSchedules();
//   } else {
//     Serial.println("WiFi not connected, using EEPROM schedules");
//   }

//   // Ab white LED blink karegi (connected status)
//   reprovisionMode = false;
//   lastHeartbeatTime = millis();
//   lastScanTime = millis();
//   lastScheduleUpdate = millis();

//   Serial.println("Setup complete!");
// }

// // ================== LOOP ==================
// void loop() {
//   // ----------------- WHITE LED STATUS -----------------
//   if (reprovisionMode) {
//     // Config portal mode - White LED ON (still)
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     // WiFi connected - White LED BLINK (slow)
//     blinkWhiteLED();
//   } else {
//     // WiFi not connected - White LED ON (still)
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // ----------------- BLUE LED (SERVER STATUS) -----------------
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   // ----------------- PERIODIC SCHEDULE UPDATE -----------------
//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//     if (millis() - lastScheduleUpdate >= scheduleUpdateInterval) {
//       fetchAndStoreSchedules();
//       lastScheduleUpdate = millis();
//     }
//   }

//   // ----------------- HEARTBEAT + WIFI SCAN -----------------
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

//     // ----------------- RFID ATTENDANCE -----------------
//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       String cardUuid = "";
//       for (byte i = 0; i < rfid.uid.size; i++) {
//         cardUuid += String(rfid.uid.uidByte[i], HEX);
//       }
//       cardUuid.toUpperCase();

//       Serial.println("Card scanned: " + cardUuid);

//       // Check in local user schedules
//       if (userSchedules.size() > 0) {
//         bool foundInSchedule = false;
//         String userName = "";

//         for (const auto& userSchedule : userSchedules) {
//           if (userSchedule.cardUuid == cardUuid) {
//             foundInSchedule = true;
//             userName = userSchedule.name;
//             Serial.println("User found: " + userName);
//             break;
//           }
//         }

//         if (!foundInSchedule) {
//           Serial.println("Card not found in user schedules");
//           blinkRedTwice();
//           rfid.PICC_HaltA();
//           rfid.PCD_StopCrypto1();
//           delay(1200);
//           return;
//         }
//       }

//       bool accepted = sendAttendance(cardUuid);
//       if (accepted) blinkGreenOnce();
//       else blinkRedTwice();

//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();
//       delay(1200);
//     }

//   } else {
//     // WiFi disconnected
//     wifiFailCount++;

//     // Try reconnecting after some failures
//     if (wifiFailCount > 10) {
//       Serial.println("WiFi disconnected. Attempting reconnect...");
//       WiFi.reconnect();
//       delay(5000);

//       if (WiFi.status() != WL_CONNECTED) {
//         checkReprovision();
//       } else {
//         wifiFailCount = 0;
//       }
//     }
//   }

//   delay(30);
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
//   doc["usersSchedulesLoaded"] = userSchedules.size();
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

// // ================== COMMAND ==================
// void handleCommand(String json) {
//   DynamicJsonDocument doc(512);
//   if (deserializeJson(doc, json)) return;

//   const char* command = doc["data"]["command"];
//   if (command && strcmp(command, "CONNECT_WIFI") == 0) {
//     connectToWifi(
//       doc["data"]["payload"]["ssid"],
//       doc["data"]["payload"]["password"]);
//   } else if (command && strcmp(command, "UPDATE_SCHEDULES") == 0) {
//     // Force schedule update from server
//     Serial.println("Force schedule update requested from server");
//     if (WiFi.status() == WL_CONNECTED) {
//       fetchAndStoreSchedules();
//     }
//   }
// }

// // ================== CONNECT WIFI ==================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);
//   isConnecting = true;
//   WiFi.disconnect(true);
//   delay(500);

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);

//   // Connecting ke dauraan white LED fast blink
//   int tries = 0;
//   while (WiFi.status() != WL_CONNECTED && tries < 25) {
//     // Fast blink while connecting
//     digitalWrite(WHITE_LED_PIN, HIGH);
//     delay(100);
//     digitalWrite(WHITE_LED_PIN, LOW);
//     delay(100);
//     tries++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Connected to new WiFi");
//     // Fetch schedules after new WiFi connection
//     fetchAndStoreSchedules();
//     isConnecting = false;
//     return;
//   }

//   Serial.println("New WiFi failed — rebooting");
//   delay(1000);
//   ESP.restart();
// }

// // ================== REPROVISION ==================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
//     Serial.println("All WiFi failed — AP Mode");
//     reprovisionMode = true;
//     digitalWrite(WHITE_LED_PIN, HIGH);  // White LED ON (still)

//     // Reset WiFi fail count
//     wifiFailCount = 0;

//     // Start config portal
//     if (!wifiManager.startConfigPortal("RFID_Device_001", "12345678")) {
//       Serial.println("Portal timeout, restarting...");
//       delay(3000);
//       ESP.restart();
//     }

//     reprovisionMode = false;
//   }
// }

// // ================== SERVER FAIL ==================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) {
//     serverUnreachable = true;
//     Serial.println("Server unreachable!");
//   }
// }

// // ================== WIFI SCAN ==================
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

// // ================== LED FUNCTIONS ==================
// void blinkWhiteLED() {
//   // WiFi connected - Slow blink (300ms ON, 300ms OFF)
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }

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

//   // Read server response
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

// void blinkGreenOnce() {
//   // RFID Success - Green blink once
//   digitalWrite(GREEN_LED_PIN, HIGH);
//   delay(120);
//   digitalWrite(GREEN_LED_PIN, LOW);
// }

// void blinkRedTwice() {
//   // RFID Fail - Red blink twice
//   for (int i = 0; i < 2; i++) {
//     digitalWrite(RED_LED_PIN, HIGH);
//     delay(120);
//     digitalWrite(RED_LED_PIN, LOW);
//     delay(120);
//   }
// }







// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>
// #include <EEPROM.h>

// // ================== EXISTING LED PINS ==================
// #define WHITE_LED_PIN D0  // Main Status LED
// #define BLUE_LED_PIN D8   // Server Status

// // ================== RFID + GREEN/RED LED PINS ==================
// #define GREEN_LED_PIN D1  // RFID Success
// #define RED_LED_PIN D2    // RFID Fail
// #define RST_PIN D3
// #define SS_PIN D4

// // ================== CONFIG ==================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define SCHEDULE_UPDATE_INTERVAL 86400000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
// const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// // ================== EEPROM CONFIG ==================
// #define EEPROM_SIZE 4096
// #define SCHEDULE_START_ADDR 0
// #define SCHEDULE_CRC_ADDR 1000
// #define SCHEDULE_COUNT_ADDR 1004
// #define LAST_UPDATE_ADDR 1008

// // ================== GLOBAL VARIABLES ==================
// unsigned long lastHeartbeatTime = 0;
// unsigned long lastScanTime = 0;
// unsigned long lastLedToggle = 0;
// unsigned long lastConnectBlinkToggle = 0;
// unsigned long lastScheduleUpdate = 0;
// unsigned long scheduleUpdateInterval = 3600000;  // Default 1 hour

// int heartbeatFailCount = 0;
// int wifiFailCount = 0;

// bool reprovisionMode = false;
// bool serverUnreachable = false;
// bool ledState = false;
// bool connectBlinkState = false;
// bool isConnecting = false;
// bool isInitialScheduleLoaded = false;

// // ================== STRUCTURES ==================
// struct EmployeeSchedule {
//   int id;
//   String cardUuid;
//   String name;
//   int dayOfWeek;
//   String checkInFrom;
//   String checkInTo;
//   String checkOutFrom;
//   String checkOutTo;
// };

// struct StoredData {
//   char lastUpdated[25];
//   int totalEmployees;
//   unsigned long nextUpdateTime;
// };

// // ================== GLOBAL VECTORS ==================
// std::vector<EmployeeSchedule> employeeSchedules;
// StoredData storedData;

// // ================== OBJECTS ==================
// WiFiManager wifiManager;
// MFRC522 rfid(SS_PIN, RST_PIN);

// // ================== CRC32 FUNCTION ==================
// uint32_t calculateCRC32(const uint8_t* data, size_t length) {
//   uint32_t crc = 0xFFFFFFFF;
//   for (size_t i = 0; i < length; i++) {
//     crc ^= data[i];
//     for (int j = 0; j < 8; j++) {
//       if (crc & 1) {
//         crc = (crc >> 1) ^ 0xEDB88320;
//       } else {
//         crc >>= 1;
//       }
//     }
//   }
//   return ~crc;
// }

// // ================== EEPROM FUNCTIONS ==================
// void saveScheduleToEEPROM() {
//   Serial.println("Saving schedules to EEPROM...");

//   DynamicJsonDocument doc(3072);  // 3KB for schedule data
//   JsonArray employees = doc.createNestedArray("employees");

//   Serial.println("Saving " + String(employeeSchedules.size()) + " schedules");

//   for (size_t i = 0; i < employeeSchedules.size(); i++) {
//     const auto& emp = employeeSchedules[i];
//     JsonObject employee = employees.createNestedObject();
//     employee["id"] = emp.id;
//     employee["cardUuid"] = emp.cardUuid;
//     employee["name"] = emp.name;
//     employee["dayOfWeek"] = emp.dayOfWeek;
//     employee["checkInFrom"] = emp.checkInFrom;
//     employee["checkInTo"] = emp.checkInTo;
//     employee["checkOutFrom"] = emp.checkOutFrom;
//     employee["checkOutTo"] = emp.checkOutTo;

//     if (i < 3) {  // Log first 3 for debugging
//       Serial.println("  Saving: " + emp.name + " (Card: " + emp.cardUuid + ", Day: " + String(emp.dayOfWeek) + ")");
//     }
//   }

//   String jsonData;
//   serializeJson(doc, jsonData);

//   // Save JSON data
//   for (size_t i = 0; i < jsonData.length(); i++) {
//     EEPROM.write(SCHEDULE_START_ADDR + i, jsonData[i]);
//   }
//   EEPROM.write(SCHEDULE_START_ADDR + jsonData.length(), '\0');

//   // Save metadata
//   storedData.nextUpdateTime = millis() + scheduleUpdateInterval;

//   // Use current time as last updated since we don't have data variable here
//   String lastUpdatedStr = String(millis());
//   strncpy(storedData.lastUpdated, lastUpdatedStr.c_str(), sizeof(storedData.lastUpdated) - 1);

//   storedData.totalEmployees = employeeSchedules.size();

//   EEPROM.put(LAST_UPDATE_ADDR, storedData);
//   EEPROM.put(SCHEDULE_COUNT_ADDR, employeeSchedules.size());

//   // Calculate and save CRC
//   uint32_t crc = calculateCRC32((uint8_t*)jsonData.c_str(), jsonData.length());
//   EEPROM.put(SCHEDULE_CRC_ADDR, crc);

//   EEPROM.commit();
//   Serial.println("Schedules saved to EEPROM successfully");
//   Serial.println("Total schedules saved: " + String(employeeSchedules.size()));
// }

// bool loadScheduleFromEEPROM() {
//   Serial.println("Loading schedules from EEPROM...");

//   // Read metadata first
//   EEPROM.get(LAST_UPDATE_ADDR, storedData);
//   int storedCount = 0;
//   EEPROM.get(SCHEDULE_COUNT_ADDR, storedCount);

//   if (storedCount == 0) {
//     Serial.println("No schedules found in EEPROM");
//     return false;
//   }

//   // Read JSON data
//   String jsonData = "";
//   for (int i = 0; i < 3000; i++) {
//     char c = EEPROM.read(SCHEDULE_START_ADDR + i);
//     if (c == '\0') break;
//     jsonData += c;
//   }

//   if (jsonData.length() == 0) {
//     Serial.println("Empty schedule data in EEPROM");
//     return false;
//   }

//   // Verify CRC
//   uint32_t storedCRC;
//   EEPROM.get(SCHEDULE_CRC_ADDR, storedCRC);
//   uint32_t calculatedCRC = calculateCRC32((uint8_t*)jsonData.c_str(), jsonData.length());

//   if (storedCRC != calculatedCRC) {
//     Serial.println("EEPROM CRC mismatch - data corrupted");
//     return false;
//   }

//   // Parse JSON
//   DynamicJsonDocument doc(3072);
//   DeserializationError error = deserializeJson(doc, jsonData);

//   if (error) {
//     Serial.print("Failed to parse EEPROM JSON: ");
//     Serial.println(error.c_str());
//     return false;
//   }

//   // Load into vector
//   employeeSchedules.clear();
//   JsonArray employees = doc["employees"];

//   for (JsonObject employee : employees) {
//     EmployeeSchedule schedule;
//     schedule.id = employee["id"];
//     schedule.cardUuid = employee["cardUuid"].as<String>();
//     schedule.name = employee["name"].as<String>();
//     schedule.dayOfWeek = employee["dayOfWeek"];
//     schedule.checkInFrom = employee["checkInFrom"].as<String>();
//     schedule.checkInTo = employee["checkInTo"].as<String>();
//     schedule.checkOutFrom = employee["checkOutFrom"].as<String>();
//     schedule.checkOutTo = employee["checkOutTo"].as<String>();

//     employeeSchedules.push_back(schedule);
//   }

//   Serial.println("Loaded " + String(employeeSchedules.size()) + " schedules from EEPROM");
//   Serial.println("Last update: " + String(storedData.lastUpdated));

//   return true;
// }

// // ================== SCHEDULE MANAGEMENT ==================
// void fetchAndStoreSchedules() {
//   Serial.println("Fetching schedules from server...");

//   HTTPClient http;
//   WiFiClient client;

//   // Increase timeout and buffer size
//   client.setTimeout(15000);
//   http.setTimeout(15000);
//   http.setReuse(true);

//   String url = String(SERVER_URL) + "/api/v1/device/schedules";
//   Serial.println("URL: " + url);

//   if (!http.begin(client, url)) {
//     Serial.println("HTTP begin failed!");
//     blinkRedTwice();
//     return;
//   }

//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);
//   http.addHeader("Connection", "close");

//   // Get current time for timeout
//   unsigned long startTime = millis();
//   int httpCode = http.GET();
//   unsigned long elapsed = millis() - startTime;

//   Serial.print("HTTP GET took ");
//   Serial.print(elapsed);
//   Serial.println(" ms");

//   if (httpCode > 0) {
//     Serial.printf("HTTP Response code: %d\n", httpCode);

//     if (httpCode == HTTP_CODE_OK) {
//       String response = http.getString();
//       Serial.println("Response length: " + String(response.length()));

//       // Print first 500 chars for debugging
//       if (response.length() > 500) {
//         Serial.println("First 500 chars: " + response.substring(0, 500));
//       } else {
//         Serial.println("Response: " + response);
//       }

//       DynamicJsonDocument doc(4096);
//       DeserializationError error = deserializeJson(doc, response);

//       if (!error) {
//         // Check if response is successful
//         bool success = doc["success"];
//         if (!success) {
//           Serial.println("Server returned error: " + String(doc["message"].as<String>()));
//           blinkRedTwice();
//           http.end();
//           return;
//         }

//         // Get the data object
//         JsonObject data = doc["data"];
//         if (data.isNull()) {
//           Serial.println("Error: No 'data' object in response");
//           blinkRedTwice();
//           http.end();
//           return;
//         }

//         // Check if data is different from current
//         bool dataChanged = true;

//         // Get lastUpdated from data object
//         String newHash = String(data["lastUpdated"].as<String>());
//         String oldHash = String(storedData.lastUpdated);
//         dataChanged = (newHash != oldHash);
//         Serial.println("Data changed: " + String(dataChanged ? "YES" : "NO"));
//         Serial.println("New hash: " + newHash);
//         Serial.println("Old hash: " + oldHash);

//         if (dataChanged) {
//           // Clear existing schedules
//           employeeSchedules.clear();

//           // Parse employees from data object
//           JsonArray employees = data["employees"];
//           Serial.println("Total employees in response: " + String(employees.size()));

//           int scheduleCount = 0;
//           int employeeCount = 0;

//           for (JsonObject employee : employees) {
//             String cardUuid = employee["cardUuid"].as<String>();
//             String name = employee["name"].as<String>();
//             int empId = employee["id"];

//             employeeCount++;
//             Serial.println("\nEmployee #" + String(employeeCount) + ": " + name);
//             Serial.println("  ID: " + String(empId));
//             Serial.println("  Card UUID: " + cardUuid);

//             // Only process employees with card UUID
//             if (cardUuid != "null" && cardUuid.length() > 0) {
//               JsonArray schedules = employee["schedules"];
//               Serial.println("  Schedules count: " + String(schedules.size()));

//               for (JsonObject schedule : schedules) {
//                 EmployeeSchedule empSchedule;
//                 empSchedule.id = empId;
//                 empSchedule.cardUuid = cardUuid;
//                 empSchedule.name = name;
//                 empSchedule.dayOfWeek = schedule["day"];
//                 empSchedule.checkInFrom = schedule["checkInFrom"].as<String>();
//                 empSchedule.checkInTo = schedule["checkInTo"].as<String>();
//                 empSchedule.checkOutFrom = schedule["checkOutFrom"].as<String>();
//                 empSchedule.checkOutTo = schedule["checkOutTo"].as<String>();

//                 employeeSchedules.push_back(empSchedule);
//                 scheduleCount++;

//                 Serial.println("    Day " + String(empSchedule.dayOfWeek) + ": " + empSchedule.checkInFrom + " to " + empSchedule.checkInTo);
//               }
//             } else {
//               Serial.println("  Skipped - no card UUID");
//             }
//           }

//           Serial.println("\n=== SUMMARY ===");
//           Serial.println("Total employees processed: " + String(employeeCount));
//           Serial.println("Total schedules added: " + String(scheduleCount));

//           // Save to EEPROM
//           saveScheduleToEEPROM();

//           Serial.println("Schedules updated. Total schedules in memory: " + String(employeeSchedules.size()));

//           // Show success indication based on count
//           int blinkCount = min(scheduleCount, 5);  // Max 5 blinks
//           for (int i = 0; i < blinkCount; i++) {
//             blinkGreenOnce();
//             delay(150);
//           }

//         } else {
//           Serial.println("Schedules unchanged - no update needed");

//           // Update stored time
//           storedData.nextUpdateTime = millis() + scheduleUpdateInterval;
//           EEPROM.put(LAST_UPDATE_ADDR, storedData);
//           EEPROM.commit();

//           blinkGreenOnce();  // Single blink for success but no change
//         }

//         serverUnreachable = false;

//       } else {
//         Serial.print("Failed to parse schedule JSON: ");
//         Serial.println(error.c_str());
//         blinkRedTwice();
//       }

//     } else {
//       Serial.println("Server returned error: " + String(httpCode));
//       blinkRedTwice();
//     }

//   } else {
//     Serial.print("HTTP GET failed: ");
//     Serial.println(http.errorToString(httpCode));
//     blinkRedTwice();
//   }

//   http.end();
//   Serial.println("HTTP connection closed\n");
// }

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   delay(1000);  // Serial stable hone ka wait karo

//   Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

//   // EEPROM INITIALIZE
//   EEPROM.begin(EEPROM_SIZE);

//   // Load existing schedules from EEPROM first
//   if (loadScheduleFromEEPROM()) {
//     Serial.println("Using stored schedules from EEPROM");
//     isInitialScheduleLoaded = true;
//   }

//   // LED PINS SETUP
//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);
//   pinMode(GREEN_LED_PIN, OUTPUT);
//   pinMode(RED_LED_PIN, OUTPUT);

//   // INITIAL LED STATES - SAB OFF KARO
//   digitalWrite(WHITE_LED_PIN, LOW);
//   digitalWrite(BLUE_LED_PIN, LOW);
//   digitalWrite(GREEN_LED_PIN, LOW);
//   digitalWrite(RED_LED_PIN, LOW);

//   // WHITE LED ON (Booting status)
//   digitalWrite(WHITE_LED_PIN, HIGH);

//   // CLEAN WIFI STATE
//   WiFi.disconnect(true);
//   delay(100);
//   WiFi.mode(WIFI_STA);
//   delay(100);

//   // RFID INITIALIZE
//   SPI.begin();
//   rfid.PCD_Init();
//   Serial.println("RFID Module Initialized");

//   // WIFI MANAGER SETUP
//   wifiManager.setDebugOutput(false);
//   wifiManager.setShowInfoErase(false);
//   wifiManager.setShowInfoUpdate(false);

//   std::vector<const char*> menu = { "wifi", "exit" };
//   wifiManager.setMenu(menu);

//   wifiManager.setAPCallback([](WiFiManager* wm) {
//     Serial.println("Config Portal Started");
//     reprovisionMode = true;
//     // Config portal mein white LED still ON
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   });

//   wifiManager.setConnectTimeout(30);        // 30 seconds connection try
//   wifiManager.setConfigPortalTimeout(180);  // 3 minutes portal timeout

//   Serial.println("Attempting WiFi connection...");

//   // CONNECT TO WIFI
//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     Serial.println("WiFi connection failed! Auto-restarting...");

//     // 5 seconds wait aur phir restart
//     delay(5000);
//     ESP.restart();  // AUTO RESTART - NO BUTTON NEEDED
//     delay(5000);
//   }

//   // SUCCESS - WiFi connected
//   Serial.println("WiFi Connected!");
//   Serial.print("IP Address: ");
//   Serial.println(WiFi.localIP());

//   // IMPORTANT: Fetch schedules immediately after WiFi connection
//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Fetching fresh schedules from server...");
//     fetchAndStoreSchedules();
//   } else {
//     Serial.println("WiFi not connected, using EEPROM schedules");
//   }

//   // Ab white LED blink karegi (connected status)
//   reprovisionMode = false;
//   lastHeartbeatTime = millis();
//   lastScanTime = millis();
//   lastScheduleUpdate = millis();

//   Serial.println("Setup complete!");
// }

// // ================== LOOP ==================
// void loop() {
//   // ----------------- WHITE LED STATUS -----------------
//   if (reprovisionMode) {
//     // Config portal mode - White LED ON (still)
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     // WiFi connected - White LED BLINK (slow)
//     blinkWhiteLED();
//   } else {
//     // WiFi not connected - White LED ON (still)
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // ----------------- BLUE LED (SERVER STATUS) -----------------
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   // ----------------- PERIODIC SCHEDULE UPDATE -----------------
//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//     if (millis() - lastScheduleUpdate >= scheduleUpdateInterval) {
//       fetchAndStoreSchedules();
//       lastScheduleUpdate = millis();
//     }
//   }

//   // ----------------- HEARTBEAT + WIFI SCAN -----------------
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

//     // ----------------- RFID ATTENDANCE -----------------
//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       String cardUuid = "";
//       for (byte i = 0; i < rfid.uid.size; i++) {
//         cardUuid += String(rfid.uid.uidByte[i], HEX);
//       }
//       cardUuid.toUpperCase();

//       Serial.println("Card scanned: " + cardUuid);

//       // First check in local schedule (if available)
//       if (employeeSchedules.size() > 0) {
//         bool foundInSchedule = false;
//         for (const auto& schedule : employeeSchedules) {
//           if (schedule.cardUuid == cardUuid) {
//             foundInSchedule = true;
//             Serial.println("Employee found in local schedule: " + schedule.name);
//             break;
//           }
//         }

//         if (!foundInSchedule) {
//           Serial.println("Card not found in local schedule");
//           blinkRedTwice();
//           rfid.PICC_HaltA();
//           rfid.PCD_StopCrypto1();
//           delay(1200);
//           return;
//         }
//       }

//       bool accepted = sendAttendance(cardUuid);
//       if (accepted) blinkGreenOnce();  // RFID Success - Green blink
//       else blinkRedTwice();            // RFID Fail - Red blink

//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();

//       delay(1200);  // anti-double tap
//     }

//   } else {
//     // WiFi disconnected
//     wifiFailCount++;

//     // Try reconnecting after some failures
//     if (wifiFailCount > 10) {
//       Serial.println("WiFi disconnected. Attempting reconnect...");
//       WiFi.reconnect();
//       delay(5000);

//       if (WiFi.status() != WL_CONNECTED) {
//         checkReprovision();
//       } else {
//         wifiFailCount = 0;
//       }
//     }
//   }

//   delay(30);
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
//   doc["schedulesLoaded"] = employeeSchedules.size();
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

// // ================== COMMAND ==================
// void handleCommand(String json) {
//   DynamicJsonDocument doc(512);
//   if (deserializeJson(doc, json)) return;

//   const char* command = doc["data"]["command"];
//   if (command && strcmp(command, "CONNECT_WIFI") == 0) {
//     connectToWifi(
//       doc["data"]["payload"]["ssid"],
//       doc["data"]["payload"]["password"]);
//   } else if (command && strcmp(command, "UPDATE_SCHEDULES") == 0) {
//     // Force schedule update from server
//     Serial.println("Force schedule update requested from server");
//     if (WiFi.status() == WL_CONNECTED) {
//       fetchAndStoreSchedules();
//     }
//   }
// }

// // ================== CONNECT WIFI ==================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);
//   isConnecting = true;
//   WiFi.disconnect(true);
//   delay(500);

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);

//   // Connecting ke dauraan white LED fast blink
//   int tries = 0;
//   while (WiFi.status() != WL_CONNECTED && tries < 25) {
//     // Fast blink while connecting
//     digitalWrite(WHITE_LED_PIN, HIGH);
//     delay(100);
//     digitalWrite(WHITE_LED_PIN, LOW);
//     delay(100);
//     tries++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Connected to new WiFi");
//     // Fetch schedules after new WiFi connection
//     fetchAndStoreSchedules();
//     isConnecting = false;
//     return;
//   }

//   Serial.println("New WiFi failed — rebooting");
//   delay(1000);
//   ESP.restart();
// }

// // ================== REPROVISION ==================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
//     Serial.println("All WiFi failed — AP Mode");
//     reprovisionMode = true;
//     digitalWrite(WHITE_LED_PIN, HIGH);  // White LED ON (still)

//     // Reset WiFi fail count
//     wifiFailCount = 0;

//     // Start config portal
//     if (!wifiManager.startConfigPortal("RFID_Device_001", "12345678")) {
//       Serial.println("Portal timeout, restarting...");
//       delay(3000);
//       ESP.restart();
//     }

//     reprovisionMode = false;
//   }
// }

// // ================== SERVER FAIL ==================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) {
//     serverUnreachable = true;
//     Serial.println("Server unreachable!");
//   }
// }

// // ================== WIFI SCAN ==================
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

// // ================== LED FUNCTIONS ==================
// void blinkWhiteLED() {
//   // WiFi connected - Slow blink (300ms ON, 300ms OFF)
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }

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

//   // Read server response
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

// void blinkGreenOnce() {
//   // RFID Success - Green blink once
//   digitalWrite(GREEN_LED_PIN, HIGH);
//   delay(120);
//   digitalWrite(GREEN_LED_PIN, LOW);
// }

// void blinkRedTwice() {
//   // RFID Fail - Red blink twice
//   for (int i = 0; i < 2; i++) {
//     digitalWrite(RED_LED_PIN, HIGH);
//     delay(120);
//     digitalWrite(RED_LED_PIN, LOW);
//     delay(120);
//   }
// }








// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>

// // ================== EXISTING LED PINS ==================
// #define WHITE_LED_PIN D0  // Main Status LED
// #define BLUE_LED_PIN D8   // Server Status

// // ================== RFID + GREEN/RED LED PINS ==================
// #define GREEN_LED_PIN D1  // RFID Success
// #define RED_LED_PIN D2    // RFID Fail
// #define RST_PIN D3
// #define SS_PIN D4

// // ================== CONFIG ==================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
// const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// // ================== GLOBAL VARIABLES ==================
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

// // ================== OBJECTS ==================
// WiFiManager wifiManager;
// MFRC522 rfid(SS_PIN, RST_PIN);

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   delay(1000);  // Serial stable hone ka wait karo

//   Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

//   // LED PINS SETUP
//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);
//   pinMode(GREEN_LED_PIN, OUTPUT);
//   pinMode(RED_LED_PIN, OUTPUT);

//   // INITIAL LED STATES - SAB OFF KARO
//   digitalWrite(WHITE_LED_PIN, LOW);
//   digitalWrite(BLUE_LED_PIN, LOW);
//   digitalWrite(GREEN_LED_PIN, LOW);
//   digitalWrite(RED_LED_PIN, LOW);

//   // WHITE LED ON (Booting status)
//   digitalWrite(WHITE_LED_PIN, HIGH);

//   // CLEAN WIFI STATE
//   WiFi.disconnect(true);
//   delay(100);
//   WiFi.mode(WIFI_STA);
//   delay(100);

//   // RFID INITIALIZE
//   SPI.begin();
//   rfid.PCD_Init();
//   Serial.println("RFID Module Initialized");

//   // WIFI MANAGER SETUP
//   wifiManager.setDebugOutput(false);
//   wifiManager.setShowInfoErase(false);
//   wifiManager.setShowInfoUpdate(false);

//   std::vector<const char*> menu = { "wifi", "exit" };
//   wifiManager.setMenu(menu);

//   wifiManager.setAPCallback([](WiFiManager* wm) {
//     Serial.println("Config Portal Started");
//     reprovisionMode = true;
//     // Config portal mein white LED still ON
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   });

//   wifiManager.setConnectTimeout(30);        // 30 seconds connection try
//   wifiManager.setConfigPortalTimeout(180);  // 3 minutes portal timeout

//   Serial.println("Attempting WiFi connection...");

//   // CONNECT TO WIFI
//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     Serial.println("WiFi connection failed! Auto-restarting...");

//     // 5 seconds wait aur phir restart
//     delay(5000);
//     ESP.restart();  // AUTO RESTART - NO BUTTON NEEDED
//     delay(5000);
//   }

//   // SUCCESS - WiFi connected
//   Serial.println("WiFi Connected!");
//   Serial.print("IP Address: ");
//   Serial.println(WiFi.localIP());

//   // Ab white LED blink karegi (connected status)
//   reprovisionMode = false;
//   lastHeartbeatTime = millis();
//   lastScanTime = millis();
// }

// // ================== LOOP ==================
// void loop() {
//   // ----------------- WHITE LED STATUS -----------------
//   if (reprovisionMode) {
//     // Config portal mode - White LED ON (still)
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     // WiFi connected - White LED BLINK (slow)
//     blinkWhiteLED();
//   } else {
//     // WiFi not connected - White LED ON (still)
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // ----------------- BLUE LED (SERVER STATUS) -----------------
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   // ----------------- HEARTBEAT + WIFI SCAN -----------------
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

//     // ----------------- RFID ATTENDANCE -----------------
//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       String cardUuid = "";
//       for (byte i = 0; i < rfid.uid.size; i++) {
//         cardUuid += String(rfid.uid.uidByte[i], HEX);
//       }
//       cardUuid.toUpperCase();

//       Serial.println("Card scanned: " + cardUuid);

//       bool accepted = sendAttendance(cardUuid);
//       if (accepted) blinkGreenOnce();  // RFID Success - Green blink
//       else blinkRedTwice();            // RFID Fail - Red blink

//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();

//       delay(1200);  // anti-double tap
//     }

//   } else {
//     // WiFi disconnected
//     wifiFailCount++;

//     // Try reconnecting after some failures
//     if (wifiFailCount > 10) {
//       Serial.println("WiFi disconnected. Attempting reconnect...");
//       WiFi.reconnect();
//       delay(5000);

//       if (WiFi.status() != WL_CONNECTED) {
//         checkReprovision();
//       } else {
//         wifiFailCount = 0;
//       }
//     }
//   }

//   delay(30);
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

// // ================== COMMAND ==================
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

// // ================== CONNECT WIFI ==================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);
//   isConnecting = true;
//   WiFi.disconnect(true);
//   delay(500);

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);

//   // Connecting ke dauraan white LED fast blink
//   int tries = 0;
//   while (WiFi.status() != WL_CONNECTED && tries < 25) {
//     // Fast blink while connecting
//     digitalWrite(WHITE_LED_PIN, HIGH);
//     delay(100);
//     digitalWrite(WHITE_LED_PIN, LOW);
//     delay(100);
//     tries++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("Connected to new WiFi");
//     isConnecting = false;
//     return;
//   }

//   Serial.println("New WiFi failed — rebooting");
//   delay(1000);
//   ESP.restart();
// }

// // ================== REPROVISION ==================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
//     Serial.println("All WiFi failed — AP Mode");
//     reprovisionMode = true;
//     digitalWrite(WHITE_LED_PIN, HIGH);  // White LED ON (still)

//     // Reset WiFi fail count
//     wifiFailCount = 0;

//     // Start config portal
//     if (!wifiManager.startConfigPortal("RFID_Device_001", "12345678")) {
//       Serial.println("Portal timeout, restarting...");
//       delay(3000);
//       ESP.restart();
//     }

//     reprovisionMode = false;
//   }
// }

// // ================== SERVER FAIL ==================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) {
//     serverUnreachable = true;
//     Serial.println("Server unreachable!");
//   }
// }

// // ================== WIFI SCAN ==================
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

// // ================== LED FUNCTIONS ==================
// void blinkWhiteLED() {
//   // WiFi connected - Slow blink (300ms ON, 300ms OFF)
//   if (millis() - lastLedToggle >= 300) {
//     lastLedToggle = millis();
//     ledState = !ledState;
//     digitalWrite(WHITE_LED_PIN, ledState ? HIGH : LOW);
//   }
// }

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

//   // Read server response
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

// void blinkGreenOnce() {
//   // RFID Success - Green blink once
//   digitalWrite(GREEN_LED_PIN, HIGH);
//   delay(120);
//   digitalWrite(GREEN_LED_PIN, LOW);
// }

// void blinkRedTwice() {
//   // RFID Fail - Red blink twice
//   for (int i = 0; i < 2; i++) {
//     digitalWrite(RED_LED_PIN, HIGH);
//     delay(120);
//     digitalWrite(RED_LED_PIN, LOW);
//     delay(120);
//   }
// }








// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>

// // ================== EXISTING LED PINS ==================
// #define WHITE_LED_PIN D0
// #define BLUE_LED_PIN D8

// // ================== RFID + GREEN/RED LED PINS ==================
// #define GREEN_LED_PIN D1
// #define RED_LED_PIN D2
// #define RST_PIN D3
// #define SS_PIN D4

// // ================== CONFIG ==================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
// const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// // ================== GLOBAL VARIABLES ==================
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

// // ================== OBJECTS ==================
// WiFiManager wifiManager;
// MFRC522 rfid(SS_PIN, RST_PIN);

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   delay(1000);  // IMPORTANT: Serial stable hone ka wait karo
//   Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

//   // LED PINS SETUP
//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);
//   pinMode(GREEN_LED_PIN, OUTPUT);
//   pinMode(RED_LED_PIN, OUTPUT);

//   // INITIAL LED STATES
//   digitalWrite(WHITE_LED_PIN, HIGH);  // White ON = Booting
//   digitalWrite(BLUE_LED_PIN, LOW);    // Blue OFF
//   digitalWrite(GREEN_LED_PIN, LOW);   // Green OFF
//   digitalWrite(RED_LED_PIN, HIGH);    // Red ON = Booting

//   // CLEAN WIFI STATE
//   WiFi.disconnect(true);
//   delay(100);
//   WiFi.mode(WIFI_STA);
//   delay(100);

//   // RFID INITIALIZE
//   SPI.begin();
//   rfid.PCD_Init();
//   Serial.println("RFID Module Initialized");

//   // WIFI MANAGER SETUP
//   wifiManager.setDebugOutput(false);
//   wifiManager.setShowInfoErase(false);
//   wifiManager.setShowInfoUpdate(false);

//   std::vector<const char*> menu = { "wifi", "exit" };
//   wifiManager.setMenu(menu);

//   wifiManager.setAPCallback([](WiFiManager* wm) {
//     Serial.println("Config Portal Started");
//     reprovisionMode = true;
//     digitalWrite(RED_LED_PIN, HIGH);
//     digitalWrite(WHITE_LED_PIN, LOW);
//   });

//   wifiManager.setConnectTimeout(30);        // 30 seconds connection try
//   wifiManager.setConfigPortalTimeout(180);  // 3 minutes portal timeout

//   Serial.println("Attempting WiFi connection...");
//   digitalWrite(RED_LED_PIN, LOW);
//   digitalWrite(GREEN_LED_PIN, HIGH);  // Green ON = Trying to connect

//   // CONNECT TO WIFI
//   if (!wifiManager.autoConnect("RFID_Device_001", "12345678")) {
//     Serial.println("WiFi connection failed! Auto-restarting...");

//     // ERROR INDICATION
//     for (int i = 0; i < 5; i++) {
//       digitalWrite(RED_LED_PIN, HIGH);
//       digitalWrite(GREEN_LED_PIN, LOW);
//       delay(300);
//       digitalWrite(RED_LED_PIN, LOW);
//       digitalWrite(GREEN_LED_PIN, HIGH);
//       delay(300);
//     }

//     delay(2000);
//     ESP.restart();  // AUTO RESTART - NO BUTTON NEEDED
//     delay(5000);
//   }

//   // SUCCESS
//   Serial.println("WiFi Connected!");
//   Serial.print("IP Address: ");
//   Serial.println(WiFi.localIP());

//   digitalWrite(GREEN_LED_PIN, LOW);
//   digitalWrite(RED_LED_PIN, LOW);
//   digitalWrite(WHITE_LED_PIN, HIGH);

//   reprovisionMode = false;
//   lastHeartbeatTime = millis();
//   lastScanTime = millis();
// }

// // ================== LOOP ==================
// void loop() {
//   // ----------------- WHITE LED -----------------
//   if (reprovisionMode) {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     blinkWhiteLED();
//   } else {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // ----------------- BLUE LED -----------------
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   // ----------------- HEARTBEAT + WIFI SCAN -----------------
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

//     // ----------------- RFID ATTENDANCE -----------------
//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       String cardUuid = "";
//       for (byte i = 0; i < rfid.uid.size; i++) {
//         cardUuid += String(rfid.uid.uidByte[i], HEX);
//       }
//       cardUuid.toUpperCase();

//       Serial.println("Card scanned: " + cardUuid);

//       bool accepted = sendAttendance(cardUuid);
//       if (accepted) blinkGreenOnce();
//       else blinkRedTwice();

//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();

//       delay(1200);  // anti-double tap
//     }

//   } else {
//     // WiFi disconnected
//     wifiFailCount++;

//     // Try reconnecting after some failures
//     if (wifiFailCount > 10) {
//       Serial.println("WiFi disconnected. Attempting reconnect...");
//       WiFi.reconnect();
//       delay(5000);

//       if (WiFi.status() != WL_CONNECTED) {
//         checkReprovision();
//       } else {
//         wifiFailCount = 0;
//       }
//     }
//   }

//   delay(30);
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

// // ================== COMMAND ==================
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

// // ================== CONNECT WIFI ==================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);
//   isConnecting = true;
//   WiFi.disconnect(true);
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

//   Serial.println("New WiFi failed — rebooting");
//   delay(1000);
//   ESP.restart();
// }

// // ================== REPROVISION ==================
// void checkReprovision() {
//   if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
//     Serial.println("All WiFi failed — AP Mode");
//     reprovisionMode = true;
//     digitalWrite(WHITE_LED_PIN, HIGH);

//     // Reset WiFi fail count
//     wifiFailCount = 0;

//     // Start config portal
//     if (!wifiManager.startConfigPortal("RFID_Device_001", "12345678")) {
//       Serial.println("Portal timeout, restarting...");
//       delay(3000);
//       ESP.restart();
//     }

//     reprovisionMode = false;
//   }
// }

// // ================== SERVER FAIL ==================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) {
//     serverUnreachable = true;
//     Serial.println("Server unreachable!");
//   }
// }

// // ================== WIFI SCAN ==================
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

// // ================== LED FUNCTIONS ==================
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

//   // Read server response
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





// #include <ESP8266WiFi.h>
// #include <ESP8266HTTPClient.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>

// // ================== EXISTING LED PINS ==================
// #define WHITE_LED_PIN D0
// #define BLUE_LED_PIN D8

// // ================== RFID + GREEN/RED LED PINS ==================
// #define GREEN_LED_PIN D1
// #define RED_LED_PIN D2
// #define RST_PIN D3
// #define SS_PIN D4

// // ================== CONFIG ==================
// #define HEARTBEAT_INTERVAL 30000
// #define SCAN_INTERVAL 15000
// #define FAIL_LIMIT 5
// #define SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"

// const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
// const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// // ================== GLOBAL VARIABLES ==================
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

// // ================== OBJECTS ==================
// WiFiManager wifiManager;
// MFRC522 rfid(SS_PIN, RST_PIN);

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   Serial.println("Starting ESP8266 Attendance Device...");

//   WiFi.mode(WIFI_OFF);
//   delay(300);
//   WiFi.mode(WIFI_STA);
//   delay(300);

//   WiFi.forceSleepBegin();
//   delay(200);
//   WiFi.forceSleepWake();
//   delay(200);

//   // Existing LEDs
//   pinMode(WHITE_LED_PIN, OUTPUT);
//   pinMode(BLUE_LED_PIN, OUTPUT);
//   digitalWrite(WHITE_LED_PIN, HIGH);
//   digitalWrite(BLUE_LED_PIN, LOW);

//   // RFID + Green/Red LEDs
//   pinMode(GREEN_LED_PIN, OUTPUT);
//   pinMode(RED_LED_PIN, OUTPUT);
//   digitalWrite(GREEN_LED_PIN, LOW);
//   digitalWrite(RED_LED_PIN, LOW);

//   SPI.begin();
//   rfid.PCD_Init();

//   // WiFi Manager setup
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

// // ================== LOOP ==================
// void loop() {
//   // ----------------- WHITE LED -----------------
//   if (reprovisionMode) {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   } else if (WiFi.status() == WL_CONNECTED) {
//     blinkWhiteLED();
//   } else {
//     digitalWrite(WHITE_LED_PIN, HIGH);
//   }

//   // ----------------- BLUE LED -----------------
//   digitalWrite(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

//   // ----------------- HEARTBEAT + WIFI SCAN -----------------
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

//     // ----------------- RFID ATTENDANCE -----------------
//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       String cardUuid = "";
//       for (byte i = 0; i < rfid.uid.size; i++) {
//         cardUuid += String(rfid.uid.uidByte[i], HEX);
//       }
//       cardUuid.toUpperCase();

//       Serial.println("Card scanned: " + cardUuid);

//       bool accepted = sendAttendance(cardUuid);
//       if (accepted) blinkGreenOnce();
//       else blinkRedTwice();

//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();

//       delay(1200);  // anti-double tap
//     }

//   } else {
//     wifiFailCount++;
//     checkReprovision();
//   }

//   delay(30);
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

// // ================== COMMAND ==================
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

// // ================== CONNECT WIFI ==================
// void connectToWifi(const char* ssid, const char* password) {
//   if (!ssid || !password) return;

//   Serial.print("Connecting to new WiFi: ");
//   Serial.println(ssid);
//   isConnecting = true;
//   WiFi.disconnect(true);
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

//   Serial.println("New WiFi failed — rebooting");
//   delay(1000);
//   ESP.restart();
// }

// // ================== REPROVISION ==================
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

// // ================== SERVER FAIL ==================
// void checkServerUnreachable() {
//   if (heartbeatFailCount >= FAIL_LIMIT) serverUnreachable = true;
// }

// // ================== WIFI SCAN ==================
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

// // ================== CONFIG CALLBACK ==================
// void configModeCallback(WiFiManager* myWiFiManager) {
//   reprovisionMode = true;
// }

// // ================== LED FUNCTIONS ==================
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
