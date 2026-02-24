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
#include <EEPROM.h>
#include <RTClib.h>
#include <time.h>
#include <PubSubClient.h>

// ================== MQTT CONFIG ==================
#define MQTT_SERVER "broker.emqx.io"
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASSWORD ""

#define MQTT_TOPIC_PREFIX "device/9908bd1f-9571-4b49-baa4-36e4f27eab36"
#define MQTT_TOPIC_COMMAND MQTT_TOPIC_PREFIX "/command"
#define MQTT_TOPIC_RESPONSE MQTT_TOPIC_PREFIX "/response"
#define MQTT_TOPIC_STATUS MQTT_TOPIC_PREFIX "/status"
#define MQTT_TOPIC_HEARTBEAT MQTT_TOPIC_PREFIX "/heartbeat"

// ================== PCF8574 CONFIG ==================
#define PCF_ADDRESS 0x20
PCF8574 pcf(PCF_ADDRESS);

// ================== EEPROM CONFIG ==================
#define EEPROM_LOCAL_STORAGE_ADDR 20
#define EEPROM_AUTO_SYNC_ADDR 21
#define EEPROM_TZ_ADDR 40
#define EEPROM_OFFSET_ADDR 100

// ================== SD CARD CONFIG ==================
#define SD_CS_PIN D8
#define SCHEDULE_FILE "/schedule.json"

// ================== LED PINS on PCF8574 ==================
#define WHITE_LED_PIN 0
#define BLUE_LED_PIN 1
#define GREEN_LED_PIN 2
#define RED_LED_PIN 3

// ================== RFID PINS ==================
#define RST_PIN D3
#define SS_PIN D4

// ================== TESTING CONFIG ==================
#define USE_NTP_TIME true

// ================== MANUAL TIME FOR TESTING ==================
int manualYear = 2026;
int manualMonth = 2;
int manualDay = 10;
int manualHour = 2;
int manualMinute = 10;
int manualSecond = 0;

// ================== ATTENDANCE LOGIC HEADER ==================
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
  String recordType;
  String status;
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

// ================== TIMEZONE CONFIG ==================
String deviceTimezone = "UTC";
int timezoneOffsetMinutes = 0;

// ================== GLOBAL VARIABLES ==================
unsigned long lastHeartbeatTime = 0;
unsigned long lastScanTime = 0;
unsigned long lastLedToggle = 0;
unsigned long lastConnectBlinkToggle = 0;
unsigned long lastScheduleUpdate = 0;
unsigned long scheduleUpdateInterval = 3600000;

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

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttReconnectAttempt = 0;
bool mqttConnected = false;

// ================== LOCAL ATTENDANCE LOGIC VARIABLES ==================
const int GRACE_EARLY_IN = 15;
const int GRACE_LATE_IN = 15;
const int GRACE_EARLY_OUT = 0;
const int GRACE_LATE_OUT = 15;
const int MIN_WORK_DURATION = 30;
const int MAX_SHIFT_HOURS = 18;
const int MIN_GAP_BETWEEN_RECORDS = 10;

bool autoSyncEnabled = true;
unsigned long lastSyncTime = 0;
const unsigned long SYNC_INTERVAL = 300000;
bool isSyncing = false;

std::vector<TodaysRecord> todaysCheckIns;
std::vector<TodaysRecord> todaysCheckOuts;
int currentProcessingDay = -1;

// ================== DAILY LOG FILE ==================
const char* LOG_FILE = "/attendance_logs.csv";
const char* DAILY_LOG_PREFIX = "/log_";

// ================== MONTHLY LOG FILE ==================
#define MONTHLY_LOG_PREFIX "/monthly_"

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

// ================== FUNCTION PROTOTYPES ==================
void mqttCallback(char* topic, byte* payload, unsigned int length);
void handleMqttCommand(String command, JsonDocument& doc);
void publishMqttResponse(bool success, String message, String commandId = "");
void publishMqttStatus(String status, String message);
void publishHeartbeat();
DateTime getLocalTime();
bool connectToWifi(String ssid, String password);
void checkReprovision();
void checkServerUnreachable();
void sendHeartbeat();
void handleRFID();
bool sendAttendance(String cardUuid);
void setRTCFromNTP();
void fetchAndStoreSchedules();
void saveScheduleToSD();
bool loadScheduleFromSD();
void loadTodayRecordsFromSD();
void cleanupOldDailyFiles();
bool syncMonthlyRecordsToServer(int year, int month);
bool sendAttendanceRecordToServer(const String& cardUuid, int userId, const String& userName, const String& timestamp, const String& recordType, const String& status, const String& message, const String& dayOfWeek, const String& checkInWindow, const String& checkOutWindow);
String getTodayAttendanceRecords();
String getUserAttendanceRecords(String targetCardUuid);
String deleteUserAttendanceRecords(String targetCardUuid);
String getMonthlyAttendanceRecords(int year, int month);
String getUserMonthlyRecords(String targetCardUuid, int year, int month);
String deleteMonthlyFile(int year, int month);
String deleteUserFromMonthlyFile(String targetCardUuid, int year, int month);
String listAllLogFiles();
int timeToSeconds(const String& timeStr);
String secondsToTimeStr(int seconds);
String formatTimeDisplay(const String& timeStr);
String addMinutesToTimeStr(const String& timeStr, int minutes);
bool isTimeInRange(int timeSec, int startSec, int endSec);
void addRecord(const String& type, const DateTime& timestamp, int userId, const String& cardUuid, const String& userName);
void resetTodayRecords();
bool isNewDay(const DateTime& currentTime);
void checkDayChange(const DateTime& currentTime);
String getDayName(int dayOfWeek);
String getNextScheduleInfo(int userId, int currentDOW);
AttendanceResult processLocalAttendance(const String& cardUuid, int userId, const String& userName, const UserSchedule& todaySchedule, const DateTime& currentTime);
bool saveAttendanceLogToSD(const String& cardUuid, int userId, const String& userName, const String& timestamp, const String& recordType, const String& status, const String& message, const String& dayOfWeek, const String& checkInWindow, const String& checkOutWindow);
bool findUserSchedule(int userId, int dayOfWeek, UserSchedule*& foundSchedule);
void blinkWhiteLED();
void blinkGreenOnce();
void blinkRedTwice();

// ================== LOCAL TIME HELPER ==================
DateTime getLocalTime() {
  DateTime utc = rtc.now();
  long totalOffsetSeconds = timezoneOffsetMinutes * 60L;
  time_t utcUnix = utc.unixtime();
  time_t localUnix = utcUnix + totalOffsetSeconds;
  DateTime local = DateTime(localUnix);

  //   // Debug output
  // Serial.printf("🌍 UTC:  %04d-%02d-%02d %02d:%02d:%02d\n",
  //               utc.year(), utc.month(), utc.day(),
  //               utc.hour(), utc.minute(), utc.second());

  // Serial.printf("📍 Local: %04d-%02d-%02d %02d:%02d:%02d (Offset: %d min)\n",
  //               local.year(), local.month(), local.day(),
  //               local.hour(), local.minute(), local.second(),
  //               timezoneOffsetMinutes);

  return local;
}

// ================== FIND USER SCHEDULE ==================
bool findUserSchedule(int userId, int dayOfWeek, UserSchedule*& foundSchedule) {
  for (auto& schedule : userSchedules) {
    if (schedule.userId == userId && schedule.dayOfWeek == dayOfWeek) {
      foundSchedule = &schedule;
      return true;
    }
  }
  return false;
}

// ================== MQTT CONNECTION ==================
bool connectToMqtt() {
  Serial.print("Connecting to MQTT...");

  String clientId = "ESP8266-";
  clientId += String(DEVICE_UUID);

  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("connected");
    mqttClient.subscribe(MQTT_TOPIC_COMMAND);
    publishMqttStatus("online", "Device connected to MQTT");
    mqttConnected = true;
    return true;
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqttClient.state());
    mqttConnected = false;
    return false;
  }
}

// ================== PUBLISH MQTT STATUS ==================
void publishMqttStatus(String status, String message) {
  DynamicJsonDocument doc(256);
  doc["deviceUuid"] = DEVICE_UUID;
  doc["status"] = status;
  doc["message"] = message;
  doc["timestamp"] = getLocalTime().unixtime();
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["localStorage"] = localStorage;
  doc["sdMounted"] = sdMounted;

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(MQTT_TOPIC_STATUS, payload.c_str());
}

// ================== PUBLISH MQTT RESPONSE ==================
void publishMqttResponse(bool success, String message, String commandId) {
  DynamicJsonDocument doc(256);
  doc["success"] = success;
  doc["message"] = message;
  doc["timestamp"] = getLocalTime().unixtime();

  if (commandId.length() > 0) {
    doc["commandId"] = commandId;
  }

  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(MQTT_TOPIC_RESPONSE, payload.c_str());
}

// ================== PUBLISH HEARTBEAT ==================
void publishHeartbeat() {
  DynamicJsonDocument doc(512);

  doc["deviceUuid"] = DEVICE_UUID;
  doc["status"] = "connected";
  doc["ip"] = WiFi.localIP().toString();
  doc["schedulesLoaded"] = userSchedules.size();
  doc["autoSyncEnabled"] = autoSyncEnabled;
  doc["timestamp"] = getLocalTime().unixtime();

  JsonObject sd = doc.createNestedObject("sd");
  sd["enabled"] = localStorage;
  sd["mounted"] = sdMounted;
  sd["scheduleFileExists"] = (localStorage && sdMounted) ? SD.exists(SCHEDULE_FILE) : false;

  String payload;
  serializeJson(doc, payload);
  bool published = mqttClient.publish(MQTT_TOPIC_HEARTBEAT, payload.c_str());

  if (published) {
    heartbeatFailCount = 0;
    serverUnreachable = false;
    Serial.println("✅ Heartbeat sent successfully");
  } else {
    heartbeatFailCount++;
    Serial.println("❌ Heartbeat publish failed");
    checkServerUnreachable();
  }
}

// ================== MQTT CALLBACK ==================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");

  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, message);

  if (error) {
    Serial.println("Failed to parse MQTT message");
    return;
  }

  String deviceId = doc["deviceUuid"] | "";
  String deviceSecret = doc["deviceSecret"] | "";

  if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
    Serial.println("MQTT auth failed");
    publishMqttResponse(false, "Authentication failed");
    return;
  }

  String command = doc["command"] | "";

  if (command.length() == 0) {
    publishMqttResponse(false, "No command specified");
    return;
  }

  handleMqttCommand(command, doc);
}

