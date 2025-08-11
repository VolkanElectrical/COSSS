/******************************************************
 * ESP32 Personenzaehler – WLAN + Webserver + JSON API
 * (Sensorlogik unverändert; nur Event-Hooks ergänzt)
 *
 * Abhängigkeiten: keine externen Libs
 *  - <WiFi.h>, <WebServer.h>, <LittleFS.h>, <FS.h>, <time.h>
 ******************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <time.h>

// ===== WLAN-Daten =====
const char* ssid     = "Wlan-Akinci";
const char* password = "12345678akinci";

// ===== Webserver =====
WebServer server(80);

// ===== Kapazität (für Dashboard) =====
const int CAPACITY_LIMIT = 120;

// ===== Zeit/Zeitzone =====
// Berlin (CET/CEST):
const char* TZ_EU_BERLIN = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// ===== Persistenz =====
struct DayRecord {
  int in[24];
  int out[24];
};
DayRecord todayRec;

String currentISODate = "";
unsigned long lastPersistMs = 0;
const unsigned long PERSIST_INTERVAL_MS = 30 * 1000; // alle 30s mindestens einmal sichern

// Jahres-/Gesamt-Totale (nur zur Auskunft; werden bei Boot aus FS summiert)
uint64_t totalInAll = 0;
uint64_t totalOutAll = 0;

// ===== Event-Log (Live) =====
struct EventItem { uint64_t ts_ms; bool dirIn; };
const size_t EVENTS_MAX = 300;
EventItem eventsBuf[EVENTS_MAX];
size_t eventsCount = 0; // <= EVENTS_MAX
size_t eventsHead  = 0;

uint64_t nowEpochMs() {
  time_t s = time(nullptr);
  if (s <= 100000) return 0; // noch keine Zeit
  return (uint64_t)s * 1000ULL;
}

void pushEvent(bool dirIn){
  EventItem e;
  e.ts_ms = nowEpochMs();
  e.dirIn = dirIn;
  eventsBuf[eventsHead] = e;
  eventsHead = (eventsHead + 1) % EVENTS_MAX;
  if (eventsCount < EVENTS_MAX) eventsCount++;
}

// ==== LittleFS Hilfen ====
String dayPathCSV(const String& iso) { return "/days/" + iso + ".csv"; }

bool saveDayCSV(const String& iso, const DayRecord& rec){
  // Verzeichnis sicherstellen
  LittleFS.mkdir("/days");
  File f = LittleFS.open(dayPathCSV(iso), FILE_WRITE);
  if(!f) return false;
  f.println("hour,in,out");
  for(int h=0; h<24; ++h){
    f.printf("%02d,%d,%d\n", h, rec.in[h], rec.out[h]);
  }
  f.close();
  return true;
}

bool loadDayCSV(const String& iso, DayRecord& rec){
  for(int h=0; h<24; ++h){ rec.in[h]=0; rec.out[h]=0; }
  File f = LittleFS.open(dayPathCSV(iso), FILE_READ);
  if(!f) return false;
  // skip header
  String header = f.readStringUntil('\n');
  while(f.available()){
    String line = f.readStringUntil('\n');
    line.trim();
    if(line.length()==0) continue;
    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1+1);
    if(c1<0 || c2<0) continue;
    int h = line.substring(0,c1).toInt();
    int vin = line.substring(c1+1,c2).toInt();
    int vout= line.substring(c2+1).toInt();
    if(h>=0 && h<24){ rec.in[h]=vin; rec.out[h]=vout; }
  }
  f.close();
  return true;
}

// Summiert alle CSVs beim Boot, um totalInAll/totalOutAll zu setzen
void sumAllTotalsFromFS(){
  totalInAll = 0; totalOutAll = 0;
  File dir = LittleFS.open("/days");
  if(!dir || !dir.isDirectory()) return;
  File f;
  while((f = dir.openNextFile())){
    if(!f.isDirectory()){
      String name = f.name();
      if(name.endsWith(".csv")){
        // summieren
        // einfache Summierung über die Datei:
        // (wir lesen pro Zeile hour,in,out)
        String dummy = f.readStringUntil('\n'); // header
        while(f.available()){
          String line = f.readStringUntil('\n');
          line.trim();
          if(line.length()==0) continue;
          int c1 = line.indexOf(',');
          int c2 = line.indexOf(',', c1+1);
          if(c1<0 || c2<0) continue;
          int vin = line.substring(c1+1,c2).toInt();
          int vout= line.substring(c2+1).toInt();
          totalInAll  += vin;
          totalOutAll += vout;
        }
      }
    }
    f.close();
  }
}

// ===== Datum/Zeit =====
bool getLocalTM(struct tm* tminfo){
  time_t nowSec = time(nullptr);
  if(nowSec <= 100000) return false;
  localtime_r(&nowSec, tminfo);
  return true;
}

String makeISODate(const struct tm& tm){
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return String(buf);
}

int currentHour(){
  struct tm tm;
  if(!getLocalTM(&tm)) return 0;
  return tm.tm_hour;
}

String todayISO(){
  struct tm tm;
  if(!getLocalTM(&tm)) return String("1970-01-01");
  return makeISODate(tm);
}

void rotateDayIfNeeded(){
  String iso = todayISO();
  if(currentISODate != iso){
    // alten Tag sichern (falls existent)
    if(currentISODate.length()){
      saveDayCSV(currentISODate, todayRec);
    }
    // neuen Tag beginnen
    currentISODate = iso;
    for(int i=0;i<24;i++){ todayRec.in[i]=0; todayRec.out[i]=0; }
  }
}

// ===== JSON-Helfer =====
String jsonArray24(const int a[24]){
  String s = "[";
  for(int i=0;i<24;i++){
    s += String(a[i]);
    if(i<23) s += ",";
  }
  s += "]";
  return s;
}

void handleApiCurrent(){
  rotateDayIfNeeded();
  // Summen heute
  int sumIn=0, sumOut=0;
  for(int i=0;i<24;i++){ sumIn+=todayRec.in[i]; sumOut+=todayRec.out[i]; }
  // "current" (Anwesende) kommt aus deiner Logik -> globaler Zähler:
  extern long current; // kommt aus dem Sensor-Teil unten
  extern long countIn; extern long countOut;
  uint64_t ts = nowEpochMs();
  // Totale: bereits aufsummiert + heute (falls noch nicht persistiert)
  uint64_t totalIn  = totalInAll  + sumIn;
  uint64_t totalOut = totalOutAll + sumOut;

  String json = "{";
  json += "\"present\":" + String(current) + ",";
  json += "\"in_today\":" + String(sumIn) + ",";
  json += "\"out_today\":" + String(sumOut) + ",";
  json += "\"in_total\":" + String(totalIn) + ",";
  json += "\"out_total\":" + String(totalOut) + ",";
  json += "\"limit\":" + String(CAPACITY_LIMIT) + ",";
  json += "\"ts\":" + String(ts);
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiDay(){
  String date = server.hasArg("date") ? server.arg("date") : todayISO();
  DayRecord rec;
  if(date == currentISODate){
    rec = todayRec;
  } else {
    if(!loadDayCSV(date, rec)){
      server.send(404, "application/json", "{\"error\":\"no data for that date\"}");
      return;
    }
  }
  String json = "{";
  json += "\"date\":\""+date+"\",";
  json += "\"in\":" + jsonArray24(rec.in) + ",";
  json += "\"out\":" + jsonArray24(rec.out) + ",";
  // present (Belegung) stündlich ableiten:
  int present[24]; int p=0;
  for(int h=0; h<24; ++h){ p += rec.in[h]; p -= rec.out[h]; if(p<0) p=0; present[h]=p; }
  json += "\"present\":" + jsonArray24(present);
  json += "}";
  server.send(200, "application/json", json);
}

void handleApiYear(){
  String ys = server.hasArg("year") ? server.arg("year") : String();
  int y = ys.length() ? ys.toInt() : 0;

  String out = "[";
  bool first = true;

  // Wenn Year angegeben: nur dieses Jahr, ansonsten das Jahr des Systems
  File dir = LittleFS.open("/days");
  if(dir && dir.isDirectory()){
    File f;
    while((f = dir.openNextFile())){
      if(!f.isDirectory()){
        String name = f.name(); // "/days/2025-08-10.csv"
        if(!name.endsWith(".csv")){ f.close(); continue; }
        String base = name.substring(String("/days/").length()); // "2025-08-10.csv"
        String iso  = base.substring(0, 10); // "2025-08-10"
        int yearFile = iso.substring(0,4).toInt();
        if(y!=0 && yearFile!=y){ f.close(); continue; }

        // Summe dieses Tages
        int sumIn=0, sumOut=0;
        // skip header
        String header = f.readStringUntil('\n');
        while(f.available()){
          String line = f.readStringUntil('\n'); line.trim();
          if(line.length()==0) continue;
          int c1=line.indexOf(','), c2=line.indexOf(',', c1+1);
          if(c1<0||c2<0) continue;
          sumIn  += line.substring(c1+1,c2).toInt();
          sumOut += line.substring(c2+1).toInt();
        }

        if(!first) out += ",";
        out += "{\"date\":\""+iso+"\",\"in\":"+String(sumIn)+",\"out\":"+String(sumOut)+"}";
        first=false;
      }
      f.close();
    }
  }

  // Heute evtl. hinzufügen, falls Jahr passt und noch nicht abgelegt
  String isoToday = todayISO();
  int yToday = isoToday.substring(0,4).toInt();
  if((y==0 || y==yToday)){
    // prüfen, ob heute bereits als Datei existiert
    if(!LittleFS.exists(dayPathCSV(isoToday))){
      int sumIn=0,sumOut=0;
      for(int h=0;h<24;h++){ sumIn+=todayRec.in[h]; sumOut+=todayRec.out[h]; }
      if(!first) out += ",";
      out += "{\"date\":\""+isoToday+"\",\"in\":"+String(sumIn)+",\"out\":"+String(sumOut)+"}";
      first=false;
    }
  }

  out += "]";
  server.send(200, "application/json", out);
}

void handleApiEvents(){
  int limit = server.hasArg("limit") ? server.arg("limit").toInt() : 50;
  if(limit < 1) limit = 1;
  if(limit > (int)EVENTS_MAX) limit = EVENTS_MAX;

  // neueste zuerst
  String out = "[";
  bool first = true;
  int emitted = 0;
  for(int i=0; i<(int)eventsCount && emitted<limit; ++i){
    int idx = ( (int)eventsHead - 1 - i );
    while(idx < 0) idx += EVENTS_MAX;
    const EventItem &e = eventsBuf[idx];
    if(!first) out += ",";
    out += "{\"ts\":" + String((unsigned long long)e.ts_ms) + ",\"dir\":\"";
    out += (e.dirIn ? "in" : "out");
    out += "\"}";
    first=false;
    emitted++;
  }
  out += "]";
  server.send(200, "application/json", out);
}

// ====== Static File / UI ======
/* Fallback-HTML (kurz). Besser: index.html in LittleFS laden.
 * Du kannst unten die Konstante durch DEIN großes HTML ersetzen.
 */
