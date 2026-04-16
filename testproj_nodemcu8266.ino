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
#define EEPROM_SERVER_URL_ADDR 200
#define EEPROM_SERVER_URL_LEN 100

#define SD_OPERATION_TIMEOUT 10000

#define EEPROM_GRACE_EARLY_IN_ADDR 450
#define EEPROM_GRACE_LATE_IN_ADDR 454
#define EEPROM_GRACE_EARLY_OUT_ADDR 458
#define EEPROM_GRACE_LATE_OUT_ADDR 462
#define EEPROM_MIN_WORK_ADDR 466

// ================== SD CARD CONFIG ==================
#define SD_CS_PIN D8
#define SCHEDULES_FOLDER "/schedules"
#define ATTENDANCE_FOLDER "/attendance/"

// ================== LED PINS on PCF8574 ==================
#define WHITE_LED_PIN 0
#define BLUE_LED_PIN 1
#define GREEN_LED_PIN 2
#define RED_LED_PIN 3
#define YELLOW_LED_PIN 4
#define BUZZER_PIN 5

// ================== RFID PINS ==================
#define RST_PIN D3
#define SS_PIN D4

// ================== TESTING CONFIG ==================
#define USE_NTP_TIME true

// ================== MANUAL TIME FOR TESTING ==================
int manualYear = 2026;
int manualMonth = 3;
int manualDay = 28;
int manualHour = 2;
int manualMinute = 10;
int manualSecond = 0;

// ================== ATTENDANCE LOGIC HEADER ==================
#ifndef ATTENDANCE_LOGIC_H
#define ATTENDANCE_LOGIC_H

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
#define FAIL_LIMIT 5
#define DEFAULT_SERVER_URL "http://brayden-nonprovident-sizeably.ngrok-free.dev"
String SERVER_URL = DEFAULT_SERVER_URL;

const char* DEVICE_UUID = "9908bd1f-9571-4b49-baa4-36e4f27eab36";
const char* DEVICE_SECRET = "ae756ac92c3e4d44361110b3ca4e7d9f";

// ================== TIMEZONE CONFIG ==================
String deviceTimezone = "UTC";
int timezoneOffsetMinutes = 0;

// ================== GLOBAL VARIABLES ==================
unsigned long lastHeartbeatTime = 0;
unsigned long lastLedToggle = 0;
unsigned long lastSDOperationTime = 0;
unsigned long lastYellowBlink = 0;

int heartbeatFailCount = 0;
int wifiFailCount = 0;

bool yellowState = false;
bool reprovisionMode = false;
bool serverUnreachable = false;
bool ledState = false;
bool isConnecting = false;
bool sdMounted = false;
bool localStorage = true;
bool attendanceFolderReady = false;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttReconnectAttempt = 0;
bool mqttConnected = false;

String savedSSID = "";
String savedPassword = "";
#define EEPROM_SSID_ADDR 300
#define EEPROM_PASS_ADDR 350

unsigned long lastRFIDReinit = 0;
#define RFID_REINIT_INTERVAL 3600000

// ================== LOCAL ATTENDANCE LOGIC VARIABLES ==================
int GRACE_EARLY_IN = 15;
int GRACE_LATE_IN = 15;
int GRACE_EARLY_OUT = 0;
int GRACE_LATE_OUT = 15;
int MIN_WORK_DURATION = 0;
const int MIN_GAP_BETWEEN_RECORDS = 10;

bool autoSyncEnabled = true;
unsigned long lastSyncTime = 0;
const unsigned long SYNC_INTERVAL = 300000;
bool isSyncing = false;

std::vector<TodaysRecord> todaysCheckIns;
std::vector<TodaysRecord> todaysCheckOuts;
int currentProcessingDay = -1;

// ================== DAILY LOG FILE ==================
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

// ================== GLOBAL VECTORS ==================
std::vector<UserSchedule> userSchedules;

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
bool loadUserScheduleFromSD(const String& cardUuid, int dayOfWeek, UserSchedule& foundSchedule);
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
String getNextScheduleInfo(int userId, const String& cardUuid, int currentDOW);
AttendanceResult processLocalAttendance(const String& cardUuid, int userId, const String& userName, const UserSchedule& todaySchedule, const DateTime& currentTime);
bool saveAttendanceLogToSD(const String& cardUuid, int userId, const String& userName, const String& timestamp, const String& recordType, const String& status, const String& message, const String& dayOfWeek, const String& checkInWindow, const String& checkOutWindow);
void blinkWhiteLED();
void blinkGreenOnce();
void blinkRedTwice();

// ================== SERVER URL FUNCTIONS ==================
void loadServerUrlFromEEPROM() {
  char url[EEPROM_SERVER_URL_LEN + 1] = { 0 };
  for (int i = 0; i < EEPROM_SERVER_URL_LEN; i++) {
    char c = EEPROM.read(EEPROM_SERVER_URL_ADDR + i);
    if (c == '\0' || c == 0xFF) break;
    url[i] = c;
  }
  url[EEPROM_SERVER_URL_LEN] = '\0';

  String loadedUrl = String(url);
  if (loadedUrl.length() > 0 && loadedUrl.startsWith("http")) {
    SERVER_URL = loadedUrl;
    Serial.println("📡 Server URL loaded from EEPROM: " + SERVER_URL);
  } else {
    SERVER_URL = DEFAULT_SERVER_URL;
    Serial.println("📡 Using default server URL: " + SERVER_URL);
  }
}

void saveServerUrlToEEPROM(String url) {
  // Pehle purani URL clear karo
  for (int i = 0; i < EEPROM_SERVER_URL_LEN; i++) {
    EEPROM.write(EEPROM_SERVER_URL_ADDR + i, 0);
  }

  // Naya URL save karo
  int len = url.length();
  if (len > EEPROM_SERVER_URL_LEN) len = EEPROM_SERVER_URL_LEN;

  for (int i = 0; i < len; i++) {
    EEPROM.write(EEPROM_SERVER_URL_ADDR + i, url[i]);
  }
  EEPROM.write(EEPROM_SERVER_URL_ADDR + len, '\0');
  EEPROM.commit();

  Serial.println("💾 Server URL saved to EEPROM: " + url);
}

// ================== Attendace setting FUNCTIONS ==================
void loadAttendanceSettingsFromEEPROM() {
  int val;

  EEPROM.get(EEPROM_GRACE_EARLY_IN_ADDR, val);
  if (val >= 0 && val <= 120) GRACE_EARLY_IN = val;

  EEPROM.get(EEPROM_GRACE_LATE_IN_ADDR, val);
  if (val >= 0 && val <= 120) GRACE_LATE_IN = val;

  EEPROM.get(EEPROM_GRACE_EARLY_OUT_ADDR, val);
  if (val >= 0 && val <= 120) GRACE_EARLY_OUT = val;

  EEPROM.get(EEPROM_GRACE_LATE_OUT_ADDR, val);
  if (val >= 0 && val <= 120) GRACE_LATE_OUT = val;

  EEPROM.get(EEPROM_MIN_WORK_ADDR, val);
  if (val >= 0 && val <= 480) MIN_WORK_DURATION = val;

  Serial.printf("⚙️ Att Settings: EarlyIn=%d LateIn=%d EarlyOut=%d LateOut=%d MinWork=%d\n",
                GRACE_EARLY_IN, GRACE_LATE_IN, GRACE_EARLY_OUT, GRACE_LATE_OUT, MIN_WORK_DURATION);
}

void saveAttendanceSettingsToEEPROM() {
  EEPROM.put(EEPROM_GRACE_EARLY_IN_ADDR, GRACE_EARLY_IN);
  EEPROM.put(EEPROM_GRACE_LATE_IN_ADDR, GRACE_LATE_IN);
  EEPROM.put(EEPROM_GRACE_EARLY_OUT_ADDR, GRACE_EARLY_OUT);
  EEPROM.put(EEPROM_GRACE_LATE_OUT_ADDR, GRACE_LATE_OUT);
  EEPROM.put(EEPROM_MIN_WORK_ADDR, MIN_WORK_DURATION);
  EEPROM.commit();
  Serial.println("💾 Attendance settings saved to EEPROM");
}

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

// ================== MQTT CONNECTION ==================
bool connectToMqtt() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (mqttClient.connected()) return true;

  Serial.print("Connecting to MQTT...");
  String clientId = "ESP8266-" + String(DEVICE_UUID);

  if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("connected");
    mqttClient.subscribe(MQTT_TOPIC_COMMAND);
    publishMqttStatus("online", "Device connected to MQTT");
    mqttConnected = true;
    return true;
  }

  Serial.println("failed, rc=" + String(mqttClient.state()));
  mqttConnected = false;
  return false;
}