// ================== MQTT COMMAND HANDLER ==================
void handleMqttCommand(String command, JsonDocument& doc) {
  Serial.println("Handling MQTT command: " + command);

  if (command == "get_status") {
    String commandId = doc["commandId"] | "";
    publishMqttResponse(true, "Device status");

    DynamicJsonDocument data(256);
    data["localStorage"] = localStorage;
    data["sdMounted"] = sdMounted;
    data["wifiConnected"] = (WiFi.status() == WL_CONNECTED);
    data["rssi"] = WiFi.RSSI();
    data["ip"] = WiFi.localIP().toString();
    data["schedulesLoaded"] = userSchedules.size();
    data["timezone"] = deviceTimezone;
    data["offset"] = timezoneOffsetMinutes;
    data["autoSync"] = autoSyncEnabled;
    data["todayCheckIns"] = todaysCheckIns.size();
    data["todayCheckOuts"] = todaysCheckOuts.size();

    // ADD commandId if present
    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
  }

  else if (command == "scan_wifi") {
    String commandId = doc["commandId"] | "";

    publishMqttResponse(true, "WiFi scan completed");

    int n = WiFi.scanNetworks();

    DynamicJsonDocument data(4096);
    JsonArray networks = data.createNestedArray("networks");

    for (int i = 0; i < n; i++) {
      JsonObject net = networks.createNestedObject();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["channel"] = WiFi.channel(i);
      net["secure"] = (WiFi.encryptionType(i) != ENC_TYPE_NONE);
    }

    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    WiFi.scanDelete();

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
  }

  else if (command == "connect_wifi") {
    String commandId = doc["commandId"] | "";
    String ssid = doc["ssid"] | "";
    String password = doc["password"] | "";

    if (ssid.length() == 0) {
      publishMqttResponse(false, "SSID missing");
      return;
    }

    publishMqttResponse(true, "WiFi credentials received. Connecting...");

    delay(500);

    bool connected = connectToWifi(ssid, password);

    if (connected) {
      publishMqttStatus("wifi_connected", "Connected to " + ssid);
    }
  }

  else if (command == "toggle_local_storage") {
    String commandId = doc["commandId"] | "";

    bool newValue = doc["enabled"] | localStorage;

    if (localStorage != newValue) {
      localStorage = newValue;
      EEPROM.write(EEPROM_LOCAL_STORAGE_ADDR, localStorage ? 1 : 0);
      EEPROM.commit();

      if (localStorage && !sdMounted) {
        sdMounted = SD.begin(SD_CS_PIN);
      }

      if (!localStorage && sdMounted) {
        SD.end();
        sdMounted = false;
        userSchedules.clear();
      }

      publishMqttResponse(true, "Local storage toggled");

      DynamicJsonDocument data(64);
      data["localStorage"] = localStorage;

      if (commandId.length() > 0) {
        data["commandId"] = commandId;
      }

      String dataPayload;
      serializeJson(data, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

      delay(1000);
      ESP.restart();
    } else {
      publishMqttResponse(true, "Already set");

      DynamicJsonDocument data(64);
      data["localStorage"] = localStorage;

      if (commandId.length() > 0) {
        data["commandId"] = commandId;
      }

      String dataPayload;
      serializeJson(data, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
    }
  }

  else if (command == "set_timezone") {
    String commandId = doc["commandId"] | "";

    deviceTimezone = doc["timezone"] | "UTC";
    String offsetStr = doc["offset"] | "+00:00";

    int hours = offsetStr.substring(1, 3).toInt();
    int minutes = offsetStr.substring(4, 6).toInt();
    timezoneOffsetMinutes = hours * 60 + minutes;

    if (offsetStr.startsWith("-")) {
      timezoneOffsetMinutes *= -1;
    }

    for (int i = 0; i < 40; i++) EEPROM.write(EEPROM_TZ_ADDR + i, 0);
    for (int i = 0; i < deviceTimezone.length(); i++) {
      EEPROM.write(EEPROM_TZ_ADDR + i, deviceTimezone[i]);
    }

    EEPROM.put(EEPROM_OFFSET_ADDR, timezoneOffsetMinutes);
    EEPROM.commit();

    publishMqttResponse(true, "Timezone updated");

    DynamicJsonDocument data(64);
    data["timezone"] = deviceTimezone;
    data["offset"] = timezoneOffsetMinutes;

    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
  }

  else if (command == "get_attendance_records") {
    String commandId = doc["commandId"] | "";

    String cardUuid = doc["cardUuid"] | "";
    String response;

    if (cardUuid.length() > 0) {
      response = getUserAttendanceRecords(cardUuid);
    } else {
      response = getTodayAttendanceRecords();
    }

    DynamicJsonDocument respDoc(2048);
    deserializeJson(respDoc, response);

    if (respDoc["success"]) {
      publishMqttResponse(true, respDoc["message"].as<String>());

      JsonVariant data = respDoc["data"];
      DynamicJsonDocument dataDoc(1024);
      dataDoc["data"] = data;

      if (commandId.length() > 0) {
        dataDoc["commandId"] = commandId;
      }

      String dataPayload;
      serializeJson(dataDoc, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
    } else {
      publishMqttResponse(false, respDoc["message"].as<String>());
    }
  }

  else if (command == "delete_attendance_records") {
    String commandId = doc["commandId"] | "";
    String cardUuid = doc["cardUuid"] | "";

    if (cardUuid.length() == 0) {
      publishMqttResponse(false, "cardUuid required");
      return;
    }

    String response = deleteUserAttendanceRecords(cardUuid);

    DynamicJsonDocument respDoc(256);
    deserializeJson(respDoc, response);

    if (respDoc["success"]) {
      DynamicJsonDocument data(128);
      data["message"] = respDoc["message"];
      if (commandId.length() > 0) {
        data["commandId"] = commandId;
      }
      String dataPayload;
      serializeJson(data, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

      // publishMqttResponse(true, respDoc["message"].as<String>());
    } else {
      publishMqttResponse(false, respDoc["message"].as<String>());
    }
  }

  else if (command == "delete_all_today_records") {
    if (!sdMounted) {
      publishMqttResponse(false, "SD card not mounted");
      return;
    }

    DateTime now = getLocalTime();
    char filename[32];
    sprintf(filename, "%s%04d%02d%02d.csv",
            DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

    if (!SD.exists(filename)) {
      publishMqttResponse(false, "No records file found");
      return;
    }

    bool deleted = SD.remove(filename);

    if (deleted) {
      if (currentProcessingDay == (now.year() * 10000 + now.month() * 100 + now.day())) {
        todaysCheckIns.clear();
        todaysCheckOuts.clear();
      }
      publishMqttResponse(true, "All today's records deleted successfully");
    } else {
      publishMqttResponse(false, "Failed to delete records file");
    }
  }

  else if (command == "get_monthly_records") {
    String commandId = doc["commandId"] | "";
    int year = doc["year"] | 0;
    int month = doc["month"] | 0;
    String cardUuid = doc["cardUuid"] | "";

    if (year == 0 || month == 0) {
      DateTime now = rtc.now();
      year = now.year();
      month = now.month();
    }

    String response;
    if (cardUuid.length() > 0) {
      response = getUserMonthlyRecords(cardUuid, year, month);
    } else {
      response = getMonthlyAttendanceRecords(year, month);
    }

    DynamicJsonDocument respDoc(2048);
    deserializeJson(respDoc, response);

    if (respDoc["success"]) {
      publishMqttResponse(true, respDoc["message"].as<String>());

      JsonVariant data = respDoc["data"];
      DynamicJsonDocument dataDoc(1024);
      dataDoc["data"] = data;

      if (commandId.length() > 0) {
        dataDoc["commandId"] = commandId;
      }

      String dataPayload;
      serializeJson(dataDoc, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
    } else {
      publishMqttResponse(false, respDoc["message"].as<String>());
    }
  }

  else if (command == "delete_monthly_records") {
    int year = doc["year"] | 0;
    int month = doc["month"] | 0;
    bool deleteAll = doc["deleteAll"] | false;
    String cardUuid = doc["cardUuid"] | "";

    if (year == 0 || month == 0) {
      DateTime now = rtc.now();
      year = now.year();
      month = now.month();
    }

    String response;
    if (deleteAll) {
      response = deleteMonthlyFile(year, month);
    } else if (cardUuid.length() > 0) {
      response = deleteUserFromMonthlyFile(cardUuid, year, month);
    } else {
      publishMqttResponse(false, "Either cardUuid or deleteAll flag required");
      return;
    }

    DynamicJsonDocument respDoc(256);
    deserializeJson(respDoc, response);

    if (respDoc["success"]) {
      publishMqttResponse(true, respDoc["message"].as<String>());
    } else {
      publishMqttResponse(false, respDoc["message"].as<String>());
    }
  }

  else if (command == "list_files") {
    String commandId = doc["commandId"] | "";
    String response = listAllLogFiles();

    DynamicJsonDocument respDoc(4096);
    deserializeJson(respDoc, response);

    if (respDoc["success"]) {
      publishMqttResponse(true, respDoc["message"].as<String>());

      JsonVariant data = respDoc["data"];
      DynamicJsonDocument dataDoc(2048);
      dataDoc["data"] = data;

      if (commandId.length() > 0) {
        dataDoc["commandId"] = commandId;
      }

      String dataPayload;
      serializeJson(dataDoc, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
    } else {
      publishMqttResponse(false, respDoc["message"].as<String>());
    }
  }

  else if (command == "toggle_auto_sync") {
    String commandId = doc["commandId"] | "";
    bool newValue = doc["enabled"] | autoSyncEnabled;

    if (autoSyncEnabled != newValue) {
      autoSyncEnabled = newValue;
      EEPROM.write(EEPROM_AUTO_SYNC_ADDR, autoSyncEnabled ? 1 : 0);
      EEPROM.commit();
    }

    publishMqttResponse(true, "Auto sync toggled");

    DynamicJsonDocument data(64);
    data["autoSync"] = autoSyncEnabled;

    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
  }

  else if (command == "manual_sync") {
    String commandId = doc["commandId"] | "";
    int year = doc["year"] | 0;
    int month = doc["month"] | 0;

    if (year == 0 || month == 0) {
      DateTime now = rtc.now();
      year = now.year();
      month = now.month();
    }

    bool result = syncMonthlyRecordsToServer(year, month);

    if (result) {
      DynamicJsonDocument data(64);
      data["message"] = "Sync completed successfully";
      // 🔥 ADD commandId if present
      if (commandId.length() > 0) {
        data["commandId"] = commandId;
      }
      String dataPayload;
      serializeJson(data, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

      // publishMqttResponse(true, "Sync completed successfully");
    } else {
      publishMqttResponse(false, "No records to sync or sync failed");
    }
  }

  else if (command == "sync_user_schedule") {
    // 🔥 STEP 1: Get commandId from incoming message
    String commandId = doc["commandId"] | "";

    bool replaceAll = doc["replaceAll"] | false;

    if (replaceAll) {
      userSchedules.clear();
    }

    JsonArray users = doc["users"];
    int addedCount = 0;
    int updatedCount = 0;

    Serial.printf("[ScheduleSync] Received %d users\n", users.size());
    if (commandId.length() > 0) {
      Serial.printf("[ScheduleSync] Command ID: %s\n", commandId.c_str());
    }

    for (JsonObject user : users) {
      int userId = user["userId"] | 0;
      String cardUuid = user["cardUuid"] | "";
      String userName = user["userName"] | "";
      JsonArray schedules = user["schedules"];

      if (userId == 0) continue;

      Serial.printf("  User %d: %s, Card: %s, %d schedules\n",
                    userId, userName.c_str(), cardUuid.c_str(), schedules.size());

      for (JsonObject sched : schedules) {
        int dayOfWeek = sched["dayOfWeek"] | 0;
        if (dayOfWeek == 0) continue;

        UserSchedule* existing = nullptr;
        bool exists = findUserSchedule(userId, dayOfWeek, existing);

        if (exists && !replaceAll) {
          existing->cardUuid = cardUuid;
          existing->userName = userName;
          existing->checkInFrom = sched["checkInFrom"] | "";
          existing->checkInTo = sched["checkInTo"] | "";
          existing->checkOutFrom = sched["checkOutFrom"] | "";
          existing->checkOutTo = sched["checkOutTo"] | "";
          updatedCount++;
        } else {
          UserSchedule newSched;
          newSched.userId = userId;
          newSched.cardUuid = cardUuid;
          newSched.userName = userName;
          newSched.dayOfWeek = dayOfWeek;
          newSched.checkInFrom = sched["checkInFrom"] | "";
          newSched.checkInTo = sched["checkInTo"] | "";
          newSched.checkOutFrom = sched["checkOutFrom"] | "";
          newSched.checkOutTo = sched["checkOutTo"] | "";

          userSchedules.push_back(newSched);
          addedCount++;
        }
      }
    }

    if (localStorage && sdMounted) {
      Serial.println("💾 Saving updated schedules to SD card...");
      saveScheduleToSD();
      Serial.println("✅ Schedules saved to SD");
    } else {
      Serial.println("⚠️ SD not mounted or localStorage disabled, not saving to SD");
    }

    // 🔥 STEP 2: FIRST RESPONSE - with commandId (critical for tracking)
    DynamicJsonDocument response(256);
    response["success"] = true;
    response["message"] = "Bulk sync completed";
    response["timestamp"] = getLocalTime().unixtime();

    // 👇 IMPORTANT: commandId must be included in response
    if (commandId.length() > 0) {
      response["commandId"] = commandId;
    }

    String responsePayload;
    serializeJson(response, responsePayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, responsePayload.c_str());
    Serial.printf("[ScheduleSync] Sent response with commandId: %s\n",
                  commandId.length() > 0 ? commandId.c_str() : "none");

    // 🔥 STEP 3: SECOND RESPONSE - with stats (optional)
    DynamicJsonDocument data(128);
    data["added"] = addedCount;
    data["updated"] = updatedCount;

    // Optional: agar chaho to yahan bhi commandId bhej sakte ho
    if (commandId.length() > 0) {
      data["commandId"] = commandId;  // iski zaroorat nahi, but helpful ho sakta hai
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

    Serial.printf("[ScheduleSync] Complete: %d added, %d updated\n", addedCount, updatedCount);
  }

  else if (command == "get_device_schedules") {
    String commandId = doc["commandId"] | "";

    publishMqttResponse(true, "Schedules fetched");

    DynamicJsonDocument data(8192);
    JsonArray users = data.createNestedArray("users");

    for (auto& u : userSchedules) {
      JsonObject user = users.createNestedObject();
      user["id"] = u.userId;
      user["cardUuid"] = u.cardUuid;
      user["name"] = u.userName;
      user["dayOfWeek"] = u.dayOfWeek;
      user["checkInFrom"] = u.checkInFrom;
      user["checkInTo"] = u.checkInTo;
      user["checkOutFrom"] = u.checkOutFrom;
      user["checkOutTo"] = u.checkOutTo;
    }

    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
  }

  else if (command == "restart") {
    // publishMqttResponse(true, "Device restarting...");

    String commandId = doc["commandId"] | "";

    DynamicJsonDocument data(64);
    data["success"] = true;
    data["message"] = "Device restarting...";
    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }
    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

    delay(1000);
    ESP.restart();
  }

  else {
    publishMqttResponse(false, "Unknown command: " + command);
  }
}

// ================== FETCH SCHEDULES ==================
void fetchAndStoreSchedules() {
  Serial.println("Fetching schedules from server...");

  HTTPClient http;
  WiFiClient client;

  client.setTimeout(15000);
  http.setTimeout(15000);

  String url = String(SERVER_URL) + "/api/v1/device/schedules";

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed!");
    blinkRedTwice();
    return;
  }

  http.addHeader("x-device-id", DEVICE_UUID);
  http.addHeader("x-device-secret", DEVICE_SECRET);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String response = http.getString();

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      bool success = doc["success"];
      if (!success) {
        Serial.println("Server returned error");
        blinkRedTwice();
        http.end();
        return;
      }

      JsonObject data = doc["data"];
      if (data.isNull()) {
        Serial.println("Error: No 'data' object");
        blinkRedTwice();
        http.end();
        return;
      }

      String newHash = data["lastUpdated"] | "";
      String oldHash = String(storedData.lastUpdated);
      bool dataChanged = (newHash != oldHash);

      if (dataChanged) {
        userSchedules.clear();

        JsonArray users = data["users"];
        int scheduleCount = 0;

        for (JsonObject user : users) {
          String cardUuid = user["cardUuid"] | "";
          String userName = user["name"] | "";
          int userId = user["id"] | 0;

          if (cardUuid != "null" && cardUuid.length() > 0) {
            JsonArray schedules = user["schedules"];

            for (JsonObject schedule : schedules) {
              UserSchedule userSchedule;
              userSchedule.userId = userId;
              userSchedule.cardUuid = cardUuid;
              userSchedule.userName = userName;
              userSchedule.dayOfWeek = schedule["day"] | 0;
              userSchedule.checkInFrom = schedule["checkInFrom"] | "";
              userSchedule.checkInTo = schedule["checkInTo"] | "";
              userSchedule.checkOutFrom = schedule["checkOutFrom"] | "";
              userSchedule.checkOutTo = schedule["checkOutTo"] | "";

              userSchedules.push_back(userSchedule);
              scheduleCount++;
            }
          }
        }

        if (localStorage && sdMounted) {
          saveScheduleToSD();
        }

        publishMqttStatus("schedule_updated", "Schedules updated: " + String(scheduleCount));

        int blinkCount = min(scheduleCount, 5);
        for (int i = 0; i < blinkCount; i++) {
          blinkGreenOnce();
          delay(150);
        }
      } else {
        blinkGreenOnce();
      }

      serverUnreachable = false;
    } else {
      blinkRedTwice();
    }
  } else {
    blinkRedTwice();
  }

  http.end();
}

// ================== WIFI CONNECTION ==================
bool connectToWifi(String ssid, String password) {
  Serial.println("\n=== WIFI CONNECT ===");
  Serial.println("SSID: " + ssid);

  isConnecting = true;
  WiFi.disconnect(true);
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) {
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

    blinkGreenOnce();

    if (localStorage) {
      fetchAndStoreSchedules();
    }

    isConnecting = false;
    return true;
  } else {
    Serial.println("\n❌ WiFi Connection Failed!");
    blinkRedTwice();
    isConnecting = false;
    return false;
  }
}

// ================== REPROVISION ==================
void checkReprovision() {
  if (wifiFailCount >= FAIL_LIMIT && !reprovisionMode && !isConnecting) {
    Serial.println("All WiFi failed — AP Mode");
    reprovisionMode = true;
    pcf.write(WHITE_LED_PIN, HIGH);

    wifiFailCount = 0;
    WiFi.disconnect(true);
    delay(500);

    wifiManager.setConfigPortalTimeout(180);
    wifiManager.setConnectTimeout(20);
    wifiManager.setAPStaticIPConfig(
      IPAddress(192, 168, 4, 1),
      IPAddress(192, 168, 4, 1),
      IPAddress(255, 255, 255, 0));

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
  doc["timestamp"] = getLocalTime().unixtime();

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  if (code == 200) {
    heartbeatFailCount = 0;
    serverUnreachable = false;
    publishHeartbeat();
  } else {
    heartbeatFailCount++;
    checkServerUnreachable();
  }

  http.end();
}

// ================== RFID HANDLER ==================
void handleRFID() {
  String cardUuid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    cardUuid += String(rfid.uid.uidByte[i], HEX);
  }
  cardUuid.toUpperCase();
  Serial.println("Card scanned: " + cardUuid);

  if (localStorage) {
    if (userSchedules.size() == 0) {
      Serial.println("No schedules loaded");
      blinkRedTwice();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      delay(1200);
      return;
    }

    bool foundInSchedule = false;
    for (const auto& schedule : userSchedules) {
      if (schedule.cardUuid == cardUuid) {
        foundInSchedule = true;
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

    DateTime currentTime = getLocalTime();
    int currentDOW = currentTime.dayOfTheWeek();
    currentDOW = (currentDOW == 0) ? 7 : currentDOW;

    const UserSchedule* todaysSchedule = nullptr;
    for (const auto& schedule : userSchedules) {
      if (schedule.cardUuid == cardUuid && schedule.dayOfWeek == currentDOW) {
        todaysSchedule = &schedule;
        break;
      }
    }
    if (!todaysSchedule) {
      Serial.println("❌ No schedule for today");
      blinkRedTwice();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      delay(1200);
      return;
    }

    AttendanceResult localResult = processLocalAttendance(
      cardUuid,
      todaysSchedule->userId,
      todaysSchedule->userName,
      *todaysSchedule,
      currentTime);

    if (localResult.success) {
      blinkGreenOnce();
      Serial.println("✅ LOCAL ATTENDANCE ACCEPTED: " + localResult.message);

      publishMqttStatus("attendance_recorded", localResult.message);

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
    if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
      bool accepted = sendAttendance(cardUuid);
      if (accepted) blinkGreenOnce();
      else blinkRedTwice();
    } else {
      Serial.println("WiFi not connected");
      blinkRedTwice();
    }
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1200);
}

// ================== SEND ATTENDANCE ==================
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

// ================== SET RTC FROM NTP ==================
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

  Serial.println("RTC updated from NTP");
#else
  DateTime manualTime(
    manualYear,
    manualMonth,
    manualDay,
    manualHour,
    manualMinute,
    manualSecond);

  rtc.adjust(manualTime);

  Serial.println("\n⚠️ TEST MODE: MANUAL TIME SET!");
#endif
}

// ================== LOCAL ATTENDANCE FUNCTIONS ==================
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
  } else {
    todaysCheckOuts.push_back(record);
  }
}

void resetTodayRecords() {
  todaysCheckIns.clear();
  todaysCheckOuts.clear();
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
    cleanupOldDailyFiles();
    resetTodayRecords();
  }
}

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

String getNextScheduleInfo(int userId, int currentDOW) {
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

  int currentDOW = currentTime.dayOfTheWeek();
  currentDOW = (currentDOW == 0) ? 7 : currentDOW;
  int currentTimeSec = currentTime.hour() * 3600 + currentTime.minute() * 60 + currentTime.second();

  if (todaySchedule.dayOfWeek != currentDOW) {
    result.message = "No schedule found for today (Day " + String(currentDOW) + ")";
    return result;
  }

  int ciFromSec = timeToSeconds(todaySchedule.checkInFrom);
  int ciToSec = timeToSeconds(todaySchedule.checkInTo);
  int coFromSec = timeToSeconds(todaySchedule.checkOutFrom);
  int coToSec = timeToSeconds(todaySchedule.checkOutTo);

  if (ciFromSec > coFromSec) {
    result.message = "This is an overnight shift. Day shift handler only.";
    return result;
  }

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

  bool hasOpenCheckIn = (todaysCheckIns.size() > todaysCheckOuts.size());

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
      result.message = "Please wait " + String(MIN_GAP_BETWEEN_RECORDS) + " seconds";
      return result;
    }
  }

  if (todaysCheckIns.size() == 0) {
    if (isBeforeCheckInWindow) {
      String checkInTime = addMinutesToTimeStr(todaySchedule.checkInFrom, -GRACE_EARLY_IN);
      result.message = "Check-in opens at " + checkInTime;
      return result;
    }

    if (isAfterCheckInWindow) {
      String closedTime = addMinutesToTimeStr(todaySchedule.checkInTo, GRACE_LATE_IN);
      result.message = "Check-in window closed at " + closedTime;
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

  else if (isAfterCheckOutWindow) {
    if (todaysCheckIns.size() > 0 && todaysCheckOuts.size() == 0) {
      String checkInTime = formatTimeDisplay(todaySchedule.checkInFrom);
      String lateCheckOutEnd = addMinutesToTimeStr(todaySchedule.checkOutTo, GRACE_LATE_OUT);
      String nextSchedule = getNextScheduleInfo(userId, currentDOW);

      result.message = "⚠️ Shift ended without check-out!\n   Check-in: " + checkInTime + "\n   Window closed: " + lateCheckOutEnd + "\n   Next: " + nextSchedule;
      result.recordType = "in";
      return result;
    } else if (todaysCheckIns.size() == 0) {
      String checkInStartTime = formatTimeDisplay(todaySchedule.checkInFrom);
      String lateCheckInEnd = addMinutesToTimeStr(todaySchedule.checkInTo, GRACE_LATE_IN);
      String nextSchedule = getNextScheduleInfo(userId, currentDOW);

      result.message = "❌ Missed shift!\n   Window: " + checkInStartTime + " - " + lateCheckInEnd + "\n   Next: " + nextSchedule;
      result.recordType = "in";
      return result;
    } else {
      result.message = "✅ Today's shift completed.";
      result.recordType = "out";
      return result;
    }
  }

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

    if (todaysCheckOuts.size() == 0 && todaysCheckIns.size() > 0) {
      TodaysRecord& lastCheckInRecord = todaysCheckIns.back();
      int minutesWorked = (currentTimeSec - lastCheckInRecord.timestamp) / 60;

      if (minutesWorked < MIN_WORK_DURATION) {
        result.success = false;
        result.message = "Minimum work duration not met: " + String(MIN_WORK_DURATION) + " minutes required";
        return result;
      }
    }

    addRecord("out", currentTime, userId, cardUuid, userName);
  }

  else if (hasOpenCheckIn && isBeforeCheckOutWindow) {
    String checkOutTime = formatTimeDisplay(todaySchedule.checkOutFrom);
    result.message = "Already checked in. Check-out opens at " + checkOutTime;
    return result;
  }

  else {
    result.message = "Unable to process attendance at this time";
    return result;
  }

  char timestampBuf[20];
  sprintf(timestampBuf, "%04d-%02d-%02d %02d:%02d:%02d",
          currentTime.year(), currentTime.month(), currentTime.day(),
          currentTime.hour(), currentTime.minute(), currentTime.second());
  result.timestamp = String(timestampBuf);
  result.formattedTime = formatTimeDisplay(secondsToTimeStr(currentTimeSec));

  return result;
}

// ================== SAVE TO SD FUNCTIONS ==================
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
    return false;
  }

  char filename[32];
  DateTime now = rtc.now();
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  bool fileExists = SD.exists(filename);

  File file = SD.open(filename, FILE_WRITE);
  if (!file) {
    return false;
  }

  if (fileExists) {
    if (!file.seek(file.size())) {
      file.close();
      return false;
    }
  } else {
    file.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
  }

  String line = timestamp + "," + cardUuid + "," + String(userId) + "," + userName + "," + recordType + "," + status + "," + message + "," + dayOfWeek + "," + checkInWindow + "," + checkOutWindow;

  file.println(line);
  file.close();

  char monthlyFilename[32];
  sprintf(monthlyFilename, "%s%04d_%02d.csv",
          MONTHLY_LOG_PREFIX, now.year(), now.month());

  bool monthlyExists = SD.exists(monthlyFilename);

  File monthlyFile = SD.open(monthlyFilename, FILE_WRITE);
  if (monthlyFile) {
    if (monthlyExists) {
      monthlyFile.seek(monthlyFile.size());
    } else {
      monthlyFile.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
    }
    monthlyFile.println(line);
    monthlyFile.close();
  }

  return true;
}

