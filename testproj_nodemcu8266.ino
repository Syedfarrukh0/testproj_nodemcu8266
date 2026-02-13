#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <PCF8574.h>
#include <SD.h>
#include <FS.h>
#include <EEPROM.h>
#include <RTClib.h>
#include <time.h>

ESP8266WebServer server(80);

// ================== PCF8574 CONFIG ==================
#define PCF_ADDRESS 0x20  // A0,A1,A2 pins according to wiring
PCF8574 pcf(PCF_ADDRESS);

// ================== EEPROM CONFIG ==================
#define EEPROM_LOCAL_STORAGE_ADDR 20

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

// ================== TESTING CONFIG ==================
#define USE_NTP_TIME true

// ================== MANUAL TIME FOR TESTING ==================
// Sirf tab use hoga jab USE_NTP_TIME = false ho
int manualYear = 2026;
int manualMonth = 2;
int manualDay = 11;
int manualHour = 2;
int manualMinute = 10;
int manualSecond = 0;

// ================== ATTENDANCE LOGIC HEADER ==================
// Server ka exact logic - Day Shift Handler
#ifndef ATTENDANCE_LOGIC_H
#define ATTENDANCE_LOGIC_H

struct AttendanceSchedule {
  int userId;
  String cardUuid;
  String userName;
  int dayOfWeek;
  String checkInFrom;
  String checkInTo;
  String checkOutFrom;
  String checkOutTo;
};

struct AttendanceResult {
  bool success;
  String message;
  String recordType;  // "in" or "out"
  String status;      // "early", "present", "late"
  String timestamp;
  String formattedTime;
  bool shouldDeletePreviousCheckOuts;
};

struct TodaysRecord {
  String recordType;
  unsigned long timestamp;
  String timestampStr;
};

#endif

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

// ================== TIMEZONE CONFIG ==================
String deviceTimezone = "UTC";
int timezoneOffsetMinutes = 0;  // +300 for Asia/Karachi

#define EEPROM_TZ_ADDR 40
#define EEPROM_OFFSET_ADDR 100


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
bool localStorage = true;

// ================== LOCAL ATTENDANCE LOGIC VARIABLES ==================
// DAY SHIFT CONSTANTS - exactly like server
const int GRACE_EARLY_IN = 15;           // 15 minutes early check-in allowed
const int GRACE_LATE_IN = 15;            // 15 minutes late check-in allowed
const int GRACE_EARLY_OUT = 0;           // 0 minutes early check-out allowed
const int GRACE_LATE_OUT = 15;           // 15 minutes late check-out allowed
const int MIN_WORK_DURATION = 30;        // 30 minutes minimum work
const int MAX_SHIFT_HOURS = 18;          // 18 hours max shift
const int MIN_GAP_BETWEEN_RECORDS = 10;  // 10 seconds gap between scans

// Today's records storage
std::vector<TodaysRecord> todaysCheckIns;
std::vector<TodaysRecord> todaysCheckOuts;
int currentProcessingDay = -1;

// Log file path
const char* LOG_FILE = "/attendance_logs.csv";
const char* DAILY_LOG_PREFIX = "/log_";

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

// ================== API RESPONSE STRUCTURE ==================
struct ApiResponse {
  bool success;
  String message;
  String data;
};

// ================== GLOBAL VECTORS ==================
std::vector<UserSchedule> userSchedules;
StoredData storedData;

// ================== OBJECTS ==================
WiFiManager wifiManager;
MFRC522 rfid(SS_PIN, RST_PIN);

RTC_DS3231 rtc;

// ================== LOCAL TIME HELPER ==================
DateTime getLocalTime() {
  DateTime utc = rtc.now();

  // Convert minutes to seconds (long for safety)
  long totalOffsetSeconds = timezoneOffsetMinutes * 60L;

  // Get Unix timestamp (seconds since 2000-01-01)
  time_t utcUnix = utc.unixtime();

  // Add offset
  time_t localUnix = utcUnix + totalOffsetSeconds;

  // Convert back to DateTime
  DateTime local = DateTime(localUnix);

  // Debug output
  Serial.printf("🌍 UTC:  %04d-%02d-%02d %02d:%02d:%02d\n",
                utc.year(), utc.month(), utc.day(),
                utc.hour(), utc.minute(), utc.second());

  Serial.printf("📍 Local: %04d-%02d-%02d %02d:%02d:%02d (Offset: %d min)\n",
                local.year(), local.month(), local.day(),
                local.hour(), local.minute(), local.second(),
                timezoneOffsetMinutes);

  return local;
}

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
          if (localStorage && sdMounted) {
            saveScheduleToSD();
          }

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

// ================== WIFI CONNECTION FUNCTION  ==================
void handleConnectWifi() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
    return;
  }

  if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
    server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
    return;
  }

  String deviceId = server.header("x-device-id");
  String deviceSecret = server.header("x-device-secret");

  if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
    server.send(403, "application/json", "{\"error\":\"Invalid device auth\"}");
    return;
  }

  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));

  if (err) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
    return;
  }

  String ssid = doc["ssid"].as<String>();
  String password = doc["password"].as<String>();

  if (ssid.length() == 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"SSID missing\"}");
    return;
  }

  server.send(
    200,
    "application/json",
    "{\"success\":true,\"message\":\"WiFi credentials received\"}");

  server.client().flush();
  delay(300);

  bool connected = connectToWifi(ssid, password);

  if (connected) {
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi connected\"}");
  } else {
    server.send(500, "application/json", "{\"success\":false,\"message\":\"WiFi connection failed\"}");
  }
}

// ================== LOCALSTORAGE HANDLE FUNCTION ==============
void handleLocalStorage() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
    return;
  }

  if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
    server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
    return;
  }

  String deviceId = server.header("x-device-id");
  String deviceSecret = server.header("x-device-secret");

  if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
    server.send(403, "application/json", "{\"error\":\"Invalid device auth\"}");
    return;
  }

  DynamicJsonDocument doc(256);
  deserializeJson(doc, server.arg("plain"));

  if (!doc.containsKey("enabled")) {
    server.send(400, "application/json", "{\"error\":\"enabled required\"}");
    return;
  }

  bool newValue = doc["enabled"];

  if (localStorage != newValue) {
    localStorage = newValue;

    EEPROM.write(
      EEPROM_LOCAL_STORAGE_ADDR,
      localStorage ? 1 : 0);
    EEPROM.commit();
  }

  // ===== SD MOUNT / UNMOUNT =====
  if (localStorage && !sdMounted) {
    sdMounted = SD.begin(SD_CS_PIN);
    Serial.println("📀 SD mounted after enable");
  }

  if (!localStorage && sdMounted) {
    SD.end();
    sdMounted = false;
    userSchedules.clear();
    Serial.println("📀 SD unmounted after disable");
  }

  Serial.println("🔁 LocalStorage toggled from server");
  Serial.print("💾 Saved localStorage = ");
  Serial.println(localStorage ? "TRUE" : "FALSE");

  server.send(
    200,
    "application/json",
    localStorage
      ? "{\"success\":true,\"localStorage\":true}"
      : "{\"success\":true,\"localStorage\":false}");

  server.client().flush();
  delay(300);

  ESP.reset();
}

// ================ LOCAL TIMEZONE HANDLE FUNCTION ==============
void handleSetTimezone() {
  if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
    server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
    return;
  }

  if (
    server.header("x-device-id") != DEVICE_UUID || server.header("x-device-secret") != DEVICE_SECRET) {
    server.send(403, "application/json", "{\"error\":\"Invalid device auth\"}");
    return;
  }

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }

  deviceTimezone = doc["timezone"].as<String>();  // Asia/Karachi
  String offsetStr = doc["offset"];               // +05:00

  int hours = offsetStr.substring(1, 3).toInt();
  int minutes = offsetStr.substring(4, 6).toInt();
  timezoneOffsetMinutes = hours * 60 + minutes;

  if (offsetStr.startsWith("-")) {
    timezoneOffsetMinutes *= -1;
  }

  // ===== SAVE TO EEPROM =====
  for (int i = 0; i < 40; i++) EEPROM.write(EEPROM_TZ_ADDR + i, 0);
  for (int i = 0; i < deviceTimezone.length(); i++) {
    EEPROM.write(EEPROM_TZ_ADDR + i, deviceTimezone[i]);
  }

  EEPROM.put(EEPROM_OFFSET_ADDR, timezoneOffsetMinutes);
  EEPROM.commit();

  Serial.printf(
    "🌍 Timezone updated: %s (%d min)\n",
    deviceTimezone.c_str(),
    timezoneOffsetMinutes);

  // IMMEDIATE TIME CHECK
  DateTime utc = rtc.now();
  DateTime local = getLocalTime();

  // Calculate difference
  int diffHours = local.hour() - utc.hour();
  int diffMinutes = local.minute() - utc.minute();

  server.send(200, "application/json", "{\"success\":true}");
}