// ================== PUBLISH MQTT STATUS ==================
void publishMqttStatus(String status, String message) {
  DynamicJsonDocument doc(256);
  doc["deviceUuid"] = DEVICE_UUID;
  doc["status"] = status;
  doc["message"] = message;
  doc["timestamp"] = getLocalTime().unixtime();
  doc["ip"] = WiFi.localIP().toString();
  if (WiFi.status() == WL_CONNECTED) {
    doc["ssid"] = WiFi.SSID();
  } else {
    doc["ssid"] = "";  // Empty string bhejo
  }
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

  doc["timezone"] = deviceTimezone;
  doc["timezoneOffset"] = timezoneOffsetMinutes;

  if (WiFi.status() == WL_CONNECTED) {

    int rssi = WiFi.RSSI();
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = rssi;

    String quality;
    int speed = 0;

    if (rssi >= -50) {
      quality = "Excellent";
      speed = 72;
    } else if (rssi >= -60) {
      quality = "Good";
      speed = 54;
    } else if (rssi >= -70) {
      quality = "Fair";
      speed = 24;
    } else if (rssi >= -80) {
      quality = "Weak";
      speed = 11;
    } else {
      quality = "Poor";
      speed = 1;
    }

    doc["signal_quality"] = quality;
    doc["wifi_speed_mbps"] = speed;

  } else {
    doc["ssid"] = WiFi.SSID();
    doc["rssi"] = -100;
    doc["signal_quality"] = "Disconnected";
    doc["wifi_speed_mbps"] = 0;
  }

  doc["schedulesLoaded"] = userSchedules.size();
  doc["autoSyncEnabled"] = autoSyncEnabled;
  doc["timestamp"] = getLocalTime().unixtime();

  doc["cpuFreqMHz"] = ESP.getCpuFreqMHz();
  doc["freeHeapBytes"] = ESP.getFreeHeap();
  doc["heapFragmentation"] = ESP.getHeapFragmentation();

  JsonObject sd = doc.createNestedObject("sd");
  sd["enabled"] = localStorage;
  sd["mounted"] = sdMounted;

  String payload;
  serializeJson(doc, payload);
  bool published = mqttClient.publish(MQTT_TOPIC_HEARTBEAT, payload.c_str());

  if (ESP.getFreeHeap() < 5000) {
    Serial.println("⚠️ Low heap — restarting!");
    SD.end();
    delay(500);
    ESP.restart();
  }

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

  DynamicJsonDocument doc(2048);
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

  else if (command == "set_server_url") {
    String commandId = doc["commandId"] | "";
    String newUrl = doc["SERVER_URL"] | "";

    if (newUrl.length() == 0) {
      publishMqttResponse(false, "serverUrl is required", commandId);
      return;
    }

    // Validate URL (basic check)
    if (!newUrl.startsWith("http")) {
      publishMqttResponse(false, "Invalid URL format. Must start with http:// or https://", commandId);
      return;
    }

    // Save to EEPROM
    saveServerUrlToEEPROM(newUrl);
    SERVER_URL = newUrl;

    // Success response
    DynamicJsonDocument data(128);
    data["success"] = true;
    data["message"] = "Server URL updated successfully";
    data["SERVER_URL"] = SERVER_URL;
    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

    Serial.println("🌐 Server URL changed to: " + SERVER_URL);
    SD.end();
    delay(1000);
    ESP.restart();
  }

  else if (command == "get_server_url") {
    String commandId = doc["commandId"] | "";

    DynamicJsonDocument data(128);
    data["success"] = true;
    data["SERVER_URL"] = SERVER_URL;
    data["defaultUrl"] = DEFAULT_SERVER_URL;
    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
  }

  else if (command == "scan_wifi") {
    String commandId = doc["commandId"] | "";

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
      publishMqttResponse(false, "SSID missing", commandId);
      return;
    }

    publishMqttResponse(true, "WiFi credentials received. Connecting...", commandId);

    delay(500);

    bool connected = connectToWifi(ssid, password);

    if (connected) {

      if (!mqttClient.connected()) {
        Serial.println("⚠️ MQTT disconnected, reconnecting...");
        connectToMqtt();
        delay(500);
      }

      DynamicJsonDocument responseData(256);
      responseData["success"] = true;
      responseData["message"] = "Successfully connected to WiFi";
      responseData["ssid"] = ssid;
      responseData["ip"] = WiFi.localIP().toString();
      responseData["mac"] = WiFi.macAddress();
      responseData["timestamp"] = getLocalTime().unixtime();

      if (commandId.length() > 0) {
        responseData["commandId"] = commandId;
      }

      String payload;
      serializeJson(responseData, payload);

      bool published = false;
      for (int retry = 0; retry < 3; retry++) {
        if (mqttClient.connected()) {
          published = mqttClient.publish(MQTT_TOPIC_RESPONSE, payload.c_str());
          if (published) {
            Serial.println("✅ Second response published successfully");
            break;
          }
        }
        Serial.println("⚠️ MQTT publish failed, retrying in 500ms...");
        delay(500);
        if (!mqttClient.connected()) {
          connectToMqtt();
        }
      }

      if (!published) {
        Serial.println("❌ Failed to publish second response after 3 retries");
      }

      publishMqttStatus("wifi_connected", "Connected to " + ssid);
    } else {
      int addr = 400;         // EEPROM address for failed command
      EEPROM.write(addr, 1);  // Flag that failure exists
      for (int i = 0; i < commandId.length() && i < 20; i++) {
        EEPROM.write(addr + 1 + i, commandId[i]);
      }
      EEPROM.write(addr + 21, 0);  // Null terminator
      EEPROM.commit();
      SD.end();
      delay(500);
      ESP.restart();
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

      DynamicJsonDocument data(128);
      data["success"] = true;
      data["message"] = "Local storage toggled";
      data["localStorage"] = localStorage;

      if (commandId.length() > 0) {
        data["commandId"] = commandId;
      }

      String dataPayload;
      serializeJson(data, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

      SD.end();
      delay(1000);
      ESP.restart();
    } else {
      DynamicJsonDocument data(128);
      data["success"] = true;
      data["message"] = "Already set";
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

    DynamicJsonDocument data(128);
    data["success"] = true;
    data["message"] = "Timezone updated successfully";
    data["timezone"] = deviceTimezone;
    data["offset"] = timezoneOffsetMinutes;
    data["offsetStr"] = offsetStr;

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
    DeserializationError error = deserializeJson(respDoc, response);

    // SINGLE RESPONSE
    DynamicJsonDocument finalResponse(2048);

    if (!error) {
      finalResponse["success"] = respDoc["success"];
      finalResponse["message"] = respDoc["message"].as<String>();

      // Add data if present
      if (respDoc.containsKey("data")) {
        finalResponse["data"] = respDoc["data"];
        if (respDoc["data"].is<JsonArray>()) {
          JsonArray dataArray = respDoc["data"].as<JsonArray>();
          finalResponse["recordCount"] = dataArray.size();
        }
      }
    } else {
      finalResponse["success"] = false;
      finalResponse["message"] = "Failed to parse records";
    }

    // Add commandId
    if (commandId.length() > 0) {
      finalResponse["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(finalResponse, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

    Serial.printf("📤 Sent attendance records response for command: %s\n",
                  commandId.c_str());
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
      DynamicJsonDocument data(256);
      data["success"] = true;
      data["message"] = respDoc["message"];
      data["deletedCount"] = respDoc["deletedCount"];
      data["totalCount"] = respDoc["totalCount"];
      data["cardUuid"] = cardUuid;
      if (commandId.length() > 0) {
        data["commandId"] = commandId;
      }
      String dataPayload;
      serializeJson(data, dataPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
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
      DateTime now = getLocalTime();
      year = now.year();
      month = now.month();
    }

    Serial.printf("DEBUG get_monthly_records: year=%d month=%d cardUuid=%s\n",
                  year, month, cardUuid.c_str());

    String response;
    if (cardUuid.length() > 0) {
      Serial.println("DEBUG: calling getUserMonthlyRecords");
      response = getUserMonthlyRecords(cardUuid, year, month);
    } else {
      Serial.println("DEBUG: calling getMonthlyAttendanceRecords");
      response = getMonthlyAttendanceRecords(year, month);
    }

    Serial.println("DEBUG raw response: " + response);

    DynamicJsonDocument respDoc(2048);
    DeserializationError error = deserializeJson(respDoc, response);

    DynamicJsonDocument finalResponse(2048);

    if (!error && respDoc["success"]) {
      finalResponse["success"] = true;
      finalResponse["message"] = respDoc["message"].as<String>();
      finalResponse["year"] = year;
      finalResponse["month"] = month;

      if (respDoc.containsKey("data")) {
        finalResponse["data"] = respDoc["data"];
        if (respDoc["data"].is<JsonArray>()) {
          JsonArray dataArray = respDoc["data"].as<JsonArray>();
          finalResponse["recordCount"] = dataArray.size();
        }
      }
    } else {
      finalResponse["success"] = false;
      finalResponse["message"] = respDoc["message"] | "Failed to fetch monthly records";
      finalResponse["year"] = year;
      finalResponse["month"] = month;
    }

    if (commandId.length() > 0) {
      finalResponse["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(finalResponse, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

    Serial.printf("📤 Sent monthly records response for %d/%d with %d records\n",
                  year, month, finalResponse["recordCount"] | 0);
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
    DeserializationError error = deserializeJson(respDoc, response);

    // SINGLE RESPONSE
    DynamicJsonDocument finalResponse(4096);

    if (!error && respDoc["success"]) {
      finalResponse["success"] = true;
      finalResponse["message"] = respDoc["message"].as<String>();

      // Add data if present
      if (respDoc.containsKey("data")) {
        finalResponse["data"] = respDoc["data"];

        // Extract values first, then add
        JsonObject data = respDoc["data"];

        int dailyCount = 0;
        int monthlyCount = 0;

        if (data.containsKey("dailyFiles")) {
          JsonArray dailyFiles = data["dailyFiles"];
          dailyCount = dailyFiles.size();
          finalResponse["dailyCount"] = dailyCount;
        }

        if (data.containsKey("monthlyFiles")) {
          JsonArray monthlyFiles = data["monthlyFiles"];
          monthlyCount = monthlyFiles.size();
          finalResponse["monthlyCount"] = monthlyCount;
        }

        // Add total after extracting values
        finalResponse["totalFiles"] = dailyCount + monthlyCount;
      }
    } else {
      finalResponse["success"] = false;
      finalResponse["message"] = respDoc["message"] | "Failed to list files";
    }

    // Add commandId
    if (commandId.length() > 0) {
      finalResponse["commandId"] = commandId;
    }

    String dataPayload;
    serializeJson(finalResponse, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());

    Serial.printf("📤 Sent file list response with %d files\n",
                  finalResponse["totalFiles"] | 0);
  }

  else if (command == "toggle_auto_sync") {
    String commandId = doc["commandId"] | "";
    bool newValue = doc["enabled"] | autoSyncEnabled;

    if (autoSyncEnabled != newValue) {
      autoSyncEnabled = newValue;
      EEPROM.write(EEPROM_AUTO_SYNC_ADDR, autoSyncEnabled ? 1 : 0);
      EEPROM.commit();
    }

    DynamicJsonDocument data(128);
    data["success"] = true;
    data["message"] = "Auto sync toggled";
    data["autoSync"] = autoSyncEnabled;
    data["enabled"] = autoSyncEnabled;

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

    DynamicJsonDocument data(128);
    data["command"] = "manual_sync";

    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    if (result) {
      data["success"] = true;
      data["message"] = "Sync completed successfully";
      data["year"] = year;
      data["month"] = month;
    } else {
      data["success"] = false;
      data["message"] = "No records to sync or sync failed";
      data["year"] = year;
      data["month"] = month;
    }

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
  }

  else if (command == "sync_user_schedule") {
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

    if (localStorage && sdMounted) {
      Serial.println("💾 Saving updated schedules to SD card...");
      saveScheduleToSD();
      Serial.println("✅ Schedules saved to SD");
    } else {
      Serial.println("⚠️ SD not mounted or localStorage disabled, not saving to SD");
    }

    // FIRST RESPONSE - with commandId (critical for tracking)
    DynamicJsonDocument response(512);
    response["success"] = true;
    response["message"] = "Bulk sync completed";
    response["timestamp"] = getLocalTime().unixtime();
    response["added"] = addedCount;
    response["updated"] = updatedCount;
    response["totalUsers"] = userSchedules.size();

    // commandId must be included in response
    if (commandId.length() > 0) {
      response["commandId"] = commandId;
    }

    String responsePayload;
    serializeJson(response, responsePayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, responsePayload.c_str());
    Serial.printf("[ScheduleSync] Sent response with commandId: %s\n",
                  commandId.length() > 0 ? commandId.c_str() : "none");
  }

  else if (command == "get_device_schedules") {
    String commandId = doc["commandId"] | "";
    String targetCardUuid = doc["cardUuid"] | "";

    DynamicJsonDocument data(8192);
    data["success"] = true;
    data["message"] = "Schedules fetched";
    JsonArray users = data.createNestedArray("users");
    int totalCount = 0;

    if (!sdMounted) {
      data["success"] = false;
      data["message"] = "SD not mounted";
    } else {
      // SD se schedules folder open karo
      if (targetCardUuid.length() > 0) {
        // Sirf ek user ki file
        String filePath = String(SCHEDULES_FOLDER) + "/" + targetCardUuid + ".csv";
        if (SD.exists(filePath.c_str())) {
          File file = SD.open(filePath.c_str(), FILE_READ);
          if (file) {
            file.readStringUntil('\n');  // header skip
            while (file.available()) {
              String line = file.readStringUntil('\n');
              line.trim();
              if (line.length() == 0) continue;
              int p1 = line.indexOf(',');
              int p2 = line.indexOf(',', p1 + 1);
              int p3 = line.indexOf(',', p2 + 1);
              int p4 = line.indexOf(',', p3 + 1);
              int p5 = line.indexOf(',', p4 + 1);
              int p6 = line.indexOf(',', p5 + 1);
              if (p6 == -1) continue;
              JsonObject user = users.createNestedObject();
              user["id"] = line.substring(0, p1).toInt();
              user["cardUuid"] = targetCardUuid;
              user["name"] = line.substring(p1 + 1, p2);
              user["dayOfWeek"] = line.substring(p2 + 1, p3).toInt();
              user["checkInFrom"] = line.substring(p3 + 1, p4);
              user["checkInTo"] = line.substring(p4 + 1, p5);
              user["checkOutFrom"] = line.substring(p5 + 1, p6);
              user["checkOutTo"] = line.substring(p6 + 1);
              totalCount++;
            }
            file.close();
          }
        }
      } else {
        // Saari files /schedules/ folder se
        File dir = SD.open(SCHEDULES_FOLDER);
        if (dir) {
          while (true) {
            File entry = dir.openNextFile();
            if (!entry) break;
            if (entry.isDirectory()) {
              entry.close();
              continue;
            }
            String fullName = String(entry.name());
            int lastSlash = fullName.lastIndexOf('/');
            String fileName = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;
            if (!fileName.endsWith(".csv")) {
              entry.close();
              continue;
            }
            String uuid = fileName.substring(0, fileName.length() - 4);
            String filePath = String(SCHEDULES_FOLDER) + "/" + fileName;
            entry.close();
            File file = SD.open(filePath.c_str(), FILE_READ);
            if (!file) continue;
            file.readStringUntil('\n');
            while (file.available()) {
              String line = file.readStringUntil('\n');
              line.trim();
              if (line.length() == 0) continue;
              int p1 = line.indexOf(',');
              int p2 = line.indexOf(',', p1 + 1);
              int p3 = line.indexOf(',', p2 + 1);
              int p4 = line.indexOf(',', p3 + 1);
              int p5 = line.indexOf(',', p4 + 1);
              int p6 = line.indexOf(',', p5 + 1);
              if (p6 == -1) continue;
              JsonObject user = users.createNestedObject();
              user["id"] = line.substring(0, p1).toInt();
              user["cardUuid"] = uuid;
              user["name"] = line.substring(p1 + 1, p2);
              user["dayOfWeek"] = line.substring(p2 + 1, p3).toInt();
              user["checkInFrom"] = line.substring(p3 + 1, p4);
              user["checkInTo"] = line.substring(p4 + 1, p5);
              user["checkOutFrom"] = line.substring(p5 + 1, p6);
              user["checkOutTo"] = line.substring(p6 + 1);
              totalCount++;
            }
            file.close();
          }
          dir.close();
        }
      }
    }

    data["totalUsers"] = totalCount;
    data["filteredByCardUuid"] = (targetCardUuid.length() > 0) ? targetCardUuid : "all";
    if (commandId.length() > 0) data["commandId"] = commandId;

    String dataPayload;
    serializeJson(data, dataPayload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, dataPayload.c_str());
    Serial.printf("Sent %d schedules from SD (commandId: %s)\n", totalCount, commandId.c_str());
  }

  else if (command == "set_attendance_settings") {
    String commandId = doc["commandId"] | "";

    int newEarlyIn = doc["graceEarlyIn"] | GRACE_EARLY_IN;
    int newLateIn = doc["graceLateIn"] | GRACE_LATE_IN;
    int newEarlyOut = doc["graceEarlyOut"] | GRACE_EARLY_OUT;
    int newLateOut = doc["graceLateOut"] | GRACE_LATE_OUT;
    int newMinWork = doc["minWorkDuration"] | MIN_WORK_DURATION;

    // Validate ranges
    if (newEarlyIn < 0 || newEarlyIn > 120 || newLateIn < 0 || newLateIn > 120 || newEarlyOut < 0 || newEarlyOut > 120 || newLateOut < 0 || newLateOut > 120 || newMinWork < 0 || newMinWork > 480) {
      publishMqttResponse(false, "Invalid settings values", commandId);
      return;
    }

    GRACE_EARLY_IN = newEarlyIn;
    GRACE_LATE_IN = newLateIn;
    GRACE_EARLY_OUT = newEarlyOut;
    GRACE_LATE_OUT = newLateOut;
    MIN_WORK_DURATION = newMinWork;

    saveAttendanceSettingsToEEPROM();

    DynamicJsonDocument data(256);
    data["success"] = true;
    data["message"] = "Attendance settings updated";
    data["graceEarlyIn"] = GRACE_EARLY_IN;
    data["graceLateIn"] = GRACE_LATE_IN;
    data["graceEarlyOut"] = GRACE_EARLY_OUT;
    data["graceLateOut"] = GRACE_LATE_OUT;
    data["minWorkDuration"] = MIN_WORK_DURATION;
    if (commandId.length() > 0) data["commandId"] = commandId;

    String payload;
    serializeJson(data, payload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, payload.c_str());

    Serial.println("⚙️ Attendance settings updated via MQTT");
  }

  else if (command == "get_attendance_settings") {
    String commandId = doc["commandId"] | "";

    DynamicJsonDocument data(256);
    data["success"] = true;
    data["message"] = "Attendance settings fetched";
    data["graceEarlyIn"] = GRACE_EARLY_IN;
    data["graceLateIn"] = GRACE_LATE_IN;
    data["graceEarlyOut"] = GRACE_EARLY_OUT;
    data["graceLateOut"] = GRACE_LATE_OUT;
    data["minWorkDuration"] = MIN_WORK_DURATION;
    if (commandId.length() > 0) data["commandId"] = commandId;

    String payload;
    serializeJson(data, payload);
    mqttClient.publish(MQTT_TOPIC_RESPONSE, payload.c_str());

    Serial.println("⚙️ Attendance settings sent via MQTT");
  }

  else if (command == "restart") {
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

    SD.end();
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

    saveWifiCredentials(ssid, password);
    savedSSID = ssid;
    savedPassword = password;
    Serial.println("💾 New WiFi credentials saved: " + ssid);

    blinkGreenOnce();

    if (!mqttClient.connected()) {
      connectToMqtt();
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
    wifiManager.startConfigPortal("RFID_Device_001", "12345678");
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

  client.setTimeout(5000);
  http.setTimeout(5000);

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
  Serial.println("🔍 handleRFID called!");
  String cardUuid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    cardUuid += String(rfid.uid.uidByte[i], HEX);
  }
  cardUuid.toUpperCase();
  Serial.println("Card scanned: " + cardUuid);

  if (localStorage) {
    DateTime currentTime = getLocalTime();
    int currentDOW = currentTime.dayOfTheWeek();
    currentDOW = (currentDOW == 0) ? 7 : currentDOW;

    UserSchedule todayScheduleObj;
    if (!loadUserScheduleFromSD(cardUuid, currentDOW, todayScheduleObj)) {
      String filePath = String(SCHEDULES_FOLDER) + "/" + cardUuid + ".csv";
      if (!SD.exists(filePath.c_str())) {
        Serial.println("❌ Card not found: " + cardUuid);
        DynamicJsonDocument scanDoc(256);
        scanDoc["event"] = "card_scan";
        scanDoc["success"] = false;
        scanDoc["cardUuid"] = cardUuid;
        scanDoc["reason"] = "card_not_registered";
        scanDoc["message"] = "Card not registered in system";
        scanDoc["timestamp"] = getLocalTime().unixtime();
        scanDoc["commandId"] = "scan_12345";
        String scanPayload;
        serializeJson(scanDoc, scanPayload);
        mqttClient.publish(MQTT_TOPIC_RESPONSE, scanPayload.c_str());
      } else {
        Serial.println("⚠️ No schedule for today: " + cardUuid);
        DynamicJsonDocument scanDoc(256);
        scanDoc["event"] = "card_scan";
        scanDoc["success"] = false;
        scanDoc["cardUuid"] = cardUuid;
        scanDoc["reason"] = "no_schedule_today";
        scanDoc["message"] = "No schedule found for today";
        scanDoc["timestamp"] = getLocalTime().unixtime();
        scanDoc["commandId"] = "scan_12345";
        String scanPayload;
        serializeJson(scanDoc, scanPayload);
        mqttClient.publish(MQTT_TOPIC_RESPONSE, scanPayload.c_str());
      }

      blinkRedTwice();
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      return;
    }
    const UserSchedule* todaysSchedule = &todayScheduleObj;

    std::vector<TodaysRecord> userCheckIns, userCheckOuts;
    getUserTodayRecords(cardUuid, userCheckIns, userCheckOuts);

    AttendanceResult localResult = processLocalAttendance(
      cardUuid,
      todaysSchedule->userId,
      todaysSchedule->userName,
      *todaysSchedule,
      currentTime,
      userCheckIns,
      userCheckOuts);

    if (localResult.success) {
      blinkGreenOnce();
      Serial.println("✅ LOCAL ATTENDANCE ACCEPTED: " + localResult.message);

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

      DynamicJsonDocument scanDoc(512);
      scanDoc["event"] = "card_scan";
      scanDoc["success"] = true;
      scanDoc["cardUuid"] = cardUuid;
      scanDoc["userId"] = todaysSchedule->userId;
      scanDoc["userName"] = todaysSchedule->userName;
      scanDoc["recordType"] = localResult.recordType;
      scanDoc["status"] = localResult.status;
      scanDoc["message"] = localResult.message;
      scanDoc["timestamp"] = getLocalTime().unixtime();
      scanDoc["formattedTime"] = localResult.formattedTime;
      scanDoc["commandId"] = "scan_12345";
      String scanPayload;
      serializeJson(scanDoc, scanPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, scanPayload.c_str());
    } else {
      blinkRedTwice();
      Serial.println("❌ LOCAL ATTENDANCE DENIED: " + localResult.message);

      DynamicJsonDocument scanDoc(512);
      scanDoc["event"] = "card_scan";
      scanDoc["success"] = false;
      scanDoc["cardUuid"] = cardUuid;
      scanDoc["userId"] = todaysSchedule->userId;
      scanDoc["userName"] = todaysSchedule->userName;
      scanDoc["reason"] = "attendance_denied";
      scanDoc["message"] = localResult.message;
      scanDoc["timestamp"] = getLocalTime().unixtime();
      scanDoc["commandId"] = "scan_12345";
      String scanPayload;
      serializeJson(scanDoc, scanPayload);
      mqttClient.publish(MQTT_TOPIC_RESPONSE, scanPayload.c_str());
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

  ESP.wdtFeed();
  yield();
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
  int retries = 5;

  while ((now = time(nullptr)) < 1000000000 && retries > 0) {
    yield();
    Serial.print(".");
    delay(500);
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

  // Remove AM/PM if present
  cleanTime.replace(" AM", "");
  cleanTime.replace(" PM", "");

  int dotIndex = cleanTime.indexOf('.');
  if (dotIndex > 0) {
    cleanTime = cleanTime.substring(0, dotIndex);
  }

  // Parse time - handle both "HH:MM:SS" and "HH:MM" formats
  int h1 = cleanTime.substring(0, 2).toInt();
  int h2 = cleanTime.substring(3, 5).toInt();
  int sec = 0;

  if (cleanTime.length() >= 8) {
    sec = cleanTime.substring(6, 8).toInt();
  }

  // Handle PM conversion
  if (timeStr.indexOf("PM") > 0 && h1 != 12) {
    h1 += 12;
  }
  // Handle 12 AM case
  if (timeStr.indexOf("AM") > 0 && h1 == 12) {
    h1 = 0;
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

String getNextScheduleInfo(int userId, const String& cardUuid, int currentDOW) {
  if (!sdMounted) return "No upcoming schedule";
  markSDOperationStart();

  String filePath = String(SCHEDULES_FOLDER) + cardUuid + ".csv";
  if (!SD.exists(filePath.c_str())) {
    markSDOperationEnd();
    return "No upcoming schedule";
  }

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file) {
    markSDOperationEnd();
    return "No upcoming schedule";
  }

  file.readStringUntil('\n');

  // Saare schedules padho aur next day dhundo
  std::vector<int> availableDays;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int p1 = line.indexOf(',');
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    if (p3 == -1) continue;

    int dow = line.substring(p2 + 1, p3).toInt();
    availableDays.push_back(dow);
  }

  file.close();

  // Next schedule dhundo
  for (int i = 1; i <= 7; i++) {
    int checkDay = currentDOW + i;
    if (checkDay > 7) checkDay -= 7;

    for (int d : availableDays) {
      if (d == checkDay) {
        markSDOperationEnd();
        return getDayName(checkDay);
      }
    }
  }

  markSDOperationEnd();
  return "No upcoming schedule";
}

bool getUserTodayRecords(const String& cardUuid,
                         std::vector<TodaysRecord>& checkIns,
                         std::vector<TodaysRecord>& checkOuts) {
  checkIns.clear();
  checkOuts.clear();

  markSDOperationStart();

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    markSDOperationEnd();
    return true;
  };

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    markSDOperationEnd();
    return false;
  }

  file.readStringUntil('\n');  // header skip

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Sirf is card ka record check karo
    if (line.indexOf(cardUuid) < 0) continue;

    // Parse karo
    int p0 = line.indexOf(',');
    int p1 = line.indexOf(',', p0 + 1);
    int p4 = line.indexOf(',', p1 + 1);
    int p5 = line.indexOf(',', p4 + 1);
    for (int i = 0; i < 2; i++) p4 = line.indexOf(',', p4 + 1);

    String timestamp = line.substring(0, p0);
    String recordType = line.substring(line.lastIndexOf(',',
                                                        line.indexOf(',', p1 + 1) + 1)
                                       + 1);

    // Simple parse
    int commaCount = 0;
    int sp = 0;
    String fields[10];
    while (sp < line.length()) {
      int cp = line.indexOf(',', sp);
      if (cp == -1) cp = line.length();
      fields[commaCount++] = line.substring(sp, cp);
      sp = cp + 1;
      if (commaCount >= 10) break;
    }

    if (commaCount < 5) continue;

    TodaysRecord record;
    record.recordType = fields[4];  // type field

    int timeStart = fields[0].indexOf(' ') + 1;
    String timeStr = fields[0].substring(timeStart);
    int h = timeStr.substring(0, 2).toInt();
    int m = timeStr.substring(3, 5).toInt();
    int s = timeStr.substring(6, 8).toInt();
    record.timestamp = h * 3600 + m * 60 + s;
    record.timestampStr = timeStr;

    if (record.recordType == "in") {
      checkIns.push_back(record);
    } else if (record.recordType == "out") {
      checkOuts.push_back(record);
    }
  }

  file.close();
  markSDOperationEnd();
  return true;
}

AttendanceResult processLocalAttendance(
  const String& cardUuid,
  int userId,
  const String& userName,
  const UserSchedule& todaySchedule,
  const DateTime& currentTime,
  const std::vector<TodaysRecord>& checkIns,
  const std::vector<TodaysRecord>& checkOuts) {

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

  // bool hasOpenCheckIn = (todaysCheckIns.size() > todaysCheckOuts.size());
  bool hasOpenCheckIn = (checkIns.size() > checkOuts.size());

  if (checkIns.size() > 0 || checkOuts.size() > 0) {
    int lastRecordTime = 0;
    if (checkOuts.size() > 0) {
      lastRecordTime = checkOuts.back().timestamp;
    }
    if (checkIns.size() > 0) {
      int lastInTime = checkIns.back().timestamp;
      if (lastInTime > lastRecordTime) lastRecordTime = lastInTime;
    }

    int secondsSinceLast = currentTimeSec - lastRecordTime;
    if (secondsSinceLast < MIN_GAP_BETWEEN_RECORDS && lastRecordTime > 0) {
      result.message = "Please wait " + String(MIN_GAP_BETWEEN_RECORDS) + " seconds";
      return result;
    }
  }

  // CASE 1: USER HASN'T CHECKED IN TODAY
  if (checkIns.size() == 0) {
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

  // CASE 2: WE'RE IN CHECK-OUT WINDOW - CHECK THIS BEFORE CHECKING IF WINDOW IS CLOSED
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

    if (checkOuts.size() == 0 && checkIns.size() > 0) {
      const TodaysRecord& lastCheckInRecord = checkIns.back();
      int minutesWorked = (currentTimeSec - lastCheckInRecord.timestamp) / 60;

      if (minutesWorked < MIN_WORK_DURATION) {
        result.success = false;
        result.message = "Minimum work duration not met: " + String(MIN_WORK_DURATION) + " minutes required";
        return result;
      }
    }

    addRecord("out", currentTime, userId, cardUuid, userName);
  }

  // CASE 3: CHECK-OUT WINDOW IS CLOSED - HANDLE MISSED CHECK-OUTS
  else if (isAfterCheckOutWindow) {
    if (checkIns.size() > 0 && checkOuts.size() == 0) {
      // User checked in but didn't check out
      String checkInTime = formatTimeDisplay(todaySchedule.checkInFrom);
      String lateCheckOutEnd = addMinutesToTimeStr(todaySchedule.checkOutTo, GRACE_LATE_OUT);
      String nextSchedule = getNextScheduleInfo(userId, cardUuid, currentDOW);

      result.message = "⚠️ Today's shift ended without check-out!\n   ✓ Check-in: " + checkInTime + "\n   ⛔ Check-out window closed: " + lateCheckOutEnd + "\n   📅 Next shift: " + nextSchedule;
      result.recordType = "in";
      return result;
    } else if (checkIns.size() == 0) {
      // User never checked in
      String checkInStartTime = formatTimeDisplay(todaySchedule.checkInFrom);
      String lateCheckInEnd = addMinutesToTimeStr(todaySchedule.checkInTo, GRACE_LATE_IN);
      String nextSchedule = getNextScheduleInfo(userId, cardUuid, currentDOW);

      result.message = "❌ You missed today's shift!\n   ✓ Check-in window was: " + checkInStartTime + " - " + lateCheckInEnd + "\n   📅 Next shift: " + nextSchedule;
      result.recordType = "in";
      return result;
    } else {
      // User already checked out
      result.message = "✅ Today's shift completed.";
      result.recordType = "out";
      return result;
    }
  }

  // CASE 4: CHECK-OUT WINDOW NOT OPEN YET BUT USER IS CHECKED IN
  else if (hasOpenCheckIn && isBeforeCheckOutWindow) {
    String checkOutTime = formatTimeDisplay(todaySchedule.checkOutFrom);
    result.message = "Already checked in. Check-out opens at " + checkOutTime;
    return result;
  }

  // DEFAULT CASE
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

String lastCreatedMonthFolder = "";

void ensureAttendanceFolders(const char* monthFolder) {
  if (!attendanceFolderReady) {
    if (!SD.exists("/attendance")) SD.mkdir("/attendance");
    attendanceFolderReady = true;
  }
  if (String(monthFolder) != lastCreatedMonthFolder) {
    if (!SD.exists(monthFolder)) SD.mkdir(monthFolder);
    lastCreatedMonthFolder = String(monthFolder);
  }
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
    Serial.println("SD Not Mounted");
    return false;
  }

  markSDOperationStart();

  DateTime now = getLocalTime();
  // DateTime now = rtc.now();

  // Daily log (same as before)
  char dailyFilename[64];
  sprintf(dailyFilename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  bool dailyExists = SD.exists(dailyFilename);

  File dailyFile = SD.open(dailyFilename, FILE_WRITE);
  if (!dailyFile) {
    Serial.println("Failed to open daily file: " + String(dailyFilename));
    return false;
  }

  if (!dailyExists) {
    dailyFile.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
  }

  String line = timestamp + "," + cardUuid + "," + String(userId) + "," + userName + "," + recordType + "," + status + "," + message + "," + dayOfWeek + "," + checkInWindow + "," + checkOutWindow;

  dailyFile.println(line);
  dailyFile.flush();
  dailyFile.close();

  Serial.println("Daily attendance saved → " + String(dailyFilename));

  // Monthly per-user in month folder
  char monthFolder[32];
  sprintf(monthFolder, "/attendance/%04d-%02d", now.year(), now.month());

  ensureAttendanceFolders(monthFolder);

  // User file
  String userFilePath = String(monthFolder) + "/" + cardUuid + ".csv";
  Serial.println("User monthly file path: " + userFilePath);

  bool userFileExists = SD.exists(userFilePath.c_str());

  File userFile;

  if (userFileExists) {
    // Pehle read mode mein open karo size pata karne ke liye
    File tempRead = SD.open(userFilePath.c_str(), FILE_READ);
    uint32_t fileSize = 0;
    if (tempRead) {
      fileSize = tempRead.size();
      tempRead.close();
    }

    userFile = SD.open(userFilePath.c_str(), FILE_WRITE);
    if (!userFile) {
      Serial.println("Failed to open user monthly file for append: " + userFilePath);
      return true;
    }
    userFile.seek(fileSize);
    Serial.println("Appending to existing user file at position: " + String(fileSize));

  } else {
    userFile = SD.open(userFilePath.c_str(), FILE_WRITE);
    if (!userFile) {
      Serial.println("Failed to create user monthly file: " + userFilePath);
      return true;
    }
    userFile.println("Timestamp,CardUUID,UserID,UserName,Type,Status,Message,DayOfWeek,CheckInWindow,CheckOutWindow");
    Serial.println("Created new user monthly file with header");
  }

  userFile.println(line);
  userFile.flush();
  userFile.close();

  Serial.println("Monthly attendance saved (per user) → " + userFilePath);
  markSDOperationEnd();
  return true;
}

void saveScheduleToSD() {
  if (!sdMounted) {
    Serial.println("❌ SD not mounted - cannot save schedules");
    return;
  }
  markSDOperationStart();

  if (!SD.exists(SCHEDULES_FOLDER)) {
    bool folderCreated = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
      if (SD.mkdir(SCHEDULES_FOLDER)) {
        folderCreated = true;
        Serial.println("📁 Created schedules folder: " + String(SCHEDULES_FOLDER));
        break;
      }
      Serial.printf("⚠️ mkdir attempt %d failed, retrying...\n", attempt);
      delay(100);
    }
    if (!folderCreated) {
      Serial.println("❌ Failed to create schedules folder after 3 attempts!");
      return;
    }
  }

  // Unique cardUuid collect karo (simple vector use karke)
  std::vector<String> uniqueUuids;
  for (auto& sched : userSchedules) {
    bool found = false;
    for (auto& u : uniqueUuids) {
      if (u == sched.cardUuid) {
        found = true;
        break;
      }
    }
    if (!found) {
      uniqueUuids.push_back(sched.cardUuid);
    }
  }

  Serial.printf("Saving %d unique users to separate CSV files...\n", uniqueUuids.size());

  // Har unique UUID ke liye file banao
  for (auto& cardUuid : uniqueUuids) {
    String filePath = String(SCHEDULES_FOLDER) + "/" + cardUuid + ".csv";

    // Purani file delete
    if (SD.exists(filePath.c_str())) {
      SD.remove(filePath.c_str());
    }

    File file = SD.open(filePath.c_str(), FILE_WRITE);
    if (!file) {
      Serial.println("❌ Failed to create file: " + filePath);
      continue;
    }

    file.println("userId,userName,dayOfWeek,checkInFrom,checkInTo,checkOutFrom,checkOutTo");

    // Is UUID ke saare schedules likho
    for (auto& s : userSchedules) {
      if (s.cardUuid == cardUuid) {
        String line = String(s.userId) + "," + s.userName + "," + String(s.dayOfWeek) + "," + s.checkInFrom + "," + s.checkInTo + "," + s.checkOutFrom + "," + s.checkOutTo;
        file.println(line);
      }
    }

    file.flush();
    file.close();

    Serial.printf("Saved schedules for card %s → %s\n", cardUuid.c_str(), filePath.c_str());
  }

  Serial.println("✅ All user schedules saved as separate CSV files");
  markSDOperationEnd();
}

bool loadUserScheduleFromSD(const String& cardUuid, int dayOfWeek, UserSchedule& foundSchedule) {
  if (!sdMounted) return false;

  markSDOperationStart();

  String filePath = String(SCHEDULES_FOLDER) + "/" + cardUuid + ".csv";

  if (!SD.exists(filePath.c_str())) {
    Serial.println("❌ No schedule file for card: " + cardUuid);
    markSDOperationEnd();
    return false;
  }

  File file = SD.open(filePath.c_str(), FILE_READ);
  // if (!file) return false;
  if (!file) {
    markSDOperationEnd();
    return false;
  }

  // Header skip
  file.readStringUntil('\n');

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse: userId,userName,dayOfWeek,checkInFrom,checkInTo,checkOutFrom,checkOutTo
    int p1 = line.indexOf(',');
    int p2 = line.indexOf(',', p1 + 1);
    int p3 = line.indexOf(',', p2 + 1);
    int p4 = line.indexOf(',', p3 + 1);
    int p5 = line.indexOf(',', p4 + 1);
    int p6 = line.indexOf(',', p5 + 1);

    if (p6 == -1) continue;

    int fileDOW = line.substring(p2 + 1, p3).toInt();

    // Sirf aaj ka day match karo
    if (fileDOW != dayOfWeek) continue;

    foundSchedule.cardUuid = cardUuid;
    foundSchedule.userId = line.substring(0, p1).toInt();
    foundSchedule.userName = line.substring(p1 + 1, p2);
    foundSchedule.dayOfWeek = fileDOW;
    foundSchedule.checkInFrom = line.substring(p3 + 1, p4);
    foundSchedule.checkInTo = line.substring(p4 + 1, p5);
    foundSchedule.checkOutFrom = line.substring(p5 + 1, p6);
    foundSchedule.checkOutTo = line.substring(p6 + 1);

    file.close();
    markSDOperationEnd();
    return true;
  }

  file.close();
  markSDOperationEnd();
  return false;
}

void loadTodayRecordsFromSD() {
  markSDOperationStart();
  todaysCheckIns.clear();
  todaysCheckOuts.clear();

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    markSDOperationEnd();
    return;
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    markSDOperationEnd();
    return;
  }

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
  markSDOperationEnd();
}

void cleanupOldDailyFiles() {
  if (!sdMounted) return;
  markSDOperationStart();

  Serial.println("🧹 Cleaning up old daily files...");

  DateTime localNow = getLocalTime();
  int todayInt = localNow.year() * 10000UL + localNow.month() * 100 + localNow.day();

  File root = SD.open("/");
  if (!root) {
    markSDOperationEnd();
    return;
  }

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
  markSDOperationEnd();
}

// ================== SYNC TO SERVER ==================
bool syncMonthlyRecordsToServer(int year, int month) {
  if (!sdMounted || !autoSyncEnabled || WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (isSyncing) return false;
  isSyncing = true;

  markSDOperationStart();

  char monthFolder[32];
  sprintf(monthFolder, ATTENDANCE_FOLDER "%04d-%02d/", year, month);

  if (!SD.exists(monthFolder)) {
    Serial.println("No attendance folder for " + String(monthFolder) + " - nothing to sync");
    markSDOperationEnd();
    isSyncing = false;
    return false;
  }

  Serial.println("Syncing monthly attendance folder: " + String(monthFolder));

  File dir = SD.open(monthFolder);
  if (!dir) {
    Serial.println("Failed to open month folder");
    markSDOperationEnd();
    isSyncing = false;
    return false;
  }

  int totalSynced = 0;
  int totalFailed = 0;

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;

    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    String fileName = entry.name();
    if (!fileName.endsWith(".csv")) {
      entry.close();
      continue;
    }

    // fileName = 31F66816.csv
    String cardUuid = fileName.substring(0, fileName.length() - 4);

    Serial.println("Syncing user file: " + fileName);

    File file = SD.open(String(monthFolder) + fileName, FILE_READ);
    if (!file) {
      entry.close();
      continue;
    }

    String header = file.readStringUntil('\n');

    String tempFilePath = String(monthFolder) + "temp_" + fileName;

    File tempFile = SD.open(tempFilePath.c_str(), FILE_WRITE);
    if (!tempFile) {
      file.close();
      entry.close();
      continue;
    }

    tempFile.println(header);

    int synced = 0;
    int failed = 0;

    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      int commaIndex = 0;
      int startPos = 0;
      int fieldIndex = 0;

      String timestamp, userIdStr, userName;
      String recordType, status, message;
      String dayOfWeek, checkInWindow, checkOutWindow;

      while (startPos < line.length()) {
        commaIndex = line.indexOf(',', startPos);
        if (commaIndex == -1) commaIndex = line.length();

        String field = line.substring(startPos, commaIndex);

        switch (fieldIndex) {
          case 0: timestamp = field; break;
          case 1: /* cardUuid skip - file name se mil raha hai */ break;
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

      if (fieldIndex < 10) {
        Serial.println("⚠️ Invalid CSV line, skipping");
        continue;
      }

      int userId = userIdStr.toInt();

      bool sent = sendAttendanceRecordToServer(
        cardUuid,
        userId,
        userName,
        timestamp,
        recordType,
        status,
        message,
        dayOfWeek,
        checkInWindow,
        checkOutWindow);

      if (sent) {
        synced++;
        totalSynced++;
      } else {
        failed++;
        totalFailed++;
        tempFile.println(line);
      }

      // delay(50);
      yield();
    }

    file.close();
    tempFile.close();

    // Cleanup
    String originalPath = String(monthFolder) + fileName;
    SD.remove(originalPath.c_str());
    if (failed > 0) {
      SD.rename(tempFilePath.c_str(), originalPath.c_str());
      Serial.printf("User %s: %d synced, %d failed → kept local\n", cardUuid.c_str(), synced, failed);
    } else {
      SD.remove(tempFilePath.c_str());
      Serial.printf("User %s: All %d records synced → removed local copy\n", cardUuid.c_str(), synced);
    }

    entry.close();
  }

  dir.close();
  Serial.printf("Monthly sync complete for %04d-%02d: Total synced %d, failed %d\n",
                year, month, totalSynced, totalFailed);
  markSDOperationEnd();
  isSyncing = false;
  return totalSynced > 0;
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

  client.setTimeout(5000);
  http.setTimeout(5000);

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
  markSDOperationStart();

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    markSDOperationEnd();
    return "{\"success\":true,\"message\":\"No records for today\",\"data\":[]}";
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    markSDOperationEnd();
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
  markSDOperationEnd();
  return jsonResponse;
}

String getUserAttendanceRecords(String targetCardUuid) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }
  markSDOperationStart();

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    markSDOperationEnd();
    return "{\"success\":true,\"message\":\"No records found\",\"data\":[]}";
  }

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    markSDOperationEnd();
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
  markSDOperationEnd();
  return jsonResponse;
}

String deleteUserAttendanceRecords(String targetCardUuid) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }
  markSDOperationStart();

  DateTime now = getLocalTime();
  char filename[32];
  sprintf(filename, "%s%04d%02d%02d.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  if (!SD.exists(filename)) {
    markSDOperationEnd();
    return "{\"success\":false,\"message\":\"No records file found\"}";
  }

  char tempFilename[32];
  sprintf(tempFilename, "%s%04d%02d%02d_temp.csv",
          DAILY_LOG_PREFIX, now.year(), now.month(), now.day());

  File originalFile = SD.open(filename, FILE_READ);
  if (!originalFile) {
    markSDOperationEnd();
    return "{\"success\":false,\"message\":\"Failed to open original file\"}";
  }

  File tempFile = SD.open(tempFilename, FILE_WRITE);
  if (!tempFile) {
    originalFile.close();
    markSDOperationEnd();
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

  String response = "{\"success\":true,\"message\":\"Daily records deleted successfully\",";
  response += "\"deletedCount\":" + String(deletedCount) + ",";
  response += "\"totalCount\":" + String(totalCount) + "}";

  markSDOperationEnd();
  return response;
}

String getMonthlyAttendanceRecords(int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }
  markSDOperationStart();

  char monthFolder[32];
  sprintf(monthFolder, "/attendance/%04d-%02d", year, month);

  Serial.println("DEBUG monthFolder: " + String(monthFolder));
  Serial.println("DEBUG folder exists: " + String(SD.exists(monthFolder) ? "YES" : "NO"));

  if (!SD.exists(monthFolder)) {
    markSDOperationEnd();
    return "{\"success\":true,\"message\":\"No records for this month\",\"data\":[]}";
  }

  File dir = SD.open(monthFolder);
  if (!dir) {
    markSDOperationEnd();
    return "{\"success\":false,\"message\":\"Failed to open month folder\"}";
  }

  Serial.println("DEBUG dir opened successfully");

  String jsonResponse = "{\"success\":true,\"message\":\"Monthly records fetched\",\"data\":[";
  bool firstRecord = true;

  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      Serial.println("DEBUG no more entries");
      break;
    }

    Serial.println("DEBUG entry.name(): " + String(entry.name()));
    Serial.println("DEBUG isDirectory: " + String(entry.isDirectory() ? "YES" : "NO"));

    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    // entry.name() full path deta hai ESP8266 par, sirf filename lo
    String fullName = String(entry.name());
    int lastSlash = fullName.lastIndexOf('/');
    String fileName = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;

    Serial.println("DEBUG fileName after parse: " + fileName);

    if (!fileName.endsWith(".csv") || fileName.startsWith("temp_")) {
      entry.close();
      continue;
    }

    String cardUuid = fileName.substring(0, fileName.length() - 4);
    Serial.println("Reading file for cardUuid: " + cardUuid);

    String filePath = String(monthFolder) + "/" + fileName;
    File file = SD.open(filePath.c_str(), FILE_READ);
    if (!file) {
      entry.close();
      continue;
    }

    file.readStringUntil('\n');

    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue;

      // Parse fields
      int pos[9];
      int sp = 0;
      for (int i = 0; i < 9; i++) {
        pos[i] = line.indexOf(',', sp);
        if (pos[i] == -1) pos[i] = line.length();
        sp = pos[i] + 1;
      }

      String timestamp = line.substring(0, pos[0]);
      // field 1 = cardUuid in file (same as filename), skip
      String userIdStr = line.substring(pos[1] + 1, pos[2]);
      String userName = line.substring(pos[2] + 1, pos[3]);
      String recordType = line.substring(pos[3] + 1, pos[4]);
      String status = line.substring(pos[4] + 1, pos[5]);
      String message = line.substring(pos[5] + 1, pos[6]);
      String dayOfWeek = line.substring(pos[6] + 1, pos[7]);
      String checkIn = line.substring(pos[7] + 1, pos[8]);
      String checkOut = line.substring(pos[8] + 1);
      checkOut.trim();

      if (!firstRecord) jsonResponse += ",";
      firstRecord = false;

      jsonResponse += "{";
      jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
      jsonResponse += "\"cardUuid\":\"" + cardUuid + "\",";
      jsonResponse += "\"userId\":" + userIdStr + ",";
      jsonResponse += "\"userName\":\"" + userName + "\",";
      jsonResponse += "\"recordType\":\"" + recordType + "\",";
      jsonResponse += "\"status\":\"" + status + "\",";
      jsonResponse += "\"message\":\"" + message + "\",";
      jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
      jsonResponse += "\"checkInWindow\":\"" + checkIn + "\",";
      jsonResponse += "\"checkOutWindow\":\"" + checkOut + "\"";
      jsonResponse += "}";
    }

    file.close();
    entry.close();
  }

  dir.close();
  jsonResponse += "]}";
  markSDOperationEnd();
  return jsonResponse;
}

