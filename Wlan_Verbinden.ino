#include <WiFi.h>
#include <WebServer.h>
#include "SPIFFS.h"

// WLAN-Daten
const char* ssid = "Wlan-Akinci";
const char* password = "12345678akinci";

WebServer server(80); // HTTP-Server auf Port 80

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n[INFO] Starte ESP32...");
  
  // SPIFFS initialisieren
  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS konnte nicht gestartet werden!");
    return;
  }

  // Beispiel-TXT-Datei erstellen (nur beim ersten Start)
  if (!SPIFFS.exists("/daten.txt")) {
    File file = SPIFFS.open("/daten.txt", FILE_WRITE);
    if (file) {
      file.println("Hallo vom ESP32!");
      file.println("Diese Datei wird über WLAN angezeigt.");
      file.close();
    } else {
      Serial.println("[ERROR] Datei konnte nicht erstellt werden!");
    }
  }

  // WLAN verbinden mit Timeout
  Serial.printf("[INFO] Verbinde mit WLAN: %s\n", ssid);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    Serial.print(".");
    delay(500); // gibt anderen Tasks Zeit → kein WDT-Problem
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OK] WLAN verbunden!");
    Serial.print("[INFO] IP-Adresse: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[ERROR] WLAN-Verbindung fehlgeschlagen!");
    // Optional: Access Point als Fallback starten
    WiFi.softAP("ESP32_AP", "12345678");
    Serial.print("[INFO] AP gestartet, IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Route für Datei-Download
  server.on("/", []() {
    File file = SPIFFS.open("/daten.txt", FILE_READ);
    if (!file) {
      server.send(404, "text/plain", "Datei nicht gefunden!");
      return;
    }
    server.streamFile(file, "text/plain");
    file.close();
  });

  // Webserver starten
  server.begin();
  Serial.println("[OK] Webserver gestartet!");
}

void loop() {
  server.handleClient();
  
  // Kleine Debug-Ausgabe alle 10 Sekunden
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 10000) {
    Serial.println("[INFO] Loop läuft...");
    lastDebug = millis();
  }
}