// ================== GET TODAY'S ATTENDANCE RECORDS ==================
String getTodayAttendanceRecords() {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    return "{\"success\":true,\"message\":\"No records for today\",\"data\":[]}";
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    return "{\"success\":false,\"message\":\"Failed to open log file\"}";
  }

  String jsonResponse = "{\"success\":true,\"message\":\"Records fetched successfully\",\"data\":[";

  // Skip header
  String header = file.readStringUntil('\n');

  bool firstLine = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse CSV line to JSON
    int commaIndex = 0;
    int startPos = 0;
    int fieldIndex = 0;

    String timestamp, cardUuid, userId, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

    while (startPos < line.length()) {
      commaIndex = line.indexOf(',', startPos);
      if (commaIndex == -1) commaIndex = line.length();

      String field = line.substring(startPos, commaIndex);

      switch (fieldIndex) {
        case 0: timestamp = field; break;
        case 1: cardUuid = field; break;
        case 2: userId = field; break;
        case 3: userName = field; break;
        case 4: recordType = field; break;
        case 5: status = field; break;
        case 6: message = field; break;
        case 7: dayOfWeek = field; break;
        case 8: checkInWindow = field; break;
        case 9: checkOutWindow = field; break;
      }

      startPos = commaIndex + 1;
      fieldIndex++;
    }

    if (!firstLine) {
      jsonResponse += ",";
    }
    firstLine = false;

    jsonResponse += "{";
    jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
    jsonResponse += "\"cardUuid\":\"" + cardUuid + "\",";
    jsonResponse += "\"userId\":" + userId + ",";
    jsonResponse += "\"userName\":\"" + userName + "\",";
    jsonResponse += "\"recordType\":\"" + recordType + "\",";
    jsonResponse += "\"status\":\"" + status + "\",";
    jsonResponse += "\"message\":\"" + message + "\",";
    jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
    jsonResponse += "\"checkInWindow\":\"" + checkInWindow + "\",";
    jsonResponse += "\"checkOutWindow\":\"" + checkOutWindow + "\"";
    jsonResponse += "}";
  }

  file.close();
  jsonResponse += "]}";

  return jsonResponse;
}

// ================== GET USER ATTENDANCE RECORDS ==================
String getUserAttendanceRecords(String targetCardUuid) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    return "{\"success\":true,\"message\":\"No records found\",\"data\":[]}";
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    return "{\"success\":false,\"message\":\"Failed to open log file\"}";
  }

  String jsonResponse = "{\"success\":true,\"message\":\"User records fetched successfully\",\"data\":[";

  // Skip header
  String header = file.readStringUntil('\n');

  bool firstLine = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse CSV line
    int commaIndex = 0;
    int startPos = 0;
    int fieldIndex = 0;

    String timestamp, cardUuid, userId, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

    while (startPos < line.length()) {
      commaIndex = line.indexOf(',', startPos);
      if (commaIndex == -1) commaIndex = line.length();

      String field = line.substring(startPos, commaIndex);

      switch (fieldIndex) {
        case 0: timestamp = field; break;
        case 1: cardUuid = field; break;
        case 2: userId = field; break;
        case 3: userName = field; break;
        case 4: recordType = field; break;
        case 5: status = field; break;
        case 6: message = field; break;
        case 7: dayOfWeek = field; break;
        case 8: checkInWindow = field; break;
        case 9: checkOutWindow = field; break;
      }

      startPos = commaIndex + 1;
      fieldIndex++;
    }

    // Sirf target user ke records filter karo
    if (cardUuid == targetCardUuid) {
      if (!firstLine) {
        jsonResponse += ",";
      }
      firstLine = false;

      jsonResponse += "{";
      jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
      jsonResponse += "\"cardUuid\":\"" + cardUuid + "\",";
      jsonResponse += "\"userId\":" + userId + ",";
      jsonResponse += "\"userName\":\"" + userName + "\",";
      jsonResponse += "\"recordType\":\"" + recordType + "\",";
      jsonResponse += "\"status\":\"" + status + "\",";
      jsonResponse += "\"message\":\"" + message + "\",";
      jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
      jsonResponse += "\"checkInWindow\":\"" + checkInWindow + "\",";
      jsonResponse += "\"checkOutWindow\":\"" + checkOutWindow + "\"";
      jsonResponse += "}";
    }
  }

  file.close();
  jsonResponse += "]}";

  return jsonResponse;
}

// ================== DELETE USER RECORDS FROM SD CARD ==================
String deleteUserAttendanceRecords(String targetCardUuid) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    return "{\"success\":false,\"message\":\"No records file found\"}";
  }

  // Temporary file banayein
  char tempFilename[32];
  sprintf(tempFilename, "%s%04d%02d%02d_temp.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  File originalFile = SD.open(filename, FILE_READ);
  if (!originalFile) {
    return "{\"success\":false,\"message\":\"Failed to open original file\"}";
  }

  File tempFile = SD.open(tempFilename, FILE_WRITE);
  if (!tempFile) {
    originalFile.close();
    return "{\"success\":false,\"message\":\"Failed to create temp file\"}";
  }

  int deletedCount = 0;
  int totalCount = 0;

  // Header copy karo
  String header = originalFile.readStringUntil('\n');
  tempFile.println(header);

  // Records process karo
  while (originalFile.available()) {
    String line = originalFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    totalCount++;

    // Check if this line contains target card UUID
    if (line.indexOf(targetCardUuid) >= 0) {
      deletedCount++;
      continue;  // Skip this line - delete karo
    }

    // Keep this line
    tempFile.println(line);
  }

  originalFile.close();
  tempFile.close();

  // Original file delete karo
  SD.remove(filename);

  // Temp file ko rename karo to original
  SD.rename(tempFilename, filename);

  // Vector records bhi update karo agar aaj ke hain
  if (currentProcessingDay == (now.year() * 10000 + now.month() * 100 + now.day())) {
    // Reload records from SD
    loadTodayRecordsFromSD();
  }

  String response = "{\"success\":true,\"message\":\"Records deleted successfully\",";
  response += "\"deletedCount\":" + String(deletedCount) + ",";
  response += "\"totalCount\":" + String(totalCount) + "}";

  return response;
}

// ================== HANDLE GET ATTENDANCE RECORDS ==================
void handleGetAttendanceRecords() {
  // Authentication check
  if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
    server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
    return;
  }

  String deviceId = server.header("x-device-id");
  String deviceSecret = server.header("x-device-secret");

  if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
    server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
    return;
  }

  // Check if cardUuid parameter exists
  if (server.hasArg("cardUuid")) {
    String cardUuid = server.arg("cardUuid");
    cardUuid.toUpperCase();
    String response = getUserAttendanceRecords(cardUuid);
    server.send(200, "application/json", response);
  } else {
    // Return all today's records
    String response = getTodayAttendanceRecords();
    server.send(200, "application/json", response);
  }
}

// ================== HANDLE DELETE ATTENDANCE RECORDS ==================
void handleDeleteAttendanceRecords() {
  // Authentication check
  if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
    server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
    return;
  }

  String deviceId = server.header("x-device-id");
  String deviceSecret = server.header("x-device-secret");

  if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
    server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
    return;
  }

  // Only POST method allowed
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
    return;
  }

  // Parse JSON body
  DynamicJsonDocument doc(256);
  DeserializationError error = deserializeJson(doc, server.arg("plain"));

  if (error) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
    return;
  }

  if (!doc.containsKey("cardUuid")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"cardUuid required\"}");
    return;
  }

  String cardUuid = doc["cardUuid"].as<String>();
  cardUuid.toUpperCase();

  String response = deleteUserAttendanceRecords(cardUuid);
  server.send(200, "application/json", response);
}

// ================== HANDLE DELETE ALL TODAY'S RECORDS ==================
void handleDeleteAllTodayRecords() {
  // Authentication check
  if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
    server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
    return;
  }

  String deviceId = server.header("x-device-id");
  String deviceSecret = server.header("x-device-secret");

  if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
    server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
    return;
  }

  if (!sdMounted) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"SD card not mounted\"}");
    return;
  }

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    server.send(404, "application/json", "{\"success\":false,\"message\":\"No records file found\"}");
    return;
  }

  // File delete karo
  bool deleted = SD.remove(filename);

  if (deleted) {
    // Vector records bhi clear karo
    if (currentProcessingDay == (now.year() * 10000 + now.month() * 100 + now.day())) {
      todaysCheckIns.clear();
      todaysCheckOuts.clear();
    }

    server.send(200, "application/json",
                "{\"success\":true,\"message\":\"All today's records deleted successfully\"}");
  } else {
    server.send(500, "application/json",
                "{\"success\":false,\"message\":\"Failed to delete records file\"}");
  }
}