String getUserMonthlyRecords(String targetCardUuid, int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }
  markSDOperationStart();

  char filePath[80];
  sprintf(filePath, "/attendance/%04d-%02d/%s.csv", year, month, targetCardUuid.c_str());

  Serial.println("Looking for user monthly file: " + String(filePath));

  if (!SD.exists(filePath)) {
    markSDOperationEnd();
    return "{\"success\":true,\"message\":\"No records for this user\",\"data\":[]}";
  }

  File file = SD.open(filePath, FILE_READ);
  if (!file) {
    markSDOperationEnd();
    return "{\"success\":false,\"message\":\"Failed to open user file\"}";
  }

  String jsonResponse = "{\"success\":true,\"message\":\"User monthly records fetched\",\"data\":[";
  bool firstRecord = true;

  file.readStringUntil('\n');

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int pos[9];
    int sp = 0;
    for (int i = 0; i < 9; i++) {
      pos[i] = line.indexOf(',', sp);
      if (pos[i] == -1) pos[i] = line.length();
      sp = pos[i] + 1;
    }

    String timestamp = line.substring(0, pos[0]);
    String userIdStr = line.substring(pos[1] + 1, pos[2]);
    String userName = line.substring(pos[2] + 1, pos[3]);
    String recordType = line.substring(pos[3] + 1, pos[4]);
    String status = line.substring(pos[4] + 1, pos[5]);
    String message = line.substring(pos[5] + 1, pos[6]);
    String dayOfWeek = line.substring(pos[6] + 1, pos[7]);
    String checkIn = line.substring(pos[7] + 1, pos[8]);
    String checkOut = line.substring(pos[8] + 1);
    checkOut.trim();

    if (!firstRecord) jsonResponse += ",";
    firstRecord = false;

    jsonResponse += "{";
    jsonResponse += "\"timestamp\":\"" + timestamp + "\",";
    jsonResponse += "\"cardUuid\":\"" + targetCardUuid + "\",";
    jsonResponse += "\"userId\":" + userIdStr + ",";
    jsonResponse += "\"userName\":\"" + userName + "\",";
    jsonResponse += "\"recordType\":\"" + recordType + "\",";
    jsonResponse += "\"status\":\"" + status + "\",";
    jsonResponse += "\"message\":\"" + message + "\",";
    jsonResponse += "\"dayOfWeek\":\"" + dayOfWeek + "\",";
    jsonResponse += "\"checkInWindow\":\"" + checkIn + "\",";
    jsonResponse += "\"checkOutWindow\":\"" + checkOut + "\"";
    jsonResponse += "}";
  }

  file.close();
  jsonResponse += "]}";
  markSDOperationEnd();
  return jsonResponse;
}

