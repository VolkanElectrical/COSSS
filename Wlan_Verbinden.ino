#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>  // Optional: OTA

// ======= WLAN-Zugangsdaten =======
const char* ssid     = "Wlan-Akinci";
const char* password = "12345678akinci";

// ======= (Einfaches) Upload-Passwort für /upload =======
// Bitte ändern! (nur Buchstaben/Zahlen, kein Sonderzeichen nötig)
const char* UPLOAD_PASS = "volkan";

// ======= Globale Zählerwerte (Beispiel) =======
// -> Später durch echte Sensormessung anpassen/inkrementieren
volatile long countIn  = 0;
volatile long countOut = 0;

WebServer server(80);

// ---- Hilfsfunktionen ----
String contentTypeFromFilename(const String& filename) {
  if (filename.endsWith(".html")) return "text/html";
  if (filename.endsWith(".css"))  return "text/css";
  if (filename.endsWith(".js"))   return "application/javascript";
  if (filename.endsWith(".json")) return "application/json";
  if (filename.endsWith(".png"))  return "image/png";
  if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) return "image/jpeg";
  if (filename.endsWith(".ico"))  return "image/x-icon";
  if (filename.endsWith(".svg"))  return "image/svg+xml";
  return "text/plain";
}

bool streamFileOr404(const String& path) {
  if (!LittleFS.exists(path)) return false;
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  String ctype = contentTypeFromFilename(path);
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(file, ctype);
  file.close();
  return true;
}

String makeJson() {
  // Für Live-Demo: simulierte Bewegung alle ~5s
  // -> Entfernen, wenn du echte Sensorwerte nutzt
  static unsigned long last = 0;
  if (millis() - last > 5000) {
    last = millis();
    countIn++;
    if (countIn % 3 == 0 && countOut < countIn) countOut++;
  }

  long inNow  = countIn;
  long outNow = countOut;
  long current = inNow - outNow;

  String json = "{";
  json += "\"in\":" + String(inNow) + ",";
  json += "\"out\":" + String(outNow) + ",";
  json += "\"current\":" + String(current) + ",";
  json += "\"ts\":" + String(millis());
  json += "}";
  return json;
}

// ---- Upload-Handling (/upload) ----
File _uploadFile;

void handleUploadPost() {
  // Wird nach Abschluss des Uploads aufgerufen
  server.send(200, "text/plain", "Upload OK. Zurueck mit /upload");
}

void handleUpload() {
  // Prüfe Passwort
  if (!server.hasArg("pass") || server.arg("pass") != UPLOAD_PASS) {
    server.send(401, "text/plain", "Unauthorized: falsches Passwort");
    return;
  }

  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    // Falls es die Datei gibt, löschen
    if (LittleFS.exists(filename)) LittleFS.remove(filename);
    _uploadFile = LittleFS.open(filename, "w");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (_uploadFile) _uploadFile.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (_uploadFile) {
      _uploadFile.close();
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n🔧 Starte Personenzaehler (Volkan)");

  // WLAN verbinden
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("⏳ Verbinde mit WLAN");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WLAN verbunden!");
  Serial.print("🌐 IP: ");
  Serial.println(WiFi.localIP());

  // LittleFS starten
  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS konnte nicht gestartet werden!");
    return;
  }

  // ---- Routen ----

  // Startseite: / oder /index.html aus LittleFS
  server.on("/", HTTP_GET, []() {
    if (!streamFileOr404("/index.html")) {
      server.send(404, "text/plain", "index.html nicht gefunden");
    }
  });
  server.on("/index.html", HTTP_GET, []() {
    if (!streamFileOr404("/index.html")) {
      server.send(404, "text/plain", "index.html nicht gefunden");
    }
  });

  // Live-Daten
  server.on("/data.json", HTTP_GET, []() {
    String payload = makeJson();
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", payload);
  });

  // Dateien direkt aus LittleFS holen (z.B. /style.css, /app.js, /favicon.ico)
  server.onNotFound([]() {
    String path = server.uri();
    if (streamFileOr404(path)) return;
    server.send(404, "text/plain", "404: " + path + " nicht gefunden");
  });

  // Upload-Seite (Formular)
  server.on("/upload", HTTP_GET, []() {
    String page =
      "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Upload</title></head><body style='font-family:system-ui;padding:16px'>"
      "<h1>LittleFS Upload</h1>"
      "<form method='POST' action='/upload' enctype='multipart/form-data'>"
      "<p>Passwort: <input type='password' name='pass' required></p>"
      "<p>Datei: <input type='file' name='data' required></p>"
      "<p><button type='submit'>Hochladen</button></p>"
      "<p>Hinweis: Der Dateiname wird exakt so im LittleFS gespeichert (z.B. index.html).</p>"
      "</form>"
      "<p><a href='/'>Zur Startseite</a></p>"
      "</body></html>";
    server.send(200, "text/html", page);
  });

  // Upload-Handler (POST + Datenstrom)
  server.on(
    "/upload",
    HTTP_POST,
    handleUploadPost,
    handleUpload
  );

  server.begin();
  Serial.println("🚀 Webserver läuft!");

  // ---- Optional: OTA aktivieren (IDE → Netzwerk-Upload) ----
  ArduinoOTA.setHostname("ESP32-Personenzaehler");
  ArduinoOTA.begin();
  Serial.println("📡 OTA bereit (Arduino IDE → Netzwerk-Ports).");
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();  // Optional: OTA
}
