#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>

const char* ssid = "Wlan-Akinci";
const char* password = "12345678akinci";

WebServer server(80);

void setup() {
  Serial.begin(115200);
  Serial.println("\n🔧 Starte Personenzähler (Volkan)");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n✅ WLAN verbunden!");
  Serial.print("🌐 IP: ");
  Serial.println(WiFi.localIP());

  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS konnte nicht gestartet werden!");
    return;
  }

  server.on("/", []() {
    File file = LittleFS.open("/index.html", "r");
    if (!file) {
      server.send(404, "text/plain", "Datei nicht gefunden");
      return;
    }
    server.streamFile(file, "text/html");
    file.close();
  });

  server.begin();
  Serial.println("🚀 Webserver läuft!");
}

void loop() {
  server.handleClient();
}