// void saveScheduleToSD() {
//   Serial.println("💾 === SAVING SCHEDULES TO SD ===");
//   Serial.printf("Current userSchedules size: %d\n", userSchedules.size());

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

//   Serial.printf("  Saving: User %d (%s) Day %d\n", u.userId, u.userName.c_str(), u.dayOfWeek);
// }

// File file = SD.open(SCHEDULE_FILE, FILE_WRITE);
// if (!file) {
//   Serial.println("❌ Failed to open schedule file for writing!");
//   return;
// }

// serializeJson(doc, file);
// file.close();
// }

void saveScheduleToSD() {
  Serial.println("💾 === SAVING SCHEDULES TO SD ===");
  Serial.printf("Current userSchedules size: %d\n", userSchedules.size());

  // Pehle purani file delete karo (ensure clean write)
  if (SD.exists(SCHEDULE_FILE)) {
    Serial.println("🗑️ Removing old schedule file...");
    if (!SD.remove(SCHEDULE_FILE)) {
      Serial.println("⚠️ Failed to remove old file, but continuing...");
    } else {
      Serial.println("✅ Old file removed");
    }
    delay(100);  // Give SD card time to process
  }

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

    Serial.printf("  Saving: User %d (%s) Day %d\n", u.userId, u.userName.c_str(), u.dayOfWeek);
  }

  // Open file with FILE_WRITE mode (creates new file)
  File file = SD.open(SCHEDULE_FILE, FILE_WRITE);
  if (!file) {
    Serial.println("❌ Failed to open schedule file for writing!");
    return;
  }

  size_t bytesWritten = serializeJson(doc, file);
  Serial.printf("✅ Written %d bytes\n", bytesWritten);

  // 🔥 CRITICAL: Force flush and close properly
  file.flush();
  file.close();
  delay(100);  // Give SD card time to settle

  Serial.println("✅ File closed successfully");

  // 🔥 FIXED: Verify file with new file handle
  File verifyFile = SD.open(SCHEDULE_FILE, FILE_READ);
  if (verifyFile) {
    Serial.printf("📁 Verification successful - file size: %d bytes\n", verifyFile.size());

    // Quick content check - read first few bytes
    char buffer[50];
    int bytesRead = verifyFile.readBytes(buffer, 49);
    buffer[bytesRead] = '\0';
    Serial.printf("📄 File starts with: %s\n", buffer);

    verifyFile.close();
  } else {
    Serial.println("❌ VERIFICATION FAILED - File cannot be opened after write!");

    // 🔥 Try one more time with different approach
    Serial.println("🔄 Attempting recovery write...");

    File retryFile = SD.open(SCHEDULE_FILE, FILE_WRITE);
    if (retryFile) {
      bytesWritten = serializeJson(doc, retryFile);
      retryFile.flush();
      retryFile.close();
      delay(100);

      File checkAgain = SD.open(SCHEDULE_FILE, FILE_READ);
      if (checkAgain) {
        Serial.printf("✅ Recovery successful! File size: %d bytes\n", checkAgain.size());
        checkAgain.close();
      } else {
        Serial.println("❌ RECOVERY FAILED - SD card may have issues");
      }
    }
  }

  Serial.println("💾 === SAVE COMPLETE ===\n");
}

// bool loadScheduleFromSD() {

//   Serial.println("📂 === LOADING SCHEDULES FROM SD ===");

//   if (!SD.exists(SCHEDULE_FILE)) {
//     Serial.println("⚠️ No schedule file found on SD");
//     return false;
//   }

//   File file = SD.open(SCHEDULE_FILE);
//   if (!file) {
//     Serial.println("❌ Failed to open schedule file for reading!");
//     return false;
//   }

//   Serial.printf("📁 File size: %d bytes\n", file.size());

//   DynamicJsonDocument doc(4096);
//   DeserializationError error = deserializeJson(doc, file);
//   if (error) {
//     Serial.printf("❌ JSON parse error: %s\n", error.c_str());
//     file.close();
//     return false;
//   }
//   file.close();

//   userSchedules.clear();

//   JsonArray usersArray = doc["users"].as<JsonArray>();
//   Serial.printf("Found %d users in schedule file\n", usersArray.size());

//   for (JsonObject u : usersArray) {
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

//     Serial.printf("  Loaded: User %d (%s) Day %d, Card: %s\n",
//                   s.userId, s.userName.c_str(), s.dayOfWeek, s.cardUuid.c_str());
//   }

//   Serial.println("✅ Loaded schedules from SD: " + String(userSchedules.size()));
//   Serial.println("📂 === LOAD COMPLETE ===\n");

//   return true;
// }

bool loadScheduleFromSD() {
  Serial.println("📂 === LOADING SCHEDULES FROM SD ===");

  if (!SD.exists(SCHEDULE_FILE)) {
    Serial.println("⚠️ No schedule file found on SD");
    return false;
  }

  // Get file size first
  File checkFile = SD.open(SCHEDULE_FILE, FILE_READ);
  if (checkFile) {
    Serial.printf("📁 File size: %d bytes\n", checkFile.size());
    checkFile.close();
  }

  File file = SD.open(SCHEDULE_FILE, FILE_READ);
  if (!file) {
    Serial.println("❌ Failed to open schedule file for reading!");
    return false;
  }

  // Check if file is empty
  if (file.size() == 0) {
    Serial.println("❌ Schedule file is empty!");
    file.close();
    return false;
  }

  DynamicJsonDocument doc(8192);  // Increased buffer size
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.printf("❌ JSON parse error: %s\n", error.c_str());
    file.close();
    return false;
  }
  file.close();

  userSchedules.clear();

  JsonArray usersArray = doc["users"].as<JsonArray>();
  Serial.printf("Found %d users in schedule file\n", usersArray.size());

  for (JsonObject u : usersArray) {
    UserSchedule s;
    s.userId = u["id"] | 0;
    s.cardUuid = u["cardUuid"].as<String>();
    s.userName = u["name"].as<String>();
    s.dayOfWeek = u["dayOfWeek"] | 0;
    s.checkInFrom = u["checkInFrom"].as<String>();
    s.checkInTo = u["checkInTo"].as<String>();
    s.checkOutFrom = u["checkOutFrom"].as<String>();
    s.checkOutTo = u["checkOutTo"].as<String>();

    userSchedules.push_back(s);

    Serial.printf("  Loaded: User %d (%s) Day %d, Card: %s\n",
                  s.userId, s.userName.c_str(), s.dayOfWeek, s.cardUuid.c_str());
  }

  Serial.printf("✅ Loaded schedules from SD: %d\n", userSchedules.size());
  Serial.println("📂 === LOAD COMPLETE ===\n");

  return true;
}

void loadTodayRecordsFromSD() {
  todaysCheckIns.clear();
  todaysCheckOuts.clear();

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) return;

  File file = SD.open(filename, FILE_READ);
  if (!file) return;

  String header = file.readStringUntil('\n');

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

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

    if (recordType == "in" || recordType == "out") {
      TodaysRecord record;
      record.recordType = recordType;

      int timeStart = timestamp.indexOf(' ') + 1;
      String timeStr = timestamp.substring(timeStart);

      int h = timeStr.substring(0, 2).toInt();
      int m = timeStr.substring(3, 5).toInt();
      int s = timeStr.substring(6, 8).toInt();
      record.timestamp = h * 3600 + m * 60 + s;
      record.timestampStr = timeStr;

      if (recordType == "in") {
        todaysCheckIns.push_back(record);
      } else {
        todaysCheckOuts.push_back(record);
      }
    }
  }

  file.close();
}

void cleanupOldDailyFiles() {
  if (!sdMounted) return;

  Serial.println("🧹 Cleaning up old daily files...");

  DateTime localNow = getLocalTime();
  int todayInt = localNow.year() * 10000UL + localNow.month() * 100 + localNow.day();

  File root = SD.open("/");
  if (!root) return;

  int deleted = 0;

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    String name = entry.name();
    if (name.startsWith("/")) name = name.substring(1);

    if (!name.startsWith("log_") || !name.endsWith(".csv")) {
      entry.close();
      continue;
    }

    String dateStr = name.substring(name.length() - 12, name.length() - 4);
    if (dateStr.length() != 8 || !dateStr.toInt()) {
      entry.close();
      continue;
    }

    int fy = dateStr.substring(0, 4).toInt();
    int fm = dateStr.substring(4, 6).toInt();
    int fd = dateStr.substring(6, 8).toInt();
    int fileDateInt = fy * 10000UL + fm * 100 + fd;

    if (fileDateInt < todayInt) {
      entry.close();
      String fullPath = "/" + name;
      if (SD.remove(fullPath)) {
        deleted++;
      } else {
        String tempPath = "/temp_" + name;
        if (SD.rename(fullPath, tempPath)) {
          deleted++;
          SD.remove(tempPath);
        }
      }
    } else {
      entry.close();
    }
  }

  root.close();
  Serial.printf("🧹 Deleted %d old files\n", deleted);
}

// ================== SYNC TO SERVER ==================
bool syncMonthlyRecordsToServer(int year, int month) {
  if (!sdMounted || !autoSyncEnabled || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (isSyncing) return false;

  isSyncing = true;

  char monthlyFilename[32];
  sprintf(monthlyFilename, "%s%04d_%02d.csv",
          MONTHLY_LOG_PREFIX, year, month);

  if (!SD.exists(monthlyFilename)) {
    isSyncing = false;
    return false;
  }

  File file = SD.open(monthlyFilename, FILE_READ);
  if (!file) {
    isSyncing = false;
    return false;
  }

  String header = file.readStringUntil('\n');

  int totalRecords = 0;
  int syncedRecords = 0;
  int failedRecords = 0;

  char tempFilename[32];
  sprintf(tempFilename, "%s%04d_%02d_temp.csv",
          MONTHLY_LOG_PREFIX, year, month);

  File tempFile = SD.open(tempFilename, FILE_WRITE);
  if (!tempFile) {
    file.close();
    isSyncing = false;
    return false;
  }

  tempFile.println(header);

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    totalRecords++;

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

    bool sent = sendAttendanceRecordToServer(
      cardUuid,
      userId.toInt(),
      userName,
      timestamp,
      recordType,
      status,
      message,
      dayOfWeek,
      checkInWindow,
      checkOutWindow);

    if (sent) {
      syncedRecords++;
    } else {
      failedRecords++;
      tempFile.println(line);
    }

    delay(100);
  }

  file.close();
  tempFile.close();

  Serial.println("\n========== SYNC SUMMARY ==========");
  Serial.print("Total Records: ");
  Serial.println(totalRecords);

  Serial.print("Synced: ");
  Serial.println(syncedRecords);

  Serial.print("Failed: ");
  Serial.println(failedRecords);

  SD.remove(monthlyFilename);
  if (failedRecords > 0) {
    SD.rename(tempFilename, monthlyFilename);
    Serial.println("⚠️ Some records failed, temp file renamed back to original");
  } else {
    SD.remove(tempFilename);
    Serial.println("🎉 All records synced, temp file removed");
  }

  isSyncing = false;

  Serial.println("========== MONTHLY SYNC END ==========\n");
  return syncedRecords > 0;
}

bool sendAttendanceRecordToServer(
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

  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  WiFiClient client;

  String url = String(SERVER_URL) + "/api/v1/device/attendance/sync-record";
  http.begin(client, url);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-id", DEVICE_UUID);
  http.addHeader("x-device-secret", DEVICE_SECRET);

  DynamicJsonDocument doc(512);
  doc["cardUuid"] = cardUuid;
  doc["deviceUuid"] = DEVICE_UUID;
  doc["userId"] = userId;
  doc["userName"] = userName;
  doc["timestamp"] = timestamp;
  doc["recordType"] = recordType;
  doc["status"] = status;
  doc["message"] = message;
  doc["dayOfWeek"] = dayOfWeek;
  doc["checkInWindow"] = checkInWindow;
  doc["checkOutWindow"] = checkOutWindow;

  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();

  return (code == 200);
}

// ================== API RESPONSE FUNCTIONS FOR FILE HANDLING ==================
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

  String header = file.readStringUntil('\n');

  bool firstLine = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

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

  String header = file.readStringUntil('\n');

  bool firstLine = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

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

  String header = originalFile.readStringUntil('\n');
  tempFile.println(header);

  while (originalFile.available()) {
    String line = originalFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    totalCount++;

    if (line.indexOf(targetCardUuid) >= 0) {
      deletedCount++;
      continue;
    }

    tempFile.println(line);
  }

  originalFile.close();
  tempFile.close();

  SD.remove(filename);
  SD.rename(tempFilename, filename);

  if (currentProcessingDay == (now.year() * 10000 + now.month() * 100 + now.day())) {
    loadTodayRecordsFromSD();
  }

  String response = "{\"success\":true,\"message\":\"Records deleted successfully\",";
  response += "\"deletedCount\":" + String(deletedCount) + ",";
  response += "\"totalCount\":" + String(totalCount) + "}";

  return response;
}

String getMonthlyAttendanceRecords(int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }

  char monthlyFilename[32];
  sprintf(monthlyFilename, "%s%04d_%02d.csv",
          MONTHLY_LOG_PREFIX, year, month);

  if (!SD.exists(monthlyFilename)) {
    return "{\"success\":true,\"message\":\"No records for this month\",\"data\":[]}";
  }

  File file = SD.open(monthlyFilename, FILE_READ);
  if (!file) {
    return "{\"success\":false,\"message\":\"Failed to open monthly file\"}";
  }

  String jsonResponse = "{\"success\":true,\"message\":\"Monthly records fetched successfully\",\"data\":[";

  String header = file.readStringUntil('\n');

  bool firstLine = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

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

String getUserMonthlyRecords(String targetCardUuid, int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }

  char monthlyFilename[32];
  sprintf(monthlyFilename, "%s%04d_%02d.csv",
          MONTHLY_LOG_PREFIX, year, month);

  if (!SD.exists(monthlyFilename)) {
    return "{\"success\":true,\"message\":\"No records for this month\",\"data\":[]}";
  }

  File file = SD.open(monthlyFilename, FILE_READ);
  if (!file) {
    return "{\"success\":false,\"message\":\"Failed to open monthly file\"}";
  }

  String jsonResponse = "{\"success\":true,\"message\":\"User monthly records fetched successfully\",\"data\":[";

  String header = file.readStringUntil('\n');

  bool firstLine = true;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

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

String deleteMonthlyFile(int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }

  char monthlyFilename[32];
  sprintf(monthlyFilename, "%s%04d_%02d.csv",
          MONTHLY_LOG_PREFIX, year, month);

  if (!SD.exists(monthlyFilename)) {
    return "{\"success\":false,\"message\":\"Monthly file not found\"}";
  }

  bool deleted = SD.remove(monthlyFilename);

  if (deleted) {
    return "{\"success\":true,\"message\":\"Monthly file deleted successfully\"}";
  } else {
    return "{\"success\":false,\"message\":\"Failed to delete monthly file\"}";
  }
}

String deleteUserFromMonthlyFile(String targetCardUuid, int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }

  char monthlyFilename[32];
  sprintf(monthlyFilename, "%s%04d_%02d.csv",
          MONTHLY_LOG_PREFIX, year, month);

  if (!SD.exists(monthlyFilename)) {
    return "{\"success\":false,\"message\":\"Monthly file not found\"}";
  }

  char tempFilename[32];
  sprintf(tempFilename, "%s%04d_%02d_temp.csv",
          MONTHLY_LOG_PREFIX, year, month);

  File originalFile = SD.open(monthlyFilename, FILE_READ);
  if (!originalFile) {
    return "{\"success\":false,\"message\":\"Failed to open monthly file\"}";
  }

  File tempFile = SD.open(tempFilename, FILE_WRITE);
  if (!tempFile) {
    originalFile.close();
    return "{\"success\":false,\"message\":\"Failed to create temp file\"}";
  }

  int deletedCount = 0;
  int totalCount = 0;

  String header = originalFile.readStringUntil('\n');
  tempFile.println(header);

  while (originalFile.available()) {
    String line = originalFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    totalCount++;

    int commaIndex = 0;
    int startPos = 0;
    int fieldIndex = 0;
    String cardUuid = "";

    while (startPos < line.length() && fieldIndex <= 1) {
      commaIndex = line.indexOf(',', startPos);
      if (commaIndex == -1) commaIndex = line.length();

      if (fieldIndex == 1) {
        cardUuid = line.substring(startPos, commaIndex);
        cardUuid.trim();
        cardUuid.toUpperCase();
        break;
      }

      startPos = commaIndex + 1;
      fieldIndex++;
    }

    if (cardUuid == targetCardUuid) {
      deletedCount++;
      continue;
    }

    tempFile.println(line);
  }

  originalFile.close();
  tempFile.close();

  SD.remove(monthlyFilename);
  SD.rename(tempFilename, monthlyFilename);

  if (deletedCount > 0) {
    String response = "{\"success\":true,\"message\":\"User records deleted from monthly file\",";
    response += "\"deletedCount\":" + String(deletedCount) + ",";
    response += "\"totalCount\":" + String(totalCount) + "}";
    return response;
  } else {
    return "{\"success\":false,\"message\":\"No records found for this user\"}";
  }
}