const char index_html_fallback[] PROGMEM = R"HTML(
<!doctype html><html lang="de"><meta charset="utf-8">
<title>ESP32 Personenzähler</title>
<body style="font-family:system-ui;margin:2rem">
<h1>ESP32 Personenzähler</h1>
<p>Die vollständige Dashboard-UI wurde nicht gefunden.
Lade <code>/index.html</code> in LittleFS hoch <em>oder</em> ersetze im Sketch die Fallback-HTML durch deine Vorlage.</p>
<p>Verfügbare Endpunkte:</p>
<ul>
<li><code>/api/current</code></li>
<li><code>/api/day?date=YYYY-MM-DD</code></li>
<li><code>/api/year?year=YYYY</code></li>
<li><code>/api/events?limit=50</code></li>
</ul>
</body></html>
)HTML";

// Wenn du deine komplette HTML hier einbetten willst, ersetze den Inhalt zwischen R"HTML(... )HTML":
const char index_html_full[] PROGMEM = R"HTML(
<!-- >>> HIER DEINE GESAMTE HTML-VORLAGE EINFÜGEN (aus deiner Nachricht) <<< -->
)HTML";

void handleRoot(){
  // 1) Falls /index.html in LittleFS, liefere diese
  if(LittleFS.exists("/index.html")){
    File f = LittleFS.open("/index.html", FILE_READ);
    if(f){
      server.streamFile(f, "text/html; charset=utf-8");
      f.close();
      return;
    }
  }
  // 2) Falls im Sketch eine vollständige Variante hinterlegt ist (mehr als Platzhalter)
  if (strstr_P(index_html_full, "HIER DEINE GESAMTE HTML-VORLAGE") == nullptr) {
    server.send_P(200, "text/html; charset=utf-8", index_html_full);
    return;
  }
  // 3) Fallback
  server.send_P(200, "text/html; charset=utf-8", index_html_fallback);
}