// ================== ESP SERVER SETUP ==================
void setupServer() {
  server.collectHeaders(
    "x-device-id",
    "x-device-secret");
  server.on("/api/connect-wifi", handleConnectWifi);
  server.on("/api/wifi/scan", HTTP_GET, scanAndSendWifi);
  server.on("/api/toggle-local-storage", handleLocalStorage);
  server.on("/api/set-timezone", HTTP_POST, handleSetTimezone);

  server.on("/api/attendance/records", HTTP_GET, handleGetAttendanceRecords);
  server.on("/api/attendance/records", HTTP_DELETE, handleDeleteAllTodayRecords);
  server.on("/api/attendance/records/delete", HTTP_POST, handleDeleteAttendanceRecords);

  server.begin();
  Serial.println("HTTP server started");
}

// ============= RTC Update REAL UTC TIME =============
void setRTCFromNTP() {
#if USE_NTP_TIME == true
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, cannot sync time.");
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  time_t now;
  int retries = 30;

  while ((now = time(nullptr)) < 1000000000 && retries > 0) {
    Serial.print(".");
    delay(1000);
    retries--;
  }

  if (now < 1000000000) {
    Serial.println("❌ Failed to get NTP time");
    return;
  }

  struct tm* timeinfo = gmtime(&now);
  DateTime ntpTime(
    timeinfo->tm_year + 1900,
    timeinfo->tm_mon + 1,
    timeinfo->tm_mday,
    timeinfo->tm_hour,
    timeinfo->tm_min,
    timeinfo->tm_sec);

  rtc.adjust(ntpTime);

  DateTime rtcTime = rtc.now();
  Serial.printf("RTC Set NTP to: %04d-%02d-%02d %02d:%02d:%02d\n",
                rtcTime.year(),
                rtcTime.month(),
                rtcTime.day(),
                rtcTime.hour(),
                rtcTime.minute(),
                rtcTime.second());

  Serial.println("RTC updated from NTP");
#else
  // 🔥 MANUAL TIME SET KARO - TESTING KE LIYE
  DateTime manualTime(
    manualYear,
    manualMonth,
    manualDay,
    manualHour,
    manualMinute,
    manualSecond);

  rtc.adjust(manualTime);

  Serial.println("\n⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️");
  Serial.println("🔧 TEST MODE: MANUAL TIME SET!");
  Serial.printf("📅 Manual Date: %04d-%02d-%02d\n", manualYear, manualMonth, manualDay);
  Serial.printf("⏰ Manual Time: %02d:%02d:%02d\n", manualHour, manualMinute, manualSecond);
  Serial.println("⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️\n");

#endif
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

  // ================== EEPROM INIT ==================
  EEPROM.begin(512);
  uint8_t storedLS = EEPROM.read(EEPROM_LOCAL_STORAGE_ADDR);

  if (storedLS == 0 || storedLS == 1) {
    localStorage = storedLS;
  } else {
    localStorage = true;  // default
    EEPROM.write(EEPROM_LOCAL_STORAGE_ADDR, 1);
    EEPROM.commit();
  }

  Serial.print("🔁 Boot LocalStorage = ");
  Serial.println(localStorage ? "TRUE" : "FALSE");

  // ================== LOAD TIMEZONE FROM EEPROM ==================
  char tzBuf[41];
  for (int i = 0; i < 40; i++) {
    tzBuf[i] = EEPROM.read(EEPROM_TZ_ADDR + i);
  }
  tzBuf[40] = '\0';
  deviceTimezone = String(tzBuf);
  EEPROM.get(EEPROM_OFFSET_ADDR, timezoneOffsetMinutes);

  if (deviceTimezone.length() == 0) {
    deviceTimezone = "UTC";
    timezoneOffsetMinutes = 0;
  }

  Serial.printf(
    "🌍 Timezone loaded: %s | Offset: %d minutes\n",
    deviceTimezone.c_str(),
    timezoneOffsetMinutes);


  // ================== SD CARD INIT ==================
  if (localStorage) {
    sdMounted = SD.begin(SD_CS_PIN);
    if (!sdMounted) {
      Serial.println("❌ SD Card mount failed");
      ESP.restart();
    } else {
      Serial.println("✅ SD Card mounted successfully");
    }
  } else {
    sdMounted = false;
    Serial.println("Local storage disabled — SD card skipped");
  }

  // ================== LOAD SCHEDULE FROM SD ==================
  if (localStorage && sdMounted) {
    if (loadScheduleFromSD()) {
      Serial.println("Using stored schedules from SD card");
      isInitialScheduleLoaded = true;
    }
  }

  // ================== PCF8574 INIT ==================
  Wire.begin(D1, D2);  // SDA, SCL
  pcf.begin();

  // INITIAL LED STATES - SAB OFF KARO
  pcf.write(WHITE_LED_PIN, LOW);
  pcf.write(BLUE_LED_PIN, LOW);
  pcf.write(GREEN_LED_PIN, LOW);
  pcf.write(RED_LED_PIN, LOW);

  // ================== RTC INIT ==================
  if (!rtc.begin()) {
    Serial.println("❌ RTC not found");
  } else {
    Serial.println("RTC detected");
    DateTime localNow = getLocalTime();
  }

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
  sendWifiStatusToServer(
    "connected",
    WiFi.SSID(),
    false,
    "old Wifi");
  setRTCFromNTP();

  // start esp server
  setupServer();

  // Fetch schedules immediately after WiFi connection
  if (WiFi.status() == WL_CONNECTED) {
    if (localStorage) {
      Serial.println("Fetching fresh schedules from server...");
      fetchAndStoreSchedules();
    }
  } else {
    Serial.println("WiFi not connected, using EEPROM schedules");
  }

  // Ab white LED blink karegi (connected status)
  reprovisionMode = false;
  lastHeartbeatTime = millis();
  lastScanTime = millis();
  lastScheduleUpdate = millis();

  // Initialize local attendance manager
  if (localStorage) {
    // Reset today's records
    todaysCheckIns.clear();
    todaysCheckOuts.clear();
    DateTime now = getLocalTime();
    currentProcessingDay = now.year() * 10000 + now.month() * 100 + now.day();
    Serial.println("[LocalAttendance] Manager initialized");

    // 🔥 NAYA: SD CARD SE TODAY'S RECORDS LOAD KARO
    if (sdMounted) {
      loadTodayRecordsFromSD();
    }

    Serial.println("[LocalAttendance] Manager initialized");
    Serial.printf("📊 Today's stats - Check-ins: %d, Check-outs: %d\n",
                  todaysCheckIns.size(), todaysCheckOuts.size());
  }

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
    server.handleClient();
    if (localStorage) {
      if (millis() - lastScheduleUpdate >= scheduleUpdateInterval) {
        fetchAndStoreSchedules();
        lastScheduleUpdate = millis();
      }
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

    // ----------------- RFID ATTENDANCE -----------------
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String cardUuid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        cardUuid += String(rfid.uid.uidByte[i], HEX);
      }
      cardUuid.toUpperCase();

      Serial.println("Card scanned: " + cardUuid);

      // First check in local schedule (if localstorage is enabled)
      if (localStorage && userSchedules.size() > 0) {
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

      // ========== 🔥 YAHAN SE NAYA CODE ADD KARO 🔥 ==========
      if (localStorage) {
        // Day change check
        DateTime currentTime = getLocalTime();
        checkDayChange(currentTime);

        // Get current day of week (1=Monday, 7=Sunday)
        int currentDOW = currentTime.dayOfTheWeek();
        currentDOW = (currentDOW == 0) ? 7 : currentDOW;

        Serial.println("\n=== ATTENDANCE CHECK ===");
        Serial.println("Card: " + cardUuid);
        Serial.println("Today: Day " + String(currentDOW) + " (" + getDayName(currentDOW) + ")");
        Serial.println("Local Time: " + String(currentTime.hour()) + ":" + String(currentTime.minute()) + ":" + String(currentTime.second()));

        // 🔥 FIND SCHEDULE FOR TODAY - SIRF EK BAAR
        const UserSchedule* todaysSchedule = nullptr;
        for (const auto& schedule : userSchedules) {
          if (schedule.cardUuid == cardUuid) {
            Serial.println("  → Found schedule for Day " + String(schedule.dayOfWeek));
            if (schedule.dayOfWeek == currentDOW) {
              todaysSchedule = &schedule;
              Serial.println("  ✅ This is TODAY's schedule!");
              break;  // Mil gaya, loop se bahar
            }
          }
        }

        // Agar aaj ka schedule nahi mila
        if (!todaysSchedule) {
          Serial.println("❌ No schedule found for today (Day " + String(currentDOW) + ")");
          blinkRedTwice();
          rfid.PICC_HaltA();
          rfid.PCD_StopCrypto1();
          delay(1200);
          return;
        }

        // 🔥 PROCESS WITH EXACT SERVER LOGIC - SIRF EK BAAR
        AttendanceResult localResult = processLocalAttendance(
          cardUuid,
          todaysSchedule->userId,
          todaysSchedule->userName,
          *todaysSchedule,
          currentTime);

        if (localResult.success) {
          blinkGreenOnce();
          Serial.println("✅ LOCAL ATTENDANCE ACCEPTED: " + localResult.message);

          // Save to SD card log
          const char* days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
          saveAttendanceLogToSD(
            cardUuid,
            todaysSchedule->userId,
            todaysSchedule->userName,
            localResult.timestamp,
            localResult.recordType,
            localResult.status,
            localResult.message,
            String(days[currentDOW - 1]),
            todaysSchedule->checkInFrom + " - " + todaysSchedule->checkInTo,
            todaysSchedule->checkOutFrom + " - " + todaysSchedule->checkOutTo);

        } else {
          blinkRedTwice();
          Serial.println("❌ LOCAL ATTENDANCE DENIED: " + localResult.message);
        }

      } else {
        // ----- EXISTING CODE - ONLINE MODE -----
        bool accepted = sendAttendance(cardUuid);
        if (accepted) blinkGreenOnce();
        else blinkRedTwice();
      }
      // ========== 🔥 YAHAN TAK NAYA CODE 🔥 ==========

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
  sd["enabled"] = localStorage;
  sd["mounted"] = localStorage ? sdMounted : false;
  sd["scheduleFileExists"] = (localStorage && sdMounted) ? SD.exists(SCHEDULE_FILE) : false;

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
  // if (command && strcmp(command, "UPDATE_SCHEDULES") == 0) {
  //   // Force schedule update from server
  //   Serial.println("Force schedule update requested from server");
  //   if (WiFi.status() == WL_CONNECTED) {
  //     fetchAndStoreSchedules();
  //   }
  // }
}

// ================== CONNECT NEW WIFI ==================
bool connectToWifi(String ssid, String password) {
  Serial.println("\n=== WIFI CONNECT ===");
  Serial.println("SSID: " + ssid);
  Serial.println("Password: " + password);


  // Disconnect previous WiFi
  isConnecting = true;
  WiFi.disconnect(true);
  delay(500);

  // Set WiFi mode
  WiFi.mode(WIFI_STA);

  // Connect to new WiFi
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.println("Connecting...");

  // Connect hone ka wait karo with timeout (20 seconds)
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) {
    // White LED fast blink during connection attempt
    pcf.write(WHITE_LED_PIN, HIGH);
    delay(100);
    pcf.write(WHITE_LED_PIN, LOW);
    delay(100);

    Serial.print(".");
    timeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // Success - green blink
    blinkGreenOnce();

    // Fetch fresh schedules after connecting
    if (localStorage) {
      fetchAndStoreSchedules();
    }

    // WIFI STATUS CALLBACK
    sendWifiStatusToServer(
      "connected",
      ssid,
      true,
      "");

    isConnecting = false;
    return true;
  } else {
    Serial.println("\n❌ WiFi Connection Failed!");

    // Failure - red blink twice
    blinkRedTwice();

    // WIFI STATUS CALLBACK (FAIL)
    sendWifiStatusToServer(
      "failed",
      ssid,
      false,
      "CONNECTION_TIMEOUT");

    delay(500);
    ESP.restart();

    return false;
  }
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

  if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
    server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
    return;
  }

  String reqUuid = server.header("x-device-id");
  String reqSecret = server.header("x-device-secret");

  // check device auth
  if (reqUuid.length() == 0 || reqSecret.length() == 0) {
    server.send(
      401,
      "application/json",
      "{\"success\":false,\"message\":\"Device credentials missing\"}");
    return;
  }

  if (reqUuid != DEVICE_UUID || reqSecret != DEVICE_SECRET) {
    Serial.println("❌ Device auth failed not send wifi scan list");

    server.send(
      403,
      "application/json",
      "{\"success\":false,\"message\":\"Invalid device credentials\"}");
    return;
  }

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
    net["secure"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
  }

  String json;
  serializeJson(doc, json);

  server.send(200, "application/json", json);

  WiFi.scanDelete();  // memory clean
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