// ================== DELETE ENTIRE MONTH FOLDER ==================
String deleteMonthlyFile(int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }
  markSDOperationStart();

  char monthFolder[32];
  sprintf(monthFolder, "/attendance/%04d-%02d", year, month);

  if (!SD.exists(monthFolder)) {
    markSDOperationEnd();
    return "{\"success\":false,\"message\":\"No records found for this month\"}";
  }

  // Pehle folder ke andar saari files delete karo
  File dir = SD.open(monthFolder);
  if (!dir) {
    markSDOperationEnd();
    return "{\"success\":false,\"message\":\"Failed to open month folder\"}";
  }

  int deletedFiles = 0;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;

    if (!entry.isDirectory()) {
      String fullName = String(entry.name());
      int lastSlash = fullName.lastIndexOf('/');
      String fileName = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;
      String filePath = String(monthFolder) + "/" + fileName;
      entry.close();

      if (SD.remove(filePath.c_str())) {
        deletedFiles++;
        Serial.println("Deleted: " + filePath);
      } else {
        Serial.println("Failed to delete: " + filePath);
      }
    } else {
      entry.close();
    }
  }
  dir.close();

  // Ab folder delete karo
  if (SD.rmdir(monthFolder)) {
    Serial.println("Month folder deleted: " + String(monthFolder));
    String response = "{\"success\":true,\"message\":\"Monthly records deleted successfully\",";
    response += "\"deletedFiles\":" + String(deletedFiles) + "}";
    markSDOperationEnd();
    return response;
  } else {
    // Folder delete na ho to bhi files to delete ho gayi
    String response = "{\"success\":true,\"message\":\"Monthly files deleted (folder may remain)\",";
    response += "\"deletedFiles\":" + String(deletedFiles) + "}";
    markSDOperationEnd();
    return response;
  }
}

