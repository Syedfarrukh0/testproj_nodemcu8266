#include <ESP8266WiFi.h>

const char* ssid = "Adam Jee";
const char* password = "Hamza786";

void setup() {
  // put your setup code here, to run once:


  // wifi connection
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(1000);

  Serial.println("\nScanning...");
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(WiFi.SSID(i));
    Serial.print(" | Ch: ");
    Serial.print(WiFi.channel(i));
    Serial.print(" | RSSI: ");
    Serial.println(WiFi.RSSI(i));
  }

  Serial.print("\nConnecting to: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    counter++;
    if (counter > 40) {
      Serial.println("\n❌ Failed! Status:");
      Serial.println(WiFi.status());
      return;
    }
  }

  Serial.println("\n✅ Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  if (WiFi.status() == WL_CONNECTED) {
    pinMode(D4, OUTPUT);
  }
}

void loop() {
  // put your main code here, to run repeatedly:
  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(D4, HIGH);
    delay(1000);
    digitalWrite(D4, LOW);
    delay(1000);
  }
}