// ================= SEND WIFI CONNECTION STATUS =================
void sendWifiStatusToServer(
  String status,
  String ssid,
  bool isNewWifi,
  String reason) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  WiFiClient client;

  String url = String(SERVER_URL) + "/api/v1/device/wifi-status";
  http.begin(client, url);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_UUID);
  http.addHeader("x-device-secret", DEVICE_SECRET);
  http.addHeader("Connection", "close");

  DynamicJsonDocument doc(256);
  doc["deviceUuid"] = DEVICE_UUID;
  doc["status"] = status;  // connected | failed
  doc["ssid"] = ssid;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["isNewWifi"] = isNewWifi;
  doc["reason"] = reason;

  String payload;
  serializeJson(doc, payload);

  Serial.println("📡 WiFi Status Report:");
  Serial.println(payload);

  http.PATCH(payload);
  http.end();
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

// ================== LOCAL ATTENDANCE HELPER FUNCTIONS ==================
// Server logic ka exact replica

int timeToSeconds(const String& timeStr) {
  if (timeStr.length() == 0 || timeStr == "Invalid Date" || timeStr == "null") {
    return 0;
  }

  String cleanTime = timeStr;
  int dotIndex = cleanTime.indexOf('.');
  if (dotIndex > 0) {
    cleanTime = cleanTime.substring(0, dotIndex);
  }

  int h1 = cleanTime.substring(0, 2).toInt();
  int h2 = cleanTime.substring(3, 5).toInt();
  int sec = 0;

  if (cleanTime.length() >= 8) {
    sec = cleanTime.substring(6, 8).toInt();
  }

  return h1 * 3600 + h2 * 60 + sec;
}