// ================== DELETE SPECIFIC USER FROM MONTH ==================
String deleteUserFromMonthlyFile(String targetCardUuid, int year, int month) {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\"}";
  }
  markSDOperationStart();

  char filePath[80];
  sprintf(filePath, "/attendance/%04d-%02d/%s.csv", year, month, targetCardUuid.c_str());

  Serial.println("Deleting user file: " + String(filePath));

  if (!SD.exists(filePath)) {
    return "{\"success\":false,\"message\":\"No records found for this user\"}";
  }

  // File ka size count karo (records count ke liye)
  File file = SD.open(filePath, FILE_READ);
  int recordCount = 0;
  if (file) {
    file.readStringUntil('\n');
    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) recordCount++;
    }
    file.close();
  }

  // Direct file delete karo
  if (SD.remove(filePath)) {
    Serial.println("User file deleted: " + String(filePath));
    String response = "{\"success\":true,\"message\":\"User records deleted successfully\",";
    response += "\"deletedCount\":" + String(recordCount) + ",";
    response += "\"cardUuid\":\"" + targetCardUuid + "\"}";
    markSDOperationEnd();
    return response;
  } else {
    markSDOperationEnd();
    return "{\"success\":false,\"message\":\"Failed to delete user file\"}";
  }
}

String listAllLogFiles() {
  if (!sdMounted) {
    return "{\"success\":false,\"message\":\"SD card not mounted\",\"data\":{\"dailyFiles\":[],\"monthlyFiles\":[]}}";
  }
  markSDOperationStart();

  DynamicJsonDocument doc(4096);
  JsonObject data = doc.createNestedObject("data");
  JsonArray dailyFiles = data.createNestedArray("dailyFiles");
  JsonArray monthlyFiles = data.createNestedArray("monthlyFiles");

  // ================== DAILY FILES (root mein) ==================
  File root = SD.open("/");
  if (!root) {
    return "{\"success\":false,\"message\":\"Failed to open root directory\",\"data\":{\"dailyFiles\":[],\"monthlyFiles\":[]}}";
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;

    if (!entry.isDirectory()) {
      String fullName = String(entry.name());
      int lastSlash = fullName.lastIndexOf('/');
      String fileName = (lastSlash >= 0) ? fullName.substring(lastSlash + 1) : fullName;

      if (fileName.startsWith("log_") && fileName.endsWith(".csv")) {
        JsonObject fileObj = dailyFiles.createNestedObject();
        fileObj["filename"] = fileName;
        String dateStr = fileName.substring(4, fileName.length() - 4);
        fileObj["date"] = dateStr;
        fileObj["size"] = entry.size();
      }
    }
    entry.close();
  }
  root.close();

  // ================== MONTHLY FILES (/attendance/ folder mein) ==================
  if (SD.exists("/attendance")) {
    File attendanceDir = SD.open("/attendance");
    if (attendanceDir) {

      while (true) {
        File monthDir = attendanceDir.openNextFile();
        if (!monthDir) break;

        if (!monthDir.isDirectory()) {
          monthDir.close();
          continue;
        }

        String monthDirName = String(monthDir.name());
        int lastSlash = monthDirName.lastIndexOf('/');
        String monthFolderName = (lastSlash >= 0) ? monthDirName.substring(lastSlash + 1) : monthDirName;

        Serial.println("DEBUG monthFolder: " + monthFolderName);

        String monthFolderPath = "/attendance/" + monthFolderName;
        File userDir = SD.open(monthFolderPath.c_str());

        if (userDir) {
          while (true) {
            File userFile = userDir.openNextFile();
            if (!userFile) break;

            if (!userFile.isDirectory()) {
              String fullUserName = String(userFile.name());
              int lastSlashU = fullUserName.lastIndexOf('/');
              String userFileName = (lastSlashU >= 0) ? fullUserName.substring(lastSlashU + 1) : fullUserName;

              if (userFileName.endsWith(".csv") && !userFileName.startsWith("temp_")) {
                String cardUuid = userFileName.substring(0, userFileName.length() - 4);

                JsonObject fileObj = monthlyFiles.createNestedObject();
                fileObj["filename"] = userFileName;
                fileObj["cardUuid"] = cardUuid;
                fileObj["yearMonth"] = monthFolderName;
                fileObj["path"] = monthFolderPath + "/" + userFileName;
                fileObj["size"] = userFile.size();
              }
            }
            userFile.close();
          }
          userDir.close();
        }

        monthDir.close();
      }
      attendanceDir.close();
    }
  }

  doc["success"] = true;
  doc["message"] = "Files listed successfully";

  String response;
  serializeJson(doc, response);
  markSDOperationEnd();
  return response;
}