String listAllLogFiles() {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\",\"data\":{\"dailyFiles\":[],\"monthlyFiles\":[]}}";
  }

  DynamicJsonDocument doc(4096);
  JsonObject data = doc.createNestedObject("data");
  JsonArray dailyFiles = data.createNestedArray("dailyFiles");
  JsonArray monthlyFiles = data.createNestedArray("monthlyFiles");

  File root = SD.open("/");
  if (!root) {
    return "{\"success\":false,\"message\":\"Failed to open root directory\",\"data\":{\"dailyFiles\":[],\"monthlyFiles\":[]}}";
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;

    if (!entry.isDirectory()) {
      String fileName = entry.name();
      if (fileName.startsWith("/")) fileName = fileName.substring(1);

      if (fileName.startsWith("log_") && fileName.endsWith(".csv")) {
        JsonObject fileObj = dailyFiles.createNestedObject();
        fileObj["filename"] = fileName;
        String dateStr = fileName.substring(strlen("log_"), fileName.length() - 4);
        fileObj["date"] = dateStr;
        fileObj["size"] = entry.size();
      }

      if (fileName.startsWith("monthly_") && fileName.endsWith(".csv")) {
        JsonObject fileObj = monthlyFiles.createNestedObject();
        fileObj["filename"] = fileName;
        String yearMonth = fileName.substring(strlen("monthly_"), fileName.length() - 4);
        fileObj["yearMonth"] = yearMonth;
        fileObj["size"] = entry.size();
      }
    }

    entry.close();
  }

  root.close();

  doc["success"] = true;
  doc["message"] = "Files listed successfully";

  String response;
  serializeJson(doc, response);

  return response;
}

// ================== LED FUNCTIONS ==================
void blinkWhiteLED() {
  if (millis() - lastLedToggle >= 300) {
    lastLedToggle = millis();
    ledState = !ledState;
    pcf.write(WHITE_LED_PIN, ledState ? HIGH : LOW);
  }
}

void blinkGreenOnce() {
  pcf.write(GREEN_LED_PIN, HIGH);
  delay(120);
  pcf.write(GREEN_LED_PIN, LOW);
}

void blinkRedTwice() {
  for (int i = 0; i < 2; i++) {
    pcf.write(RED_LED_PIN, HIGH);
    delay(120);
    pcf.write(RED_LED_PIN, LOW);
    delay(120);
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n=== ESP8266 Attendance Device Starting (MQTT Version) ===");

  EEPROM.begin(512);
  uint8_t storedLS = EEPROM.read(EEPROM_LOCAL_STORAGE_ADDR);

  if (storedLS == 0 || storedLS == 1) {
    localStorage = storedLS;
  } else {
    localStorage = true;
    EEPROM.write(EEPROM_LOCAL_STORAGE_ADDR, 1);
    EEPROM.commit();
  }

  uint8_t storedAutoSync = EEPROM.read(EEPROM_AUTO_SYNC_ADDR);
  if (storedAutoSync == 0 || storedAutoSync == 1) {
    autoSyncEnabled = storedAutoSync;
  } else {
    autoSyncEnabled = true;
    EEPROM.write(EEPROM_AUTO_SYNC_ADDR, 1);
    EEPROM.commit();
  }

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

  Serial.println("Initializing SD card...");

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  SPI.begin();

  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("❌ SD Card init failed");
    sdMounted = false;
  } else {
    Serial.println("✅ SD Card initialized");
    sdMounted = true;

    if (localStorage) {
      Serial.println("Local storage enabled — loading schedule");

      if (loadScheduleFromSD()) {
        Serial.println("📅 Schedule loaded from SD");
        isInitialScheduleLoaded = true;
      } else {
        Serial.println("⚠️ No schedule found on SD");
      }
    }
  }

  Wire.begin(D1, D2);
  pcf.begin();

  pcf.write(WHITE_LED_PIN, LOW);
  pcf.write(BLUE_LED_PIN, LOW);
  pcf.write(GREEN_LED_PIN, LOW);
  pcf.write(RED_LED_PIN, LOW);

  if (!rtc.begin()) {
    Serial.println("❌ RTC not found");
  } else {
    Serial.println("RTC detected");
  }

  pcf.write(WHITE_LED_PIN, HIGH);

  WiFi.mode(WIFI_STA);
  WiFi.begin();

  SPI.begin();
  rfid.PCD_Init();
  Serial.println("RFID Module Initialized");

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);

  Serial.println("Attempting WiFi connection...");

  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 40) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    setRTCFromNTP();

    connectToMqtt();

    if (localStorage) {
      Serial.println("Fetching fresh schedules from server...");
      fetchAndStoreSchedules();
    }
  } else {
    Serial.println("\nWiFi not connected, using offline mode");
  }

  reprovisionMode = false;
  lastHeartbeatTime = millis();
  lastScanTime = millis();
  lastScheduleUpdate = millis();

  if (localStorage) {
    todaysCheckIns.clear();
    todaysCheckOuts.clear();
    DateTime now = getLocalTime();
    currentProcessingDay = now.year() * 10000 + now.month() * 100 + now.day();

    if (sdMounted) {
      loadTodayRecordsFromSD();
    }
  }

  Serial.println("Setup complete!");
}

// ================== LOOP ==================
void loop() {
  static bool rtcSynced = false;
  unsigned long currentMillis = millis();

  static unsigned long lastRFIDCheck = 0;
  if (currentMillis - lastRFIDCheck >= 50) {
    lastRFIDCheck = currentMillis;

    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      handleRFID();
      delay(200);
    }
  }

  if (reprovisionMode) {
    pcf.write(WHITE_LED_PIN, HIGH);
  } else if (WiFi.status() == WL_CONNECTED) {
    blinkWhiteLED();
  } else {
    pcf.write(WHITE_LED_PIN, HIGH);
  }

  pcf.write(BLUE_LED_PIN, serverUnreachable ? HIGH : LOW);

  static unsigned long lastDayCheck = 0;
  if (millis() - lastDayCheck >= 10000) {
    lastDayCheck = millis();
    if (localStorage) {
      DateTime currentTime = getLocalTime();
      checkDayChange(currentTime);
    }
  }

  if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
    wifiFailCount = 0;
    isConnecting = false;

    if (!mqttClient.connected()) {
      unsigned long now = millis();
      if (now - lastMqttReconnectAttempt > 5000) {
        lastMqttReconnectAttempt = now;
        if (connectToMqtt()) {
          lastMqttReconnectAttempt = 0;
        }
      }
    } else {
      mqttClient.loop();
    }

    if (!rtcSynced) {
      setRTCFromNTP();
      rtcSynced = true;
    }

    if (currentMillis - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
      sendHeartbeat();
      lastHeartbeatTime = currentMillis;
    }

    if (localStorage) {
      if (currentMillis - lastScheduleUpdate >= scheduleUpdateInterval) {
        fetchAndStoreSchedules();
        lastScheduleUpdate = currentMillis;
      }

      if (autoSyncEnabled && currentMillis - lastSyncTime >= SYNC_INTERVAL) {
        lastSyncTime = currentMillis;
        DateTime now = rtc.now();
        syncMonthlyRecordsToServer(now.year(), now.month());
      }
    }

  } else {
    static unsigned long lastReconnectAttempt = 0;
    if (currentMillis - lastReconnectAttempt >= 30000) {
      lastReconnectAttempt = currentMillis;

      wifiFailCount++;
      Serial.printf("WiFi disconnected. Attempt %d to reconnect...\n", wifiFailCount);

      WiFi.reconnect();

      if (wifiFailCount >= FAIL_LIMIT) {
        checkReprovision();
      }
    }
  }

  delay(10);
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
// #define EEPROM_AUTO_SYNC_ADDR 21

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

// // ================== TESTING CONFIG ==================
// #define USE_NTP_TIME true  // if you are use in testing for use manual time so, Use_NTP_TIME false

// // ================== MANUAL TIME FOR TESTING ==================
// // Sirf tab use hoga jab USE_NTP_TIME = false ho
// int manualYear = 2026;
// int manualMonth = 2;
// int manualDay = 10;
// int manualHour = 2;
// // int manualHour = 18;
// int manualMinute = 10;
// // int manualMinute = 59;
// int manualSecond = 0;

// // ================== ATTENDANCE LOGIC HEADER ==================
// // Server ka exact logic - Day Shift Handler
// #ifndef ATTENDANCE_LOGIC_H
// #define ATTENDANCE_LOGIC_H

// struct AttendanceSchedule {
//   int userId;
//   String cardUuid;
//   String userName;
//   int dayOfWeek;
//   String checkInFrom;
//   String checkInTo;
//   String checkOutFrom;
//   String checkOutTo;
// };

// struct AttendanceResult {
//   bool success;
//   String message;
//   String recordType;  // "in" or "out"
//   String status;      // "early", "present", "late"
//   String timestamp;
//   String formattedTime;
//   bool shouldDeletePreviousCheckOuts;
// };

// struct TodaysRecord {
//   String recordType;
//   unsigned long timestamp;
//   String timestampStr;
// };

// #endif

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

// // ================== LOCAL ATTENDANCE LOGIC VARIABLES ==================
// // DAY SHIFT CONSTANTS - exactly like server
// const int GRACE_EARLY_IN = 15;           // 15 minutes early check-in allowed
// const int GRACE_LATE_IN = 15;            // 15 minutes late check-in allowed
// const int GRACE_EARLY_OUT = 0;           // 0 minutes early check-out allowed
// const int GRACE_LATE_OUT = 15;           // 15 minutes late check-out allowed
// const int MIN_WORK_DURATION = 30;        // 30 minutes minimum work
// const int MAX_SHIFT_HOURS = 18;          // 18 hours max shift
// const int MIN_GAP_BETWEEN_RECORDS = 10;  // 10 seconds gap between scans

// bool autoSyncEnabled = true;  // Default true
// unsigned long lastSyncTime = 0;
// const unsigned long SYNC_INTERVAL = 300000;  // 5 minutes (300,000 ms)
// bool isSyncing = false;

// // Today's records storage
// std::vector<TodaysRecord> todaysCheckIns;
// std::vector<TodaysRecord> todaysCheckOuts;
// int currentProcessingDay = -1;

// // ================== DAILY LOG FILE ==================
// // Log file path
// const char* LOG_FILE = "/attendance_logs.csv";
// const char* DAILY_LOG_PREFIX = "/log_";

// // ================== MONTHLY LOG FILE ==================
// #define MONTHLY_LOG_PREFIX "/monthly_"

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

// // ================== API RESPONSE STRUCTURE ==================
// struct ApiResponse {
//   bool success;
//   String message;
//   String data;
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

//   // Convert minutes to seconds (long for safety)
//   long totalOffsetSeconds = timezoneOffsetMinutes * 60L;

//   // Get Unix timestamp (seconds since 2000-01-01)
//   time_t utcUnix = utc.unixtime();

//   // Add offset
//   time_t localUnix = utcUnix + totalOffsetSeconds;

//   // Convert back to DateTime
//   DateTime local = DateTime(localUnix);

//   // Debug output
//   Serial.printf("🌍 UTC:  %04d-%02d-%02d %02d:%02d:%02d\n",
//                 utc.year(), utc.month(), utc.day(),
//                 utc.hour(), utc.minute(), utc.second());

//   Serial.printf("📍 Local: %04d-%02d-%02d %02d:%02d:%02d (Offset: %d min)\n",
//                 local.year(), local.month(), local.day(),
//                 local.hour(), local.minute(), local.second(),
//                 timezoneOffsetMinutes);

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

// // ================== FIND USER SCHEDULE BY ID AND DAY ==================
// bool findUserSchedule(int userId, int dayOfWeek, UserSchedule*& foundSchedule) {
//   for (auto& schedule : userSchedules) {
//     if (schedule.userId == userId && schedule.dayOfWeek == dayOfWeek) {
//       foundSchedule = &schedule;
//       return true;
//     }
//   }
//   return false;
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

//   SD.end();
//   SPI.end();
//   WiFi.disconnect(true);
//   WiFi.mode(WIFI_OFF);
//   delay(200);

//   // 2. CS pin ko forcefully control karo
//   pinMode(SD_CS_PIN, OUTPUT);
//   digitalWrite(SD_CS_PIN, HIGH);
//   delay(100);
//   digitalWrite(SD_CS_PIN, LOW);  // briefly low
//   delay(50);
//   digitalWrite(SD_CS_PIN, HIGH);


//   delay(1500);

//   ESP.restart();
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

// // ================== GET TODAY'S ATTENDANCE RECORDS ==================
// String getTodayAttendanceRecords() {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\"}";
//   }

//   DateTime now = getLocalTime();
//   char filename[32];
//   sprintf(filename, "%s%04d%02d%02d.csv",
//           DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

//   if (!SD.exists(filename)) {
//     return "{\"success\":true,\"message\":\"No records for today\",\"data\":[]}";
//   }

//   File file = SD.open(filename, FILE_READ);
//   if (!file) {
//     return "{\"success\":false,\"message\":\"Failed to open log file\"}";
//   }

//   String jsonResponse = "{\"success\":true,\"message\":\"Records fetched successfully\",\"data\":[";

//   // Skip header
//   String header = file.readStringUntil('\n');

//   bool firstLine = true;
//   while (file.available()) {
//     String line = file.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     // Parse CSV line to JSON
//     int commaIndex = 0;
//     int startPos = 0;
//     int fieldIndex = 0;

//     String timestamp, cardUuid, userId, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

//     while (startPos < line.length()) {
//       commaIndex = line.indexOf(',', startPos);
//       if (commaIndex == -1) commaIndex = line.length();

//       String field = line.substring(startPos, commaIndex);

//       switch (fieldIndex) {
//         case 0: timestamp = field; break;
//         case 1: cardUuid = field; break;
//         case 2: userId = field; break;
//         case 3: userName = field; break;
//         case 4: recordType = field; break;
//         case 5: status = field; break;
//         case 6: message = field; break;
//         case 7: dayOfWeek = field; break;
//         case 8: checkInWindow = field; break;
//         case 9: checkOutWindow = field; break;
//       }

//       startPos = commaIndex + 1;
//       fieldIndex++;
//     }

//     if (!firstLine) {
//       jsonResponse += ",";
//     }
//     firstLine = false;

//     jsonResponse += "{";
//     jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
//     jsonResponse += "\"cardUuid\":\"" + cardUuid + "\",";
//     jsonResponse += "\"userId\":" + userId + ",";
//     jsonResponse += "\"userName\":\"" + userName + "\",";
//     jsonResponse += "\"recordType\":\"" + recordType + "\",";
//     jsonResponse += "\"status\":\"" + status + "\",";
//     jsonResponse += "\"message\":\"" + message + "\",";
//     jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
//     jsonResponse += "\"checkInWindow\":\"" + checkInWindow + "\",";
//     jsonResponse += "\"checkOutWindow\":\"" + checkOutWindow + "\"";
//     jsonResponse += "}";
//   }

//   file.close();
//   jsonResponse += "]}";

//   return jsonResponse;
// }

// // ================== GET USER ATTENDANCE RECORDS ==================
// String getUserAttendanceRecords(String targetCardUuid) {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\"}";
//   }

//   DateTime now = getLocalTime();
//   char filename[32];
//   sprintf(filename, "%s%04d%02d%02d.csv",
//           DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

//   if (!SD.exists(filename)) {
//     return "{\"success\":true,\"message\":\"No records found\",\"data\":[]}";
//   }

//   File file = SD.open(filename, FILE_READ);
//   if (!file) {
//     return "{\"success\":false,\"message\":\"Failed to open log file\"}";
//   }

//   String jsonResponse = "{\"success\":true,\"message\":\"User records fetched successfully\",\"data\":[";

//   // Skip header
//   String header = file.readStringUntil('\n');

//   bool firstLine = true;
//   while (file.available()) {
//     String line = file.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     // Parse CSV line
//     int commaIndex = 0;
//     int startPos = 0;
//     int fieldIndex = 0;

//     String timestamp, cardUuid, userId, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

//     while (startPos < line.length()) {
//       commaIndex = line.indexOf(',', startPos);
//       if (commaIndex == -1) commaIndex = line.length();

//       String field = line.substring(startPos, commaIndex);

//       switch (fieldIndex) {
//         case 0: timestamp = field; break;
//         case 1: cardUuid = field; break;
//         case 2: userId = field; break;
//         case 3: userName = field; break;
//         case 4: recordType = field; break;
//         case 5: status = field; break;
//         case 6: message = field; break;
//         case 7: dayOfWeek = field; break;
//         case 8: checkInWindow = field; break;
//         case 9: checkOutWindow = field; break;
//       }

//       startPos = commaIndex + 1;
//       fieldIndex++;
//     }

//     // Sirf target user ke records filter karo
//     if (cardUuid == targetCardUuid) {
//       if (!firstLine) {
//         jsonResponse += ",";
//       }
//       firstLine = false;

//       jsonResponse += "{";
//       jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
//       jsonResponse += "\"cardUuid\":\"" + cardUuid + "\",";
//       jsonResponse += "\"userId\":" + userId + ",";
//       jsonResponse += "\"userName\":\"" + userName + "\",";
//       jsonResponse += "\"recordType\":\"" + recordType + "\",";
//       jsonResponse += "\"status\":\"" + status + "\",";
//       jsonResponse += "\"message\":\"" + message + "\",";
//       jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
//       jsonResponse += "\"checkInWindow\":\"" + checkInWindow + "\",";
//       jsonResponse += "\"checkOutWindow\":\"" + checkOutWindow + "\"";
//       jsonResponse += "}";
//     }
//   }

//   file.close();
//   jsonResponse += "]}";