void setupWeb(){
  // Routen
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/current", HTTP_GET, handleApiCurrent);
  server.on("/api/day", HTTP_GET, handleApiDay);
  server.on("/api/year", HTTP_GET, handleApiYear);
  server.on("/api/events", HTTP_GET, handleApiEvents);
  server.onNotFound([](){
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  });
  server.begin();
}

// ===================================================
// =============  DEINE SENSORLOGIK  =================
// (nur mit minimalen Hooks zum Loggen/Persistieren)
// ===================================================

/***** 2x HC-SR04 Richtungszähler – „kein Früh-Abort nach Warten bei A“ *****/
/* Pins: A TRIG=7 ECHO=6  |  B TRIG=4 ECHO=3  |  ENTER=90cm, EXIT=110cm */

#define TRIG_PIN_A  4
#define ECHO_PIN_A  3
#define TRIG_PIN_B  7
#define ECHO_PIN_B  6

// --- Schwellwerte (Hysterese) ---
const float TH_ENTER_CM = 90.0;
const float TH_EXIT_CM  = 110.0;

// --- Zeiten ---
const unsigned long MIN_ACTIVE_TIME = 200;    // ms
const unsigned long EXIT_HOLD_MS    = 150;    // ms über TH_EXIT, um „frei“ zu sein
const unsigned long REARM_OFF_MS    = 200;    // ms „frei“, bevor neuer Start erlaubt
const unsigned long MAX_PASSAGE_TIME = 6000;  // ms absolute Obergrenze (erhöht)
const unsigned long SENSOR_GAP_MS   = 30;     // ms
const unsigned long LOOP_DELAY_MS   = 25;     // ms
const uint32_t      ECHO_TIMEOUT_US = 30000;  // µs

