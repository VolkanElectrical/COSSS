/***** 2x HC‑SR04 Richtungszähler – „kein Früh‑Abort nach Warten bei A“ *****/
/* Pins: A TRIG=7 ECHO=6  |  B TRIG=4 ECHO=3  |  ENTER=90cm, EXIT=110cm */

#define TRIG_PIN_A  7
#define ECHO_PIN_A  6
#define TRIG_PIN_B  4
#define ECHO_PIN_B  3

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
unsigned long firstActiveDur=0;        // Dauer der Unter‑Phase des ersten Sensors
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

// Hysterese‑Update mit Exit‑Hold & Re‑Arm
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

void setup(){
  Serial.begin(115200);
  pinMode(TRIG_PIN_A,OUTPUT); pinMode(ECHO_PIN_A,INPUT);
  pinMode(TRIG_PIN_B,OUTPUT); pinMode(ECHO_PIN_B,INPUT);
  digitalWrite(TRIG_PIN_A,LOW); digitalWrite(TRIG_PIN_B,LOW);

  Serial.println("🚀 Start: Richtungszaehler – tolerant, kein Früh‑Abort nach Pause/Warten");
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

  // Hysterese‑Zustand aktualisieren
  bool prevUnderA=underA, prevUnderB=underB;
  if(!inLockout && !sensorA_stuck) updateHysteresis(vA, underA, underSinceA, freeSinceA);
  if(!inLockout && !sensorB_stuck) updateHysteresis(vB, underB, underSinceB, freeSinceB);

  // Start‑Events
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
      if(firstSensor=='A'){ countOut++; if(current>0) current--; Serial.printf("⬅️ OUT  | In=%ld Out=%ld Curr=%ld (first=A)\n", countIn, countOut, current); }
      else               { countIn++;  current++;            Serial.printf("✅ IN   | In=%ld Out=%ld Curr=%ld (first=B)\n", countIn, countOut, current); }
    } else {
      abort_timeout++; Serial.println("⚠️ Abbruch: MAX_PASSAGE_TIME überschritten (beide done)");
    }
    resetPassage("eval");
  }

  // Nur einer done -> Warten/Abbruch
  if((sensorA_done ^ sensorB_done)){
    // Wenn es jemals eine Pause gab ODER der zweite Sensor IRGENDWANN „unter“ war,
    // dann NICHT single‑abbrechen – nur harte Obergrenze gilt.
    bool relaxAbort = pauseOccurred || secondEverUnder;

    if(relaxAbort){
      // Nur harte Obergrenze
      if(passageStart && (now - passageStart > MAX_PASSAGE_TIME)){
        abort_timeout++; Serial.println("⏱️ Timeout: langer Vorgang (Pause/2nd seen)");
        resetPassage("timeout-relaxed");
      }
    } else {
      // Klassischer Single‑Abort mit adaptiver Wartezeit
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

  delay(LOOP_DELAY_MS);
}