//   return jsonResponse;
// }

// // ================== DELETE USER RECORDS FROM SD CARD ==================
// String deleteUserAttendanceRecords(String targetCardUuid) {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\"}";
//   }

//   DateTime now = getLocalTime();
//   char filename[32];
//   sprintf(filename, "%s%04d%02d%02d.csv",
//           DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

//   if (!SD.exists(filename)) {
//     return "{\"success\":false,\"message\":\"No records file found\"}";
//   }

//   // Temporary file banayein
//   char tempFilename[32];
//   sprintf(tempFilename, "%s%04d%02d%02d_temp.csv",
//           DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

//   File originalFile = SD.open(filename, FILE_READ);
//   if (!originalFile) {
//     return "{\"success\":false,\"message\":\"Failed to open original file\"}";
//   }

//   File tempFile = SD.open(tempFilename, FILE_WRITE);
//   if (!tempFile) {
//     originalFile.close();
//     return "{\"success\":false,\"message\":\"Failed to create temp file\"}";
//   }

//   int deletedCount = 0;
//   int totalCount = 0;

//   // Header copy karo
//   String header = originalFile.readStringUntil('\n');
//   tempFile.println(header);

//   // Records process karo
//   while (originalFile.available()) {
//     String line = originalFile.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     totalCount++;

//     // Check if this line contains target card UUID
//     if (line.indexOf(targetCardUuid) >= 0) {
//       deletedCount++;
//       continue;  // Skip this line - delete karo
//     }

//     // Keep this line
//     tempFile.println(line);
//   }

//   originalFile.close();
//   tempFile.close();

//   // Original file delete karo
//   SD.remove(filename);

//   // Temp file ko rename karo to original
//   SD.rename(tempFilename, filename);

//   // Vector records bhi update karo agar aaj ke hain
//   if (currentProcessingDay == (now.year() * 10000 + now.month() * 100 + now.day())) {
//     // Reload records from SD
//     loadTodayRecordsFromSD();
//   }

//   String response = "{\"success\":true,\"message\":\"Records deleted successfully\",";
//   response += "\"deletedCount\":" + String(deletedCount) + ",";
//   response += "\"totalCount\":" + String(totalCount) + "}";

//   return response;
// }

// // ================== GET MONTHLY ATTENDANCE RECORDS ==================
// String getMonthlyAttendanceRecords(int year, int month) {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\"}";
//   }

//   // Monthly filename: /monthly_2026_02.csv
//   char monthlyFilename[32];
//   sprintf(monthlyFilename, "%s%04d_%02d.csv",
//           MONTHLY_LOG_PREFIX, year, month);

//   Serial.printf("📁 Reading monthly file: %s\n", monthlyFilename);

//   if (!SD.exists(monthlyFilename)) {
//     return "{\"success\":true,\"message\":\"No records for this month\",\"data\":[]}";
//   }

//   File file = SD.open(monthlyFilename, FILE_READ);
//   if (!file) {
//     return "{\"success\":false,\"message\":\"Failed to open monthly file\"}";
//   }

//   String jsonResponse = "{\"success\":true,\"message\":\"Monthly records fetched successfully\",\"data\":[";

//   // Skip header
//   String header = file.readStringUntil('\n');

//   bool firstLine = true;
//   while (file.available()) {
//     String line = file.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     // Parse CSV line to JSON
//     int commaIndex = 0;
//     int startPos = 0;
//     int fieldIndex = 0;

//     String timestamp, cardUuid, userId, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

//     while (startPos < line.length()) {
//       commaIndex = line.indexOf(',', startPos);
//       if (commaIndex == -1) commaIndex = line.length();

//       String field = line.substring(startPos, commaIndex);

//       switch (fieldIndex) {
//         case 0: timestamp = field; break;
//         case 1: cardUuid = field; break;
//         case 2: userId = field; break;
//         case 3: userName = field; break;
//         case 4: recordType = field; break;
//         case 5: status = field; break;
//         case 6: message = field; break;
//         case 7: dayOfWeek = field; break;
//         case 8: checkInWindow = field; break;
//         case 9: checkOutWindow = field; break;
//       }

//       startPos = commaIndex + 1;
//       fieldIndex++;
//     }

//     if (!firstLine) {
//       jsonResponse += ",";
//     }
//     firstLine = false;

//     jsonResponse += "{";
//     jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
//     jsonResponse += "\"cardUuid\":\"" + cardUuid + "\",";
//     jsonResponse += "\"userId\":" + userId + ",";
//     jsonResponse += "\"userName\":\"" + userName + "\",";
//     jsonResponse += "\"recordType\":\"" + recordType + "\",";
//     jsonResponse += "\"status\":\"" + status + "\",";
//     jsonResponse += "\"message\":\"" + message + "\",";
//     jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
//     jsonResponse += "\"checkInWindow\":\"" + checkInWindow + "\",";
//     jsonResponse += "\"checkOutWindow\":\"" + checkOutWindow + "\"";
//     jsonResponse += "}";
//   }

//   file.close();
//   jsonResponse += "]}";

//   return jsonResponse;
// }

// // ================== GET USER MONTHLY RECORDS ==================
// String getUserMonthlyRecords(String targetCardUuid, int year, int month) {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\"}";
//   }

//   char monthlyFilename[32];
//   sprintf(monthlyFilename, "%s%04d_%02d.csv",
//           MONTHLY_LOG_PREFIX, year, month);

//   if (!SD.exists(monthlyFilename)) {
//     return "{\"success\":true,\"message\":\"No records for this month\",\"data\":[]}";
//   }

//   File file = SD.open(monthlyFilename, FILE_READ);
//   if (!file) {
//     return "{\"success\":false,\"message\":\"Failed to open monthly file\"}";
//   }

//   String jsonResponse = "{\"success\":true,\"message\":\"User monthly records fetched successfully\",\"data\":[";

//   // Skip header
//   String header = file.readStringUntil('\n');

//   bool firstLine = true;
//   while (file.available()) {
//     String line = file.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     // Parse CSV line
//     int commaIndex = 0;
//     int startPos = 0;
//     int fieldIndex = 0;

//     String timestamp, cardUuid, userId, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

//     while (startPos < line.length()) {
//       commaIndex = line.indexOf(',', startPos);
//       if (commaIndex == -1) commaIndex = line.length();

//       String field = line.substring(startPos, commaIndex);

//       switch (fieldIndex) {
//         case 0: timestamp = field; break;
//         case 1: cardUuid = field; break;
//         case 2: userId = field; break;
//         case 3: userName = field; break;
//         case 4: recordType = field; break;
//         case 5: status = field; break;
//         case 6: message = field; break;
//         case 7: dayOfWeek = field; break;
//         case 8: checkInWindow = field; break;
//         case 9: checkOutWindow = field; break;
//       }

//       startPos = commaIndex + 1;
//       fieldIndex++;
//     }

//     // Filter by target card UUID
//     if (cardUuid == targetCardUuid) {
//       if (!firstLine) {
//         jsonResponse += ",";
//       }
//       firstLine = false;

//       jsonResponse += "{";
//       jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
//       jsonResponse += "\"cardUuid\":\"" + cardUuid + "\",";
//       jsonResponse += "\"userId\":" + userId + ",";
//       jsonResponse += "\"userName\":\"" + userName + "\",";
//       jsonResponse += "\"recordType\":\"" + recordType + "\",";
//       jsonResponse += "\"status\":\"" + status + "\",";
//       jsonResponse += "\"message\":\"" + message + "\",";
//       jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
//       jsonResponse += "\"checkInWindow\":\"" + checkInWindow + "\",";
//       jsonResponse += "\"checkOutWindow\":\"" + checkOutWindow + "\"";
//       jsonResponse += "}";
//     }
//   }

//   file.close();
//   jsonResponse += "]}";

//   return jsonResponse;
// }

// // ================== DELETE MONTHLY FILE ==================
// String deleteMonthlyFile(int year, int month) {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\"}";
//   }

//   char monthlyFilename[32];
//   sprintf(monthlyFilename, "%s%04d_%02d.csv",
//           MONTHLY_LOG_PREFIX, year, month);

//   Serial.printf("🗑️ Attempting to delete monthly file: %s\n", monthlyFilename);

//   if (!SD.exists(monthlyFilename)) {
//     return "{\"success\":false,\"message\":\"Monthly file not found\"}";
//   }

//   bool deleted = SD.remove(monthlyFilename);

//   if (deleted) {
//     Serial.println("✅ Monthly file deleted successfully");
//     return "{\"success\":true,\"message\":\"Monthly file deleted successfully\"}";
//   } else {
//     Serial.println("❌ Failed to delete monthly file");
//     return "{\"success\":false,\"message\":\"Failed to delete monthly file\"}";
//   }
// }

// // ================== DELETE USER RECORDS FROM MONTHLY FILE ==================
// String deleteUserFromMonthlyFile(String targetCardUuid, int year, int month) {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\"}";
//   }

//   char monthlyFilename[32];
//   sprintf(monthlyFilename, "%s%04d_%02d.csv",
//           MONTHLY_LOG_PREFIX, year, month);

//   if (!SD.exists(monthlyFilename)) {
//     return "{\"success\":false,\"message\":\"Monthly file not found\"}";
//   }

//   // Temporary file banayein
//   char tempFilename[32];
//   sprintf(tempFilename, "%s%04d_%02d_temp.csv",
//           MONTHLY_LOG_PREFIX, year, month);

//   File originalFile = SD.open(monthlyFilename, FILE_READ);
//   if (!originalFile) {
//     return "{\"success\":false,\"message\":\"Failed to open monthly file\"}";
//   }

//   File tempFile = SD.open(tempFilename, FILE_WRITE);
//   if (!tempFile) {
//     originalFile.close();
//     return "{\"success\":false,\"message\":\"Failed to create temp file\"}";
//   }

//   int deletedCount = 0;
//   int totalCount = 0;

//   // Header copy karo
//   String header = originalFile.readStringUntil('\n');
//   tempFile.println(header);

//   // Records process karo
//   while (originalFile.available()) {
//     String line = originalFile.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     totalCount++;

//     // Parse CSV to get cardUuid (field index 1)
//     int commaIndex = 0;
//     int startPos = 0;
//     int fieldIndex = 0;
//     String cardUuid = "";

//     while (startPos < line.length() && fieldIndex <= 1) {
//       commaIndex = line.indexOf(',', startPos);
//       if (commaIndex == -1) commaIndex = line.length();

//       if (fieldIndex == 1) {
//         cardUuid = line.substring(startPos, commaIndex);
//         cardUuid.trim();
//         cardUuid.toUpperCase();
//         break;
//       }

//       startPos = commaIndex + 1;
//       fieldIndex++;
//     }

//     // Check if this line contains target card UUID
//     if (cardUuid == targetCardUuid) {
//       deletedCount++;
//       continue;  // Skip this line - delete karo
//     }

//     // Keep this line
//     tempFile.println(line);
//   }

//   originalFile.close();
//   tempFile.close();

//   // Original file delete karo
//   SD.remove(monthlyFilename);

//   // Temp file ko rename karo to original
//   SD.rename(tempFilename, monthlyFilename);

//   // Agar koi records delete hue to success message
//   if (deletedCount > 0) {
//     String response = "{\"success\":true,\"message\":\"User records deleted from monthly file\",";
//     response += "\"deletedCount\":" + String(deletedCount) + ",";
//     response += "\"totalCount\":" + String(totalCount) + "}";
//     return response;
//   } else {
//     return "{\"success\":false,\"message\":\"No records found for this user in monthly file\"}";
//   }
// }

// // ================== FIXED LIST FILES FUNCTION ==================
// String listAllLogFiles() {
//   if (!sdMounted) {
//     return "{\"success\":false,\"message\":\"SD card not mounted\",\"data\":{\"dailyFiles\":[],\"monthlyFiles\":[]}}";
//   }

//   DynamicJsonDocument doc(4096);
//   JsonObject data = doc.createNestedObject("data");
//   JsonArray dailyFiles = data.createNestedArray("dailyFiles");
//   JsonArray monthlyFiles = data.createNestedArray("monthlyFiles");

//   File root = SD.open("/");
//   if (!root) {
//     return "{\"success\":false,\"message\":\"Failed to open root directory\",\"data\":{\"dailyFiles\":[],\"monthlyFiles\":[]}}";
//   }

//   Serial.println("\n📂 Scanning SD card for log files...");

//   while (true) {
//     File entry = root.openNextFile();
//     if (!entry) break;

//     if (!entry.isDirectory()) {
//       String fileName = entry.name();

//       // Debug: Print every file found
//       Serial.printf("  Found file: %s\n", fileName.c_str());

//       // Daily files - check exact pattern (without slash)
//       if (fileName.startsWith("log_") && fileName.endsWith(".csv")) {
//         Serial.printf("  ✅ DAILY FILE MATCH: %s\n", fileName.c_str());

//         JsonObject fileObj = dailyFiles.createNestedObject();
//         fileObj["filename"] = fileName;

//         // Extract date from filename (format: log_YYYYMMDD.csv)
//         String dateStr = fileName.substring(strlen("log_"), fileName.length() - 4);
//         fileObj["date"] = dateStr;
//         fileObj["size"] = entry.size();
//       }

//       // Monthly files - check exact pattern (without slash)
//       if (fileName.startsWith("monthly_") && fileName.endsWith(".csv")) {
//         Serial.printf("  ✅ MONTHLY FILE MATCH: %s\n", fileName.c_str());

//         JsonObject fileObj = monthlyFiles.createNestedObject();
//         fileObj["filename"] = fileName;

//         // Extract year and month from filename (format: monthly_YYYY_MM.csv)
//         String yearMonth = fileName.substring(strlen("monthly_"), fileName.length() - 4);
//         fileObj["yearMonth"] = yearMonth;
//         fileObj["size"] = entry.size();
//       }
//     }

//     entry.close();
//   }

//   root.close();

//   doc["success"] = true;
//   doc["message"] = "Files listed successfully";

//   String response;
//   serializeJson(doc, response);

//   Serial.println("\n📤 Final JSON Response:");
//   Serial.println(response);
//   Serial.println();

//   return response;
// }

// // ================== GET SCHEDULES FROM MEMORY - FIXED ==================
// String getSchedulesFromMemory() {
//   DynamicJsonDocument doc(8192);

//   // Create users array
//   JsonArray users = doc.createNestedArray("users");

//   // Simple duplicate check using array
//   const int MAX_SCHEDULES = 50;
//   String seenKeys[MAX_SCHEDULES];
//   int seenCount = 0;

//   for (auto& u : userSchedules) {
//     // Create unique key
//     String key = String(u.userId) + "_" + String(u.dayOfWeek);

//     // Check if already seen
//     bool found = false;
//     for (int i = 0; i < seenCount; i++) {
//       if (seenKeys[i] == key) {
//         found = true;
//         break;
//       }
//     }

//     if (!found && seenCount < MAX_SCHEDULES) {
//       seenKeys[seenCount++] = key;

//       JsonObject user = users.createNestedObject();
//       user["id"] = u.userId;
//       user["cardUuid"] = u.cardUuid;
//       user["name"] = u.userName;
//       user["dayOfWeek"] = u.dayOfWeek;
//       user["checkInFrom"] = u.checkInFrom;
//       user["checkInTo"] = u.checkInTo;
//       user["checkOutFrom"] = u.checkOutFrom;
//       user["checkOutTo"] = u.checkOutTo;
//     }
//   }

//   // Create final response
//   String response;
//   serializeJson(doc, response);

//   return response;
// }

// // ================== HANDLE LIST FILES ==================
// void handleListFiles() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   Serial.println("\n📋 ===== HANDLING LIST FILES REQUEST =====");

//   // Debug: Show all files first
//   testSDCardReading();

//   // Then send response
//   String response = listAllLogFiles();

//   Serial.println("📤 Response being sent to client:");
//   Serial.println(response);
//   Serial.println("========================================\n");

//   server.send(200, "application/json", response);
// }

// // ================== IMPROVED SD CARD TEST ==================
// void testSDCardReading() {
//   Serial.println("\n========== SD CARD DETAILED TEST ==========");

//   if (!sdMounted) {
//     Serial.println("❌ SD CARD NOT MOUNTED!");
//     return;
//   }

//   Serial.println("✅ SD CARD MOUNTED SUCCESSFULLY");

//   Serial.println("\n📂 LISTING ALL FILES IN ROOT DIRECTORY:");

//   File root = SD.open("/");
//   if (!root) {
//     Serial.println("❌ CANNOT OPEN ROOT DIRECTORY!");
//     return;
//   }

//   if (!root.isDirectory()) {
//     Serial.println("❌ ROOT IS NOT A DIRECTORY!");
//     root.close();
//     return;
//   }

//   int fileCount = 0;
//   int dirCount = 0;

//   while (true) {
//     File entry = root.openNextFile();
//     if (!entry) break;

//     if (entry.isDirectory()) {
//       dirCount++;
//       Serial.printf("📁 DIR: %s\n", entry.name());
//     } else {
//       fileCount++;
//       String fileName = entry.name();
//       Serial.printf("📄 FILE: %s (%d bytes)\n", fileName.c_str(), entry.size());

//       // Check if it matches our patterns
//       if (fileName.startsWith("/log_")) {
//         Serial.println("   🔍 This is a DAILY log file!");
//       }
//       if (fileName.startsWith("/monthly_")) {
//         Serial.println("   🔍 This is a MONTHLY log file!");
//       }
//     }
//     entry.close();
//   }

//   root.close();

//   Serial.printf("\n📊 SUMMARY: %d directories, %d files\n", dirCount, fileCount);
//   Serial.println("============================================\n");
// }

// // ================== HANDLE RFID ===================
// void handleRFID() {
//   String cardUuid = "";
//   for (byte i = 0; i < rfid.uid.size; i++) {
//     cardUuid += String(rfid.uid.uidByte[i], HEX);
//   }
//   cardUuid.toUpperCase();
//   Serial.println("Card scanned: " + cardUuid);

//   if (localStorage) {
//     // ----- LOCAL MODE (attendance on SD, WiFi not needed) -----
//     if (userSchedules.size() == 0) {
//       Serial.println("No schedules loaded – cannot accept any card");
//       blinkRedTwice();
//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();
//       delay(1200);
//       return;
//     }