// check wifi failure stored msg
void checkPendingWifiFailure() {
  int addr = 400;
  if (EEPROM.read(addr) == 1) {
    Serial.println("📨 Found pending WiFi failure");

    // Read stored commandId
    char buf[21] = { 0 };
    for (int i = 0; i < 20; i++) {
      char c = EEPROM.read(addr + 1 + i);
      if (c == 0) break;
      buf[i] = c;
    }
    String commandId = String(buf);

    // Clear from EEPROM
    EEPROM.write(addr, 0);
    for (int i = 0; i < 20; i++) {
      EEPROM.write(addr + 1 + i, 0);
    }
    EEPROM.commit();

    // Send failure response now that device is connected
    DynamicJsonDocument data(128);
    data["success"] = false;
    data["message"] = "Failed connect to WiFi. Please check password";
    data["ssid"] = "";
    if (commandId.length() > 0) {
      data["commandId"] = commandId;
    }

    String payload;
    serializeJson(data, payload);

    if (mqttClient.connected()) {
      mqttClient.publish(MQTT_TOPIC_RESPONSE, payload.c_str());
      Serial.println("✅ Pending failure response sent");
    }
  }
}

// =================== check sd init ========================
bool initSDCard() {
  // SD ko settle hone do
  delay(500);

  for (int attempt = 1; attempt <= 5; attempt++) {
    Serial.printf("SD init attempt %d/5...\n", attempt);

    // Pehle end karo agar pehle se initialized tha
    SD.end();
    delay(200);

    if (SD.begin(SD_CS_PIN)) {
      Serial.println("✅ SD initialized successfully");
      return true;
    }

    Serial.printf("❌ SD init failed, retry in %dms\n", attempt * 500);
    delay(attempt * 500);  // Progressive delay: 500, 1000, 1500...
  }

  Serial.println("💀 SD init failed after 5 attempts");
  return false;
}