// Adaptive Wartezeit NUR wenn der zweite Sensor NIE „unter“ war und KEINE Pause stattfand
const float          WAIT_OTHER_FACTOR = 2.0f;
const unsigned long  WAIT_OTHER_MIN    = 800;
const unsigned long  WAIT_OTHER_MAX    = 6000; // erhöht

// Robustheit
const unsigned long LOCKOUT_AFTER_ABORT_MS = 250;
const unsigned long STUCK_MS = 5000;
const float MIN_VALID_CM = 5.0;
const float MAX_VALID_CM = 400.0;

// Debug (nur Ereignisse)
#define DEBUG_LEVEL 1

// Glättung
const float ALPHA = 0.3f;

// --- Zähler ---
long countIn  = 0;
long countOut = 0;
long current  = 0;

// Diagnose
unsigned long abort_singleA = 0, abort_singleB = 0, abort_timeout = 0;
unsigned long discard_shortA = 0, discard_shortB = 0;
unsigned long stuckCountA = 0, stuckCountB = 0;

// --- Zustände/Zeitstempel ---
bool underA=false, underB=false;       // Hysterese-Zustand
bool sensorA_done=false, sensorB_done=false;
bool sensorA_stuck=false, sensorB_stuck=false;

unsigned long underSinceA=0, underSinceB=0;
unsigned long freeSinceA=0,  freeSinceB=0;