//     // Check if card exists in any schedule
//     bool foundInSchedule = false;
//     for (const auto& schedule : userSchedules) {
//       if (schedule.cardUuid == cardUuid) {
//         foundInSchedule = true;
//         break;
//       }
//     }
//     if (!foundInSchedule) {
//       Serial.println("Card not found in local schedule");
//       blinkRedTwice();
//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();
//       delay(1200);
//       return;
//     }

//     // Get current time and day
//     DateTime currentTime = getLocalTime();
//     int currentDOW = currentTime.dayOfTheWeek();
//     currentDOW = (currentDOW == 0) ? 7 : currentDOW;

//     // Find today's schedule for this card
//     const UserSchedule* todaysSchedule = nullptr;
//     for (const auto& schedule : userSchedules) {
//       if (schedule.cardUuid == cardUuid && schedule.dayOfWeek == currentDOW) {
//         todaysSchedule = &schedule;
//         break;
//       }
//     }
//     if (!todaysSchedule) {
//       Serial.println("❌ No schedule found for today");
//       blinkRedTwice();
//       rfid.PICC_HaltA();
//       rfid.PCD_StopCrypto1();
//       delay(1200);
//       return;
//     }

//     AttendanceResult localResult = processLocalAttendance(
//       cardUuid,
//       todaysSchedule->userId,
//       todaysSchedule->userName,
//       *todaysSchedule,
//       currentTime);

//     if (localResult.success) {
//       blinkGreenOnce();
//       Serial.println("✅ LOCAL ATTENDANCE ACCEPTED: " + localResult.message);
//       const char* days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
//       saveAttendanceLogToSD(
//         cardUuid,
//         todaysSchedule->userId,
//         todaysSchedule->userName,
//         localResult.timestamp,
//         localResult.recordType,
//         localResult.status,
//         localResult.message,
//         String(days[currentDOW - 1]),
//         todaysSchedule->checkInFrom + " - " + todaysSchedule->checkInTo,
//         todaysSchedule->checkOutFrom + " - " + todaysSchedule->checkOutTo);
//     } else {
//       blinkRedTwice();
//       Serial.println("❌ LOCAL ATTENDANCE DENIED: " + localResult.message);
//     }
//   } else {
//     // ----- ONLINE MODE (server attendance) -----
//     if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//       bool accepted = sendAttendance(cardUuid);
//       if (accepted) blinkGreenOnce();
//       else blinkRedTwice();
//     } else {
//       Serial.println("WiFi not connected – cannot send attendance to server");
//       blinkRedTwice();
//     }
//   }

//   rfid.PICC_HaltA();
//   rfid.PCD_StopCrypto1();
//   delay(1200);  // anti double‑tap
// }

// // ================== HANDLE DELETE MONTHLY RECORDS ==================
// void handleDeleteMonthlyRecords() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   // Only POST method allowed
//   if (server.method() != HTTP_POST) {
//     server.send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
//     return;
//   }

//   // Parse JSON body
//   DynamicJsonDocument doc(256);
//   DeserializationError error = deserializeJson(doc, server.arg("plain"));

//   if (error) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
//     return;
//   }

//   // Get year and month from request
//   int year = doc["year"] | 0;
//   int month = doc["month"] | 0;

//   // If year/month not provided, use current
//   if (year == 0 || month == 0) {
//     DateTime now = rtc.now();
//     year = now.year();
//     month = now.month();
//   }

//   // Validate month
//   if (month < 1 || month > 12) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid month. Must be 1-12\"}");
//     return;
//   }

//   // Check if deleteAll flag is true
//   bool deleteAll = doc["deleteAll"] | false;

//   if (deleteAll) {
//     // Delete entire monthly file
//     String response = deleteMonthlyFile(year, month);
//     server.send(200, "application/json", response);
//   } else if (doc.containsKey("cardUuid")) {
//     // Delete specific user records
//     String cardUuid = doc["cardUuid"].as<String>();
//     cardUuid.toUpperCase();
//     String response = deleteUserFromMonthlyFile(cardUuid, year, month);
//     server.send(200, "application/json", response);
//   } else {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Either cardUuid or deleteAll flag required\"}");
//   }
// }

// // ================== HANDLE DELETE ALL MONTHLY RECORDS ==================
// void handleDeleteAllMonthlyRecords() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   // Get year and month from query parameters
//   int year = server.arg("year").toInt();
//   int month = server.arg("month").toInt();

//   // If year/month not provided, use current
//   if (year == 0 || month == 0) {
//     DateTime now = rtc.now();
//     year = now.year();
//     month = now.month();
//   }

//   // Validate month
//   if (month < 1 || month > 12) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid month. Must be 1-12\"}");
//     return;
//   }

//   String response = deleteMonthlyFile(year, month);
//   server.send(200, "application/json", response);
// }

// // ================== HANDLE GET MONTHLY RECORDS ==================
// void handleGetMonthlyRecords() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   // Get year and month from query parameters
//   int year = server.arg("year").toInt();
//   int month = server.arg("month").toInt();

//   // If year/month not provided, use current
//   if (year == 0 || month == 0) {
//     DateTime now = rtc.now();
//     year = now.year();
//     month = now.month();
//   }

//   // Validate month
//   if (month < 1 || month > 12) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid month. Must be 1-12\"}");
//     return;
//   }

//   // Check if cardUuid parameter exists
//   if (server.hasArg("cardUuid")) {
//     String cardUuid = server.arg("cardUuid");
//     cardUuid.toUpperCase();
//     String response = getUserMonthlyRecords(cardUuid, year, month);
//     server.send(200, "application/json", response);
//   } else {
//     // Return all monthly records
//     String response = getMonthlyAttendanceRecords(year, month);
//     server.send(200, "application/json", response);
//   }
// }

// // ================== HANDLE GET ATTENDANCE RECORDS ==================
// void handleGetAttendanceRecords() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   // Check if cardUuid parameter exists
//   if (server.hasArg("cardUuid")) {
//     String cardUuid = server.arg("cardUuid");
//     cardUuid.toUpperCase();
//     String response = getUserAttendanceRecords(cardUuid);
//     server.send(200, "application/json", response);
//   } else {
//     // Return all today's records
//     String response = getTodayAttendanceRecords();
//     server.send(200, "application/json", response);
//   }
// }

// // ================== HANDLE DELETE ATTENDANCE RECORDS ==================
// void handleDeleteAttendanceRecords() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   // Only POST method allowed
//   if (server.method() != HTTP_POST) {
//     server.send(405, "application/json", "{\"success\":false,\"message\":\"Method not allowed\"}");
//     return;
//   }

//   // Parse JSON body
//   DynamicJsonDocument doc(256);
//   DeserializationError error = deserializeJson(doc, server.arg("plain"));

//   if (error) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
//     return;
//   }

//   if (!doc.containsKey("cardUuid")) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"cardUuid required\"}");
//     return;
//   }

//   String cardUuid = doc["cardUuid"].as<String>();
//   cardUuid.toUpperCase();

//   String response = deleteUserAttendanceRecords(cardUuid);
//   server.send(200, "application/json", response);
// }

// // ================== HANDLE DELETE ALL TODAY'S RECORDS ==================
// void handleDeleteAllTodayRecords() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   if (!sdMounted) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"SD card not mounted\"}");
//     return;
//   }

//   DateTime now = getLocalTime();
//   char filename[32];
//   sprintf(filename, "%s%04d%02d%02d.csv",
//           DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

//   if (!SD.exists(filename)) {
//     server.send(404, "application/json", "{\"success\":false,\"message\":\"No records file found\"}");
//     return;
//   }

//   // File delete karo
//   bool deleted = SD.remove(filename);

//   if (deleted) {
//     // Vector records bhi clear karo
//     if (currentProcessingDay == (now.year() * 10000 + now.month() * 100 + now.day())) {
//       todaysCheckIns.clear();
//       todaysCheckOuts.clear();
//     }

//     server.send(200, "application/json",
//                 "{\"success\":true,\"message\":\"All today's records deleted successfully\"}");
//   } else {
//     server.send(500, "application/json",
//                 "{\"success\":false,\"message\":\"Failed to delete records file\"}");
//   }
// }

// // ================== HANDLE AUTO SYNC TOGGLE ==================
// void handleAutoSync() {
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

//   if (autoSyncEnabled != newValue) {
//     autoSyncEnabled = newValue;
//     EEPROM.write(EEPROM_AUTO_SYNC_ADDR, autoSyncEnabled ? 1 : 0);
//     EEPROM.commit();
//   }

//   Serial.print("🔄 Auto Sync toggled to: ");
//   Serial.println(autoSyncEnabled ? "TRUE" : "FALSE");

//   server.send(200, "application/json",
//               autoSyncEnabled
//                 ? "{\"success\":true,\"autoSync\":true}"
//                 : "{\"success\":true,\"autoSync\":false}");
// }

// // ================== HANDLE MANUAL SYNC ==================
// void handleManualSync() {
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

//   // Parse JSON body
//   DynamicJsonDocument doc(256);
//   DeserializationError error = deserializeJson(doc, server.arg("plain"));

//   if (error) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
//     return;
//   }

//   // Get year and month from request (optional)
//   int year = doc["year"] | 0;
//   int month = doc["month"] | 0;

//   // If not provided, use current
//   if (year == 0 || month == 0) {
//     DateTime now = rtc.now();
//     year = now.year();
//     month = now.month();
//   }

//   // Validate month
//   if (month < 1 || month > 12) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid month. Must be 1-12\"}");
//     return;
//   }

//   Serial.printf("🔄 Manual sync triggered for %04d-%02d\n", year, month);

//   // Call sync function
//   bool result = syncMonthlyRecordsToServer();

//   if (result) {
//     server.send(200, "application/json",
//                 "{\"success\":true,\"message\":\"Sync completed successfully\"}");
//   } else {
//     server.send(200, "application/json",
//                 "{\"success\":false,\"message\":\"No records to sync or sync failed\"}");
//   }
// }

// // ================== SYNC USER SCHEDULE ==================
// void handleSyncUserSchedule() {
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

//   // Parse JSON body
//   DynamicJsonDocument doc(4096);
//   DeserializationError error = deserializeJson(doc, server.arg("plain"));

//   if (error) {
//     server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
//     return;
//   }

//   // Check if it's a user update or full replacement
//   bool replaceAll = doc["replaceAll"] | false;

//   if (replaceAll) {
//     // Full replacement - clear all and add new
//     userSchedules.clear();
//   }

//   // Parse users array
//   JsonArray users = doc["users"];
//   int addedCount = 0;
//   int updatedCount = 0;

//   for (JsonObject user : users) {
//     int userId = user["userId"] | 0;
//     String cardUuid = user["cardUuid"].as<String>();
//     String userName = user["userName"].as<String>();
//     JsonArray schedules = user["schedules"];

//     if (userId == 0) continue;

//     // Agar replaceAll true hai to hume existing schedules check nahi karne
//     if (!replaceAll) {
//       // Delete all existing schedules for this user (optional - depends on requirement)
//       // deleteAllUserSchedules(userId);
//     }

//     for (JsonObject sched : schedules) {
//       int dayOfWeek = sched["dayOfWeek"] | 0;
//       if (dayOfWeek == 0) continue;

//       // Check if exists
//       UserSchedule* existing = nullptr;
//       bool exists = findUserSchedule(userId, dayOfWeek, existing);

//       if (exists && !replaceAll) {
//         // Update existing
//         existing->cardUuid = cardUuid;
//         existing->userName = userName;
//         existing->checkInFrom = sched["checkInFrom"].as<String>();
//         existing->checkInTo = sched["checkInTo"].as<String>();
//         existing->checkOutFrom = sched["checkOutFrom"].as<String>();
//         existing->checkOutTo = sched["checkOutTo"].as<String>();
//         updatedCount++;
//       } else {
//         // Add new
//         UserSchedule newSched;
//         newSched.userId = userId;
//         newSched.cardUuid = cardUuid;
//         newSched.userName = userName;
//         newSched.dayOfWeek = dayOfWeek;
//         newSched.checkInFrom = sched["checkInFrom"].as<String>();
//         newSched.checkInTo = sched["checkInTo"].as<String>();
//         newSched.checkOutFrom = sched["checkOutFrom"].as<String>();
//         newSched.checkOutTo = sched["checkOutTo"].as<String>();

//         userSchedules.push_back(newSched);
//         addedCount++;
//       }
//     }
//   }

//   // Save to SD
//   if (localStorage && sdMounted) {
//     saveScheduleToSD();
//   }

//   Serial.printf("✅ Bulk sync complete: %d added, %d updated\n", addedCount, updatedCount);

//   String response = "{\"success\":true,\"message\":\"Bulk sync completed\",";
//   response += "\"added\":" + String(addedCount) + ",";
//   response += "\"updated\":" + String(updatedCount) + "}";

//   server.send(200, "application/json", response);
// }

// // ================== HANDLE GET DEVICE SCHEDULES ==================
// void handleGetDeviceSchedules() {
//   // Authentication check
//   if (!server.hasHeader("x-device-id") || !server.hasHeader("x-device-secret")) {
//     server.send(401, "application/json", "{\"success\":false,\"message\":\"Missing auth headers\"}");
//     return;
//   }

//   String deviceId = server.header("x-device-id");
//   String deviceSecret = server.header("x-device-secret");

//   if (deviceId != DEVICE_UUID || deviceSecret != DEVICE_SECRET) {
//     server.send(403, "application/json", "{\"success\":false,\"message\":\"Invalid device credentials\"}");
//     return;
//   }

//   Serial.println("\n📋 ===== HANDLING GET SCHEDULES REQUEST =====");

//   String response;

//   // 🔥 Option 1: Memory se bhejein (fastest & most reliable)
//   // response = getSchedulesFromMemory();

//   // 🔥 Option 2: Agar SD se hi bhejna ho to:
//   response = getSchedulesFromMemory();

//   Serial.println("📤 Sending schedules response:");
//   Serial.println(response);
//   Serial.println("===========================================\n");

//   server.send(200, "application/json", response);
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

//   server.on("/api/attendance/records", HTTP_GET, handleGetAttendanceRecords);
//   server.on("/api/attendance/records", HTTP_DELETE, handleDeleteAllTodayRecords);
//   server.on("/api/attendance/records/delete", HTTP_POST, handleDeleteAttendanceRecords);

//   server.on("/api/attendance/monthly", HTTP_GET, handleGetMonthlyRecords);
//   server.on("/api/attendance/monthly", HTTP_DELETE, handleDeleteAllMonthlyRecords);
//   server.on("/api/attendance/monthly/delete", HTTP_POST, handleDeleteMonthlyRecords);

//   server.on("/api/attendance/files", HTTP_GET, handleListFiles);

//   server.on("/api/auto-sync", HTTP_POST, handleAutoSync);
//   server.on("/api/attendance/sync", HTTP_POST, handleManualSync);

//   server.on("/api/attendance/schedule/sync", HTTP_POST, handleSyncUserSchedule);

//   server.on("/api/device/schedules", HTTP_GET, handleGetDeviceSchedules);

//   server.begin();
//   Serial.println("HTTP server started");
// }

// // ============= RTC Update REAL UTC TIME =============
// void setRTCFromNTP() {
// #if USE_NTP_TIME == true
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
// #else
//   // 🔥 MANUAL TIME SET KARO - TESTING KE LIYE
//   DateTime manualTime(
//     manualYear,
//     manualMonth,
//     manualDay,
//     manualHour,
//     manualMinute,
//     manualSecond);

//   rtc.adjust(manualTime);

//   Serial.println("\n⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️");
//   Serial.println("🔧 TEST MODE: MANUAL TIME SET!");
//   Serial.printf("📅 Manual Date: %04d-%02d-%02d\n", manualYear, manualMonth, manualDay);
//   Serial.printf("⏰ Manual Time: %02d:%02d:%02d\n", manualHour, manualMinute, manualSecond);
//   Serial.println("⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️⚠️\n");

// #endif
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

//     Serial.print("🔁 Boot LocalStorage = ");
//     Serial.println(localStorage ? "TRUE" : "FALSE");
//   }

//   // ================== LOAD AUTO SYNC FROM EEPROM ==================
//   uint8_t storedAutoSync = EEPROM.read(EEPROM_AUTO_SYNC_ADDR);
//   if (storedAutoSync == 0 || storedAutoSync == 1) {
//     autoSyncEnabled = storedAutoSync;
//   } else {
//     autoSyncEnabled = true;  // default
//     EEPROM.write(EEPROM_AUTO_SYNC_ADDR, 1);
//     EEPROM.commit();

//     Serial.print("🔁 Auto Sync = ");
//     Serial.println(autoSyncEnabled ? "TRUE" : "FALSE");
//   }

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
//   Serial.println("Initializing SD card...");

//   pinMode(SD_CS_PIN, OUTPUT);
//   digitalWrite(SD_CS_PIN, HIGH);

//   SPI.begin();

//   if (!SD.begin(SD_CS_PIN)) {
//     Serial.println("❌ SD Card init failed");
//     sdMounted = false;
//   } else {
//     Serial.println("✅ SD Card initialized");
//     sdMounted = true;

//     // 🔹 Schedule only if localStorage enabled
//     if (localStorage) {
//       Serial.println("Local storage enabled — loading schedule");

//       if (loadScheduleFromSD()) {
//         Serial.println("📅 Schedule loaded from SD");
//         isInitialScheduleLoaded = true;
//       } else {
//         Serial.println("⚠️ No schedule found on SD");
//       }
//     } else {
//       Serial.println("Local storage disabled — SD mounted but schedule skipped");
//     }
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

//   WiFi.mode(WIFI_STA);
//   WiFi.begin();

//   // RFID INITIALIZE
//   SPI.begin();
//   rfid.PCD_Init();
//   Serial.println("RFID Module Initialized");

//   Serial.println("Attempting WiFi connection...");

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