// =============== check sd progress =================
void markSDOperationStart() {
  lastSDOperationTime = millis();
}

void markSDOperationEnd() {
  lastSDOperationTime = 0;
}

void checkSDWatchdog() {
  if (lastSDOperationTime > 0 && millis() - lastSDOperationTime > SD_OPERATION_TIMEOUT) {
    Serial.println("⚠️ SD operation timeout detected! Restarting...");
    SD.end();
    delay(100);
    ESP.restart();
  }
}

// ============== Save wifi credentials ==============
void saveWifiCredentials(String ssid, String pass) {
  for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_SSID_ADDR + i, 0);
  for (int i = 0; i < 32; i++) EEPROM.write(EEPROM_PASS_ADDR + i, 0);
  for (int i = 0; i < ssid.length() && i < 32; i++)
    EEPROM.write(EEPROM_SSID_ADDR + i, ssid[i]);
  for (int i = 0; i < pass.length() && i < 32; i++)
    EEPROM.write(EEPROM_PASS_ADDR + i, pass[i]);
  EEPROM.commit();
  Serial.println("💾 WiFi credentials saved: " + ssid);
}

// ============= Load wifi credentials ===============
void loadWifiCredentials() {
  char ssid[33] = { 0 }, pass[33] = { 0 };
  for (int i = 0; i < 32; i++) {
    char c = EEPROM.read(EEPROM_SSID_ADDR + i);
    if (c == 0 || c == 0xFF) break;
    ssid[i] = c;
  }
  for (int i = 0; i < 32; i++) {
    char c = EEPROM.read(EEPROM_PASS_ADDR + i);
    if (c == 0 || c == 0xFF) break;
    pass[i] = c;
  }
  savedSSID = String(ssid);
  savedPassword = String(pass);
  if (savedSSID.length() > 0)
    Serial.println("📡 Saved WiFi loaded: " + savedSSID);
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
  pcf.write(BUZZER_PIN, HIGH);
  delay(120);
  pcf.write(GREEN_LED_PIN, LOW);
  pcf.write(BUZZER_PIN, LOW);
}