unsigned long firstStartTime=0;        // Zeit des allerersten „unter“
char firstSensor='-';                  // 'A' oder 'B'
unsigned long firstActiveDur=0;        // Dauer der Unter-Phase des ersten Sensors
unsigned long passageStart=0;
unsigned long lockoutUntil=0;
unsigned long stuckStartA=0, stuckStartB=0;

unsigned long pauseStart=0;            // beide gleichzeitig „unter“
bool pauseOccurred=false;              // Merker: gab es jemals eine Pause?
bool secondEverUnder=false;            // Merker: war der „andere“ Sensor irgendwann „unter“?

// EMA
float emaA=NAN, emaB=NAN;

// ---- Helpers ----
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

void resetPassage(const char* reason){
  sensorA_done=sensorB_done=false;
  firstStartTime=0; firstSensor='-'; firstActiveDur=0;
  passageStart=0;
  pauseStart=0; pauseOccurred=false;
  secondEverUnder=false;
  lockoutUntil=millis()+LOCKOUT_AFTER_ABORT_MS;
  if(DEBUG_LEVEL>=1) Serial.printf("↩️ Reset (%s) | Lockout %lums\n", reason, LOCKOUT_AFTER_ABORT_MS);
}

// Hysterese-Update mit Exit-Hold & Re-Arm
void updateHysteresis(float v, bool &under, unsigned long &underSince, unsigned long &freeSince){
  const unsigned long now = millis();
  if (v < 0) return;

  if (under) {
    if (v > TH_EXIT_CM) {
      if (freeSince == 0) freeSince = now;
      if (now - freeSince >= EXIT_HOLD_MS) {
        under = false;
        // underSince bleibt zur Auswertung erhalten; freeSince behält „frei seit“
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

// ===== Event-Hook (füllt Tagesprofil + Persistenz-Taktung) =====
void logEvent(bool dirIn){
  // Live-Events
  pushEvent(dirIn);

  // Tagesprofil
  rotateDayIfNeeded();
  int h = currentHour();
  if(h<0||h>23) h=0;
  if(dirIn) todayRec.in[h]++; else todayRec.out[h]++;

  // optionale Zwischenspeicherung zeitgesteuert
  unsigned long nowMs = millis();
  if(nowMs - lastPersistMs >= PERSIST_INTERVAL_MS){
    saveDayCSV(currentISODate, todayRec);
    lastPersistMs = nowMs;
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(TRIG_PIN_A,OUTPUT); pinMode(ECHO_PIN_A,INPUT);
  pinMode(TRIG_PIN_B,OUTPUT); pinMode(ECHO_PIN_B,INPUT);
  digitalWrite(TRIG_PIN_A,LOW); digitalWrite(TRIG_PIN_B,LOW);

  Serial.println("🚀 Start: Richtungszaehler – tolerant, kein Früh-Abort nach Pause/Warten");

  // ===== FS
  if(!LittleFS.begin()){
    Serial.println("⚠️ LittleFS mount failed");
  } else {
    LittleFS.mkdir("/days");
  }

  // ===== WLAN
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("🔌 Verbinde mit WLAN \"%s\" ...\n", ssid);
  unsigned long t0 = millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000){
    delay(300); Serial.print(".");
  }
  Serial.println();
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("✅ WLAN verbunden: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("❌ WLAN nicht verbunden (weiter offline) – UI funktioniert mit Demo-Daten weiterhin");
  }

  // ===== Zeit (NTP)
  setenv("TZ", TZ_EU_BERLIN, 1);
  tzset();
  configTime(0,0,"pool.ntp.org","time.nist.gov");
  Serial.print("⏱️ Warte auf Zeit ... ");
  for(int i=0;i<30;i++){
    if(time(nullptr)>100000){ Serial.println("ok"); break; }
    delay(500); Serial.print(".");
  }
  Serial.println();

  // ===== Tag initialisieren + Totale aus FS
  currentISODate = todayISO();
  for(int i=0;i<24;i++){ todayRec.in[i]=0; todayRec.out[i]=0; }
  sumAllTotalsFromFS();

  // ===== Webserver
  setupWeb();
}

void loop(){
  const unsigned long now=millis();

  // --- Messen ---
  auto read = [&](int t,int e)->float{
    float d = measureCm(t,e,ECHO_TIMEOUT_US);
    if(!validDist(d)) d=-1;
    return d;
  };
  float dA=read(TRIG_PIN_A,ECHO_PIN_A);
  delay(SENSOR_GAP_MS);
  float dB=read(TRIG_PIN_B,ECHO_PIN_B);

  // EMA
  if(dA>0) emaA=isnan(emaA)? dA : (ALPHA*dA+(1-ALPHA)*emaA);
  if(dB>0) emaB=isnan(emaB)? dB : (ALPHA*dB+(1-ALPHA)*emaB);
  float vA=(dA>0 && !isnan(emaA))? emaA : dA;
  float vB=(dB>0 && !isnan(emaB))? emaB : dB;

  bool inLockout=(now<lockoutUntil);

  // STUCK
  if(vA>0 && vA<TH_ENTER_CM){ if(!sensorA_stuck){ if(!stuckStartA) stuckStartA=now; else if(now-stuckStartA>STUCK_MS){ sensorA_stuck=true; stuckCountA++; Serial.println("🧯 A stuck -> ignore"); } } }
  else { stuckStartA=0; if(sensorA_stuck && (vA>=TH_EXIT_CM || vA<0)){ sensorA_stuck=false; Serial.println("🧯 A recovered"); } }

  if(vB>0 && vB<TH_ENTER_CM){ if(!sensorB_stuck){ if(!stuckStartB) stuckStartB=now; else if(now-stuckStartB>STUCK_MS){ sensorB_stuck=true; stuckCountB++; Serial.println("🧯 B stuck -> ignore"); } } }
  else { stuckStartB=0; if(sensorB_stuck && (vB>=TH_EXIT_CM || vB<0)){ sensorB_stuck=false; Serial.println("🧯 B recovered"); } }

  // Hysterese-Zustand aktualisieren
  bool prevUnderA=underA, prevUnderB=underB;
  if(!inLockout && !sensorA_stuck) updateHysteresis(vA, underA, underSinceA, freeSinceA);
  if(!inLockout && !sensorB_stuck) updateHysteresis(vB, underB, underSinceB, freeSinceB);

  // Start-Events
  if(!prevUnderA && underA){
    if(firstSensor=='-'){ firstSensor='A'; firstStartTime=now; if(passageStart==0) passageStart=now; }
    if(DEBUG_LEVEL>=1) Serial.printf("A↓ start @%lums\n", now);
  }
  if(!prevUnderB && underB){
    if(firstSensor=='-'){ firstSensor='B'; firstStartTime=now; if(passageStart==0) passageStart=now; }
    if(DEBUG_LEVEL>=1) Serial.printf("B↓ start @%lums\n", now);
  }

  // Merker: hat der „zweite“ Sensor irgendwann „unter“ gesehen?
  if(firstSensor=='A' && underB) secondEverUnder = true;
  if(firstSensor=='B' && underA) secondEverUnder = true;

  // Done/Discard beim Verlassen
  if (prevUnderA && !underA){
    unsigned long endT = (freeSinceA ? freeSinceA : now);
    unsigned long active = (underSinceA>0 && endT>=underSinceA) ? (endT-underSinceA) : 0;
    if (active >= MIN_ACTIVE_TIME){
      sensorA_done = true;
      if (firstSensor=='A') firstActiveDur = active;
      if (DEBUG_LEVEL>=1) Serial.printf("A↑ done (dur=%lums)\n", active);
    } else {
      discard_shortA++;
      if (DEBUG_LEVEL>=1) Serial.printf("A↑ discard (dur=%lums < %lums)\n", active, MIN_ACTIVE_TIME);
    }
  }
  if (prevUnderB && !underB){
    unsigned long endT = (freeSinceB ? freeSinceB : now);
    unsigned long active = (underSinceB>0 && endT>=underSinceB) ? (endT-underSinceB) : 0;
    if (active >= MIN_ACTIVE_TIME){
      sensorB_done = true;
      if (firstSensor=='B') firstActiveDur = active;
      if (DEBUG_LEVEL>=1) Serial.printf("B↑ done (dur=%lums)\n", active);
    } else {
      discard_shortB++;
      if (DEBUG_LEVEL>=1) Serial.printf("B↑ discard (dur=%lums < %lums)\n", active, MIN_ACTIVE_TIME);
    }
  }

  // Pause: beide unter
  bool bothBelow = underA && underB;
  if(bothBelow){
    if(pauseStart==0){ pauseStart=now; if(DEBUG_LEVEL>=1) Serial.println("⏸️ Pause begonnen"); }
    pauseOccurred = true;
  } else {
    if(pauseStart>0 && DEBUG_LEVEL>=1){
      Serial.printf("▶️ Pause beendet (dauer=%lums)\n", now - pauseStart);
    }
    pauseStart=0;
  }

  // Beide done -> werten
  if(sensorA_done && sensorB_done){
    bool valid = !(passageStart && (now - passageStart > MAX_PASSAGE_TIME));
    if(valid){
      if(firstSensor=='A'){
        countOut++; if(current>0) current--;
        Serial.printf("⬅️ OUT  | In=%ld Out=%ld Curr=%ld (first=A)\n", countIn, countOut, current);
        logEvent(false);          // <<<<<< Event-Hook
      } else {
        countIn++;  current++;
        Serial.printf("✅ IN   | In=%ld Out=%ld Curr=%ld (first=B)\n", countIn, countOut, current);
        logEvent(true);           // <<<<<< Event-Hook
      }
    } else {
      abort_timeout++; Serial.println("⚠️ Abbruch: MAX_PASSAGE_TIME überschritten (beide done)");
    }
    resetPassage("eval");
  }

  // Nur einer done -> Warten/Abbruch
  if((sensorA_done ^ sensorB_done)){
    // Wenn es jemals eine Pause gab ODER der zweite Sensor IRGENDWANN „unter“ war,
    // dann NICHT single-abbrechen – nur harte Obergrenze gilt.
    bool relaxAbort = pauseOccurred || secondEverUnder;

    if(relaxAbort){
      // Nur harte Obergrenze
      if(passageStart && (now - passageStart > MAX_PASSAGE_TIME)){
        abort_timeout++; Serial.println("⏱️ Timeout: langer Vorgang (Pause/2nd seen)");
        resetPassage("timeout-relaxed");
      }
    } else {
      // Klassischer Single-Abort mit adaptiver Wartezeit
      unsigned long waitDyn = clampUL((unsigned long)(firstActiveDur * WAIT_OTHER_FACTOR), WAIT_OTHER_MIN, WAIT_OTHER_MAX);
      if(now - firstStartTime > waitDyn){
        if(sensorA_done && !sensorB_done){ abort_singleA++; Serial.printf("⚠️ Single abort: nur A; waited=%lums (dyn=%lums)\n", now-firstStartTime, waitDyn); }
        if(sensorB_done && !sensorA_done){ abort_singleB++; Serial.printf("⚠️ Single abort: nur B; waited=%lums (dyn=%lums)\n", now-firstStartTime, waitDyn); }
        resetPassage("single-adaptive");
      }
    }
  }

  // Absolute Safety
  if(passageStart && (now - passageStart > MAX_PASSAGE_TIME)){
    abort_timeout++; Serial.printf("⏱️ Timeout MAX_PASSAGE_TIME (%lums)\n", now - passageStart);
    resetPassage("max-pass");
  }

  // Webserver bedienen
  server.handleClient();

  // Tageswechsel prüfen + gelegentlich persistieren
  rotateDayIfNeeded();

  delay(LOOP_DELAY_MS);
}