//   // Initialize local attendance manager
//   if (localStorage) {
//     // Reset today's records
//     todaysCheckIns.clear();
//     todaysCheckOuts.clear();
//     DateTime now = getLocalTime();
//     currentProcessingDay = now.year() * 10000 + now.month() * 100 + now.day();
//     Serial.println("[LocalAttendance] Manager initialized");

//     // 🔥 NAYA: SD CARD SE TODAY'S RECORDS LOAD KARO
//     if (sdMounted) {
//       loadTodayRecordsFromSD();
//     }

//     Serial.println("[LocalAttendance] Manager initialized");
//     Serial.printf("📊 Today's stats - Check-ins: %d, Check-outs: %d\n",
//                   todaysCheckIns.size(), todaysCheckOuts.size());
//   }

//   Serial.println("Setup complete!");
// }

// // ================== LOOP ==================
// void loop() {

//   static bool rtcSynced = false;
//   unsigned long currentMillis = millis();

//   // ================== RFID CHECK - ALWAYS RUN ==================
//   // RFID ko hamesha check karo, WiFi status se independent
//   static unsigned long lastRFIDCheck = 0;
//   if (currentMillis - lastRFIDCheck >= 50) {  // Har 50ms check
//     lastRFIDCheck = currentMillis;

//     if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//       handleRFID();  // Ye call hamesha hoga
//       delay(200);    // Thoda delay anti-spam ke liye
//     }
//   }

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

//   // ----------------- DAY CHANGE CHECK (every 10 sec) -----------------
//   static unsigned long lastDayCheck = 0;
//   if (millis() - lastDayCheck >= 10000) {  // Har 10 seconds
//     lastDayCheck = millis();
//     if (localStorage) {
//       DateTime currentTime = getLocalTime();
//       checkDayChange(currentTime);
//     }
//   }

//   // ----------------- RFID ATTENDANCE -----------------
//   if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
//     handleRFID();
//   }

//   // ----------------- HEARTBEAT + WIFI SCAN -----------------
//   if (WiFi.status() == WL_CONNECTED && !reprovisionMode) {
//     wifiFailCount = 0;
//     isConnecting = false;

//     server.handleClient();

//     if (!rtcSynced) {
//       Serial.println("🕒 First WiFi connection - syncing RTC from NTP...");
//       setRTCFromNTP();
//       rtcSynced = true;
//     }

//     if (currentMillis - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
//       sendHeartbeat();
//       lastHeartbeatTime = currentMillis;
//     }

//     if (localStorage) {
//       if (currentMillis - lastScheduleUpdate >= scheduleUpdateInterval) {
//         fetchAndStoreSchedules();
//         lastScheduleUpdate = currentMillis;
//       }

//       // AUTO SYNC CHECK - Har 5 minutes
//       if (autoSyncEnabled && currentMillis - lastSyncTime >= SYNC_INTERVAL) {
//         lastSyncTime = currentMillis;
//         syncMonthlyRecordsToServer();
//       }
//     }

//   } else {
//     static unsigned long lastReconnectAttempt = 0;
//     if (currentMillis - lastReconnectAttempt >= 30000) {  // Har 30 sec try
//       lastReconnectAttempt = currentMillis;

//       wifiFailCount++;
//       Serial.printf("WiFi disconnected. Attempt %d to reconnect...\n", wifiFailCount);

//       WiFi.reconnect();  // Non-blocking call

//       if (wifiFailCount >= FAIL_LIMIT) {
//         checkReprovision();
//       }
//     }
//   }

//   delay(10);
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
//   doc["autoSyncEnabled"] = autoSyncEnabled;

//   // ================== SD CARD STATUS ==================
//   JsonObject sd = doc.createNestedObject("sd");
//   sd["enabled"] = localStorage;
//   sd["mounted"] = sdMounted ? true : false;
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

//     // Important: Stop any ongoing WiFi operations
//     WiFi.disconnect(true);
//     delay(500);

//     // WiFiManager configuration
//     wifiManager.setConfigPortalTimeout(180);  // 3 minutes
//     wifiManager.setConnectTimeout(20);
//     wifiManager.setAPCallback([](WiFiManager* myWiFiManager) {
//       Serial.println("Entered config mode");
//       Serial.println("SSID: " + myWiFiManager->getConfigPortalSSID());
//       Serial.println("IP: 192.168.4.1");
//     });

//     // Set static IP for AP
//     wifiManager.setAPStaticIPConfig(
//       IPAddress(192, 168, 4, 1),
//       IPAddress(192, 168, 4, 1),
//       IPAddress(255, 255, 255, 0));

//     // Start config portal
//     Serial.println("Starting WiFi Manager Portal...");
//     Serial.println("Connect to SSID: RFID_Device_001");
//     Serial.println("Password: 12345678");
//     Serial.println("Then open browser to: 192.168.4.1");

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

// // ================== LOCAL ATTENDANCE HELPER FUNCTIONS ==================
// // Server logic ka exact replica
// int timeToSeconds(const String& timeStr) {
//   if (timeStr.length() == 0 || timeStr == "Invalid Date" || timeStr == "null") {
//     return 0;
//   }

//   String cleanTime = timeStr;
//   int dotIndex = cleanTime.indexOf('.');
//   if (dotIndex > 0) {
//     cleanTime = cleanTime.substring(0, dotIndex);
//   }

//   int h1 = cleanTime.substring(0, 2).toInt();
//   int h2 = cleanTime.substring(3, 5).toInt();
//   int sec = 0;

//   if (cleanTime.length() >= 8) {
//     sec = cleanTime.substring(6, 8).toInt();
//   }

//   return h1 * 3600 + h2 * 60 + sec;
// }

// // ================== SERVER LOGIC EXACT REPLICA ==================
// AttendanceResult processLocalAttendance(
//   const String& cardUuid,
//   int userId,
//   const String& userName,
//   const UserSchedule& todaySchedule,
//   const DateTime& currentTime) {

//   AttendanceResult result;
//   result.success = false;
//   result.recordType = "";
//   result.status = "present";
//   result.shouldDeletePreviousCheckOuts = false;

//   // === 1. GET CURRENT DAY AND TIME ===
//   int currentDOW = currentTime.dayOfTheWeek();
//   currentDOW = (currentDOW == 0) ? 7 : currentDOW;
//   int currentTimeSec = currentTime.hour() * 3600 + currentTime.minute() * 60 + currentTime.second();

//   // === 2. CHECK IF TODAY'S SCHEDULE MATCHES ===
//   if (todaySchedule.dayOfWeek != currentDOW) {
//     result.message = "No schedule found for today (Day " + String(currentDOW) + ")";
//     return result;
//   }

//   // === 3. CONVERT SCHEDULE TIMES TO SECONDS ===
//   int ciFromSec = timeToSeconds(todaySchedule.checkInFrom);
//   int ciToSec = timeToSeconds(todaySchedule.checkInTo);
//   int coFromSec = timeToSeconds(todaySchedule.checkOutFrom);
//   int coToSec = timeToSeconds(todaySchedule.checkOutTo);

//   // === 4. VERIFY THIS IS DAY SHIFT ===
//   if (ciFromSec > coFromSec) {
//     result.message = "This is an overnight shift. Day shift handler only.";
//     return result;
//   }

//   // === 5. CALCULATE WINDOWS WITH GRACE PERIODS ===
//   int earliestCheckIn = ciFromSec - (GRACE_EARLY_IN * 60);
//   int latestCheckIn = ciToSec + (GRACE_LATE_IN * 60);
//   bool isCheckInWindow = isTimeInRange(currentTimeSec, earliestCheckIn, latestCheckIn);
//   bool isBeforeCheckInWindow = currentTimeSec < earliestCheckIn;
//   bool isAfterCheckInWindow = currentTimeSec > latestCheckIn;

//   int earliestCheckOut = coFromSec - (GRACE_EARLY_OUT * 60);
//   int latestCheckOut = coToSec + (GRACE_LATE_OUT * 60);
//   bool isCheckOutWindow = isTimeInRange(currentTimeSec, earliestCheckOut, latestCheckOut);
//   bool isBeforeCheckOutWindow = currentTimeSec < earliestCheckOut;
//   bool isAfterCheckOutWindow = currentTimeSec > latestCheckOut;

//   // Debug prints
//   Serial.println("\n=== TIME WINDOW DEBUG ===");
//   Serial.printf("currentTimeSec: %d (%02d:%02d)\n", currentTimeSec, currentTimeSec / 3600, (currentTimeSec % 3600) / 60);
//   Serial.printf("checkOut window: %d (%02d:%02d) to %d (%02d:%02d)\n",
//                 earliestCheckOut, earliestCheckOut / 3600, (earliestCheckOut % 3600) / 60,
//                 latestCheckOut, latestCheckOut / 3600, (latestCheckOut % 3600) / 60);
//   Serial.printf("isCheckOutWindow: %s\n", isCheckOutWindow ? "TRUE" : "FALSE");
//   Serial.printf("isAfterCheckOutWindow: %s\n", isAfterCheckOutWindow ? "TRUE" : "FALSE");
//   Serial.printf("isBeforeCheckOutWindow: %s\n", isBeforeCheckOutWindow ? "TRUE" : "FALSE");
//   Serial.println("=========================\n");

//   // === 6. CHECK FOR OPEN CHECK-IN ===
//   bool hasOpenCheckIn = false;
//   if (todaysCheckIns.size() > todaysCheckOuts.size()) {
//     hasOpenCheckIn = true;
//   }

//   // === 7. ANTI-SPAM CHECK ===
//   if (todaysCheckIns.size() > 0 || todaysCheckOuts.size() > 0) {
//     int lastRecordTime = 0;
//     if (todaysCheckOuts.size() > 0) {
//       lastRecordTime = todaysCheckOuts.back().timestamp;
//     }
//     if (todaysCheckIns.size() > 0) {
//       int lastInTime = todaysCheckIns.back().timestamp;
//       if (lastInTime > lastRecordTime) lastRecordTime = lastInTime;
//     }

//     int secondsSinceLast = currentTimeSec - lastRecordTime;
//     if (secondsSinceLast < MIN_GAP_BETWEEN_RECORDS && lastRecordTime > 0) {
//       result.message = "Please wait " + String(MIN_GAP_BETWEEN_RECORDS) + " seconds before next scan";
//       return result;
//     }
//   }

//   // === 🔥 FIXED DECISION LOGIC - SAHI ORDER ===

//   // CASE 1: USER HASN'T CHECKED IN TODAY
//   if (todaysCheckIns.size() == 0) {
//     if (isBeforeCheckInWindow) {
//       String checkInTime = addMinutesToTimeStr(todaySchedule.checkInFrom, -GRACE_EARLY_IN);
//       result.message = "Shift hasn't started yet. Check-in window opens at " + checkInTime;
//       return result;
//     }

//     if (isAfterCheckInWindow) {
//       String closedTime = addMinutesToTimeStr(todaySchedule.checkInTo, GRACE_LATE_IN);
//       result.message = "Check-in window closed at " + closedTime + ". Please wait for next shift.";
//       return result;
//     }

//     if (isCheckInWindow) {
//       result.recordType = "in";
//       result.success = true;

//       if (currentTimeSec < ciFromSec) {
//         result.status = "early";
//         result.message = "Checked in early";
//       } else if (currentTimeSec <= ciToSec) {
//         result.status = "present";
//         result.message = "Checked in on time";
//       } else {
//         result.status = "late";
//         result.message = "Checked in late";
//       }

//       addRecord("in", currentTime, userId, cardUuid, userName);
//     }
//   }

//   // 🔥 CASE 4: CHECK-OUT WINDOW CLOSED - YEH SABSE PEHLE CHECK KARO!
//   else if (isAfterCheckOutWindow) {
//     if (todaysCheckIns.size() > 0 && todaysCheckOuts.size() == 0) {
//       // User checked in but didn't check out
//       String checkInTime = formatTimeDisplay(todaySchedule.checkInFrom);
//       String checkOutEndTime = formatTimeDisplay(todaySchedule.checkOutTo);
//       String lateCheckOutEnd = addMinutesToTimeStr(todaySchedule.checkOutTo, GRACE_LATE_OUT);

//       // Get next schedule
//       String nextSchedule = getNextScheduleInfo(userId, currentDOW);

//       result.message = "⚠️ Today's shift ended without check-out!\n";
//       result.message += "   ✓ Check-in: " + checkInTime + "\n";
//       result.message += "   ⛔ Check-out window closed: " + lateCheckOutEnd + "\n";
//       result.message += "   📅 Next shift: " + nextSchedule;
//       result.recordType = "in";
//       return result;
//     } else if (todaysCheckIns.size() == 0) {
//       // User never checked in
//       String checkInStartTime = formatTimeDisplay(todaySchedule.checkInFrom);
//       String lateCheckInEnd = addMinutesToTimeStr(todaySchedule.checkInTo, GRACE_LATE_IN);

//       String nextSchedule = getNextScheduleInfo(userId, currentDOW);

//       result.message = "❌ You missed today's shift!\n";
//       result.message += "   ✓ Check-in window was: " + checkInStartTime + " - " + lateCheckInEnd + "\n";
//       result.message += "   📅 Next shift: " + nextSchedule;
//       result.recordType = "in";
//       return result;
//     } else {
//       // User already checked out
//       String checkOutTime = "N/A";
//       if (todaysCheckOuts.size() > 0) {
//         checkOutTime = todaysCheckOuts.back().timestampStr;
//       }

//       result.message = "✅ Today's shift completed.\n";
//       result.message += "   ✓ Check-out: " + formatTimeDisplay(checkOutTime);
//       result.recordType = "out";
//       return result;
//     }
//   }

//   // CASE 2: WE'RE IN CHECK-OUT WINDOW - ALLOW MULTIPLE CHECK-OUTS
//   else if (isCheckOutWindow) {
//     result.recordType = "out";
//     result.success = true;
//     result.shouldDeletePreviousCheckOuts = true;

//     if (currentTimeSec < coFromSec) {
//       result.status = "early";
//       result.message = "Checked out early";
//     } else if (currentTimeSec <= coToSec) {
//       result.status = "present";
//       result.message = "Checked out on time";
//     } else {
//       result.status = "late";
//       result.message = "Checked out late";
//     }

//     // Check minimum work duration for first check-out
//     if (todaysCheckOuts.size() == 0 && todaysCheckIns.size() > 0) {
//       TodaysRecord& lastCheckInRecord = todaysCheckIns.back();
//       int minutesWorked = (currentTimeSec - lastCheckInRecord.timestamp) / 60;

//       if (minutesWorked < MIN_WORK_DURATION) {
//         result.success = false;
//         result.message = "Minimum work duration not met. You need to work at least " + String(MIN_WORK_DURATION) + " minutes";
//         return result;
//       }
//     }

//     addRecord("out", currentTime, userId, cardUuid, userName);
//   }

//   // CASE 3: CHECK-OUT WINDOW NOT OPEN YET BUT USER CHECKED IN
//   else if (hasOpenCheckIn && isBeforeCheckOutWindow) {
//     String checkOutTime = formatTimeDisplay(todaySchedule.checkOutFrom);
//     result.message = "Already checked in. Check-out window opens at " + checkOutTime;
//     return result;
//   }

//   // DEFAULT CASE
//   else {
//     result.message = "Unable to process attendance at this time";
//     return result;
//   }

//   // === 8. FORMAT TIMESTAMP FOR RESPONSE ===
//   char timestampBuf[20];
//   sprintf(timestampBuf, "%04d-%02d-%02d %02d:%02d:%02d",
//           currentTime.year(), currentTime.month(), currentTime.day(),
//           currentTime.hour(), currentTime.minute(), currentTime.second());
//   result.timestamp = String(timestampBuf);
//   result.formattedTime = formatTimeDisplay(secondsToTimeStr(currentTimeSec));

//   return result;
// }

// String secondsToTimeStr(int seconds) {
//   int h = (seconds / 3600) % 24;
//   int m = (seconds % 3600) / 60;
//   int s = seconds % 60;

//   char buf[9];
//   sprintf(buf, "%02d:%02d:%02d", h, m, s);
//   return String(buf);
// }

// String formatTimeDisplay(const String& timeStr) {
//   if (timeStr.length() == 0 || timeStr == "Invalid Date" || timeStr == "null") {
//     return "N/A";
//   }

//   int seconds = timeToSeconds(timeStr);
//   int h = seconds / 3600;
//   int m = (seconds % 3600) / 60;

//   String period = (h >= 12) ? "PM" : "AM";
//   int displayHour = (h % 12 == 0) ? 12 : h % 12;

//   char buf[9];
//   sprintf(buf, "%d:%02d %s", displayHour, m, period.c_str());
//   return String(buf);
// }

// String addMinutesToTimeStr(const String& timeStr, int minutes) {
//   int baseSec = timeToSeconds(timeStr);
//   int newSec = baseSec + (minutes * 60);

//   // Handle wrap around midnight
//   newSec = newSec % 86400;

//   int h = newSec / 3600;
//   int m = (newSec % 3600) / 60;

//   String period = (h >= 12) ? "PM" : "AM";
//   int displayHour = (h % 12 == 0) ? 12 : h % 12;

//   char buf[9];
//   sprintf(buf, "%d:%02d %s", displayHour, m, period.c_str());
//   return String(buf);
// }

// bool isTimeInRange(int timeSec, int startSec, int endSec) {
//   if (startSec <= endSec) {
//     return timeSec >= startSec && timeSec <= endSec;
//   } else {
//     return timeSec >= startSec || timeSec <= endSec;
//   }
// }

// void addRecord(const String& type, const DateTime& timestamp, int userId, const String& cardUuid, const String& userName) {
//   TodaysRecord record;
//   record.recordType = type;

//   record.timestamp = timestamp.hour() * 3600 + timestamp.minute() * 60 + timestamp.second();

//   char buf[9];
//   sprintf(buf, "%02d:%02d:%02d",
//           timestamp.hour(), timestamp.minute(), timestamp.second());
//   record.timestampStr = String(buf);

//   if (type == "in") {
//     todaysCheckIns.push_back(record);
//     Serial.printf("📝 Added check-in at %s\n", record.timestampStr.c_str());
//   } else {
//     todaysCheckOuts.push_back(record);
//     Serial.printf("📝 Added check-out at %s\n", record.timestampStr.c_str());
//   }