// ================== SERVER LOGIC EXACT REPLICA ==================
AttendanceResult processLocalAttendance(
  const String& cardUuid,
  int userId,
  const String& userName,
  const UserSchedule& todaySchedule,
  const DateTime& currentTime) {

  AttendanceResult result;
  result.success = false;
  result.recordType = "";
  result.status = "present";
  result.shouldDeletePreviousCheckOuts = false;

  // === 1. GET CURRENT DAY AND TIME ===
  int currentDOW = currentTime.dayOfTheWeek();
  currentDOW = (currentDOW == 0) ? 7 : currentDOW;
  int currentTimeSec = currentTime.hour() * 3600 + currentTime.minute() * 60 + currentTime.second();

  // === 2. CHECK IF TODAY'S SCHEDULE MATCHES ===
  if (todaySchedule.dayOfWeek != currentDOW) {
    result.message = "No schedule found for today (Day " + String(currentDOW) + ")";
    return result;
  }

  // === 3. CONVERT SCHEDULE TIMES TO SECONDS ===
  int ciFromSec = timeToSeconds(todaySchedule.checkInFrom);
  int ciToSec = timeToSeconds(todaySchedule.checkInTo);
  int coFromSec = timeToSeconds(todaySchedule.checkOutFrom);
  int coToSec = timeToSeconds(todaySchedule.checkOutTo);

  // === 4. VERIFY THIS IS DAY SHIFT ===
  if (ciFromSec > coFromSec) {
    result.message = "This is an overnight shift. Day shift handler only.";
    return result;
  }

  // === 5. CALCULATE WINDOWS WITH GRACE PERIODS ===
  int earliestCheckIn = ciFromSec - (GRACE_EARLY_IN * 60);
  int latestCheckIn = ciToSec + (GRACE_LATE_IN * 60);
  bool isCheckInWindow = isTimeInRange(currentTimeSec, earliestCheckIn, latestCheckIn);
  bool isBeforeCheckInWindow = currentTimeSec < earliestCheckIn;
  bool isAfterCheckInWindow = currentTimeSec > latestCheckIn;

  int earliestCheckOut = coFromSec - (GRACE_EARLY_OUT * 60);
  int latestCheckOut = coToSec + (GRACE_LATE_OUT * 60);
  bool isCheckOutWindow = isTimeInRange(currentTimeSec, earliestCheckOut, latestCheckOut);
  bool isBeforeCheckOutWindow = currentTimeSec < earliestCheckOut;
  bool isAfterCheckOutWindow = currentTimeSec > latestCheckOut;

  // Debug prints
  Serial.println("\n=== TIME WINDOW DEBUG ===");
  Serial.printf("currentTimeSec: %d (%02d:%02d)\n", currentTimeSec, currentTimeSec / 3600, (currentTimeSec % 3600) / 60);
  Serial.printf("checkOut window: %d (%02d:%02d) to %d (%02d:%02d)\n",
                earliestCheckOut, earliestCheckOut / 3600, (earliestCheckOut % 3600) / 60,
                latestCheckOut, latestCheckOut / 3600, (latestCheckOut % 3600) / 60);
  Serial.printf("isCheckOutWindow: %s\n", isCheckOutWindow ? "TRUE" : "FALSE");
  Serial.printf("isAfterCheckOutWindow: %s\n", isAfterCheckOutWindow ? "TRUE" : "FALSE");
  Serial.printf("isBeforeCheckOutWindow: %s\n", isBeforeCheckOutWindow ? "TRUE" : "FALSE");
  Serial.println("=========================\n");

  // === 6. CHECK FOR OPEN CHECK-IN ===
  bool hasOpenCheckIn = false;
  if (todaysCheckIns.size() > todaysCheckOuts.size()) {
    hasOpenCheckIn = true;
  }

  // === 7. ANTI-SPAM CHECK ===
  if (todaysCheckIns.size() > 0 || todaysCheckOuts.size() > 0) {
    int lastRecordTime = 0;
    if (todaysCheckOuts.size() > 0) {
      lastRecordTime = todaysCheckOuts.back().timestamp;
    }
    if (todaysCheckIns.size() > 0) {
      int lastInTime = todaysCheckIns.back().timestamp;
      if (lastInTime > lastRecordTime) lastRecordTime = lastInTime;
    }

    int secondsSinceLast = currentTimeSec - lastRecordTime;
    if (secondsSinceLast < MIN_GAP_BETWEEN_RECORDS && lastRecordTime > 0) {
      result.message = "Please wait " + String(MIN_GAP_BETWEEN_RECORDS) + " seconds before next scan";
      return result;
    }
  }

  // === 🔥 FIXED DECISION LOGIC - SAHI ORDER ===

  // CASE 1: USER HASN'T CHECKED IN TODAY
  if (todaysCheckIns.size() == 0) {
    if (isBeforeCheckInWindow) {
      String checkInTime = addMinutesToTimeStr(todaySchedule.checkInFrom, -GRACE_EARLY_IN);
      result.message = "Shift hasn't started yet. Check-in window opens at " + checkInTime;
      return result;
    }

    if (isAfterCheckInWindow) {
      String closedTime = addMinutesToTimeStr(todaySchedule.checkInTo, GRACE_LATE_IN);
      result.message = "Check-in window closed at " + closedTime + ". Please wait for next shift.";
      return result;
    }

    if (isCheckInWindow) {
      result.recordType = "in";
      result.success = true;

      if (currentTimeSec < ciFromSec) {
        result.status = "early";
        result.message = "Checked in early";
      } else if (currentTimeSec <= ciToSec) {
        result.status = "present";
        result.message = "Checked in on time";
      } else {
        result.status = "late";
        result.message = "Checked in late";
      }

      addRecord("in", currentTime, userId, cardUuid, userName);
    }
  }

  // 🔥 CASE 4: CHECK-OUT WINDOW CLOSED - YEH SABSE PEHLE CHECK KARO!
  else if (isAfterCheckOutWindow) {
    if (todaysCheckIns.size() > 0 && todaysCheckOuts.size() == 0) {
      // User checked in but didn't check out
      String checkInTime = formatTimeDisplay(todaySchedule.checkInFrom);
      String checkOutEndTime = formatTimeDisplay(todaySchedule.checkOutTo);
      String lateCheckOutEnd = addMinutesToTimeStr(todaySchedule.checkOutTo, GRACE_LATE_OUT);

      // Get next schedule
      String nextSchedule = getNextScheduleInfo(userId, currentDOW);

      result.message = "⚠️ Today's shift ended without check-out!\n";
      result.message += "   ✓ Check-in: " + checkInTime + "\n";
      result.message += "   ⛔ Check-out window closed: " + lateCheckOutEnd + "\n";
      result.message += "   📅 Next shift: " + nextSchedule;
      result.recordType = "in";
      return result;
    } else if (todaysCheckIns.size() == 0) {
      // User never checked in
      String checkInStartTime = formatTimeDisplay(todaySchedule.checkInFrom);
      String lateCheckInEnd = addMinutesToTimeStr(todaySchedule.checkInTo, GRACE_LATE_IN);

      String nextSchedule = getNextScheduleInfo(userId, currentDOW);

      result.message = "❌ You missed today's shift!\n";
      result.message += "   ✓ Check-in window was: " + checkInStartTime + " - " + lateCheckInEnd + "\n";
      result.message += "   📅 Next shift: " + nextSchedule;
      result.recordType = "in";
      return result;
    } else {
      // User already checked out
      String checkOutTime = "N/A";
      if (todaysCheckOuts.size() > 0) {
        checkOutTime = todaysCheckOuts.back().timestampStr;
      }

      result.message = "✅ Today's shift completed.\n";
      result.message += "   ✓ Check-out: " + formatTimeDisplay(checkOutTime);
      result.recordType = "out";
      return result;
    }
  }

  // CASE 2: WE'RE IN CHECK-OUT WINDOW - ALLOW MULTIPLE CHECK-OUTS
  else if (isCheckOutWindow) {
    result.recordType = "out";
    result.success = true;
    result.shouldDeletePreviousCheckOuts = true;

    if (currentTimeSec < coFromSec) {
      result.status = "early";
      result.message = "Checked out early";
    } else if (currentTimeSec <= coToSec) {
      result.status = "present";
      result.message = "Checked out on time";
    } else {
      result.status = "late";
      result.message = "Checked out late";
    }

    // Check minimum work duration for first check-out
    if (todaysCheckOuts.size() == 0 && todaysCheckIns.size() > 0) {
      TodaysRecord& lastCheckInRecord = todaysCheckIns.back();
      int minutesWorked = (currentTimeSec - lastCheckInRecord.timestamp) / 60;

      if (minutesWorked < MIN_WORK_DURATION) {
        result.success = false;
        result.message = "Minimum work duration not met. You need to work at least " + String(MIN_WORK_DURATION) + " minutes";
        return result;
      }
    }

    addRecord("out", currentTime, userId, cardUuid, userName);
  }

  // CASE 3: CHECK-OUT WINDOW NOT OPEN YET BUT USER CHECKED IN
  else if (hasOpenCheckIn && isBeforeCheckOutWindow) {
    String checkOutTime = formatTimeDisplay(todaySchedule.checkOutFrom);
    result.message = "Already checked in. Check-out window opens at " + checkOutTime;
    return result;
  }

  // DEFAULT CASE
  else {
    result.message = "Unable to process attendance at this time";
    return result;
  }

  // === 8. FORMAT TIMESTAMP FOR RESPONSE ===
  char timestampBuf[20];
  sprintf(timestampBuf, "%04d-%02d-%02d %02d:%02d:%02d",
          currentTime.year(), currentTime.month(), currentTime.day(),
          currentTime.hour(), currentTime.minute(), currentTime.second());
  result.timestamp = String(timestampBuf);
  result.formattedTime = formatTimeDisplay(secondsToTimeStr(currentTimeSec));

  return result;
}

