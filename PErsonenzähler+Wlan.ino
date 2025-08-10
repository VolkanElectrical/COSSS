/* ===== ESP32 Zwei-Sensor Personenzaehler – Webserver + /data.json + Upload + Persistenz =====
 * Pins: Sensor A TRIG=7 ECHO=6 | Sensor B TRIG=4 ECHO=3
 * Hysterese: ENTER 90 cm, EXIT 110 cm
 * Web:  /            -> index.html (LittleFS)
 *       /data.json   -> Live-Zustand (JSON)
 *       /upload      -> Datei-Upload (LittleFS) [Passwort]
 *       /reset       -> Zaehler zuruecksetzen
 * Optional: OTA (IDE -> Netzwerk-Upload)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>

// ===== WLAN =====
const char* ssid     = "Wlan-Akinci";
const char* password = "12345678akinci";

// ===== Upload-Passwort (für /upload) =====
const char* UPLOAD_PASS = "volkan";

// ===== Pins (deine Belegung) =====
#define TRIG_PIN_A  7
#define ECHO_PIN_A  6
#define TRIG_PIN_B  4
#define ECHO_PIN_B  3

// ===== Schwellwerte / Zeiten =====
const float TH_ENTER_CM = 90.0;    // unter -> "unter"
const float TH_EXIT_CM  = 110.0;   // über -> "frei"

const unsigned long MIN_ACTIVE_TIME = 200;   // ms Mindestdauer unterhalb ENTER
const unsigned long EXIT_HOLD_MS    = 150;   // ms über EXIT bleiben
const unsigned long REARM_OFF_MS    = 200;   // ms frei sein vor neuem Start

const unsigned long SENSOR_GAP_MS   = 30;    // ms Abstand A/B Messung
const unsigned long LOOP_DELAY_MS   = 25;    // ms Loop-Takt
const uint32_t      ECHO_TIMEOUT_US = 30000; // µs pulseIn Timeout

// Vorgangsdauer / Pausen
const unsigned long MAX_PASSAGE_TIME = 6000; // ms absolute Obergrenze
const unsigned long PAUSE_MAX_MS     = 6000; // ms: beide unter -> erlaubt

// Adaptive Single-Abbruch (nur wenn kein Pause/kein 2. Sensor je unter)
const float          WAIT_OTHER_FACTOR = 2.0f;
const unsigned long  WAIT_OTHER_MIN    = 800;
const unsigned long  WAIT_OTHER_MAX    = 6000;

// Robustheit
const unsigned long LOCKOUT_AFTER_ABORT_MS = 250;
const unsigned long STUCK_MS = 5000;
const float MIN_VALID_CM = 5.0;
const float MAX_VALID_CM = 400.0;

// Glättung (EMA)
const float ALPHA = 0.3f;

// ===== Webserver =====
WebServer server(80);

// ===== Zähler & Diagnose =====
volatile long countIn  = 0;
volatile long countOut = 0;
volatile long current  = 0;

unsigned long abort_singleA = 0, abort_singleB = 0, abort_timeout = 0;
unsigned long discard_shortA = 0, discard_shortB = 0;
unsigned long stuckCountA = 0, stuckCountB = 0;

// ===== Laufzeit-Zustände =====
bool underA=false, underB=false;         // hysterese: derzeit "unter"?
bool sensorA_done=false, sensorB_done=false;
bool sensorA_stuck=false, sensorB_stuck=false;

unsigned long underSinceA=0, underSinceB=0;
unsigned long freeSinceA=0,  freeSinceB=0;

unsigned long firstStartTime=0;          // Zeit des ersten Unter-Ereignisses
char firstSensor='-';                    // 'A'/'B'
unsigned long firstActiveDur=0;          // Dauer der Unter-Phase des ersten
unsigned long passageStart=0;
unsigned long lockoutUntil=0;
unsigned long stuckStartA=0, stuckStartB=0;

unsigned long pauseStart=0;              // beide gleichzeitig "unter"
bool pauseOccurred=false;                // gab es jemals Pause?
bool secondEverUnder=false;              // war der zweite jemals "unter"?

// Log / UI
String last_event = "init";
unsigned long last_event_ms = 0;

// Persistenz
const char* STATE_PATH = "/state.json";
unsigned long lastPersist = 0;
const unsigned long PERSIST_EVERY_MS = 60000; // jede 60s

// ===== Helpers =====
float usToCm(long us){ return (float)us/29.1f/2.0f; }

float measureCm(int trigPin,int echoPin,uint32_t timeout_us){
  digitalWrite(trigPin,LOW); delayMicroseconds(2);
  digitalWrite(trigPin,HIGH); delayMicroseconds(10);
  digitalWrite(trigPin,LOW);
  long dur=pulseIn(echoPin,HIGH,timeout_us);
  if(dur==0) return -1.0f;
  return usToCm(dur);
}
bool validDist(float d){ return d>=MIN_VALID_CM && d<=MAX_VALID_CM; }
unsigned long clampUL(unsigned long v,unsigned long lo,unsigned long hi){
  if(v<lo) return lo; if(v>hi) return hi; return v;
}
void setEvent(const String& e){
  last_event = e;
  last_event_ms = millis();
}

// Hysterese-Update
void updateHysteresis(float v, bool &under, unsigned long &underSince, unsigned long &freeSince){
  const unsigned long now = millis();
  if (v < 0) return;

  if (under) {
    if (v > TH_EXIT_CM) {
      if (freeSince == 0) freeSince = now;
      if (now - freeSince >= EXIT_HOLD_MS) {
        under = false;
        // underSince bleibt für Auswertung erhalten, freeSince behält "frei seit"
      }
    } else {
      freeSince = 0;
    }
  } else {
    if (v < TH_ENTER_CM) {
      if (freeSince == 0) freeSince = now;
      if (now - freeSince >= REARM_OFF_MS) {
        under = true;
        underSince = now;
        freeSince = 0;
      }
    } else {
      if (freeSince == 0) freeSince = now;
    }
  }
}

// Reset eines Vorgangs
void resetPassage(const char* reason){
  sensorA_done=sensorB_done=false;
  firstStartTime=0; firstSensor='-'; firstActiveDur=0;
  passageStart=0; pauseStart=0;
  pauseOccurred=false; secondEverUnder=false;
  lockoutUntil=millis()+LOCKOUT_AFTER_ABORT_MS;
  setEvent(String("reset:")+reason);
}

// ===== Persistenz =====
void saveState(){
  DynamicJsonDocument doc(512);
  doc["in"] = countIn;
  doc["out"] = countOut;
  doc["current"] = current;
  doc["t"] = millis();

  File f = LittleFS.open(STATE_PATH, "w");
  if(!f) return;
  serializeJson(doc, f);
  f.close();
}
void loadState(){
  if(!LittleFS.exists(STATE_PATH)) return;
  File f = LittleFS.open(STATE_PATH, "r");
  if(!f) return;
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if(err) return;
  countIn  = doc["in"] | 0;
  countOut = doc["out"] | 0;
  current  = doc["current"] | (countIn - countOut);
}

// ===== Webserver Hilfen =====
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
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(file, contentTypeFromFilename(path));
  file.close();
  return true;
}

// ===== Setup =====
void setup(){
  Serial.begin(115200);

  pinMode(TRIG_PIN_A,OUTPUT); pinMode(ECHO_PIN_A,INPUT);
  pinMode(TRIG_PIN_B,OUTPUT); pinMode(ECHO_PIN_B,INPUT);
  digitalWrite(TRIG_PIN_A,LOW); digitalWrite(TRIG_PIN_B,LOW);

  // WLAN
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("WLAN verbinden");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nVerbunden: %s  IP=%s\n", ssid, WiFi.localIP().toString().c_str());

  // LittleFS
  if(!LittleFS.begin(true)){
    Serial.println("LittleFS Start FEHLER");
  }
  loadState();

  // Routen
  server.on("/", HTTP_GET, [](){
    if(!streamFileOr404("/index.html")) server.send(404,"text/plain","index.html fehlt");
  });
  server.on("/data.json", HTTP_GET, [](){
    DynamicJsonDocument doc(1024);
    doc["in"]      = countIn;
    doc["out"]     = countOut;
    doc["current"] = current;
    doc["ab_singleA"] = abort_singleA;
    doc["ab_singleB"] = abort_singleB;
    doc["ab_timeout"] = abort_timeout;
    doc["discardA"]   = discard_shortA;
    doc["discardB"]   = discard_shortB;
    doc["stuckA"]     = stuckCountA;
    doc["stuckB"]     = stuckCountB;
    doc["last_event"] = last_event;
    doc["last_ms"]    = last_event_ms;
    doc["uptime_ms"]  = millis();
    String payload; serializeJson(doc, payload);
    server.sendHeader("Cache-Control","no-store");
    server.send(200,"application/json",payload);
  });
  // Reset-Zähler
  server.on("/reset", HTTP_POST, [](){
    countIn = countOut = current = 0;
    abort_singleA=abort_singleB=abort_timeout=0;
    discard_shortA=discard_shortB=0;
    stuckCountA=stuckCountB=0;
    saveState();
    setEvent("manual_reset");
    server.send(200,"text/plain","OK");
  });
  // Upload-Seite
  server.on("/upload", HTTP_GET, [](){
    String page =
      "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Upload</title></head><body style='font-family:system-ui;padding:16px;background:#0b1220;color:#e6edf3'>"
      "<h2>LittleFS Upload</h2>"
      "<form method='POST' action='/upload' enctype='multipart/form-data'>"
      "Passwort: <input type='password' name='pass' required><br><br>"
      "Datei: <input type='file' name='data' required><br><br>"
      "<button>Hochladen</button></form><p><a href='/'>Zurück</a></p>"
      "</body></html>";
    server.send(200,"text/html",page);
  });
  static File _uploadFile;
  server.on("/upload", HTTP_POST,
    [](){ server.send(200,"text/plain","Upload OK"); },
    [&](){
      if(!server.hasArg("pass") || server.arg("pass")!=UPLOAD_PASS){ server.send(401,"text/plain","Unauthorized"); return; }
      HTTPUpload& up = server.upload();
      if(up.status==UPLOAD_FILE_START){
        String fn = up.filename; if(!fn.startsWith("/")) fn="/"+fn;
        if(LittleFS.exists(fn)) LittleFS.remove(fn);
        _uploadFile = LittleFS.open(fn,"w");
      } else if(up.status==UPLOAD_FILE_WRITE){
        if(_uploadFile) _uploadFile.write(up.buf, up.currentSize);
      } else if(up.status==UPLOAD_FILE_END){
        if(_uploadFile) _uploadFile.close();
      }
    }
  );

  server.onNotFound([](){ if(streamFileOr404(server.uri())) return; server.send(404,"text/plain","404: "+server.uri()); });

  server.begin();
  Serial.println("Webserver läuft.");

  // OTA (optional, kann entfernt werden)
  ArduinoOTA.setHostname("ESP32-Personenzaehler");
  ArduinoOTA.begin();
}

// ===== Hauptlogik =====
void loop(){
  const unsigned long now=millis();

  // --- Messen (A dann B) ---
  auto readDist = [&](int t,int e)->float{
    float d = measureCm(t,e,ECHO_TIMEOUT_US);
    if(!validDist(d)) d=-1;
    return d;
  };
  float dA=readDist(TRIG_PIN_A,ECHO_PIN_A);
  delay(SENSOR_GAP_MS);
  float dB=readDist(TRIG_PIN_B,ECHO_PIN_B);

  // EMA
  static float emaA=NAN, emaB=NAN;
  if(dA>0) emaA=isnan(emaA)? dA : (ALPHA*dA+(1-ALPHA)*emaA);
  if(dB>0) emaB=isnan(emaB)? dB : (ALPHA*dB+(1-ALPHA)*emaB);
  float vA=(dA>0 && !isnan(emaA))? emaA : dA;
  float vB=(dB>0 && !isnan(emaB))? emaB : dB;

  bool inLockout=(now<lockoutUntil);

  // STUCK
  if(vA>0 && vA<TH_ENTER_CM){ if(!sensorA_stuck){ if(!stuckStartA) stuckStartA=now; else if(now-stuckStartA>STUCK_MS){ sensorA_stuck=true; stuckCountA++; setEvent("A_stuck"); } } }
  else { stuckStartA=0; if(sensorA_stuck && (vA>=TH_EXIT_CM || vA<0)){ sensorA_stuck=false; setEvent("A_recovered"); } }

  if(vB>0 && vB<TH_ENTER_CM){ if(!sensorB_stuck){ if(!stuckStartB) stuckStartB=now; else if(now-stuckStartB>STUCK_MS){ sensorB_stuck=true; stuckCountB++; setEvent("B_stuck"); } } }
  else { stuckStartB=0; if(sensorB_stuck && (vB>=TH_EXIT_CM || vB<0)){ sensorB_stuck=false; setEvent("B_recovered"); } }

  // Hysterese-Update
  bool prevUnderA=underA, prevUnderB=underB;
  if(!inLockout && !sensorA_stuck) updateHysteresis(vA, underA, underSinceA, freeSinceA);
  if(!inLockout && !sensorB_stuck) updateHysteresis(vB, underB, underSinceB, freeSinceB);

  // Start-Events
  if(!prevUnderA && underA){
    if(firstSensor=='-'){ firstSensor='A'; firstStartTime=now; if(passageStart==0) passageStart=now; }
    setEvent("A_start");
  }
  if(!prevUnderB && underB){
    if(firstSensor=='-'){ firstSensor='B'; firstStartTime=now; if(passageStart==0) passageStart=now; }
    setEvent("B_start");
  }

  // „zweiter Sensor je unter?“
  if(firstSensor=='A' && underB) secondEverUnder = true;
  if(firstSensor=='B' && underA) secondEverUnder = true;

  // Done/Discard beim Verlassen
  if (prevUnderA && !underA){
    unsigned long endT = (freeSinceA ? freeSinceA : now);
    unsigned long active = (underSinceA>0 && endT>=underSinceA) ? (endT-underSinceA) : 0;
    if (active >= MIN_ACTIVE_TIME){
      sensorA_done = true;
      if (firstSensor=='A') firstActiveDur = active;
      setEvent("A_done");
    } else {
      discard_shortA++; setEvent("A_discard");
    }
  }
  if (prevUnderB && !underB){
    unsigned long endT = (freeSinceB ? freeSinceB : now);
    unsigned long active = (underSinceB>0 && endT>=underSinceB) ? (endT-underSinceB) : 0;
    if (active >= MIN_ACTIVE_TIME){
      sensorB_done = true;
      if (firstSensor=='B') firstActiveDur = active;
      setEvent("B_done");
    } else {
      discard_shortB++; setEvent("B_discard");
    }
  }

  // Pause: beide unter -> merken
  bool bothBelow = underA && underB;
  if(bothBelow){
    if(pauseStart==0){ pauseStart=now; setEvent("pause_start"); }
    pauseOccurred = true;
  } else {
    if(pauseStart>0){ setEvent("pause_end"); }
    pauseStart=0;
  }

  // Beide done -> werten
  if(sensorA_done && sensorB_done){
    bool valid = !(passageStart && (now - passageStart > MAX_PASSAGE_TIME));
    if(valid){
      if(firstSensor=='A'){ countOut++; if(current>0) current--; setEvent("OUT"); }
      else               { countIn++;  current++;            setEvent("IN");  }
      saveState(); // nach erfolgreicher Wertung sichern
    } else {
      abort_timeout++; setEvent("abort:timeout_both");
    }
    resetPassage("eval");
  }

  // Nur einer done -> warten oder adaptiv abbrechen
  if((sensorA_done ^ sensorB_done)){
    bool relaxAbort = pauseOccurred || secondEverUnder;
    if(relaxAbort){
      if(passageStart && (now - passageStart > MAX_PASSAGE_TIME)){
        abort_timeout++; setEvent("abort:timeout_relaxed");
        resetPassage("timeout-relaxed");
      }
    } else {
      unsigned long waitDyn = clampUL((unsigned long)(firstActiveDur * WAIT_OTHER_FACTOR), WAIT_OTHER_MIN, WAIT_OTHER_MAX);
      if(now - firstStartTime > waitDyn){
        if(sensorA_done && !sensorB_done){ abort_singleA++; setEvent("abort:single_A"); }
        if(sensorB_done && !sensorA_done){ abort_singleB++; setEvent("abort:single_B"); }
        resetPassage("single-adaptive");
      }
    }
  }

  // Absolute Safety
  if(passageStart && (now - passageStart > MAX_PASSAGE_TIME)){
    abort_timeout++; setEvent("abort:maxpass");
    resetPassage("max-pass");
  }

  // Periodisch sichern
  if(now - lastPersist > PERSIST_EVERY_MS){
    saveState(); lastPersist = now;
  }

  server.handleClient();
  ArduinoOTA.handle(); // optional
  delay(LOOP_DELAY_MS);
}