//   // Debug: Show current counts
//   Serial.printf("📊 Today's totals - IN: %d, OUT: %d\n",
//                 todaysCheckIns.size(), todaysCheckOuts.size());
// }

// void resetTodayRecords() {
//   todaysCheckIns.clear();
//   todaysCheckOuts.clear();
//   Serial.println("[LocalAttendance] Records reset for new day");
// }

// bool isNewDay(const DateTime& currentTime) {
//   int todayInt = currentTime.year() * 10000 + currentTime.month() * 100 + currentTime.day();

//   if (currentProcessingDay == -1) {
//     currentProcessingDay = todayInt;
//     return false;
//   }

//   if (currentProcessingDay != todayInt) {
//     currentProcessingDay = todayInt;
//     return true;
//   }

//   return false;
// }

// void checkDayChange(const DateTime& currentTime) {
//   if (isNewDay(currentTime)) {
//     // 🔥 Clean up old daily file
//     cleanupOldDailyFiles();
//     // Reset today's records
//     resetTodayRecords();
//   }
// }

// // ================== SAVE TO DAILY LOG FILE ==================
// bool saveAttendanceLogToSD(
//   const String& cardUuid,
//   int userId,
//   const String& userName,
//   const String& timestamp,
//   const String& recordType,
//   const String& status,
//   const String& message,
//   const String& dayOfWeek,
//   const String& checkInWindow,
//   const String& checkOutWindow) {

//   if (!sdMounted) {
//     Serial.println("❌ SD not mounted, cannot save log");
//     return false;
//   }

//   // Daily log file
//   char filename[32];
//   DateTime now = rtc.now();
//   sprintf(filename, "%s%04d%02d%02d.csv",
//           DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

//   // Check if file exists
//   bool fileExists = SD.exists(filename);
//   Serial.printf("File exists: %s\n", fileExists ? "YES" : "NO");

//   // Open file for writing
//   File file = SD.open(filename, FILE_WRITE);
//   if (!file) {
//     Serial.println("❌ Failed to open file!");
//     return false;
//   }

//   // If file exists, seek to end for appending
//   if (fileExists) {
//     if (!file.seek(file.size())) {
//       Serial.println("❌ Failed to seek to end!");
//       file.close();
//       return false;
//     }
//     Serial.println("✅ Seeking to end of file");
//   } else {
//     // New file: write header
//     file.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
//     Serial.println("✅ Created new file with header");
//   }

//   // Create record line
//   String line = timestamp + "," + cardUuid + "," + String(userId) + "," + userName + "," + recordType + "," + status + "," + message + "," + dayOfWeek + "," + checkInWindow + "," + checkOutWindow;

//   // Write record
//   file.println(line);

//   // Get file size after writing
//   size_t fileSize = file.size();

//   file.close();

//   Serial.printf("✅ Record saved! File size: %d bytes\n", fileSize);

//   // 🔥 NEW: Also save to monthly file
//   saveToMonthlyLogFile(line);

//   // Verify file was written
//   File verifyFile = SD.open(filename, FILE_READ);
//   if (verifyFile) {
//     int lineCount = 0;
//     while (verifyFile.available()) {
//       verifyFile.readStringUntil('\n');
//       lineCount++;
//     }
//     verifyFile.close();
//     Serial.printf("📊 Total lines in file: %d\n", lineCount);
//   }

//   return true;
// }

// // ================== DAY NAME HELPER ==================
// String getDayName(int dayOfWeek) {
//   switch (dayOfWeek) {
//     case 1: return "Monday";
//     case 2: return "Tuesday";
//     case 3: return "Wednesday";
//     case 4: return "Thursday";
//     case 5: return "Friday";
//     case 6: return "Saturday";
//     case 7: return "Sunday";
//     default: return "Unknown";
//   }
// }

// // ================== GET NEXT SCHEDULE ==================
// String getNextScheduleInfo(int userId, int currentDOW) {
//   String nextInfo = "No upcoming schedule";

//   // Next 7 days check karo
//   for (int i = 1; i <= 7; i++) {
//     int checkDay = currentDOW + i;
//     if (checkDay > 7) checkDay -= 7;

//     for (const auto& schedule : userSchedules) {
//       if (schedule.userId == userId && schedule.dayOfWeek == checkDay) {
//         String dayName = getDayName(checkDay);
//         String checkInTime = formatTimeDisplay(schedule.checkInFrom);
//         return dayName + " at " + checkInTime;
//       }
//     }
//   }
//   return "No upcoming schedule";
// }

// // ================== LOAD TODAY'S RECORDS FROM SD CARD ==================
// void loadTodayRecordsFromSD() {
//   // Pehle existing records clear karo
//   todaysCheckIns.clear();
//   todaysCheckOuts.clear();

//   // Aaj ki date ke hisaab se filename banayo
//   DateTime now = getLocalTime();
//   char filename[32];
//   sprintf(filename, "%s%04d%02d%02d.csv",
//           DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

//   Serial.print("📂 Checking for today's log file: ");
//   Serial.println(filename);

//   if (!SD.exists(filename)) {
//     Serial.println("📝 No log file for today yet");
//     return;
//   }

//   File file = SD.open(filename, FILE_READ);
//   if (!file) {
//     Serial.println("❌ Failed to open today's log file");
//     return;
//   }

//   int recordCount = 0;
//   int checkInCount = 0;
//   int checkOutCount = 0;

//   // Skip header line
//   String header = file.readStringUntil('\n');

//   // Read each line
//   while (file.available()) {
//     String line = file.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     // Parse CSV line
//     // Format: Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow
//     int commaIndex = 0;
//     int startPos = 0;
//     int fieldIndex = 0;

//     String timestamp, cardUuid, userIdStr, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

//     while (startPos < line.length()) {
//       commaIndex = line.indexOf(',', startPos);
//       if (commaIndex == -1) commaIndex = line.length();

//       String field = line.substring(startPos, commaIndex);

//       switch (fieldIndex) {
//         case 0: timestamp = field; break;
//         case 1: cardUuid = field; break;
//         case 2: userIdStr = field; break;
//         case 3: userName = field; break;
//         case 4: recordType = field; break;
//         case 5: status = field; break;
//         case 6: message = field; break;
//         case 7: dayOfWeek = field; break;
//         case 8: checkInWindow = field; break;
//         case 9: checkOutWindow = field; break;
//       }

//       startPos = commaIndex + 1;
//       fieldIndex++;
//     }

//     // Sirf aaj ke records add karo (jo already is file mein hain woh sab aaj ke hi hain)
//     if (recordType == "in" || recordType == "out") {
//       TodaysRecord record;
//       record.recordType = recordType;

//       // Timestamp se time nikaalo (format: YYYY-MM-DD HH:MM:SS)
//       int timeStart = timestamp.indexOf(' ') + 1;
//       String timeStr = timestamp.substring(timeStart);

//       int h = timeStr.substring(0, 2).toInt();
//       int m = timeStr.substring(3, 5).toInt();
//       int s = timeStr.substring(6, 8).toInt();
//       record.timestamp = h * 3600 + m * 60 + s;
//       record.timestampStr = timeStr;

//       if (recordType == "in") {
//         todaysCheckIns.push_back(record);
//         checkInCount++;
//       } else {
//         todaysCheckOuts.push_back(record);
//         checkOutCount++;
//       }

//       recordCount++;
//     }
//   }

//   file.close();

//   Serial.println("✅ Loaded records from SD card:");
//   Serial.printf("   Total records: %d\n", recordCount);
//   Serial.printf("   Check-ins: %d\n", checkInCount);
//   Serial.printf("   Check-outs: %d\n", checkOutCount);
// }

// // ================== SAVE TO MONTHLY LOG FILE ==================
// bool saveToMonthlyLogFile(const String& line) {
//   if (!sdMounted) return false;

//   // Monthly filename: /monthly_2026_02.csv
//   DateTime now = rtc.now();
//   char monthlyFilename[32];
//   sprintf(monthlyFilename, "%s%04d_%02d.csv",
//           MONTHLY_LOG_PREFIX, now.year(), now.month());

//   Serial.printf("📁 Appending to monthly file: %s\n", monthlyFilename);

//   // Check if monthly file exists
//   bool monthlyExists = SD.exists(monthlyFilename);

//   // Open monthly file for writing
//   File monthlyFile = SD.open(monthlyFilename, FILE_WRITE);
//   if (!monthlyFile) {
//     Serial.println("❌ Failed to open monthly file!");
//     return false;
//   }

//   // If file exists, seek to end
//   if (monthlyExists) {
//     if (!monthlyFile.seek(monthlyFile.size())) {
//       Serial.println("❌ Failed to seek in monthly file!");
//       monthlyFile.close();
//       return false;
//     }
//   } else {
//     // New monthly file: write header
//     monthlyFile.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
//     Serial.println("✅ Created new monthly file with header");
//   }

//   // Append the record
//   monthlyFile.println(line);
//   monthlyFile.close();

//   Serial.printf("✅ Appended to monthly file\n");
//   return true;
// }

// // ================== CLEANUP OLD DAILY FILES ==================
// void cleanupOldDailyFiles() {
//   if (!sdMounted) {
//     Serial.println("❌ SD not mounted, cannot cleanup");
//     return;
//   }

//   Serial.println("\n🧹 Running old daily files cleanup...");

//   DateTime localNow = getLocalTime();
//   int todayInt = localNow.year() * 10000UL + localNow.month() * 100 + localNow.day();
//   Serial.printf("Today (local) = %d  (%04d-%02d-%02d)\n",
//                 todayInt, localNow.year(), localNow.month(), localNow.day());

//   File root = SD.open("/");
//   if (!root) {
//     Serial.println("❌ Cannot open root directory");
//     return;
//   }

//   int deleted = 0, kept = 0, skipped = 0;

//   while (true) {
//     File entry = root.openNextFile();
//     if (!entry) break;
//     if (entry.isDirectory()) {
//       entry.close();
//       continue;
//     }

//     String fname = entry.name();
//     String name = fname;
//     if (name.startsWith("/")) name = name.substring(1);

//     if (!name.startsWith("log_") || !name.endsWith(".csv")) {
//       skipped++;
//       entry.close();
//       continue;
//     }

//     // Extract date from filename (last 8 chars before .csv)
//     String dateStr = name.substring(name.length() - 12, name.length() - 4);
//     if (dateStr.length() != 8 || !dateStr.toInt()) {
//       Serial.printf("  ⚠️ Invalid date format: %s\n", fname.c_str());
//       skipped++;
//       entry.close();
//       continue;
//     }

//     int fy = dateStr.substring(0, 4).toInt();
//     int fm = dateStr.substring(4, 6).toInt();
//     int fd = dateStr.substring(6, 8).toInt();
//     int fileDateInt = fy * 10000UL + fm * 100 + fd;

//     Serial.printf("  File: %-18s  date=%d  ", fname.c_str(), fileDateInt);

//     if (fileDateInt < todayInt) {
//       Serial.print("→ DELETE ... ");
//       entry.close();  // Must close before deletion

//       String fullPath = "/" + name;
//       if (SD.remove(fullPath)) {
//         Serial.println("SUCCESS");
//         deleted++;
//       } else {
//         Serial.println("FAILED");
//         // Try rename trick
//         String tempPath = "/temp_" + name;
//         if (SD.rename(fullPath, tempPath)) {
//           Serial.println("  → Rename succeeded, original file gone.");
//           // Original is gone – count as deleted even if temp remains
//           deleted++;

//           // Attempt to delete the temp file with retries
//           delay(100);
//           bool tempDeleted = false;
//           for (int i = 0; i < 3; i++) {
//             if (SD.remove(tempPath)) {
//               tempDeleted = true;
//               break;
//             }
//             delay(200);
//           }
//           if (tempDeleted) {
//             Serial.println("  → Temp file also deleted.");
//           } else {
//             Serial.println("  → Temp file could not be deleted (ignored).");
//           }
//         } else {
//           Serial.println("  → Rename also failed.");
//         }
//       }
//     } else {
//       Serial.println("→ KEEP");
//       kept++;
//       entry.close();
//     }
//   }

//   root.close();
//   Serial.printf("\nSummary:\n  Deleted: %d\n  Kept: %d\n  Skipped: %d\n", deleted, kept, skipped);
//   Serial.println("🧹 Cleanup done.\n");
// }

// // ================== SYNC MONTHLY RECORDS TO SERVER ==================
// bool syncMonthlyRecordsToServer() {
//   if (!sdMounted || !autoSyncEnabled || WiFi.status() != WL_CONNECTED) {
//     return false;
//   }

//   if (isSyncing) {
//     Serial.println("⚠️ Sync already in progress...");
//     return false;
//   }

//   isSyncing = true;
//   Serial.println("\n🔄 Starting auto sync with server...");

//   // Get current month
//   DateTime now = rtc.now();
//   char monthlyFilename[32];
//   sprintf(monthlyFilename, "%s%04d_%02d.csv",
//           MONTHLY_LOG_PREFIX, now.year(), now.month());

//   if (!SD.exists(monthlyFilename)) {
//     Serial.println("📁 No monthly file to sync");
//     isSyncing = false;
//     return false;
//   }

//   File file = SD.open(monthlyFilename, FILE_READ);
//   if (!file) {
//     Serial.println("❌ Failed to open monthly file");
//     isSyncing = false;
//     return false;
//   }

//   // Skip header
//   String header = file.readStringUntil('\n');

//   int totalRecords = 0;
//   int syncedRecords = 0;
//   int failedRecords = 0;

//   // Temporary file for unsynced records
//   char tempFilename[32];
//   sprintf(tempFilename, "%s%04d_%02d_temp.csv",
//           MONTHLY_LOG_PREFIX, now.year(), now.month());

//   File tempFile = SD.open(tempFilename, FILE_WRITE);
//   if (!tempFile) {
//     Serial.println("❌ Failed to create temp file");
//     file.close();
//     isSyncing = false;
//     return false;
//   }

//   // Write header to temp file
//   tempFile.println(header);

//   // Process each record
//   while (file.available()) {
//     String line = file.readStringUntil('\n');
//     line.trim();
//     if (line.length() == 0) continue;

//     totalRecords++;

//     // Parse CSV line
//     int commaIndex = 0;
//     int startPos = 0;
//     int fieldIndex = 0;

//     String timestamp, cardUuid, userId, userName, recordType, status, message, dayOfWeek, checkInWindow, checkOutWindow;

//     while (startPos < line.length()) {
//       commaIndex = line.indexOf(',', startPos);
//       if (commaIndex == -1) commaIndex = line.length();

//       String field = line.substring(startPos, commaIndex);

//       switch (fieldIndex) {
//         case 0: timestamp = field; break;
//         case 1: cardUuid = field; break;
//         case 2: userId = field; break;
//         case 3: userName = field; break;
//         case 4: recordType = field; break;
//         case 5: status = field; break;
//         case 6: message = field; break;
//         case 7: dayOfWeek = field; break;
//         case 8: checkInWindow = field; break;
//         case 9: checkOutWindow = field; break;
//       }

//       startPos = commaIndex + 1;
//       fieldIndex++;
//     }

//     // Send to server
//     bool sent = sendAttendanceRecordToServer(
//       cardUuid,
//       userId.toInt(),
//       userName,
//       timestamp,
//       recordType,
//       status,
//       message,
//       dayOfWeek,
//       checkInWindow,
//       checkOutWindow);

//     if (sent) {
//       syncedRecords++;
//       Serial.printf("✅ Synced: %s - %s\n", timestamp.c_str(), cardUuid.c_str());
//       // Don't write to temp file (delete from monthly)
//     } else {
//       failedRecords++;
//       Serial.printf("❌ Failed: %s - %s\n", timestamp.c_str(), cardUuid.c_str());
//       // Keep in temp file (retry later)
//       tempFile.println(line);
//     }

//     delay(100);  // Small delay to avoid overwhelming server
//   }

//   file.close();
//   tempFile.close();

//   // Replace original with temp file (only failed records remain)
//   SD.remove(monthlyFilename);
//   if (failedRecords > 0) {
//     SD.rename(tempFilename, monthlyFilename);
//     Serial.printf("📁 %d failed records kept for retry\n", failedRecords);
//   } else {
//     SD.remove(tempFilename);
//     Serial.println("📁 All records synced, monthly file deleted");
//   }

//   Serial.printf("🔄 Sync complete: %d/%d synced, %d failed\n",
//                 syncedRecords, totalRecords, failedRecords);

//   isSyncing = false;
//   return syncedRecords > 0;
// }

// // ================== SEND SINGLE ATTENDANCE RECORD TO SERVER ==================
// bool sendAttendanceRecordToServer(
//   const String& cardUuid,
//   int userId,
//   const String& userName,
//   const String& timestamp,
//   const String& recordType,
//   const String& status,
//   const String& message,
//   const String& dayOfWeek,
//   const String& checkInWindow,
//   const String& checkOutWindow) {

//   if (WiFi.status() != WL_CONNECTED) return false;

//   HTTPClient http;
//   WiFiClient client;

//   String url = String(SERVER_URL) + "/api/v1/device/attendance/sync-record";
//   http.begin(client, url);

//   http.addHeader("Content-Type", "application/json");
//   http.addHeader("x-device-id", DEVICE_UUID);
//   http.addHeader("x-device-secret", DEVICE_SECRET);

//   // EXACT payload as per your server requirement
//   DynamicJsonDocument doc(512);
//   doc["cardUuid"] = cardUuid;
//   doc["deviceUuid"] = DEVICE_UUID;
//   doc["userId"] = userId;
//   doc["userName"] = userName;
//   doc["timestamp"] = timestamp;
//   doc["recordType"] = recordType;
//   doc["status"] = status;
//   doc["message"] = message;
//   doc["dayOfWeek"] = dayOfWeek;
//   doc["checkInWindow"] = checkInWindow;
//   doc["checkOutWindow"] = checkOutWindow;

//   String payload;
//   serializeJson(doc, payload);

//   int code = http.POST(payload);

//   if (code == 200) {
//     http.end();
//     return true;
//   } else {
//     Serial.printf("Server returned: %d\n", code);
//     http.end();
//     return false;
//   }
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