#include <ESP8266WiFi.h>

const char* ssid = "Stom fiber Adam jee";
const char* password = "freefree";

void setup() {
  Serial.begin(115200);
  delay(1000);

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
}



// #include <ESP8266WiFi.h>

// const char* ssid = "Adam Jee";
// const char* password = "Hamza786";

// void setup() {
//   // put your setup code here, to run once:


//   // wifi connection
//   Serial.begin(115200);
//   delay(1000);

//   WiFi.mode(WIFI_STA);
//   WiFi.disconnect();
//   delay(1000);

//   Serial.println("\nScanning...");
//   int n = WiFi.scanNetworks();
//   for (int i = 0; i < n; i++) {
//     Serial.print(i + 1);
//     Serial.print(": ");
//     Serial.print(WiFi.SSID(i));
//     Serial.print(" | Ch: ");
//     Serial.print(WiFi.channel(i));
//     Serial.print(" | RSSI: ");
//     Serial.println(WiFi.RSSI(i));
//   }

//   Serial.print("\nConnecting to: ");
//   Serial.println(ssid);

//   WiFi.begin(ssid, password);

//   int counter = 0;
//   while (WiFi.status() != WL_CONNECTED) {
//     delay(500);
//     Serial.print(".");
//     counter++;
//     if (counter > 40) {
//       Serial.println("\n❌ Failed! Status:");
//       Serial.println(WiFi.status());
//       return;
//     }
//   }

//   Serial.println("\n✅ Connected!");
//   Serial.print("IP: ");
//   Serial.println(WiFi.localIP());

//   if (WiFi.status() == WL_CONNECTED) {
//     pinMode(D4, OUTPUT);
//   }
// }

// void loop() {
//   // put your main code here, to run repeatedly:
//   if (WiFi.status() == WL_CONNECTED) {
//     digitalWrite(D4, HIGH);
//     delay(1000);
//     digitalWrite(D4, LOW);
//     delay(1000);
//   }
// }