void blinkRedTwice() {
  for (int i = 0; i < 2; i++) {
    pcf.write(RED_LED_PIN, HIGH);
    pcf.write(BUZZER_PIN, HIGH);
    delay(120);
    pcf.write(RED_LED_PIN, LOW);
    pcf.write(BUZZER_PIN, LOW);
    delay(120);
  }
}

void blinkYellowHeartbeat() {
  if (millis() - lastYellowBlink >= 1000) {
    lastYellowBlink = millis();
    yellowState = !yellowState;
    pcf.write(YELLOW_LED_PIN, yellowState ? HIGH : LOW);
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(D2, D1);
  pcf.begin();

  pcf.write(WHITE_LED_PIN, LOW);
  pcf.write(BLUE_LED_PIN, LOW);
  pcf.write(GREEN_LED_PIN, LOW);
  pcf.write(RED_LED_PIN, LOW);
  pcf.write(YELLOW_LED_PIN, LOW);
  pcf.write(BUZZER_PIN, LOW);

  Serial.println("\n\n=== ESP8266 Attendance Device Starting (MQTT Version) ===");

  EEPROM.begin(512);
  loadWifiCredentials();
  loadServerUrlFromEEPROM();
  loadAttendanceSettingsFromEEPROM();

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

  sdMounted = initSDCard();
  if (sdMounted) {
    if (!SD.exists("/schedules")) SD.mkdir("/schedules");
    if (!SD.exists("/attendance")) {
      SD.mkdir("/attendance");
      attendanceFolderReady = true;
    }
  }

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
  delay(50);
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  Serial.println("RFID Module Initialized");

  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(2048);
  mqttClient.setSocketTimeout(1);

  Serial.println("Attempting WiFi connection...");
  WiFi.mode(WIFI_STA);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConnectTimeout(20);

  wifiManager.setSaveConfigCallback([]() {
    // Portal se naya WiFi connect hua — credentials save karo
    saveWifiCredentials(
      wifiManager.getWiFiSSID(),
      wifiManager.getWiFiPass());
    savedSSID = wifiManager.getWiFiSSID();
    savedPassword = wifiManager.getWiFiPass();
    Serial.println("✅ New WiFi saved from portal!");
  });

  Serial.println("🔆 Boot animation...");
  // Round 1 — ek ek kar ke on
  pcf.write(WHITE_LED_PIN, HIGH);
  delay(150);
  pcf.write(BLUE_LED_PIN, HIGH);
  delay(150);
  pcf.write(GREEN_LED_PIN, HIGH);
  delay(150);
  pcf.write(RED_LED_PIN, HIGH);
  delay(150);
  pcf.write(YELLOW_LED_PIN, HIGH);
  delay(150);
  // Sab on — 300ms ruko
  delay(300);
  // Ek ek kar ke band
  pcf.write(YELLOW_LED_PIN, LOW);
  delay(150);
  pcf.write(RED_LED_PIN, LOW);
  delay(150);
  pcf.write(GREEN_LED_PIN, LOW);
  delay(150);
  pcf.write(BLUE_LED_PIN, LOW);
  delay(150);

  pcf.write(YELLOW_LED_PIN, HIGH);

  bool res = wifiManager.autoConnect("RFID_Device_001", "12345678");
  if (!res) {
    Serial.println("Initial WiFi connection failed");
    pcf.write(WHITE_LED_PIN, HIGH);
  }

  if (WiFi.status() == WL_CONNECTED) {

    for (int i = 0; i < 5; i++) {
      pcf.write(BLUE_LED_PIN, HIGH);
      delay(200);
      pcf.write(BLUE_LED_PIN, LOW);
      delay(200);
    }

    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    connectToMqtt();
    setRTCFromNTP();

    pcf.write(GREEN_LED_PIN, HIGH);
    delay(500);
    pcf.write(GREEN_LED_PIN, LOW);
  } else {
    Serial.println("\nWiFi not connected, using offline mode");
  }

  pcf.write(YELLOW_LED_PIN, LOW);

  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
    checkPendingWifiFailure();
  }

  reprovisionMode = false;
  lastHeartbeatTime = millis();
  // lastScheduleUpdate = millis();

  if (localStorage) {
    todaysCheckIns.clear();
    todaysCheckOuts.clear();
    DateTime now = getLocalTime();
    currentProcessingDay = now.year() * 10000 + now.month() * 100 + now.day();

    if (sdMounted) {
      loadTodayRecordsFromSD();
    }

    Serial.printf("🕐 Device started at: %04d-%02d-%02d %02d:%02d:%02d (Local)\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
  }

  Serial.println("Setup complete!");
}

// ================== LOOP ==================
void loop() {
  checkSDWatchdog();
  blinkYellowHeartbeat();

  if (WiFi.status() == WL_CONNECTED && reprovisionMode) {
    reprovisionMode = false;
    wifiFailCount = 0;
    wifiManager.stopConfigPortal();
    Serial.println("✅ WiFi reconnected — exiting AP mode");
    connectToMqtt();
  }

  wifiManager.process();
  static bool rtcSynced = false;
  unsigned long currentMillis = millis();

  static unsigned long lastRFIDCheck = 0;
  static unsigned long lastCardScan = 0;
  if (currentMillis - lastRFIDCheck >= 50) {
    lastRFIDCheck = currentMillis;
    if (currentMillis - lastCardScan >= 500) {
      bool cardPresent = rfid.PICC_IsNewCardPresent();
      bool cardRead = false;
      if (cardPresent) cardRead = rfid.PICC_ReadCardSerial();
      if (cardPresent && cardRead) {
        lastCardScan = currentMillis;
        handleRFID();
      }

      // if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      //   lastCardScan = currentMillis;
      //   handleRFID();
      // }
    }
  }

  if (currentMillis - lastRFIDReinit >= RFID_REINIT_INTERVAL) {
    lastRFIDReinit = currentMillis;
    rfid.PCD_Init();
    Serial.println("🔄 Periodic RFID reinit");
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
      if (now - lastMqttReconnectAttempt > 15000) {
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
      // if (currentMillis - lastScheduleUpdate >= scheduleUpdateInterval) {
      //   fetchAndStoreSchedules();
      //   lastScheduleUpdate = currentMillis;
      // }

      if (autoSyncEnabled && currentMillis - lastSyncTime >= SYNC_INTERVAL) {
        lastSyncTime = currentMillis;
        DateTime now = rtc.now();
        syncMonthlyRecordsToServer(now.year(), now.month());
      }
    }
  } else if (!reprovisionMode) {
    static unsigned long lastReconnectAttempt = 0;
    if (currentMillis - lastReconnectAttempt >= 500) {
      lastReconnectAttempt = currentMillis;
      wifiFailCount++;
      Serial.printf("WiFi disconnected. Attempt %d to reconnect...\n", wifiFailCount);
      WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
      pcf.write(WHITE_LED_PIN, HIGH);
      pcf.write(WHITE_LED_PIN, LOW);
      if (wifiFailCount >= FAIL_LIMIT) {
        checkReprovision();
      }
    }
  } else {
    // AP mode — passive scan se check karo
    static unsigned long lastApReconnect = 0;
    if (currentMillis - lastApReconnect >= 20000) {
      lastApReconnect = currentMillis;

      if (savedSSID.length() > 0) {
        // Scan karo — kya saved network visible hai?
        int n = WiFi.scanNetworks(false, false);  // blocking scan
        bool found = false;
        for (int i = 0; i < n; i++) {
          if (WiFi.SSID(i) == savedSSID) {
            found = true;
            break;
          }
        }
        WiFi.scanDelete();

        if (found) {
          Serial.println("✅ Saved network found! Connecting...");
          wifiManager.stopConfigPortal();
          reprovisionMode = false;
          wifiFailCount = 0;
          WiFi.begin(savedSSID.c_str(), savedPassword.c_str());

          unsigned long tryStart = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - tryStart < 8000) {
            delay(100);
            yield();
          }

          if (WiFi.status() == WL_CONNECTED) {
            Serial.println("✅ Reconnected!");
            connectToMqtt();
            setRTCFromNTP();
          } else {
            // Connect nahi hua — AP wapas on karo
            reprovisionMode = true;
            wifiManager.startConfigPortal("RFID_Device_001", "12345678");
          }
        } else {
          Serial.println("📡 Saved network not visible yet...");
        }
      }
    }
  }

  delay(10);
}