String secondsToTimeStr(int seconds) {
  int h = (seconds / 3600) % 24;
  int m = (seconds % 3600) / 60;
  int s = seconds % 60;

  char buf[9];
  sprintf(buf, "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

String formatTimeDisplay(const String& timeStr) {
  if (timeStr.length() == 0 || timeStr == "Invalid Date" || timeStr == "null") {
    return "N/A";
  }

  int seconds = timeToSeconds(timeStr);
  int h = seconds / 3600;
  int m = (seconds % 3600) / 60;

  String period = (h >= 12) ? "PM" : "AM";
  int displayHour = (h % 12 == 0) ? 12 : h % 12;

  char buf[9];
  sprintf(buf, "%d:%02d %s", displayHour, m, period.c_str());
  return String(buf);
}

String addMinutesToTimeStr(const String& timeStr, int minutes) {
  int baseSec = timeToSeconds(timeStr);
  int newSec = baseSec + (minutes * 60);

  // Handle wrap around midnight
  newSec = newSec % 86400;

  int h = newSec / 3600;
  int m = (newSec % 3600) / 60;

  String period = (h >= 12) ? "PM" : "AM";
  int displayHour = (h % 12 == 0) ? 12 : h % 12;

  char buf[9];
  sprintf(buf, "%d:%02d %s", displayHour, m, period.c_str());
  return String(buf);
}

bool isTimeInRange(int timeSec, int startSec, int endSec) {
  if (startSec <= endSec) {
    return timeSec >= startSec && timeSec <= endSec;
  } else {
    return timeSec >= startSec || timeSec <= endSec;
  }
}

void addRecord(const String& type, const DateTime& timestamp, int userId, const String& cardUuid, const String& userName) {
  TodaysRecord record;
  record.recordType = type;

  record.timestamp = timestamp.hour() * 3600 + timestamp.minute() * 60 + timestamp.second();

  char buf[9];
  sprintf(buf, "%02d:%02d:%02d",
          timestamp.hour(), timestamp.minute(), timestamp.second());
  record.timestampStr = String(buf);

  if (type == "in") {
    todaysCheckIns.push_back(record);
    Serial.printf("📝 Added check-in at %s\n", record.timestampStr.c_str());
  } else {
    todaysCheckOuts.push_back(record);
    Serial.printf("📝 Added check-out at %s\n", record.timestampStr.c_str());
  }

  // Debug: Show current counts
  Serial.printf("📊 Today's totals - IN: %d, OUT: %d\n",
                todaysCheckIns.size(), todaysCheckOuts.size());
}

void resetTodayRecords() {
  todaysCheckIns.clear();
  todaysCheckOuts.clear();
  Serial.println("[LocalAttendance] Records reset for new day");
}

bool isNewDay(const DateTime& currentTime) {
  int todayInt = currentTime.year() * 10000 + currentTime.month() * 100 + currentTime.day();

  if (currentProcessingDay == -1) {
    currentProcessingDay = todayInt;
    return false;
  }

  if (currentProcessingDay != todayInt) {
    currentProcessingDay = todayInt;
    return true;
  }

  return false;
}

void checkDayChange(const DateTime& currentTime) {
  if (isNewDay(currentTime)) {
    resetTodayRecords();
  }
}

bool saveAttendanceLogToSD(
  const String& cardUuid,
  int userId,
  const String& userName,
  const String& timestamp,
  const String& recordType,
  const String& status,
  const String& message,
  const String& dayOfWeek,
  const String& checkInWindow,
  const String& checkOutWindow) {

  if (!sdMounted) {
    Serial.println("❌ SD not mounted, cannot save log");
    return false;
  }

  // Daily log file
  char filename[32];
  DateTime now = rtc.now();
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  // Check if file exists
  bool fileExists = SD.exists(filename);
  Serial.printf("File exists: %s\n", fileExists ? "YES" : "NO");

  // Open file for writing
  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Failed to open file!");
    return false;
  }

  // if (!file) {
  //   file = SD.open(filename, FILE_WRITE);
  //   if (!file) return false;
  //   file.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
  // }

  // String line = timestamp + "," + cardUuid + "," + String(userId) + "," + userName + "," + recordType + "," + status + "," + message + "," + dayOfWeek + "," + checkInWindow + "," + checkOutWindow;

  // file.println(line);
  // file.close();

  // // Main log file
  // File mainLog = SD.open(LOG_FILE, FILE_WRITE);
  // if (mainLog) {
  //   mainLog.println(line);
  //   mainLog.close();
  // }

  // If file exists, seek to end for appending
  if (fileExists) {
    if (!file.seek(file.size())) {
      Serial.println("❌ Failed to seek to end!");
      file.close();
      return false;
    }
    Serial.println("✅ Seeking to end of file");
  } else {
    // New file: write header
    file.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
    Serial.println("✅ Created new file with header");
  }

  // Create record line
  String line = timestamp + "," + cardUuid + "," + String(userId) + "," + userName + "," + recordType + "," + status + "," + message + "," + dayOfWeek + "," + checkInWindow + "," + checkOutWindow;

  // Write record
  file.println(line);

  // Get file size after writing
  size_t fileSize = file.size();

  file.close();

  Serial.printf("✅ Record saved! File size: %d bytes\n", fileSize);

  // Verify file was written
  File verifyFile = SD.open(filename, FILE_READ);
  if (verifyFile) {
    int lineCount = 0;
    while (verifyFile.available()) {
      verifyFile.readStringUntil('\n');
      lineCount++;
    }
    verifyFile.close();
    Serial.printf("📊 Total lines in file: %d\n", lineCount);
  }

  return true;
}

// ================== DAY NAME HELPER ==================
String getDayName(int dayOfWeek) {
  switch (dayOfWeek) {
    case 1: return "Monday";
    case 2: return "Tuesday";
    case 3: return "Wednesday";
    case 4: return "Thursday";
    case 5: return "Friday";
    case 6: return "Saturday";
    case 7: return "Sunday";
    default: return "Unknown";
  }
}

// ================== GET NEXT SCHEDULE ==================
String getNextScheduleInfo(int userId, int currentDOW) {
  String nextInfo = "No upcoming schedule";

  // Next 7 days check karo
  for (int i = 1; i <= 7; i++) {
    int checkDay = currentDOW + i;
    if (checkDay > 7) checkDay -= 7;

    for (const auto& schedule : userSchedules) {
      if (schedule.userId == userId && schedule.dayOfWeek == checkDay) {
        String dayName = getDayName(checkDay);
        String checkInTime = formatTimeDisplay(schedule.checkInFrom);
        return dayName + " at " + checkInTime;
      }
    }
  }
  return "No upcoming schedule";
}

// ================== LOAD TODAY'S RECORDS FROM SD CARD ==================
void loadTodayRecordsFromSD() {
  // Pehle existing records clear karo
  todaysCheckIns.clear();
  todaysCheckOuts.clear();

  // Aaj ki date ke hisaab se filename banayo
  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  Serial.print("📂 Checking for today's log file: ");
  Serial.println(filename);

  if (!SD.exists(filename)) {
    Serial.println("📝 No log file for today yet");
    return;
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    Serial.println("❌ Failed to open today's log file");
    return;
  }

  int recordCount = 0;
  int checkInCount = 0;
  int checkOutCount = 0;

  // Skip header line
  String header = file.readStringUntil('\n');

  // Read each line
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse CSV line
    // Format: Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow
    int commaIndex = 0;
    int startPos = 0;
    int fieldIndex = 0;

    String timestamp, cardUuid, userIdStr, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

    while (startPos < line.length()) {
      commaIndex = line.indexOf(',', startPos);
      if (commaIndex == -1) commaIndex = line.length();

      String field = line.substring(startPos, commaIndex);

      switch (fieldIndex) {
        case 0: timestamp = field; break;
        case 1: cardUuid = field; break;
        case 2: userIdStr = field; break;
        case 3: userName = field; break;
        case 4: recordType = field; break;
        case 5: status = field; break;
        case 6: message = field; break;
        case 7: dayOfWeek = field; break;
        case 8: checkInWindow = field; break;
        case 9: checkOutWindow = field; break;
      }

      startPos = commaIndex + 1;
      fieldIndex++;
    }

    // Sirf aaj ke records add karo (jo already is file mein hain woh sab aaj ke hi hain)
    if (recordType == "in" || recordType == "out") {
      TodaysRecord record;
      record.recordType = recordType;

      // Timestamp se time nikaalo (format: YYYY-MM-DD HH:MM:SS)
      int timeStart = timestamp.indexOf(' ') + 1;
      String timeStr = timestamp.substring(timeStart);

      int h = timeStr.substring(0, 2).toInt();
      int m = timeStr.substring(3, 5).toInt();
      int s = timeStr.substring(6, 8).toInt();
      record.timestamp = h * 3600 + m * 60 + s;
      record.timestampStr = timeStr;

      if (recordType == "in") {
        todaysCheckIns.push_back(record);
        checkInCount++;
      } else {
        todaysCheckOuts.push_back(record);
        checkOutCount++;
      }

      recordCount++;
    }
  }

  file.close();

  Serial.println("✅ Loaded records from SD card:");
  Serial.printf("   Total records: %d\n", recordCount);
  Serial.printf("   Check-ins: %d\n", checkInCount);
  Serial.printf("   Check-outs: %d\n", checkOutCount);

  // Sort records by timestamp (oldest first)
  // Not necessary but helpful for debugging
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
// #include <ESP8266WebServer.h>
// #include <WiFiManager.h>
// #include <ArduinoJson.h>
// #include <SPI.h>
// #include <MFRC522.h>
// #include <Wire.h>
// #include <PCF8574.h>
// #include <SD.h>
// #include <FS.h>
// #include <EEPROM.h>
// #include <RTClib.h>
// #include <time.h>

// ESP8266WebServer server(80);

// // ================== PCF8574 CONFIG ==================
// #define PCF_ADDRESS 0x20  // A0,A1,A2 pins according to wiring
// PCF8574 pcf(PCF_ADDRESS);

// // ================== EEPROM CONFIG ==================
// #define EEPROM_LOCAL_STORAGE_ADDR 20

// // ================== SD CARD CONFIG ==================
// #define SD_CS_PIN D8  // SD card CS pin
// #define SCHEDULE_FILE "/schedule.json"

// // ================== LED PINS on PCF8574 ==================
// #define WHITE_LED_PIN 0  // Main Status LED
// #define BLUE_LED_PIN 1   // Server Status

// // ================== RFID + GREEN/RED LED PINS ==================
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
// // #define EEPROM_SIZE 4096
// #define SCHEDULE_START_ADDR 0
// #define SCHEDULE_CRC_ADDR 1000
// #define SCHEDULE_COUNT_ADDR 1004
// #define LAST_UPDATE_ADDR 1008

// // ================== TIMEZONE CONFIG ==================
// String deviceTimezone = "UTC";
// int timezoneOffsetMinutes = 0;  // +300 for Asia/Karachi

// #define EEPROM_TZ_ADDR 40
// #define EEPROM_OFFSET_ADDR 100


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
// bool sdMounted = false;
// bool localStorage = true;

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

// RTC_DS3231 rtc;

// // ================== LOCAL TIME HELPER ==================
// DateTime getLocalTime() {
//   DateTime utc = rtc.now();

//   // Convert minutes to hours and minutes
//   int offsetHours = timezoneOffsetMinutes / 60;
//   int offsetMinutes = timezoneOffsetMinutes % 60;

//   // Create TimeSpan with correct parameters
//   TimeSpan ts(0, offsetHours, offsetMinutes, 0);

//   DateTime local = utc + ts;

//   Serial.printf("Local: %02d:%02d:%02d\n",
//                 local.hour(), local.minute(), local.second());

//   return local;
// }

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

//           // Save to sd card
//           if (localStorage && sdMounted) {
//             saveScheduleToSD();
//           }

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

// // ================== WIFI CONNECTION FUNCTION  ==================
// void handleConnectWifi() {
//   if (server.method() != HTTP_POST) {
//     server.send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
//     return;
//   }

//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"error\":\"Invalid device auth\"}");
//     return;
//   }

//   DynamicJsonDocument doc(256);
//   DeserializationError err = deserializeJson(doc, server.arg("plain"));

//   if (err) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
//     return;
//   }

//   String ssid = doc["ssid"].as<String>();
//   String password = doc["password"].as<String>();

//   if (ssid.length() == 0) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"SSID missing\"}");
//     return;
//   }

//   server.send(
//     200,
//     "application/json",
//     "{\"success\":true,\"message\":\"WiFi credentials received\"}");

//   server.client().flush();
//   delay(300);

//   bool connected = connectToWifi(ssid, password);

//   if (connected) {
//     server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi connected\"}");
//   } else {
//     server.send(500, "application/json", "{\"success\":false,\"message\":\"WiFi connection failed\"}");
//   }
// }

// // ================== LOCALSTORAGE HANDLE FUNCTION ==============
// void handleLocalStorage() {
//   if (server.method() != HTTP_POST) {
//     server.send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
//     return;
//   }

//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"error\":\"Invalid device auth\"}");
//     return;
//   }

//   DynamicJsonDocument doc(256);
//   deserializeJson(doc, server.arg("plain"));

//   if (!doc.containsKey("enabled")) {
//     server.send(400, "application/json", "{\"error\":\"enabled required\"}");
//     return;
//   }

//   bool newValue = doc["enabled"];

//   if (localStorage != newValue) {
//     localStorage = newValue;

//     EEPROM.write(
//       EEPROM_LOCAL_STORAGE_ADDR,
//       localStorage ? 1 : 0);
//     EEPROM.commit();
//   }

//   // ===== SD MOUNT / UNMOUNT =====
//   if (localStorage && !sdMounted) {
//     sdMounted = SD.begin(SD_CS_PIN);
//     Serial.println("📀 SD mounted after enable");
//   }

//   if (!localStorage && sdMounted) {
//     SD.end();
//     sdMounted = false;
//     userSchedules.clear();
//     Serial.println("📀 SD unmounted after disable");
//   }

//   Serial.println("🔁 LocalStorage toggled from server");
//   Serial.print("💾 Saved localStorage = ");
//   Serial.println(localStorage ? "TRUE" : "FALSE");

//   server.send(
//     200,
//     "application/json",
//     localStorage
//       ? "{\"success\":true,\"localStorage\":true}"
//       : "{\"success\":true,\"localStorage\":false}");

//   server.client().flush();
//   delay(300);

//   ESP.reset();
// }

// // ================ LOCAL TIMEZONE HANDLE FUNCTION ==============
// void handleSetTimezone() {
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
//     return;
//   }

//   if (
//     server.header("x-device-id") != DEVICE_UUID || server.header("x-device-secret") != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"error\":\"Invalid device auth\"}");
//     return;
//   }

//   DynamicJsonDocument doc(256);
//   if (deserializeJson(doc, server.arg("plain"))) {
//     server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
//     return;
//   }

//   deviceTimezone = doc["timezone"].as<String>();  // Asia/Karachi
//   String offsetStr = doc["offset"];               // +05:00

//   int hours = offsetStr.substring(1, 3).toInt();
//   int minutes = offsetStr.substring(4, 6).toInt();
//   timezoneOffsetMinutes = hours * 60 + minutes;

//   if (offsetStr.startsWith("-")) {
//     timezoneOffsetMinutes *= -1;
//   }

//   // ===== SAVE TO EEPROM =====
//   for (int i = 0; i < 40; i++) EEPROM.write(EEPROM_TZ_ADDR + i, 0);
//   for (int i = 0; i < deviceTimezone.length(); i++) {
//     EEPROM.write(EEPROM_TZ_ADDR + i, deviceTimezone[i]);
//   }

//   EEPROM.put(EEPROM_OFFSET_ADDR, timezoneOffsetMinutes);
//   EEPROM.commit();

//   Serial.printf(
//     "🌍 Timezone updated: %s (%d min)\n",
//     deviceTimezone.c_str(),
//     timezoneOffsetMinutes);

//   // IMMEDIATE TIME CHECK
//   DateTime utc = rtc.now();
//   DateTime local = getLocalTime();

//   // Calculate difference
//   int diffHours = local.hour() - utc.hour();
//   int diffMinutes = local.minute() - utc.minute();

//   server.send(200, "application/json", "{\"success\":true}");
// }

// // ================== ESP SERVER SETUP ==================
// void setupServer() {
//   server.collectHeaders(
//     "x-device-id",
//     "x-device-secret");
//   server.on("/api/connect-wifi", handleConnectWifi);
//   server.on("/api/wifi/scan", HTTP_GET, scanAndSendWifi);
//   server.on("/api/toggle-local-storage", handleLocalStorage);
//   server.on("/api/set-timezone", HTTP_POST, handleSetTimezone);

//   server.begin();
//   Serial.println("HTTP server started");
// }

// // ============= RTC Update REAL UTC TIME =============
// void setRTCFromNTP() {
//   if (WiFi.status() != WL_CONNECTED) {
//     Serial.println("WiFi not connected, cannot sync time.");
//     return;
//   }

//   configTime(0, 0, "pool.ntp.org", "time.nist.gov");

//   time_t now;
//   int retries = 30;

//   while ((now = time(nullptr)) < 1000000000 && retries > 0) {
//     Serial.print(".");
//     delay(1000);
//     retries--;
//   }

//   if (now < 1000000000) {
//     Serial.println("❌ Failed to get NTP time");
//     return;
//   }

//   struct tm* timeinfo = gmtime(&now);
//   DateTime ntpTime(
//     timeinfo->tm_year + 1900,
//     timeinfo->tm_mon + 1,
//     timeinfo->tm_mday,
//     timeinfo->tm_hour,
//     timeinfo->tm_min,
//     timeinfo->tm_sec);

//   rtc.adjust(ntpTime);

//   DateTime rtcTime = rtc.now();
//   Serial.printf("RTC Set NTP to: %04d-%02d-%02d %02d:%02d:%02d\n",
//                 rtcTime.year(),
//                 rtcTime.month(),
//                 rtcTime.day(),
//                 rtcTime.hour(),
//                 rtcTime.minute(),
//                 rtcTime.second());

//   Serial.println("RTC updated from NTP");
// }

// // ================== SETUP ==================
// void setup() {
//   Serial.begin(115200);
//   delay(1000);

//   Serial.println("\n\n=== ESP8266 Attendance Device Starting ===");

//   // ================== EEPROM INIT ==================
//   EEPROM.begin(512);
//   uint8_t storedLS = EEPROM.read(EEPROM_LOCAL_STORAGE_ADDR);

//   if (storedLS == 0 || storedLS == 1) {
//     localStorage = storedLS;
//   } else {
//     localStorage = true;  // default
//     EEPROM.write(EEPROM_LOCAL_STORAGE_ADDR, 1);
//     EEPROM.commit();
//   }

//   Serial.print("🔁 Boot LocalStorage = ");
//   Serial.println(localStorage ? "TRUE" : "FALSE");

//   // ================== LOAD TIMEZONE FROM EEPROM ==================
//   char tzBuf[41];
//   for (int i = 0; i < 40; i++) {
//     tzBuf[i] = EEPROM.read(EEPROM_TZ_ADDR + i);
//   }
//   tzBuf[40] = '\0';
//   deviceTimezone = String(tzBuf);
//   EEPROM.get(EEPROM_OFFSET_ADDR, timezoneOffsetMinutes);

//   if (deviceTimezone.length() == 0) {
//     deviceTimezone = "UTC";
//     timezoneOffsetMinutes = 0;
//   }

//   Serial.printf(
//     "🌍 Timezone loaded: %s | Offset: %d minutes\n",
//     deviceTimezone.c_str(),
//     timezoneOffsetMinutes);


//   // ================== SD CARD INIT ==================
//   if (localStorage) {
//     sdMounted = SD.begin(SD_CS_PIN);
//     if (!sdMounted) {
//       Serial.println("❌ SD Card mount failed");
//     } else {
//       Serial.println("✅ SD Card mounted successfully");
//     }
//   } else {
//     sdMounted = false;
//     Serial.println("Local storage disabled — SD card skipped");
//   }

//   // ================== LOAD SCHEDULE FROM SD ==================
//   if (localStorage && sdMounted) {
//     if (loadScheduleFromSD()) {
//       Serial.println("Using stored schedules from SD card");
//       isInitialScheduleLoaded = true;
//     }
//   }

//   // ================== PCF8574 INIT ==================
//   Wire.begin(D1, D2);  // SDA, SCL
//   pcf.begin();

//   // INITIAL LED STATES - SAB OFF KARO
//   pcf.write(WHITE_LED_PIN, LOW);
//   pcf.write(BLUE_LED_PIN, LOW);
//   pcf.write(GREEN_LED_PIN, LOW);
//   pcf.write(RED_LED_PIN, LOW);

//   // ================== RTC INIT ==================
//   if (!rtc.begin()) {
//     Serial.println("❌ RTC not found");
//   } else {
//     Serial.println("RTC detected");
//     DateTime localNow = getLocalTime();
//   }

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
//   sendWifiStatusToServer(
//     "connected",
//     WiFi.SSID(),
//     false,
//     "old Wifi");
//   setRTCFromNTP();

//   // start esp server
//   setupServer();

//   // Fetch schedules immediately after WiFi connection
//   if (WiFi.status() == WL_CONNECTED) {
//     if (localStorage) {
//       Serial.println("Fetching fresh schedules from server...");
//       fetchAndStoreSchedules();
//     }
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
//     server.handleClient();
//     if (localStorage) {
//       if (millis() - lastScheduleUpdate >= scheduleUpdateInterval) {
//         fetchAndStoreSchedules();
//         lastScheduleUpdate = millis();
//       }
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

//     // ----------------- RFID ATTENDANCE -----------------
//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       String cardUuid = "";
//       for (byte i = 0; i < rfid.uid.size; i++) {
//         cardUuid += String(rfid.uid.uidByte[i], HEX);
//       }
//       cardUuid.toUpperCase();

//       Serial.println("Card scanned: " + cardUuid);

//       // First check in local schedule (if localstorage is enabled)
//       if (localStorage && userSchedules.size() > 0) {
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

//   // ================== SD CARD STATUS ==================
//   JsonObject sd = doc.createNestedObject("sd");
//   sd["enabled"] = localStorage;
//   sd["mounted"] = localStorage ? sdMounted : false;
//   sd["scheduleFileExists"] = (localStorage && sdMounted) ? SD.exists(SCHEDULE_FILE) : false;

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
//   // if (command && strcmp(command, "UPDATE_SCHEDULES") == 0) {
//   //   // Force schedule update from server
//   //   Serial.println("Force schedule update requested from server");
//   //   if (WiFi.status() == WL_CONNECTED) {
//   //     fetchAndStoreSchedules();
//   //   }
//   // }
// }

// // ================== CONNECT NEW WIFI ==================
// bool connectToWifi(String ssid, String password) {
//   Serial.println("\n=== WIFI CONNECT ===");
//   Serial.println("SSID: " + ssid);
//   Serial.println("Password: " + password);


//   // Disconnect previous WiFi
//   isConnecting = true;
//   WiFi.disconnect(true);
//   delay(500);

//   // Set WiFi mode
//   WiFi.mode(WIFI_STA);

//   // Connect to new WiFi
//   WiFi.begin(ssid.c_str(), password.c_str());

//   Serial.println("Connecting...");

//   // Connect hone ka wait karo with timeout (20 seconds)
//   int timeout = 0;
//   while (WiFi.status() != WL_CONNECTED && timeout < 40) {
//     // White LED fast blink during connection attempt
//     pcf.write(WHITE_LED_PIN, HIGH);
//     delay(100);
//     pcf.write(WHITE_LED_PIN, LOW);
//     delay(100);

//     Serial.print(".");
//     timeout++;
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("\n✅ WiFi Connected!");
//     Serial.print("IP Address: ");
//     Serial.println(WiFi.localIP());

//     // Success - green blink
//     blinkGreenOnce();

//     // Fetch fresh schedules after connecting
//     if (localStorage) {
//       fetchAndStoreSchedules();
//     }

//     // WIFI STATUS CALLBACK
//     sendWifiStatusToServer(
//       "connected",
//       ssid,
//       true,
//       "");

//     isConnecting = false;
//     return true;
//   } else {
//     Serial.println("\n❌ WiFi Connection Failed!");

//     // Failure - red blink twice
//     blinkRedTwice();

//     // WIFI STATUS CALLBACK (FAIL)
//     sendWifiStatusToServer(
//       "failed",
//       ssid,
//       false,
//       "CONNECTION_TIMEOUT");

//     delay(500);
//     ESP.restart();

//     return false;
//   }
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

//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"error\":\"Missing auth headers\"}");
//     return;
//   }

//   String reqUuid = server.header("x-device-id");
//   String reqSecret = server.header("x-device-secret");

//   // check device auth
//   if (reqUuid.length() == 0 || reqSecret.length() == 0) {
//     server.send(
//       401,
//       "application/json",
//       "{\"success\":false,\"message\":\"Device credentials missing\"}");
//     return;
//   }

//   if (reqUuid != DEVICE_UUID || reqSecret != DEVICE_SECRET) {
//     Serial.println("❌ Device auth failed not send wifi scan list");

//     server.send(
//       403,
//       "application/json",
//       "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

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
//     net["secure"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
//   }

//   String json;
//   serializeJson(doc, json);

//   server.send(200, "application/json", json);

//   WiFi.scanDelete();  // memory clean
// }

// // ================== SEND ATTENDANCE FUNCTIONS ==================
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

// // ================= SEND WIFI CONNECTION STATUS =================
// void sendWifiStatusToServer(
//   String status,
//   String ssid,
//   bool isNewWifi,
//   String reason) {
//   if (WiFi.status() != WL_CONNECTED) return;

//   HTTPClient http;
//   WiFiClient client;

//   String url = String(SERVER_URL) + "/api/v1/device/wifi-status";
//   http.begin(client, url);

//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);
//   http.addHeader("Connection", "close");

//   DynamicJsonDocument doc(256);
//   doc["deviceUuid"] = DEVICE_UUID;
//   doc["status"] = status;  // connected | failed
//   doc["ssid"] = ssid;
//   doc["ip"] = WiFi.localIP().toString();
//   doc["rssi"] = WiFi.RSSI();
//   doc["isNewWifi"] = isNewWifi;
//   doc["reason"] = reason;

//   String payload;
//   serializeJson(doc, payload);

//   Serial.println("📡 WiFi Status Report:");
//   Serial.println(payload);

//   http.PATCH(payload);
//   http.end();
// }

// // ================== SAVE SCHEDULE FUNCTIONS ==================
// void saveScheduleToSD() {
//   DynamicJsonDocument doc(4096);
//   JsonArray users = doc.createNestedArray("users");

//   for (auto& u : userSchedules) {
//     JsonObject o = users.createNestedObject();
//     o["id"] = u.userId;
//     o["cardUuid"] = u.cardUuid;
//     o["name"] = u.userName;
//     o["dayOfWeek"] = u.dayOfWeek;
//     o["checkInFrom"] = u.checkInFrom;
//     o["checkInTo"] = u.checkInTo;
//     o["checkOutFrom"] = u.checkOutFrom;
//     o["checkOutTo"] = u.checkOutTo;
//   }

//   File file = SD.open(SCHEDULE_FILE, FILE_WRITE);
//   if (!file) {
//     Serial.println("❌ Failed to open SD file for writing");
//     return;
//   }

//   serializeJson(doc, file);
//   file.close();

//   Serial.println("✅ Schedule saved to SD card");
// }

// // ================== LOAD SCHEDULE FUNCTIONS ==================
// bool loadScheduleFromSD() {
//   if (!SD.exists(SCHEDULE_FILE)) {
//     Serial.println("⚠️ No schedule file on SD");
//     return false;
//   }

//   File file = SD.open(SCHEDULE_FILE);
//   if (!file) return false;

//   DynamicJsonDocument doc(4096);
//   if (deserializeJson(doc, file)) {
//     file.close();
//     return false;
//   }
//   file.close();

//   userSchedules.clear();

//   for (JsonObject u : doc["users"].as<JsonArray>()) {
//     UserSchedule s;
//     s.userId = u["id"];
//     s.cardUuid = u["cardUuid"].as<String>();
//     s.userName = u["name"].as<String>();
//     s.dayOfWeek = u["dayOfWeek"];
//     s.checkInFrom = u["checkInFrom"].as<String>();
//     s.checkInTo = u["checkInTo"].as<String>();
//     s.checkOutFrom = u["checkOutFrom"].as<String>();
//     s.checkOutTo = u["checkOutTo"].as<String>();
//     userSchedules.push_back(s);
//   }

//   Serial.println("✅ Loaded schedules from SD: " + String(userSchedules.size()));
//   return true;
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