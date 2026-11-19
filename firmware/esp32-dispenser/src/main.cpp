#include <Arduino.h>
#ifdef ESP8266
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  #include <EEPROM.h>
  #include <Updater.h>
#else
  #include <WiFi.h>
  #include <WebServer.h>
  #include <Preferences.h>
  #include <Update.h>
  #include <ESPmDNS.h>
#endif
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#ifndef ESP8266
  #include <WiFiClientSecure.h>
  #include <time.h>
  #include "certs.h"
#endif
#include "config.h"

// ---------- State ----------
#ifdef ESP8266
ESP8266WebServer server(80);
#else
Preferences prefs;
WebServer server(80);
#endif

volatile unsigned long flowPulseCountCold = 0;
volatile unsigned long flowPulseCountNormal = 0;
// Diagnostics: edges rejected by the FLOW_MIN_PULSE_US noise gate.
volatile unsigned long flowRejectedCold = 0;
volatile unsigned long flowRejectedNormal = 0;
volatile unsigned long flowLastEdgeUsCold = 0;
volatile unsigned long flowLastEdgeUsNormal = 0;
#ifndef ESP8266
portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;
#endif

enum Channel { CH_NONE = 0, CH_COLD = 1, CH_NORMAL = 2 };

struct DispenseJob {
  bool active = false;
  bool paused = false;
  // Set when the target is reached: pump is already OFF, the solenoid stays
  // open until finishStartMs + VALVE_TAIL_MS, then the job ends.
  bool finishing = false;
  unsigned long finishStartMs = 0;
  float triggerMl = 0;         // volume when the pump was stopped (tail comp)
  // Free-run calibration job: no volume/pulse target, ends on STOP (or the
  // CAL_RUN_MAX_MS safety cap). Used so calibration never depends on the
  // currently stored PPL.
  bool calibrate = false;
  Channel channel = CH_NONE;
  unsigned long startMs = 0;
  unsigned long lastPulseMs = 0;
  unsigned long pausedAtMs = 0;
  unsigned long startPulses = 0;
  float targetMl = 0;
  String error;
};
DispenseJob job;

// Per-line calibration. The two sensors/plumbing runs are never identical, so
// each channel keeps its own pulses-per-liter value.
float pplCold = FLOW_PULSES_PER_L;
float pplNormal = FLOW_PULSES_PER_L;
const float PPL_MIN = 100.0f;
const float PPL_MAX = 50000.0f;

float pplFor(Channel ch) { return ch == CH_NORMAL ? pplNormal : pplCold; }

// Learned drain volume of the pump-off → valve-close tail, in mL, per line
// (the hose/valve run differs between Cold and Normal).
float tailCompCold = VALVE_TAIL_COMP_ML;
float tailCompNormal = VALVE_TAIL_COMP_ML;
bool tailCompColdLearned = false;
bool tailCompNormalLearned = false;

float& tailCompRef(Channel ch) { return ch == CH_NORMAL ? tailCompNormal : tailCompCold; }
bool& tailLearnedRef(Channel ch) { return ch == CH_NORMAL ? tailCompNormalLearned : tailCompColdLearned; }
String staSsid, staPass;
char apSsid[32];
// Set while /api/wifiscan detaches the STA. loop()'s auto-retry must not call
// WiFi.begin() during a scan or it aborts the scan and the list comes back empty.
bool wifiScanning = false;

// ---------- Static IPv4 (optional) ----------
// Defaults to DHCP. A saved static address is applied before WiFi.begin() on
// every boot, so the tablet/kiosk can always reach the dispenser at the same IP
// while the tablet itself stays on the shop router (internet for QRIS).
bool netStatic = false;
String netIp, netGw, netMask = "255.255.255.0", netDns;

bool parseIPv4(const String &s, IPAddress &out) {
  return s.length() > 0 && out.fromString(s.c_str());
}

// Validate a static config before it is saved: parseable, not 0.0.0.0 /
// 255.255.255.255, not on the AP's 192.168.4.0/24 subnet, gateway on the same
// subnet, and the host part is neither the network nor the broadcast address.
bool isUsableStaticIp(const String &ip, const String &gw, const String &mask) {
  IPAddress a, g, m;
  if (!parseIPv4(ip, a) || !parseIPv4(gw, g) || !parseIPv4(mask, m)) return false;
  if ((a[0] | a[1] | a[2] | a[3]) == 0) return false;
  if ((a[0] & a[1] & a[2] & a[3]) == 255) return false;
  if (a[0] == 192 && a[1] == 168 && a[2] == 4) return false; // AP subnet
  for (uint8_t i = 0; i < 4; i++)
    if ((a[i] & m[i]) != (g[i] & m[i])) return false;
  bool hostAll0 = true, hostAll1 = true;
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t hm = (uint8_t)~m[i];
    if ((a[i] & hm) != 0) hostAll0 = false;
    if ((a[i] & hm) != hm) hostAll1 = false;
  }
  return !(hostAll0 || hostAll1);
}

void resetStaticIp() {
  netStatic = false;
  netIp = ""; netGw = ""; netMask = "255.255.255.0"; netDns = "";
#ifndef ESP8266
  prefs.putBool("net_static", false);
  prefs.remove("net_ip");
  prefs.remove("net_gw");
  prefs.remove("net_mask");
  prefs.remove("net_dns");
#endif
}

// Apply the saved static address to the STA interface. Must run after
// WiFi.mode() and before WiFi.begin(); returns false when the saved values are
// unusable, in which case the caller keeps DHCP.
bool applyStaticIp() {
  IPAddress a, g, m, d;
  if (!isUsableStaticIp(netIp, netGw, netMask)) return false;
  parseIPv4(netIp, a);
  parseIPv4(netGw, g);
  parseIPv4(netMask, m);
  if (!netDns.length() || !parseIPv4(netDns, d)) d = g;
  WiFi.config(a, g, m, d);
  return true;
}

float liveLpm = 0;
unsigned long flowLastSeenCount = 0;

// ---------- LAN simulator (2nd board, no wiring) ----------
// Off by default; enabled live via GET /api/sim?enable=1&hz=15&tank=ok
// Virtual pulses accumulate from hz; tank pin is overridden.
// Auto-stops flow if sim goes silent >8 s. Cleared on reboot (real sensors win).
bool simEnable = false;
float simHz = 0;
bool simTankLowRx = false;
unsigned long simPulses = 0;
float simPulseFrac = 0;
unsigned long simLastAccumMs = 0;
unsigned long simLastRxMs = 0;
const unsigned long SIM_QUIET_MS = 8000;

bool tankLowLatched = false;
unsigned long tankLastChangeMs = 0;
bool tankLastRawLow = false;
unsigned long cfgTankDebounceMs = TANK_DEBOUNCE_DEFAULT_MS;

bool cfgFlowTimeout = FLOW_TIMEOUT_ENABLED;
bool cfgTankSensor = TANK_SENSOR_ENABLED;
int cfgTankPinMode = TANK_PIN_MODE_DEFAULT;  // 0 pulldown, 1 floating, 2 pullup

// ---------- OTA state ----------
bool otaInProgress = false;
bool otaOk = false;

// ---------- MQTT telemetry ----------
// Disabled by default; values are loaded from NVS in setup() and can be changed
// live via /api/mqtt (Config -> MQTT in the web UI). Only the ESP32 build keeps
// the settings across reboots; the D1 mini test build can use them per boot.
bool mqttEnabled = MQTT_DEFAULT_ENABLED;
String mqttHost = MQTT_DEFAULT_HOST;
uint16_t mqttPort = MQTT_DEFAULT_PORT;
String mqttUser = MQTT_DEFAULT_USER;
String mqttPass = MQTT_DEFAULT_PASS;
String mqttBase = MQTT_DEFAULT_BASE;
bool mqttAllowCmd = MQTT_DEFAULT_ALLOW_CMD;
bool mqttTls = MQTT_DEFAULT_TLS;
String mqttCa;          // optional custom root CA (PEM)
String mqttClientCert;  // optional mutual-TLS client certificate (PEM)
String mqttClientKey;   // optional mutual-TLS private key (PEM)
unsigned long mqttIntervalMs = MQTT_DEFAULT_INTERVAL_MS;
String mqttDeviceId;      // esp32-dispenser-c34c
String mqttTopicBase;     // banyu/esp32-dispenser-c34c
uint32_t mqttPublishCount = 0;
uint32_t mqttConnectCount = 0;
String mqttLastError;
WiFiClient mqttNet;
#ifndef ESP8266
WiFiClientSecure mqttSecure;
#endif
PubSubClient mqtt(mqttNet);

#ifdef ESP8266
// EEPROM persistence for D1 mini (no Preferences lib): 512 B
// 0-1 magic D1 57 | 2-5 float ppl | 6 ssidLen | 7 passLen | 8-39 ssid | 40-103 pass
// The D1 test build keeps one shared ppl (applied to both lines).
#define EE_SIZE 512
void eeLoad() {
  EEPROM.begin(EE_SIZE);
  if (EEPROM.read(0) == 0xD1 && EEPROM.read(1) == 0x57) {
    float v = FLOW_PULSES_PER_L;
    EEPROM.get(2, v);
    if (!(v > PPL_MIN && v < PPL_MAX)) v = FLOW_PULSES_PER_L;
    pplCold = pplNormal = v;
    uint8_t sl = EEPROM.read(6); if (sl > 32) sl = 32;
    uint8_t pl = EEPROM.read(7); if (pl > 64) pl = 64;
    char b[65];
    for (int i = 0; i < sl; i++) b[i] = (char)EEPROM.read(8 + i);
    b[sl] = 0; staSsid = String(b);
    for (int i = 0; i < pl; i++) b[i] = (char)EEPROM.read(40 + i);
    b[pl] = 0; staPass = String(b);
  } else {
    pplCold = pplNormal = FLOW_PULSES_PER_L;
    staSsid = ""; staPass = "";
  }
}
void eeSave() {
  EEPROM.write(0, 0xD1); EEPROM.write(1, 0x57);
  EEPROM.put(2, pplCold);
  uint8_t sl = staSsid.length() > 32 ? 32 : staSsid.length();
  uint8_t pl = staPass.length() > 64 ? 64 : staPass.length();
  EEPROM.write(6, sl); EEPROM.write(7, pl);
  for (int i = 0; i < 32; i++) EEPROM.write(8 + i, i < sl ? staSsid[i] : 0);
  for (int i = 0; i < 64; i++) EEPROM.write(40 + i, i < pl ? staPass[i] : 0);
  EEPROM.commit();
}
#endif

// ---------- Helpers ----------
#ifdef ESP8266
void ICACHE_RAM_ATTR onFlowPulseCold() {
  unsigned long now = micros();
  if (now - flowLastEdgeUsCold < FLOW_MIN_PULSE_US) { flowRejectedCold++; return; }
  flowLastEdgeUsCold = now;
  flowPulseCountCold++;
}
void ICACHE_RAM_ATTR onFlowPulseNormal() {
  unsigned long now = micros();
  if (now - flowLastEdgeUsNormal < FLOW_MIN_PULSE_US) { flowRejectedNormal++; return; }
  flowLastEdgeUsNormal = now;
  flowPulseCountNormal++;
}
#else
void IRAM_ATTR onFlowPulseCold() {
  unsigned long now = micros();
  if (now - flowLastEdgeUsCold < FLOW_MIN_PULSE_US) { flowRejectedCold++; return; }
  flowLastEdgeUsCold = now;
  portENTER_CRITICAL_ISR(&flowMux);
  flowPulseCountCold++;
  portEXIT_CRITICAL_ISR(&flowMux);
}

void IRAM_ATTR onFlowPulseNormal() {
  unsigned long now = micros();
  if (now - flowLastEdgeUsNormal < FLOW_MIN_PULSE_US) { flowRejectedNormal++; return; }
  flowLastEdgeUsNormal = now;
  portENTER_CRITICAL_ISR(&flowMux);
  flowPulseCountNormal++;
  portEXIT_CRITICAL_ISR(&flowMux);
}
#endif

inline void relayWrite(uint8_t pin, bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(pin, level ? HIGH : LOW);
}

// Pump MOSFET module input driven by LEDC. pin < 0 disables the output
// (D1 mini test build). 0 = off, PUMP_PWM_DUTY_ON (255) = 100% duty.
inline int pumpLedcChannel(int pin) {
  if (pin == PIN_PUMP_COLD) return PUMP_LEDC_CH_COLD;
  if (pin == PIN_PUMP_NORMAL) return PUMP_LEDC_CH_NORMAL;
  return -1;
}

inline uint32_t pumpDuty(bool on) {
  uint32_t level = on ? PUMP_PWM_DUTY_ON : 0;
  if (PUMP_ACTIVE_LOW) level = PUMP_PWM_MAX - level;   // inverted module
  return level;
}

inline void pumpWrite(int pin, bool on) {
  int ch = pumpLedcChannel(pin);
  if (ch < 0) return;
#ifndef ESP8266
  ledcWrite(ch, pumpDuty(on));
#endif
}

void pumpsOff() {
  pumpWrite(PIN_PUMP_COLD, false);
  pumpWrite(PIN_PUMP_NORMAL, false);
}

void relaysOff() {
  // Pump first, then valve: avoids dead-heading the pump against a closed line.
  pumpsOff();
  relayWrite(PIN_RELAY_COLD, false);
  relayWrite(PIN_RELAY_NORMAL, false);
}

inline void ledWrite(uint8_t pin, bool activeLow, bool on) {
  bool level = activeLow ? !on : on;
  digitalWrite(pin, level ? HIGH : LOW);
}

#if !defined(ESP8266) && LED_BOARD_IS_WS2812
// WS2812/NeoPixel on-board LED. Cache the last colour so we only refresh on change.
void ledColor(uint8_t r, uint8_t g, uint8_t b) {
  static uint8_t lastR = 255, lastG = 255, lastB = 255;
  if (r == lastR && g == lastG && b == lastB) return;
  lastR = r; lastG = g; lastB = b;
  neopixelWrite(PIN_LED_BOARD, r, g, b);
}
#else
void ledColor(uint8_t r, uint8_t g, uint8_t b) {
  ledWrite(PIN_LED_BOARD, LED_BOARD_ACTIVE_LOW, r || g || b);
}
#endif

// Single on-board LED: network mode indicator.
// STA connected = green slow blink (500 ms); AP-only / disconnected = steady blue.
void updateLeds() {
  bool staConnected = WiFi.status() == WL_CONNECTED;
  if (staConnected) {
    bool on = ((millis() / 500) % 2 == 0);
    if (on) ledColor(0, 64, 0); else ledColor(0, 0, 0);
  } else {
    ledColor(0, 0, 64);
  }
}

unsigned long hwFlowPulses(Channel ch) {
#ifdef ESP8266
  noInterrupts();
  unsigned long c = (ch == CH_NORMAL) ? flowPulseCountNormal : flowPulseCountCold;
  interrupts();
  return c;
#else
  unsigned long c;
  portENTER_CRITICAL(&flowMux);
  c = (ch == CH_NORMAL) ? flowPulseCountNormal : flowPulseCountCold;
  portEXIT_CRITICAL(&flowMux);
  return c;
#endif
}

unsigned long hwFlowPulsesTotal() {
#ifdef ESP8266
  noInterrupts();
  unsigned long c = flowPulseCountCold + flowPulseCountNormal;
  interrupts();
  return c;
#else
  unsigned long c;
  portENTER_CRITICAL(&flowMux);
  c = flowPulseCountCold + flowPulseCountNormal;
  portEXIT_CRITICAL(&flowMux);
  return c;
#endif
}

unsigned long flowPulsesTotal() {
  unsigned long c = hwFlowPulsesTotal();
  if (simEnable) c += simPulses;
  return c;
}

unsigned long flowPulsesForChannel(Channel ch) {
#if FLOW_SENSOR_COUNT < 2
  (void)ch;
  return flowPulsesTotal();
#else
  if (ch == CH_NONE) return flowPulsesTotal();
  unsigned long c = hwFlowPulses(ch);
  if (simEnable) c += simPulses;
  return c;
#endif
}

unsigned long flowPulses() {
  return flowPulsesTotal();
}

// Call every loop: turn sim Hz into virtual pulses (fraction-safe).
void simAccumulate() {
  if (!simEnable) { simLastAccumMs = 0; simPulseFrac = 0; return; }
  unsigned long now = millis();
  if (simLastAccumMs == 0) { simLastAccumMs = now; return; }
  unsigned long dt = now - simLastAccumMs;
  simLastAccumMs = now;
  bool fresh = (now - simLastRxMs < SIM_QUIET_MS);
  if (fresh && simHz > 0 && dt < 2000) {
    simPulseFrac += simHz * dt / 1000.0f;
    unsigned long whole = (unsigned long)simPulseFrac;
    if (whole) { simPulses += whole; simPulseFrac -= whole; }
  }
}

float pulsesToMl(unsigned long pulses, Channel ch) {
  return (pulses / pplFor(ch)) * 1000.0f;
}

float currentJobMl() {
  unsigned long now = flowPulsesForChannel(job.channel);
  unsigned long d = (now >= job.startPulses) ? (now - job.startPulses) : 0;
  return pulsesToMl(d, job.channel);
}

bool readTankLow() {
  if (simEnable) {  // LAN sim overrides the pin (RAM only, reboot clears)
    tankLowLatched = simTankLowRx;
    tankLastRawLow = simTankLowRx;
    return simTankLowRx;
  }
  if (!cfgTankSensor) {
    tankLowLatched = false;
    tankLastRawLow = false;
    return false;
  }
  int v = digitalRead(PIN_TANK_LOW);
  bool waterPresent = TANK_WATER_PRESENT_HIGH ? (v == HIGH) : (v == LOW);
  bool rawLow = !waterPresent;
  unsigned long now = millis();
  if (rawLow != tankLastRawLow) {
    tankLastRawLow = rawLow;
    tankLastChangeMs = now;
  }
  if (now - tankLastChangeMs >= cfgTankDebounceMs) {
    tankLowLatched = rawLow;
  }
  return tankLowLatched;
}

// Apply the runtime tank-sensor setting to the pin and reset the debounce state.
// Must run after the saved setting is loaded from NVS, not before.
int tankPinModeArduino() {
  if (cfgTankPinMode == 2) return INPUT_PULLUP;
  if (cfgTankPinMode == 1) return INPUT;
  return INPUT_PULLDOWN;
}

void applyTankConfig() {
  if (cfgTankSensor) {
    pinMode(PIN_TANK_LOW, tankPinModeArduino());
    int v = digitalRead(PIN_TANK_LOW);
    bool waterPresent = TANK_WATER_PRESENT_HIGH ? (v == HIGH) : (v == LOW);
    tankLastRawLow = !waterPresent;
    tankLowLatched = tankLastRawLow;  // don't wait through the debounce on (re)enable
    tankLastChangeMs = millis();
  } else {
    tankLastRawLow = false;
    tankLowLatched = false;
  }
}

const char* wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

const char* channelName(Channel c) {
  return c == CH_COLD ? "cold" : c == CH_NORMAL ? "normal" : "none";
}

const char* jobStateName() {
  if (!job.active) return "idle";
  if (job.paused) return "paused";
  if (job.calibrate) return "calibrating";
  if (job.finishing) return "finishing";
  return "dispensing";
}

Channel parseChannel(const String& s) {
  String t = s;
  t.toLowerCase();
  if (t == "cold" || t == "1" || t == "c") return CH_COLD;
  if (t == "normal" || t == "2" || t == "n") return CH_NORMAL;
  return CH_NONE;
}

void stopDispense(const String &err = "") {
  // Learn the tail drain from a successful target completion: it is the water
  // that passed the sensor between pump-off and valve-close, which the cup
  // received after the target was counted.
  if (job.finishing && err.length() == 0) {
    float extra = currentJobMl() - job.triggerMl;
    if (extra > 0 && extra <= 150) {
      float &tc = tailCompRef(job.channel);
      bool &learned = tailLearnedRef(job.channel);
      float before = tc;
      if (!learned) tc = extra;
      else tc = tc * 0.7f + extra * 0.3f;
      learned = true;
      Serial.printf("[JOB] %s tail drain %.1f mL, compensation %.1f mL\n",
                    channelName(job.channel), extra, tc);
#ifndef ESP8266
      // Only touch NVS when the estimate actually moved, to limit flash wear.
      if (fabsf(tc - before) >= 0.5f)
        prefs.putFloat(job.channel == CH_NORMAL ? "tail_norm" : "tail_cold", tc);
#endif
    }
  }
  job.active = false;
  job.paused = false;
  job.pausedAtMs = 0;
  job.finishing = false;
  job.finishStartMs = 0;
  job.calibrate = false;
  job.error = err;
  relaysOff();
  if (err.length()) Serial.printf("[JOB] stopped (%s): %.0f mL\n", err.c_str(), currentJobMl());
  else Serial.printf("[JOB] done: %.0f mL (%s)\n", currentJobMl(), channelName(job.channel));
}

// Target reached: drop the pump now, keep the solenoid open for VALVE_TAIL_MS
// so the line can drain, then loop() closes the valve via stopDispense().
void beginValveTail() {
  pumpsOff();
  job.finishing = true;
  job.finishStartMs = millis();
  job.triggerMl = currentJobMl();
  Serial.printf("[JOB] target reached at %.0f mL: pump OFF, valve closes in %lu ms\n",
                job.triggerMl, (unsigned long)VALVE_TAIL_MS);
}

// How many mL early to stop the pump for a given target (safety-clamped).
float tailCompClamped(Channel ch, float targetMl) {
  float v = tailCompRef(ch);
  float cap = targetMl * VALVE_TAIL_COMP_MAX_FRAC;
  if (!(v > 0)) return 0;
  return v > cap ? cap : v;
}

bool pauseDispense() {
  if (!job.active || job.paused || job.finishing) return false;
  job.paused = true;
  job.pausedAtMs = millis();
  relaysOff();
  Serial.printf("[JOB] paused at %.0f / %.0f mL\n", currentJobMl(), job.targetMl);
  return true;
}

bool resumeDispense() {
  if (!job.active || !job.paused) return false;
  if (readTankLow()) {
    job.error = "Tank LOW — refill to resume";
    return false;
  }
  unsigned long pauseMs = millis() - job.pausedAtMs;
  job.startMs += pauseMs;
  job.lastPulseMs = millis();
  flowLastSeenCount = flowPulsesForChannel(job.channel);
  job.paused = false;
  job.pausedAtMs = 0;
  job.error = "";
  relaysOff();
  delay(20);
  if (job.channel == CH_COLD) {
    relayWrite(PIN_RELAY_COLD, true);
    pumpWrite(PIN_PUMP_COLD, true);
  } else {
    relayWrite(PIN_RELAY_NORMAL, true);
    pumpWrite(PIN_PUMP_NORMAL, true);
  }
  Serial.printf("[JOB] resumed at %.0f / %.0f mL after %lu ms\n",
                currentJobMl(), job.targetMl, pauseMs);
  return true;
}

bool startDispense(Channel ch, float ml) {
  if (job.active) return false;
  if (otaInProgress) { job.error = "OTA update in progress"; return false; }
  if (ch == CH_NONE) { job.error = "Select Cold or Normal first"; return false; }
  if (ml <= 0 || ml > MAX_VOLUME_ML) { job.error = "Bad volume"; return false; }
  if (readTankLow()) {
    job.error = "Tank LOW — refill to start";
    Serial.println("[SAFETY] blocked: tank low");
    return false;
  }
  job.active = true;
  job.paused = false;
  job.pausedAtMs = 0;
  job.finishing = false;
  job.finishStartMs = 0;
  job.calibrate = false;
  job.error = "";
  job.channel = ch;
  job.targetMl = ml;
  job.startMs = millis();
  job.lastPulseMs = millis();
  job.startPulses = flowPulsesForChannel(ch);
  flowLastSeenCount = job.startPulses;
  relaysOff();
  delay(20);
  if (ch == CH_COLD) {
    relayWrite(PIN_RELAY_COLD, true);
    pumpWrite(PIN_PUMP_COLD, true);
  } else {
    relayWrite(PIN_RELAY_NORMAL, true);
    pumpWrite(PIN_PUMP_NORMAL, true);
  }
  Serial.printf("[JOB] start %s %.0f mL (target %lu pulses)\n",
                channelName(ch), ml, (unsigned long)(ml / 1000.0f * pplFor(ch)));
  return true;
}

// Free-run calibration: open the valve and pump with no volume or pulse target,
// so the reference volume is set by the user (weighing it) instead of by the
// currently stored PPL. Ends on STOP or the CAL_RUN_MAX_MS safety cap.
bool startCalibration(Channel ch) {
  if (job.active) return false;
  if (otaInProgress) { job.error = "OTA update in progress"; return false; }
  if (ch == CH_NONE) { job.error = "Select Cold or Normal first"; return false; }
  if (readTankLow()) {
    job.error = "Tank LOW — refill to start";
    Serial.println("[SAFETY] calibration blocked: tank low");
    return false;
  }
  job.active = true;
  job.paused = false;
  job.pausedAtMs = 0;
  job.finishing = false;
  job.finishStartMs = 0;
  job.calibrate = true;
  job.error = "";
  job.channel = ch;
  job.targetMl = 0;            // 0 = no target while calibrating
  job.startMs = millis();
  job.lastPulseMs = millis();
  job.startPulses = flowPulsesForChannel(ch);
  flowLastSeenCount = job.startPulses;
  relaysOff();
  delay(20);
  if (ch == CH_COLD) {
    relayWrite(PIN_RELAY_COLD, true);
    pumpWrite(PIN_PUMP_COLD, true);
  } else {
    relayWrite(PIN_RELAY_NORMAL, true);
    pumpWrite(PIN_PUMP_NORMAL, true);
  }
  Serial.printf("[CAL] free run %s start (max %lu ms) — STOP to finish\n",
                channelName(ch), (unsigned long)CAL_RUN_MAX_MS);
  return true;
}

// ---------- OTA update ----------
// Stops any running job, drops both relays and flashes the new image.
void otaPrepare() {
  if (job.active) stopDispense("OTA update");
  relaysOff();
  otaInProgress = true;
  otaOk = false;
}

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaPrepare();
    Serial.println("[OTA] ArduinoOTA start — relays OFF");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPct = 101;
    unsigned int pct = total ? (progress * 100U / total) : 0;
    if (pct != lastPct && (pct % 10U == 0 || pct == 100U)) {
      lastPct = pct;
      Serial.printf("[OTA] %u%%\n", pct);
    }
  });
  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    otaOk = true;
    Serial.println("\n[OTA] ArduinoOTA done — rebooting");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    otaInProgress = false;
    otaOk = false;
    Serial.printf("[OTA] ArduinoOTA error %u\n", (unsigned)e);
  });
  ArduinoOTA.begin();
  Serial.printf("[OTA] ready: %s.local:3232 (password required)\n", OTA_HOSTNAME);
}

// mDNS makes the dispenser discoverable as <HOSTNAME>.local on the shop LAN,
// so the tablet does not need a hardcoded IP. ArduinoOTA.begin() already starts
// the mDNS responder with the same hostname; add the HTTP service on top of it
// instead of calling MDNS.begin() again (a second mdns_init() would fail).
void setupMDNS() {
#ifndef ESP8266
  if (!MDNS.addService("http", "tcp", 80)) {
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    else Serial.println("[MDNS] responder not available");
  }
  MDNS.setInstanceName("HydroNode Dispenser");
  Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
#endif
}

// Web OTA requires the OTA password in the X-OTA-Pass header.
bool otaAuthorized() {
  return server.header("X-OTA-Pass") == OTA_PASSWORD;
}

// Web upload: POST /update with multipart/form-data file field.
void handleOtaUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    if (!otaAuthorized()) {
      Serial.printf("[OTA] web upload REJECTED (bad/missing X-OTA-Pass): %s\n",
                    upload.filename.c_str());
      otaInProgress = false;
      otaOk = false;
      return;
    }
    Serial.printf("[OTA] web upload start: %s\n", upload.filename.c_str());
    otaPrepare();
#ifdef ESP8266
    size_t otaSize = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
#else
    size_t otaSize = UPDATE_SIZE_UNKNOWN;
#endif
    if (!Update.begin(otaSize)) {
      Update.printError(Serial);
      otaInProgress = false;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (otaInProgress &&
        Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      otaInProgress = false;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (otaInProgress && Update.end(true)) {
      otaOk = true;
      Serial.printf("[OTA] web update OK: %u bytes — rebooting\n", upload.totalSize);
    } else {
      Update.printError(Serial);
      otaOk = false;
    }
    otaInProgress = false;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
#ifdef ESP8266
    Update.end(false);
#else
    Update.abort();
#endif
    otaInProgress = false;
    otaOk = false;
    Serial.println("[OTA] web upload aborted");
  }
}

// ---------- Web UI ----------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name=theme-color content="#0b1220"><meta name=apple-mobile-web-app-capable content=yes><meta name=mobile-web-app-capable content=yes>
<title>Water Dispenser</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
:root{
  --bg:#040812;--bg2:#07111f;--panel:rgba(9,20,36,.78);--panel2:rgba(12,26,46,.92);
  --line:rgba(94,234,212,.16);--line2:rgba(148,163,184,.14);
  --txt:#e9f6ff;--mut:#8195ad;--aqua:#22e4ff;--cyan:#06b6d4;--mint:#5eead4;
  --amber:#fbbf24;--red:#fb7185;--violet:#8b5cf6;--r:22px;
  --mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  --shadow:0 24px 70px rgba(0,0,0,.42);
}
html{-webkit-text-size-adjust:100%;color-scheme:dark}
body{
  font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
  margin:0;color:var(--txt);min-height:100vh;min-height:100dvh;
  padding:0 14px calc(24px + env(safe-area-inset-bottom));
  background:
    radial-gradient(900px 520px at 12% -8%,rgba(34,228,255,.15),transparent 60%),
    radial-gradient(720px 520px at 100% 0%,rgba(139,92,246,.14),transparent 55%),
    radial-gradient(900px 700px at 50% 115%,rgba(94,234,212,.08),transparent 60%),
    linear-gradient(rgba(148,163,184,.035) 1px,transparent 1px),
    linear-gradient(90deg,rgba(148,163,184,.035) 1px,transparent 1px),
    var(--bg);
  background-size:auto,auto,auto,44px 44px,44px 44px,auto;
  background-attachment:fixed;
}
::selection{background:rgba(34,228,255,.3);color:#fff}
.wrap{max-width:980px;margin:0 auto;position:relative;z-index:1}
.top{
  position:sticky;top:0;z-index:10;margin:0 -14px 14px;
  padding:calc(12px + env(safe-area-inset-top)) 14px 12px;
  background:rgba(4,8,18,.78);border-bottom:1px solid var(--line2);
  backdrop-filter:blur(18px);-webkit-backdrop-filter:blur(18px);
}
.top-inner{max-width:980px;margin:0 auto;display:flex;align-items:center;gap:12px}
.brand{display:flex;align-items:center;gap:12px;min-width:0;flex:1}
.logo{
  width:44px;height:44px;border-radius:15px;flex:none;display:grid;place-items:center;
  color:#bff7ff;border:1px solid rgba(34,228,255,.34);
  background:linear-gradient(145deg,rgba(34,228,255,.18),rgba(139,92,246,.18));
  box-shadow:0 0 26px rgba(34,228,255,.18),inset 0 1px 0 rgba(255,255,255,.12);
}
.logo svg{width:23px;height:23px;display:block}
.brand h1{
  margin:0;font-size:clamp(16px,4.2vw,20px);line-height:1.1;
  letter-spacing:.14em;text-transform:uppercase;white-space:nowrap;
  overflow:hidden;text-overflow:ellipsis;
}
.brand p{margin:4px 0 0;font-size:10px;letter-spacing:.16em;text-transform:uppercase;color:var(--mut);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.status-pill{
  display:flex;align-items:center;gap:8px;flex:none;padding:8px 11px;border-radius:999px;
  border:1px solid var(--line2);background:rgba(5,12,24,.62);
  font-size:10px;letter-spacing:.14em;text-transform:uppercase;color:#b7c7da;
}
.dot{width:9px;height:9px;border-radius:50%;background:var(--mint);box-shadow:0 0 10px var(--mint);flex:none}
.dot.bad{background:var(--red);box-shadow:0 0 10px var(--red)}
.card{
  position:relative;background:linear-gradient(160deg,var(--panel2),var(--panel));
  border:1px solid var(--line2);border-radius:var(--r);padding:clamp(16px,4vw,22px);
  margin:14px 0;box-shadow:var(--shadow);overflow:hidden;
}
.card::before{
  content:"";position:absolute;inset:0;pointer-events:none;
  background:linear-gradient(120deg,rgba(34,228,255,.07),transparent 32%,transparent 68%,rgba(139,92,246,.07));
}
.card>*{position:relative;z-index:1}
.card-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}
.chip{
  font-family:var(--mono);font-size:9px;letter-spacing:.16em;text-transform:uppercase;
  color:#9fefff;border:1px solid rgba(34,228,255,.28);background:rgba(34,228,255,.08);
  padding:5px 8px;border-radius:999px;white-space:nowrap;
}
.step{
  display:flex;align-items:center;gap:8px;margin:0 0 12px;
  font-size:10px;font-weight:800;letter-spacing:.18em;text-transform:uppercase;color:var(--mut);
}
.step::before{content:"";width:6px;height:6px;border-radius:50%;background:var(--aqua);box-shadow:0 0 12px var(--aqua)}
.banner{
  display:flex;align-items:center;justify-content:center;gap:8px;
  border-radius:15px;padding:13px 14px;margin:14px 0 0;text-align:center;
  font-size:clamp(13px,3.8vw,15px);font-weight:700;letter-spacing:.02em;
  overflow-wrap:anywhere;
}
.ok{background:rgba(6,53,31,.48);color:#a7f3d0;border:1px solid rgba(94,234,212,.28);box-shadow:0 0 26px rgba(94,234,212,.06)}
.alert{background:rgba(69,10,10,.55);color:#fecdd3;border:1px solid rgba(251,113,133,.4);animation:blink 1.2s infinite}
@keyframes blink{50%{opacity:.6}}
.tabs{display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px;margin:14px 0 0;padding:5px;border:1px solid var(--line2);border-radius:16px;background:rgba(4,10,20,.55)}
.tabs button{min-height:44px;border:1px solid transparent;border-radius:12px;background:transparent;color:var(--mut);font:inherit;font-size:12px;font-weight:800;letter-spacing:.12em;text-transform:uppercase;cursor:pointer;touch-action:manipulation}
.tabs button.on{color:#e9f6ff;border-color:rgba(34,228,255,.32);background:linear-gradient(135deg,rgba(34,228,255,.16),rgba(99,102,241,.14));box-shadow:inset 0 1px 0 rgba(255,255,255,.08),0 0 24px rgba(34,228,255,.08)}
.tabpane[hidden]{display:none!important}
.cal-row{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px}
.calch{min-height:52px;border-radius:12px;border:1px solid var(--line2);background:rgba(15,30,54,.8);color:#dff7ff;font:inherit;font-size:13px;font-weight:800;cursor:pointer;touch-action:manipulation}
.calch.on{border-color:rgba(34,228,255,.66);background:rgba(34,228,255,.12);box-shadow:0 0 0 1px rgba(34,228,255,.2)}
.calch:disabled{opacity:.45;cursor:not-allowed}
.cal-save-row{display:grid;grid-template-columns:1fr auto;gap:8px;align-items:stretch;margin-top:10px}
.cal-save{min-height:48px;padding:0 16px;border:none;border-radius:12px;font:inherit;font-size:13px;font-weight:800;cursor:pointer;white-space:nowrap}
.cal-save:disabled{opacity:.45;cursor:not-allowed}
.cal-actions{grid-template-columns:1.4fr 1fr}
.cfg-toggle{display:flex;align-items:center;gap:10px;padding:12px;border:1px solid var(--line2);border-radius:12px;background:rgba(4,10,20,.52);min-height:48px;cursor:pointer;font-size:13px;color:var(--txt)}
.cfg-toggle input{width:auto;flex:none;min-width:18px;min-height:18px;accent-color:var(--aqua)}
.cfg-toggle:hover{border-color:rgba(34,228,255,.3)}
.seg{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.seg button{
  position:relative;min-height:112px;padding:14px 10px;
  display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px;
  border-radius:18px;border:1px solid var(--line2);
  background:linear-gradient(160deg,rgba(15,30,54,.76),rgba(6,13,25,.9));
  color:#b8c7da;cursor:pointer;font-weight:800;font-size:clamp(15px,4vw,17px);
  touch-action:manipulation;transition:transform .08s,border-color .2s,box-shadow .2s,background .2s;
}
.seg button:hover{border-color:rgba(34,228,255,.35);background:linear-gradient(160deg,rgba(18,38,66,.8),rgba(7,16,30,.92))}
.seg button:active{transform:scale(.98)}
.seg button.sel-cold{
  border-color:rgba(34,228,255,.72);color:#fff;
  background:linear-gradient(160deg,rgba(8,74,104,.82),rgba(5,30,48,.92));
  box-shadow:0 0 0 1px rgba(34,228,255,.22),0 0 32px rgba(34,228,255,.16);
}
.seg button.sel-normal{
  border-color:rgba(94,234,212,.72);color:#fff;
  background:linear-gradient(160deg,rgba(6,78,59,.82),rgba(4,39,31,.92));
  box-shadow:0 0 0 1px rgba(94,234,212,.22),0 0 32px rgba(94,234,212,.14);
}
.wico{width:40px;height:40px;display:grid;place-items:center;margin-bottom:2px}
.wico svg{width:31px;height:31px;display:block;filter:drop-shadow(0 0 12px rgba(34,228,255,.25))}
.sel-cold .wico{color:#9fefff}
.sel-normal .wico{color:#a7f3d0}
.lbl{font-size:clamp(16px,4.2vw,18px);letter-spacing:.04em}
.sub{font-size:10px;font-weight:600;letter-spacing:.12em;text-transform:uppercase;color:var(--mut)}
@keyframes pour{50%{box-shadow:0 0 0 2px rgba(251,191,36,.1),0 0 12px rgba(251,191,36,.14)}}
.cup{width:clamp(30px,9vw,38px);height:auto;display:block;color:currentColor;opacity:.92}
/* Dose carousel: 3 cups visible, selected cup centered and enlarged */
.carousel{display:grid;grid-template-columns:40px minmax(0,1fr) 40px;align-items:center;gap:6px}
.track{position:relative;height:156px;overflow:visible}
.slide{
  position:absolute;top:50%;left:50%;z-index:1;width:38%;max-width:180px;min-height:104px;padding:10px 6px;
  display:flex;flex-direction:column;align-items:center;justify-content:center;gap:5px;
  border-radius:18px;border:1px solid var(--line2);
  background:linear-gradient(160deg,rgba(15,30,54,.7),rgba(6,13,25,.9));
  color:#8fa3b8;cursor:pointer;font:inherit;
  opacity:.5;filter:grayscale(.9) brightness(.78);
  transform:translate(-50%,-50%) translateX(var(--x,0px)) scale(.68);
  transform-origin:center;touch-action:pan-y;will-change:transform;
  transition:transform .48s cubic-bezier(.2,.8,.2,1),opacity .35s,filter .35s,border-color .35s,box-shadow .35s,background .35s,min-height .42s;
}
.slide .ml{font-size:clamp(13px,4vw,17px);font-weight:900;letter-spacing:.01em;transition:font-size .35s}
.slide .cup{width:clamp(23px,7vw,30px);transition:width .35s,transform .35s,opacity .35s}
.slide .sub{display:none}
.slide.pos-left:hover,.slide.pos-right:hover{opacity:.8;filter:grayscale(.4) brightness(.95);border-color:rgba(34,228,255,.3)}
.slide.pos-center{
  z-index:2;min-height:148px;padding:16px 10px;gap:7px;
  color:#e6fdff;opacity:1;filter:none;
  transform:translate(-50%,-50%) translateX(var(--x,0px)) scale(1);
  border-color:rgba(34,228,255,.68);
  background:linear-gradient(160deg,rgba(8,56,78,.9),rgba(5,16,30,.95));
  box-shadow:0 0 0 1px rgba(34,228,255,.22),0 18px 44px rgba(0,0,0,.42),0 0 34px rgba(34,228,255,.18);
  animation:centerGlow 3s ease-in-out infinite;
}
.slide.pos-center .ml{font-size:clamp(20px,6vw,25px)}
.slide.pos-center .cup{width:clamp(36px,11vw,46px)}
.slide.pos-center .sub{display:block}
.slide.pos-center:hover{filter:brightness(1.06)}
@keyframes centerGlow{50%{box-shadow:0 0 0 1px rgba(34,228,255,.28),0 18px 48px rgba(0,0,0,.46),0 0 46px rgba(34,228,255,.28)}}
.slide.run{
  border-color:rgba(251,191,36,.8);color:#fde68a;
  box-shadow:0 0 0 2px rgba(251,191,36,.22),0 0 30px rgba(251,191,36,.16);
  animation:pour 1.15s ease-in-out infinite;
}
.cbtn{
  min-height:72px;border-radius:14px;border:1px solid var(--line2);
  background:rgba(15,30,54,.8);color:#dff7ff;font-size:24px;font-weight:800;
  cursor:pointer;touch-action:manipulation;transition:filter .15s,border-color .2s;
}
.cbtn:hover{border-color:rgba(34,228,255,.35);filter:brightness(1.08)}
.cbtn:active{transform:scale(.96)}
.dots{display:flex;justify-content:center;align-items:center;gap:6px;margin-top:12px}
.dots span{width:6px;height:6px;border-radius:99px;background:rgba(148,163,184,.32);transition:width .2s,background .2s}
.dots span.on{width:20px;background:var(--aqua);box-shadow:0 0 10px rgba(34,228,255,.5)}
.big{display:grid;grid-template-columns:1.2fr 1fr 1fr;gap:10px;margin-top:16px}
.big button{
  position:relative;min-width:0;min-height:90px;padding:12px;border:none;border-radius:17px;
  display:flex;flex-direction:column;align-items:center;justify-content:center;gap:3px;
  font-size:clamp(13px,3.8vw,16px);font-weight:900;letter-spacing:.06em;
  cursor:pointer;touch-action:manipulation;overflow:hidden;
  transition:transform .08s,filter .15s,box-shadow .2s;
}
.big button:active{transform:scale(.98)}
.big button:disabled{opacity:.42;filter:grayscale(.45);cursor:not-allowed;transform:none}
.big button>*{position:relative;z-index:1}
.hand{display:grid;place-items:center;margin-bottom:2px}
.hand svg{width:25px;height:25px;display:block}
.start{
  color:#03121f;background:linear-gradient(135deg,#8ff8ff 0%,#22e4ff 38%,#38bdf8 62%,#818cf8 100%);
  box-shadow:0 12px 34px rgba(34,228,255,.27),inset 0 1px 0 rgba(255,255,255,.5);
}
.start::after{
  content:"";position:absolute;inset:-40%;background:linear-gradient(115deg,transparent 32%,rgba(255,255,255,.4) 50%,transparent 68%);
  transform:translateX(-130%);animation:sheen 4.5s ease-in-out infinite;
}
@keyframes sheen{0%,58%{transform:translateX(-130%)}100%{transform:translateX(130%)}}
.pause{
  color:#241400;background:linear-gradient(135deg,#fde68a,#fbbf24 55%,#f59e0b);
  box-shadow:0 12px 34px rgba(251,191,36,.2),inset 0 1px 0 rgba(255,255,255,.4);
}
.stop{
  color:#fff;background:linear-gradient(135deg,#fb7185,#e11d48 55%,#be123c);
  box-shadow:0 12px 34px rgba(251,113,133,.22),inset 0 1px 0 rgba(255,255,255,.22);
}
.start:hover,.pause:hover,.stop:hover{filter:brightness(1.07)}
.readout{margin-top:14px;padding:14px 14px 12px;border-radius:17px;border:1px solid var(--line2);background:rgba(4,10,20,.52)}
#live{
  display:none;text-align:center;font-size:clamp(38px,12vw,56px);font-weight:900;
  letter-spacing:-.05em;line-height:1;color:#f8fdff;text-shadow:0 0 30px rgba(34,228,255,.4);
  font-variant-numeric:tabular-nums;
}
#live small{font-size:14px;font-weight:600;letter-spacing:.02em;color:var(--mut);text-shadow:none}
.prog{height:10px;background:rgba(148,163,184,.12);border:1px solid var(--line2);border-radius:999px;overflow:hidden;margin-top:12px}
.prog>div{
  height:100%;width:0%;border-radius:999px;
  background:linear-gradient(90deg,#22e4ff,#5eead4,#8b5cf6);
  box-shadow:0 0 18px rgba(34,228,255,.65);transition:width .4s ease;
}
#st{font-size:clamp(12px,3.5vw,13px);color:var(--mut);line-height:1.65;margin-top:10px;overflow-wrap:anywhere}
.meta{font-size:clamp(12px,3.5vw,13px);color:var(--mut);line-height:1.65;overflow-wrap:anywhere}
.meta b{color:var(--txt);font-weight:600}
.metrics{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px;margin-top:2px}
.metric{padding:10px 11px;border:1px solid var(--line2);border-radius:13px;background:rgba(4,10,20,.52);min-width:0}
.metric span{display:block;font-size:9px;letter-spacing:.14em;text-transform:uppercase;color:var(--mut);margin-bottom:5px}
.metric b{display:block;font-family:var(--mono);font-size:12px;color:#e9f6ff;font-weight:600;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}
.metric.error{border-color:rgba(251,113,133,.35);background:rgba(76,5,25,.28)}
.metric.error b{color:#fda4af}
small{color:var(--mut)}
.row{display:flex;gap:8px}.row>*{min-width:0}
input,select{
  width:100%;color:#f2fbff;border:1px solid var(--line2);border-radius:12px;padding:12px;
  font-size:16px;background:rgba(4,10,20,.78);outline:none;transition:border-color .2s,box-shadow .2s;
}
input:focus,select:focus{border-color:rgba(34,228,255,.55);box-shadow:0 0 0 3px rgba(34,228,255,.1)}
input::placeholder{color:#5f728a}
input[type=file]{padding:9px;font-size:13px}
input[type=file]::file-selector-button{
  margin-right:10px;padding:8px 11px;border-radius:9px;border:1px solid rgba(34,228,255,.28);
  background:rgba(34,228,255,.1);color:#bff7ff;font-weight:700;cursor:pointer;
}
button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid var(--aqua);outline-offset:2px}
.wifi-btns{display:grid;grid-template-columns:1fr 1.4fr;gap:8px;margin-top:10px}
.wifi-btns button{min-height:46px;border-radius:12px;border:1px solid var(--line2);font-weight:800;font-size:14px;cursor:pointer;touch-action:manipulation;background:rgba(15,30,54,.8);color:#dff7ff;transition:filter .15s,border-color .2s}
.wifi-btns button:hover{border-color:rgba(34,228,255,.35);filter:brightness(1.06)}
#bScan{background:rgba(148,163,184,.12);color:#dbeafe}
.btn-save{background:linear-gradient(135deg,#22e4ff,#6366f1)!important;color:#04121f!important;border:none!important;box-shadow:0 8px 24px rgba(34,228,255,.18)}
.wifi-inputs{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}
.ota-card .wifi-btns{grid-template-columns:1fr}
.grid2{display:grid;gap:14px;align-items:start}
.grid2 .card{margin:0}
.hint{font-size:12px;color:var(--mut);line-height:1.6;margin:0 0 10px}
code{font-family:var(--mono);font-size:.92em;color:#9fefff;background:rgba(34,228,255,.08);border:1px solid rgba(34,228,255,.16);padding:1px 5px;border-radius:5px}
.foot{text-align:center;color:#5f728a;font-family:var(--mono);font-size:10px;letter-spacing:.12em;text-transform:uppercase;padding:8px 0 4px}
@media(min-width:820px){
  .wrap{max-width:980px}
  .grid2{grid-template-columns:1.05fr .95fr}
  .ota-card{grid-column:1/-1}
}
@media(max-width:560px){
  .big{grid-template-columns:1fr 1fr}
  .big .start{grid-column:1/-1}
}
@media(max-width:420px){
  .wifi-inputs{grid-template-columns:1fr}
  .metrics{grid-template-columns:1fr}
  .cal-save-row{grid-template-columns:1fr}
  .row{flex-direction:column}
  .brand p{display:none}
  .status-pill span:last-child{display:none}
  .carousel{grid-template-columns:32px minmax(0,1fr) 32px;gap:4px}
  .track{height:142px}
  .slide{min-height:94px;padding:8px 4px}
  .slide.pos-center{min-height:136px;padding:14px 8px}
}
@media(prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style></head>
<body>
<header class="top">
  <div class="top-inner">
    <div class="brand">
      <div class="logo" aria-hidden="true">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
          <path d="M12 2.7s6.4 7.1 6.4 12.1a6.4 6.4 0 0 1-12.8 0C5.6 9.8 12 2.7 12 2.7Z"/>
          <path d="M9.1 15.2c.6 1.2 1.6 1.9 3 1.9"/>
        </svg>
      </div>
      <div>
        <h1>HydroNode</h1>
        <p>Smart dispenser console</p>
        <p style="margin:2px 0 0;font-size:9px;letter-spacing:.12em;text-transform:uppercase;color:#4a6070">ESP32 Dev by SHD 081261316988</p>
      </div>
    </div>
    <div class="status-pill" title="Device connection status">
      <span class="dot" id="dot"></span><span>Online</span>
    </div>
  </div>
</header>
<main class="wrap">
  <div id="banner" class="banner ok" role="status" aria-live="polite">Tank level normal</div>

  <nav class="tabs" role="tablist" aria-label="Console sections">
    <button id="tabDisp" class="on" type="button" role="tab" aria-controls="paneDisp" aria-selected="true" onclick="showTab('disp')">Dispense</button>
    <button id="tabCal" type="button" role="tab" aria-controls="paneCal" aria-selected="false" onclick="showTab('cal')">Calibration</button>
    <button id="tabCfg" type="button" role="tab" aria-controls="paneCfg" aria-selected="false" onclick="showTab('cfg')">Config</button>
  </nav>

  <div id="paneDisp" class="tabpane" role="tabpanel" aria-labelledby="tabDisp">
  <section class="card" aria-labelledby="dispenseTitle">
    <div class="step" id="dispenseTitle">01 · Select line</div>
    <div class="seg">
      <button id="bCold" type="button" onclick="setCh('cold')" aria-label="Select cold water">
        <span class="wico" aria-hidden="true">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round">
            <path d="M12 2v20M4.2 6.5l15.6 11M19.8 6.5l-15.6 11"/>
            <path d="M12 2l-2.2 2.2M12 2l2.2 2.2M12 22l-2.2-2.2M12 22l2.2-2.2"/>
          </svg>
        </span>
        <span class="lbl">Cold</span>
        <span class="sub">Chilled line</span>
      </button>
      <button id="bNorm" type="button" onclick="setCh('normal')" aria-label="Select normal water">
        <span class="wico" aria-hidden="true">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
            <path d="M12 2.8s6.3 7 6.3 11.9a6.3 6.3 0 0 1-12.6 0C5.7 9.8 12 2.8 12 2.8Z"/>
            <path d="M8.8 15.3c.7 1.3 1.8 2 3.2 2"/>
          </svg>
        </span>
        <span class="lbl">Normal</span>
        <span class="sub">Room temp</span>
      </button>
    </div>

    <div class="step" style="margin-top:22px">02 · Dispense volume</div>
    <div class="carousel">
      <button class="cbtn" type="button" onclick="slideBy(-1)" aria-label="Previous dose">‹</button>
      <div class="track" id="doseTrack" role="group" aria-label="Dose">
        <button type="button" class="slide" data-ml="300" onclick="pickDose(300)">
          <svg class="cup" viewBox="0 0 30 38" aria-hidden="true"><path d="M5 4h20l-2.5 28a3 3 0 0 1-3 2.6h-9a3 3 0 0 1-3-2.6L5 4z" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linejoin="round" opacity=".9"/><rect x="8.5" y="24" width="13" height="8" rx="1.5" fill="currentColor" opacity=".9"/></svg>
          <span class="ml">300 mL</span>
          <span class="sub">Small cup</span>
        </button>
        <button type="button" class="slide" data-ml="600" onclick="pickDose(600)">
          <svg class="cup" viewBox="0 0 30 38" aria-hidden="true"><path d="M5 4h20l-2.5 28a3 3 0 0 1-3 2.6h-9a3 3 0 0 1-3-2.6L5 4z" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linejoin="round" opacity=".9"/><rect x="8.5" y="16" width="13" height="16" rx="1.5" fill="currentColor" opacity=".9"/></svg>
          <span class="ml">600 mL</span>
          <span class="sub">Medium cup</span>
        </button>
        <button type="button" class="slide" data-ml="900" onclick="pickDose(900)">
          <svg class="cup" viewBox="0 0 30 38" aria-hidden="true"><path d="M5 4h20l-2.5 28a3 3 0 0 1-3 2.6h-9a3 3 0 0 1-3-2.6L5 4z" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linejoin="round" opacity=".9"/><rect x="8.5" y="8" width="13" height="24" rx="1.5" fill="currentColor" opacity=".9"/></svg>
          <span class="ml">900 mL</span>
          <span class="sub">Large cup</span>
        </button>
      </div>
      <button class="cbtn" type="button" onclick="slideBy(1)" aria-label="Next dose">›</button>
    </div>
    <div class="dots" id="dots" aria-hidden="true"><span class="on"></span><span></span><span></span></div>

    <div class="big">
      <button class="start" id="bStart" type="button" onclick="go()">
        <span class="hand" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M8 5.5v13l10-6.5-10-6.5Z"/></svg></span>
        <span>START DISPENSE</span>
      </button>
      <button class="pause" id="bPause" type="button" onclick="togglePause()">
        <span class="hand" aria-hidden="true"><svg viewBox="0 0 24 24"><g id="pauseGlyph" fill="currentColor"><rect x="6.5" y="5" width="4" height="14" rx="1.2"/><rect x="13.5" y="5" width="4" height="14" rx="1.2"/></g></svg></span>
        <span id="pauseLbl">PAUSE</span>
      </button>
      <button class="stop" id="bStop" type="button" onclick="stop()">
        <span class="hand" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><rect x="6" y="6" width="12" height="12" rx="2"/></svg></span>
        <span>STOP</span>
      </button>
    </div>

    <div class="readout">
      <div id="live"></div>
      <div class="prog" aria-hidden="true"><div id="bar"></div></div>
      <div id="st" aria-live="polite">Connecting…</div>
    </div>
  </section>

  <div class="grid2">
    <section class="card" aria-labelledby="telemetryTitle">
      <div class="card-head"><div class="step" id="telemetryTitle" style="margin:0">Telemetry</div><span class="chip">Live</span></div>
      <div class="meta metrics" id="meta"></div>
    </section>
  </div>
  </div>

  <div id="paneCal" class="tabpane" role="tabpanel" aria-labelledby="tabCal" hidden>
    <section class="card" aria-labelledby="calTitle">
      <div class="card-head"><div class="step" id="calTitle" style="margin:0">Flow calibration</div><span class="chip">PPL</span></div>
      <p class="hint">Calibration uses the raw pulses from the selected flow sensor. Use a tared container on a scale: <b>1 g ≈ 1 mL</b>.</p>
      <ol class="hint" style="padding-left:18px;margin:0 0 12px">
        <li>Stop any running dispense and disable simulation.</li>
        <li>Pick the line, then press <b>Start test</b>. The test has <b>no volume target</b> — it runs until you press <b>Stop</b>, so the stored PPL cannot skew it.</li>
        <li>Let the line prime first, then collect roughly 900 mL and press <b>Stop</b>.</li>
        <li>Weigh the collected water, enter the measured mL below, then <b>Calculate &amp; save</b>. Repeat for the other line.</li>
      </ol>
      <p class="hint"><b>Note:</b> Cold and Normal each store their own PPL. Ignore the nominal 450 when it disagrees — a high-resolution sensor can be 10,000+ pulses/L. What matters is that a re-test lands within ±2%.</p>

      <div class="cal-row">
        <button id="calCold" class="calch on" type="button" onclick="setCalCh('cold')">Cold · GPIO18</button>
        <button id="calNorm" class="calch" type="button" onclick="setCalCh('normal')">Normal · GPIO4</button>
      </div>
      <div class="wifi-btns cal-actions">
        <button id="calStartBtn" class="btn-save" type="button" onclick="calStart()">Start test</button>
        <button id="calStopBtn" type="button" onclick="stop()" disabled>Stop</button>
      </div>

      <div class="meta metrics" id="calMeta" style="margin-top:10px"></div>

      <div class="cal-save-row">
        <input id="calActual" type="number" inputmode="decimal" min="1" step="1" placeholder="Actual measured mL" aria-label="Actual measured milliliters">
        <button id="calSaveBtn" class="btn-save cal-save" type="button" onclick="calSave()" disabled>Calculate &amp; save</button>
      </div>
      <div class="wifi-btns" style="margin-top:10px">
        <button type="button" onclick="calResetPpl()">Reset this line to 450 PPL</button>
      </div>
      <div class="meta" id="calMsg" style="margin-top:10px">Ready.</div>
    </section>
  </div>

  <div id="paneCfg" class="tabpane" role="tabpanel" aria-labelledby="tabCfg" hidden>
    <section class="card" aria-labelledby="sensorTitle">
      <div class="card-head"><div class="step" id="sensorTitle" style="margin:0">Sensor settings</div><span class="chip">Runtime</span></div>
      <p class="hint">Toggle safety sensors on or off. Settings persist across reboot.</p>
      <div class="cal-row" style="grid-template-columns:1fr">
        <label class="cfg-toggle"><input type="checkbox" id="cfgFlowTimeout"> <span>Flow timeout (stop after 5 s no flow)</span></label>
        <label class="cfg-toggle"><input type="checkbox" id="cfgTankSensor"> <span>Tank level sensor</span></label>
        <label class="cfg-toggle"><input type="checkbox" id="cfgSim"> <span>LAN sensor simulator</span></label>
      </div>
      <div class="wifi-inputs" style="grid-template-columns:1fr;margin-top:8px">
        <select id="cfgTankPin" aria-label="Tank sensor input mode">
          <option value="0">Tank input: pull-down (fail-safe, default)</option>
          <option value="1">Tank input: floating (level shifter / marginal HIGH)</option>
          <option value="2">Tank input: pull-up (NPN / open-collector)</option>
        </select>
      </div>
      <div class="meta" id="tankRawMeta" style="margin-top:8px">Raw GPIO13: —</div>
      <div class="wifi-inputs" style="grid-template-columns:1fr;margin-top:8px">
        <input id="cfgTankDebounce" type="number" inputmode="numeric" min="200" max="10000" step="100" placeholder="Tank filter ms (default 2000)">
      </div>
      <div class="wifi-btns"><button onclick="saveConfig()" class="btn-save" type="button">Save config</button></div>
      <div class="meta" id="cfgMsg" style="margin-top:10px">Loading…</div>
    </section>

    <section class="card" aria-labelledby="networkTitle">
      <div class="card-head"><div class="step" id="networkTitle" style="margin:0">Network</div><span class="chip">Wi-Fi</span></div>
      <select id="ssidSel" onchange="pickSsid()" aria-label="Available Wi-Fi networks"><option value="">— tap scan first —</option></select>
      <div class="wifi-btns">
        <button onclick="scanWifi()" id="bScan" type="button">Scan</button>
        <button onclick="saveWifi()" class="btn-save" type="button">Save &amp; reboot</button>
      </div>
      <div class="wifi-btns" style="grid-template-columns:1fr;margin-top:8px">
        <button onclick="forgetWifi()" id="bForget" type="button">Forget saved Wi-Fi</button>
      </div>
      <div class="wifi-inputs">
        <input id="ssid" placeholder="SSID" autocomplete="off" autocapitalize="none" spellcheck="false">
        <input id="pass" type="password" placeholder="password" autocomplete="off">
      </div>
      <div class="meta" id="wifi" style="margin-top:10px">not scanned yet</div>
      <div class="meta" style="margin-top:6px">AP: <b id="ap"></b> · <span id="ip"></span></div>

      <div class="step" style="margin-top:16px">Static IPv4 (optional)</div>
      <p class="hint" style="margin:8px 0 0">Reach the device at <code>esp32-dispenser.local</code>. Changing the Wi-Fi SSID resets this to DHCP.</p>
      <label class="cfg-toggle"><input type="checkbox" id="netStatic" onchange="syncNetFields()"> Use a fixed IP instead of DHCP</label>
      <div id="netFields" class="wifi-inputs" style="margin-top:8px" hidden>
        <input id="netIp" inputmode="decimal" placeholder="IP e.g. 192.168.1.50" autocomplete="off" spellcheck="false">
        <input id="netGw" inputmode="decimal" placeholder="Gateway e.g. 192.168.1.1" autocomplete="off" spellcheck="false">
        <input id="netMask" inputmode="decimal" placeholder="Subnet 255.255.255.0" autocomplete="off" spellcheck="false">
        <input id="netDns" inputmode="decimal" placeholder="DNS (optional)" autocomplete="off" spellcheck="false">
      </div>
      <div class="wifi-btns" style="grid-template-columns:1fr;margin-top:8px">
        <button class="btn-save" type="button" onclick="saveNet()">Save network &amp; reboot</button>
      </div>
      <div class="meta" id="netMsg" style="margin-top:8px">Loading…</div>
    </section>

    <section class="card" aria-labelledby="mqttTitle">
      <div class="card-head"><div class="step" id="mqttTitle" style="margin:0">MQTT</div><span class="chip">Telemetry</span></div>
      <p class="hint">Publish the same data as <code>/api/status</code> to your own broker, so an external dashboard can subscribe. Use TLS for cloud brokers (8883); plain 1883 is only for a LAN broker.</p>
      <div class="cal-row" style="grid-template-columns:1fr">
        <label class="cfg-toggle"><input type="checkbox" id="mqOn"> <span>Enable MQTT</span></label>
        <label class="cfg-toggle"><input type="checkbox" id="mqCmd"> <span>Accept remote commands (start / stop)</span></label>
        <label class="cfg-toggle"><input type="checkbox" id="mqTls"> <span>Use TLS (port 8883, cloud brokers)</span></label>
      </div>
      <div class="wifi-inputs" style="margin-top:8px">
        <input id="mqHost" placeholder="Broker e.g. 192.168.1.10" autocomplete="off" autocapitalize="none" spellcheck="false">
        <input id="mqPort" type="number" inputmode="numeric" min="1" max="65535" placeholder="1883" autocomplete="off">
        <input id="mqUser" placeholder="Username (optional)" autocomplete="off" autocapitalize="none" spellcheck="false">
        <input id="mqPass" type="password" placeholder="Password (blank = unchanged)" autocomplete="off">
        <input id="mqBase" placeholder="Topic base e.g. banyu" autocomplete="off" autocapitalize="none" spellcheck="false">
        <input id="mqInt" type="number" inputmode="numeric" min="1000" max="600000" placeholder="Publish interval ms" autocomplete="off">
      </div>
      <div class="wifi-btns" style="margin-top:8px"><button class="btn-save" type="button" onclick="saveMqtt()">Save MQTT</button></div>
      <div class="meta" id="mqMsg" style="margin-top:8px">Loading…</div>
    </section>

    <section class="card ota-card" aria-labelledby="otaTitle">
      <div class="card-head"><div class="step" id="otaTitle" style="margin:0">Firmware</div><span class="chip">OTA</span></div>
      <p class="hint">Upload a compiled <code>.bin</code> to update over Wi-Fi.</p>
      <input id="fw" type="file" accept=".bin" aria-label="Firmware binary">
      <input id="otapw" type="password" placeholder="OTA password" autocomplete="off" style="margin-top:8px" aria-label="OTA password">
      <div class="wifi-btns"><button onclick="uploadFw()" class="btn-save" id="bOta" type="button">Upload &amp; reboot</button></div>
      <div class="prog" aria-hidden="true"><div id="otabar"></div></div>
      <div class="meta" id="ota" style="margin-top:10px">Current build: <span id="build">—</span></div>
    </section>
  </div>

  <div class="foot">ESP32 · ZJ-S201 · <span>HydroNode</span></div>
</main>
<script>
let ch='cold', ml=300, isPaused=false;
let calCh='cold', calBaseline=null, calDelta=0, calResult=0, calRunning=false, calRunSeenActive=false, calStartedAt=0, lastStatus=null;
const DOSES=[300,600,900];
function setCh(c){ch=c;document.getElementById('bCold').className=c==='cold'?'sel-cold':'';document.getElementById('bNorm').className=c==='normal'?'sel-normal':'';}
function pickDose(v){ml=v;syncDose();}
function syncDose(){
  const i=DOSES.indexOf(ml);
  if(i<0)return;
  const t=document.getElementById('doseTrack');
  const w=t.clientWidth;
  if(!w)return;
  const base=Math.min(w*.38,180);
  const sideOffset=base/2+8+(base*.68)/2;
  document.querySelectorAll('.slide').forEach(s=>{
    const j=DOSES.indexOf(+s.dataset.ml);
    const rel=(j-i+DOSES.length)%DOSES.length;
    s.classList.toggle('pos-center',rel===0);
    s.classList.toggle('pos-right',rel===1);
    s.classList.toggle('pos-left',rel===2);
    s.style.width=base+'px';
    s.style.setProperty('--x',rel===0?'0px':(rel===1?sideOffset:-sideOffset)+'px');
    s.setAttribute('aria-pressed',rel===0?'true':'false');
  });
  document.querySelectorAll('#dots span').forEach((d,k)=>d.classList.toggle('on',DOSES[k]===ml));
}
function slideBy(d){
  const i=DOSES.indexOf(ml);
  const n=(i+d+DOSES.length)%DOSES.length;
  pickDose(DOSES[n]);
}
function showTab(name){
  const ids=['disp','cal','cfg'];
  ids.forEach(id=>{
    document.getElementById('pane'+id.charAt(0).toUpperCase()+id.slice(1)).hidden=(id!==name);
    const btn=document.getElementById('tab'+id.charAt(0).toUpperCase()+id.slice(1));
    btn.className=id===name?'on':'';
    btn.setAttribute('aria-selected',id===name?'true':'false');
  });
  if(name==='cfg'){loadConfig();loadNet();loadMqtt();}
}
function syncCalCh(){
  document.getElementById('calCold').className='calch'+(calCh==='cold'?' on':'');
  document.getElementById('calNorm').className='calch'+(calCh==='normal'?' on':'');
}
function updateCalUI(j){
  const busy=!!(j.job_active||j.ota_in_progress);
  const raw=calCh==='cold'?j.pulses_cold:j.pulses_normal;
  const rej=calCh==='cold'?(j.pulses_rejected_cold||0):(j.pulses_rejected_normal||0);
  if(calBaseline!==null)calDelta=Math.max(0,raw-calBaseline);
  document.getElementById('calCold').disabled=busy||calRunning;
  document.getElementById('calNorm').disabled=busy||calRunning;
  document.getElementById('calStartBtn').disabled=busy||calRunning||j.tank_low;
  document.getElementById('calStopBtn').disabled=!j.job_active||j.ota_in_progress;
  document.getElementById('calSaveBtn').disabled=calResult<=0||j.job_active||calRunning;
  const state=calRunning?(j.job_calibrate?'Running — press Stop when done':'Starting…')
    :(calResult>0?'Measured':(j.job_active?'Device busy':'Ready'));
  document.getElementById('calMeta').innerHTML=`
    <div class="metric"><span>Selected line</span><b>${calCh==='cold'?'Cold · GPIO18':'Normal · GPIO4'}</b></div>
    <div class="metric"><span>Pulses counted</span><b>${calBaseline===null?'—':calDelta}</b></div>
    <div class="metric"><span>Pulses rejected</span><b>${rej}</b></div>
    <div class="metric"><span>PPL ${calCh==='cold'?'Cold':'Normal'}</span><b>${(calCh==='cold'?j.ppl_cold:j.ppl_normal).toFixed(1)}</b></div>
    <div class="metric"><span>Test state</span><b>${state}</b></div>`;
}
async function loadConfig(){
  try{
    const r=await fetch('/api/config',{cache:'no-store'});
    const j=await r.json();
    document.getElementById('cfgFlowTimeout').checked=!!j.flow_timeout;
    document.getElementById('cfgTankSensor').checked=!!j.tank_sensor;
    document.getElementById('cfgTankPin').value=String(j.tank_pin_mode!=null?j.tank_pin_mode:0);
    document.getElementById('cfgTankDebounce').value=j.tank_debounce_ms||2000;
    document.getElementById('cfgSim').checked=!!(lastStatus&&lastStatus.sim);
    document.getElementById('cfgMsg').textContent='Loaded.';
  }catch(e){document.getElementById('cfgMsg').textContent='Could not load config.';}
}
function updateTankRaw(j){
  const el=document.getElementById('tankRawMeta');
  if(!el)return;
  const modes=['pull-down','floating','pull-up'];
  if(!j.tank_sensor){el.textContent='Tank sensor disabled.';return;}
  const raw=j.tank_raw===1?'HIGH':j.tank_raw===0?'LOW':'?';
  el.textContent=`Raw GPIO13: ${raw} · input ${modes[j.tank_pin_mode]||'?'} · filter ${j.tank_debounce_ms||'?'} ms`;
}
async function saveConfig(){
  const ft=document.getElementById('cfgFlowTimeout').checked?1:0;
  const ts=document.getElementById('cfgTankSensor').checked?1:0;
  const sim=document.getElementById('cfgSim').checked?1:0;
  const tp=document.getElementById('cfgTankPin').value||'0';
  const td=document.getElementById('cfgTankDebounce').value||'2000';
  try{
    await fetch(`/api/config?flow_timeout=${ft}&tank_sensor=${ts}&tank_pin=${tp}&tank_debounce=${td}`,{cache:'no-store'});
    await fetch(`/api/sim?enable=${sim}`,{cache:'no-store'});
    document.getElementById('cfgMsg').textContent='Saved.';
    refresh();
  }catch(e){document.getElementById('cfgMsg').textContent='Save failed.';}
}
function syncNetFields(){
  document.getElementById('netFields').hidden=!document.getElementById('netStatic').checked;
}
async function loadNet(){
  try{
    const r=await fetch('/api/net',{cache:'no-store'});
    const j=await r.json();
    if(!r.ok||!j.ok)throw new Error(j.error||'load failed');
    document.getElementById('netStatic').checked=(j.mode==='static');
    document.getElementById('netIp').value=j.ip||'';
    document.getElementById('netGw').value=j.gw||'';
    document.getElementById('netMask').value=j.mask||'255.255.255.0';
    document.getElementById('netDns').value=j.dns||'';
    syncNetFields();
    document.getElementById('netMsg').innerHTML=
      `Mode <b>${j.mode}</b> · current IP <b>${j.current_ip||'—'}</b> · mDNS <b>${j.mdns}</b>`;
  }catch(e){
    syncNetFields();
    document.getElementById('netMsg').textContent='Could not read network config.';
  }
}
async function saveNet(){
  const useStatic=document.getElementById('netStatic').checked;
  let q='mode='+(useStatic?'static':'dhcp');
  let target='';
  if(useStatic){
    const ip=document.getElementById('netIp').value.trim();
    const gw=document.getElementById('netGw').value.trim();
    const mask=document.getElementById('netMask').value.trim()||'255.255.255.0';
    const dns=document.getElementById('netDns').value.trim();
    if(!ip||!gw){alert('Enter the IP address and gateway.');return;}
    q+=`&ip=${encodeURIComponent(ip)}&gw=${encodeURIComponent(gw)}&mask=${encodeURIComponent(mask)}&dns=${encodeURIComponent(dns)}`;
    target=ip;
  }
  const msg=useStatic
    ?`Save static IP ${target} and reboot?\n\nIf this address is wrong you lose the LAN link. The device AP (192.168.4.1) still works for recovery.`
    :'Switch back to DHCP and reboot? The router will assign a new IP.';
  if(!confirm(msg))return;
  try{
    const r=await fetch(`/api/net?${q}&reboot=1`,{cache:'no-store'});
    const j=await r.json();
    if(!r.ok||!j.ok){alert(j.error||'Save failed');return;}
    alert(useStatic
      ?`Saved ${target}. Rebooting — reopen the UI at http://${target}/ or http://${j.mdns}/`
      :'Saved DHCP mode. Rebooting — check the new IP on the Network card.');
  }catch(e){
    alert('Device rebooted — reconnect to its AP (192.168.4.1) if the new IP is unreachable.');
  }
}
async function loadMqtt(){
  try{
    const r=await fetch('/api/mqtt',{cache:'no-store'});
    const j=await r.json();
    document.getElementById('mqOn').checked=!!j.enabled;
    document.getElementById('mqCmd').checked=!!j.allow_cmd;
    document.getElementById('mqTls').checked=!!j.tls;
    document.getElementById('mqHost').value=j.host||'';
    document.getElementById('mqPort').value=j.port||(j.tls?8883:1883);
    document.getElementById('mqUser').value=j.user||'';
    document.getElementById('mqPass').value='';
    document.getElementById('mqBase').value=j.base||'banyu';
    document.getElementById('mqInt').value=j.interval_ms||5000;
    const m=document.getElementById('mqMsg');
    if(!j.enabled){m.textContent='MQTT is off.';return;}
    const topics='<code>'+j.topic_base+'/#</code>';
    const tls=j.tls?(j.clock_ok?' · TLS · clock ok':' · TLS · waiting for NTP clock'):' · plain TCP';
    m.innerHTML=j.connected
      ?'Connected to <b>'+j.host+':'+j.port+'</b>'+tls+' · topics '+topics+' · '+j.publishes+' publishes'
      :'Not connected'+tls+(j.last_error?' · '+j.last_error:'')+' · topics '+topics;
  }catch(e){document.getElementById('mqMsg').textContent='Could not read MQTT config.';}
}
async function saveMqtt(){
  const q=new URLSearchParams();
  q.set('enabled',document.getElementById('mqOn').checked?'1':'0');
  q.set('allow_cmd',document.getElementById('mqCmd').checked?'1':'0');
  q.set('tls',document.getElementById('mqTls').checked?'1':'0');
  q.set('host',document.getElementById('mqHost').value.trim());
  q.set('port',document.getElementById('mqPort').value||'1883');
  q.set('user',document.getElementById('mqUser').value.trim());
  q.set('base',document.getElementById('mqBase').value.trim()||'banyu');
  q.set('interval',document.getElementById('mqInt').value||'5000');
  const pw=document.getElementById('mqPass').value;
  if(pw)q.set('pass',pw);
  try{
    const r=await fetch('/api/mqtt?'+q.toString(),{cache:'no-store'});
    const j=await r.json();
    if(!r.ok||!j.ok){alert(j.error||'Save failed');return;}
    document.getElementById('mqPass').value='';
    await loadMqtt();
    refresh();
  }catch(e){document.getElementById('mqMsg').textContent='Save failed.';}
}
function updateCfgUI(j){
  document.getElementById('cfgSim').checked=!!j.sim;
  updateTankRaw(j);
}
function setCalCh(c){
  if(calRunning||(lastStatus&&lastStatus.job_active)){alert('Stop the current dispense first.');return;}
  if(c===calCh)return;
  calCh=c;calBaseline=null;calDelta=0;calResult=0;
  document.getElementById('calActual').value='';
  document.getElementById('calMsg').textContent='Ready.';
  syncCalCh();
  if(lastStatus)updateCalUI(lastStatus);
}
async function calStart(){
  if(!lastStatus){alert('Wait for the device status.');return;}
  if(lastStatus.job_active){alert('Stop the current dispense first.');return;}
  if(lastStatus.tank_low){alert('Tank is low — refill before calibration.');return;}
  calBaseline=calCh==='cold'?lastStatus.pulses_cold:lastStatus.pulses_normal;
  calDelta=0;calResult=0;calRunning=true;calRunSeenActive=false;calStartedAt=Date.now();
  document.getElementById('calActual').value='';
  document.getElementById('calMsg').textContent='Test running — collect ~900 mL, then press Stop.';
  updateCalUI(lastStatus);
  try{
    const r=await fetch(`/api/calrun?channel=${calCh}`,{cache:'no-store'});
    const t=await r.text();
    if(!r.ok){calRunning=false;calBaseline=null;alert(t);document.getElementById('calMsg').textContent=t;}
  }catch(e){
    calRunning=false;calBaseline=null;
    document.getElementById('calMsg').textContent='Could not start the calibration test.';
  }
  refresh();
}
async function calSave(){
  if(calResult<=0){alert('Run a completed calibration test first.');return;}
  const actual=parseFloat(document.getElementById('calActual').value);
  if(!(actual>0)){alert('Enter the actual measured volume in mL.');document.getElementById('calActual').focus();return;}
  const ppl=calResult*1000/actual;
  if(ppl<100||ppl>50000){alert('Calculated PPL is outside the allowed range (100..50000).');return;}
  if(!confirm(`Save ${ppl.toFixed(1)} PPL for the ${calCh==='cold'?'Cold':'Normal'} line?`))return;
  const r=await fetch(`/api/cal?ppl=${ppl.toFixed(2)}&channel=${calCh}`,{cache:'no-store'});
  const t=await r.text();
  if(!r.ok){alert(t);return;}
  document.getElementById('calMsg').textContent=`Saved ${ppl.toFixed(1)} PPL for ${calCh==='cold'?'Cold':'Normal'}. Recalibrate the other line separately.`;
  refresh();
}
async function calResetPpl(){
  const name=calCh==='cold'?'Cold':'Normal';
  if(!confirm(`Reset ${name} PPL to the nominal 450?`))return;
  try{
    const r=await fetch(`/api/cal?channel=${calCh}&ppl=450`,{cache:'no-store'});
    const t=await r.text();
    if(!r.ok){alert(t);return;}
    calResult=0;calBaseline=null;calDelta=0;
    document.getElementById('calActual').value='';
    document.getElementById('calMsg').textContent=`${name} reset to 450 PPL. Run a fresh 900 mL test to calibrate.`;
    refresh();
  }catch(e){alert('Reset failed.');}
}
(function(){
  const t=document.getElementById('doseTrack');
  let sx=0,sy=0,dx=0,dy=0,suppress=false;
  t.addEventListener('touchstart',e=>{
    const p=e.changedTouches[0];sx=p.clientX;sy=p.clientY;dx=0;dy=0;
  },{passive:true});
  t.addEventListener('touchmove',e=>{
    const p=e.changedTouches[0];dx=p.clientX-sx;dy=p.clientY-sy;
  },{passive:true});
  t.addEventListener('touchend',()=>{
    if(Math.abs(dx)>45&&Math.abs(dx)>Math.abs(dy)){
      suppress=true;slideBy(dx<0?1:-1);
      setTimeout(()=>{suppress=false;},350);
    }
    dx=0;dy=0;
  },{passive:true});
  t.addEventListener('click',e=>{
    if(suppress){e.preventDefault();e.stopPropagation();suppress=false;}
  },true);
  document.addEventListener('keydown',e=>{
    if(e.key==='ArrowLeft')slideBy(-1);
    else if(e.key==='ArrowRight')slideBy(1);
  });
  let rz;
  window.addEventListener('resize',()=>{clearTimeout(rz);rz=setTimeout(syncDose,80);});
})();
async function refresh(){try{const r=await fetch('/api/status');const j=await r.json();
lastStatus=j;
const b=document.getElementById('banner');
if(j.tank_low){b.className='banner alert';b.textContent='Tank low · refill required';}
else{b.className='banner ok';b.textContent='Tank level normal';}
document.getElementById('dot').className='dot'+(j.tank_low?' bad':'');
isPaused=!!j.job_paused;
const isFinishing=j.job_state==='finishing';
const isCalibrating=j.job_state==='calibrating';
document.getElementById('st').textContent=isCalibrating
  ?`Calibrating ${j.channel} · collect ~900 mL then press Stop (Calibration tab)`
  :(isFinishing
  ?`Target reached · pump OFF · closing ${j.channel} valve`
  :(isPaused
  ?`Paused ${j.channel} · ${j.job_ml.toFixed(0)} / ${j.job_target_ml.toFixed(0)} mL · ${j.job_remaining_ml.toFixed(0)} mL remaining`
  :(j.job_active
    ?`Dispensing ${j.channel} · ${j.job_ml.toFixed(0)} / ${j.job_target_ml.toFixed(0)} mL · ${j.flow_lpm.toFixed(2)} L/min`
    :(j.error||'Ready · select a line and volume, then start'))));
const pct=j.job_target_ml>0?Math.min(100,j.job_ml/j.job_target_ml*100):0;
document.getElementById('bar').style.width=(j.job_active?pct:0)+'%';
const live=document.getElementById('live');
if(j.job_active){live.style.display='block';live.innerHTML=`${j.job_ml.toFixed(0)} <small>/ ${j.job_target_ml.toFixed(0)} mL</small>`;}
else live.style.display='none';
document.querySelectorAll('.slide').forEach(b=>{
const m=parseFloat(b.dataset.ml||b.textContent);
b.classList.toggle('run',!!j.job_active&&!isPaused&&Math.abs(m-j.job_target_ml)<0.6);
});
document.getElementById('bStart').disabled=j.job_active||j.tank_low||j.ota_in_progress;
const bp=document.getElementById('bPause');
bp.disabled=!j.job_active||j.ota_in_progress||isFinishing;
bp.setAttribute('aria-label',isPaused?'Resume dispensing':'Pause dispensing');
document.getElementById('pauseLbl').textContent=isPaused?'RESUME':'PAUSE';
document.getElementById('pauseGlyph').innerHTML=isPaused
  ?'<path d="M8 5.5v13l10-6.5-10-6.5Z" fill="currentColor"/>'
  :'<rect x="6.5" y="5" width="4" height="14" rx="1.2" fill="currentColor"/><rect x="13.5" y="5" width="4" height="14" rx="1.2" fill="currentColor"/>';
document.getElementById('bStop').disabled=!j.job_active||j.ota_in_progress;
document.getElementById('build').textContent=j.build||'—';
document.getElementById('meta').innerHTML=`
<div class="metric"><span>Line</span><b>${j.job_active?j.channel:'—'}</b></div>
<div class="metric"><span>Target</span><b>${j.job_target_ml.toFixed(0)} mL</b></div>
<div class="metric"><span>Flow total</span><b>${j.total_ml.toFixed(0)} mL</b></div>
<div class="metric"><span>Pulses</span><b>${j.pulses}</b></div>
<div class="metric"><span>Calibration</span><b>${j.ppl.toFixed(0)} PPL</b></div>
<div class="metric"><span>Flow rate</span><b>${j.flow_lpm.toFixed(2)} L/min</b></div>
<div class="metric"><span>Signal</span><b>${j.rssi} dBm</b></div>
<div class="metric"><span>Wi-Fi</span><b>${j.wifi_status_name}</b></div>
${j.error?`<div class="metric error"><span>Last event</span><b>${j.error}</b></div>`:''}
`;
document.getElementById('ap').textContent=j.ap_ssid;document.getElementById('ip').textContent='STA IP: '+j.ip;
document.getElementById('wifi').innerHTML=`Saved: <b>${j.sta_ssid||'(none)'}</b> · ${j.wifi_status_name} · IP ${j.ip} · RSSI ${j.rssi} dBm`;
if(calRunning){
  if(j.job_active)calRunSeenActive=true;
  if(calBaseline!==null)calDelta=Math.max(0,(calCh==='cold'?j.pulses_cold:j.pulses_normal)-calBaseline);
  if(!j.job_active&&(calRunSeenActive||Date.now()-calStartedAt>2500)){
    calRunning=false;calResult=calDelta;
    document.getElementById('calMsg').textContent=j.error
      ?`Test ended: ${j.error}. Enter only the water actually collected.`
      :'Test complete. Enter the actual measured volume.';
  }
}
updateCalUI(j);
updateCfgUI(j);
}catch(e){}}
async function go(){const r=await fetch(`/api/dispense?channel=${ch}&ml=${ml}`,{cache:'no-store'});const t=await r.text();if(!r.ok)alert(t);refresh();}
async function togglePause(){const r=await fetch(isPaused?'/api/resume':'/api/pause',{cache:'no-store'});const t=await r.text();if(!r.ok)alert(t);refresh();}
async function stop(){await fetch('/api/stop',{cache:'no-store'});refresh();}
function uploadFw(){
const f=document.getElementById('fw').files[0];
const pw=document.getElementById('otapw').value;
if(!f){alert('Choose a firmware .bin file first');return;}
if(!pw){alert('Enter the OTA password');document.getElementById('otapw').focus();return;}
if(!confirm('Upload '+f.name+' ('+Math.round(f.size/1024)+' KB) and reboot?'))return;
const btn=document.getElementById('bOta'),bar=document.getElementById('otabar'),st=document.getElementById('ota');
btn.disabled=true;bar.style.width='0%';st.textContent='Uploading… 0%';
const fd=new FormData();fd.append('firmware',f);
const xhr=new XMLHttpRequest();
xhr.open('POST','/update');
xhr.setRequestHeader('X-OTA-Pass',pw);
xhr.upload.onprogress=e=>{if(e.lengthComputable){const p=Math.round(e.loaded/e.total*100);bar.style.width=p+'%';st.textContent='Uploading… '+p+'%';}};
xhr.onload=()=>{const ok=xhr.status===200&&xhr.responseText.trim()==='OK';
if(ok){bar.style.width='100%';st.textContent='Update OK — rebooting…';setTimeout(()=>location.reload(),10000);}
else if(xhr.status===401){st.textContent='Wrong OTA password.';btn.disabled=false;}
else{st.textContent='Update failed: '+xhr.responseText;btn.disabled=false;}};
xhr.onerror=()=>{st.textContent='Upload interrupted — device may be rebooting; reconnect in ~15 s.';};
xhr.send(fd);
}
async function saveWifi(){const s=document.getElementById('ssid').value.trim(),p=document.getElementById('pass').value;if(!s){alert('Pick/enter an SSID first');return;}const r=await fetch(`/api/wifi?ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent(p)}`);const t=await r.text();if(!r.ok){alert(t);return;}alert('Saved "'+s+'". Rebooting — reconnect AP, then check STA IP.');}
async function forgetWifi(){if(!confirm('Delete the saved Wi-Fi and reboot into AP-only mode?'))return;const btn=document.getElementById('bForget');btn.disabled=true;btn.textContent='Clearing…';try{const r=await fetch('/api/wifi?clear=1',{cache:'no-store'});const t=await r.text();alert(t);}catch(e){alert('Device rebooted — join its AP and reload 192.168.4.1');}}
async function scanWifi(){const btn=document.getElementById('bScan');btn.disabled=true;btn.textContent='Scanning…';try{const r=await fetch('/api/wifiscan');const list=await r.json();const sel=document.getElementById('ssidSel');sel.innerHTML='';if(!list.length){sel.innerHTML='<option value="">(no networks found)</option>';}list.sort((a,b)=>b.rssi-a.rssi).forEach(n=>{const o=document.createElement('option');o.value=n.ssid;o.textContent=`${n.ssid} (${n.rssi} dBm, ch${n.ch})`;sel.appendChild(o);});pickSsid();document.getElementById('wifi').textContent=`Found ${list.length} network(s) — pick one, enter password, Save.`;}catch(e){alert('Scan failed');}btn.disabled=false;btn.textContent='Scan';}
function pickSsid(){const v=document.getElementById('ssidSel').value;if(v)document.getElementById('ssid').value=v;}
setCh('cold');syncDose();syncCalCh();showTab('disp');setInterval(refresh,1000);refresh();
</script></body></html>)rawliteral";

// ---------- API response helpers (CORS for tablet/external frontends) ----------
void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Max-Age", "86400");
}

void sendApiText(int code, const String &body) {
  addCorsHeaders();
  server.send(code, "text/plain", body);
}

void sendApiJson(int code, const String &body) {
  addCorsHeaders();
  server.send(code, "application/json", body);
}

void sendApiOptions() {
  addCorsHeaders();
  server.send(204, "text/plain", "");
}

String jsonEsc(const String &in) {
  String o;
  o.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else o += c;
  }
  return o;
}

String statusJson() {
  bool low = readTankLow();
  float ml = currentJobMl();
  float remaining = job.active ? (job.targetMl - ml) : 0;
  if (remaining < 0) remaining = 0;
  unsigned long totalPulses = flowPulses();
  unsigned long coldPulses = hwFlowPulses(CH_COLD);
  unsigned long normalPulses = hwFlowPulses(CH_NORMAL);
  String s = "{";
  bool pumpCold = job.active && !job.paused && !job.finishing && job.channel == CH_COLD;
  bool pumpNormal = job.active && !job.paused && !job.finishing && job.channel == CH_NORMAL;
  s += "\"job_active\":" + String(job.active ? "true" : "false") + ",";
  s += "\"job_paused\":" + String(job.paused ? "true" : "false") + ",";
  s += "\"job_calibrate\":" + String(job.calibrate ? "true" : "false") + ",";
  s += "\"job_state\":\"" + String(jobStateName()) + "\",";
  s += "\"channel\":\"" + String(channelName(job.active ? job.channel : CH_NONE)) + "\",";
  s += "\"pump_cold\":" + String(pumpCold ? "true" : "false") + ",";
  s += "\"pump_normal\":" + String(pumpNormal ? "true" : "false") + ",";
  s += "\"job_ml\":" + String(ml, 1) + ",";
  s += "\"job_target_ml\":" + String(job.targetMl, 1) + ",";
  s += "\"job_remaining_ml\":" + String(remaining, 1) + ",";
  s += "\"tank_low\":" + String(low ? "true" : "false") + ",";
  s += "\"tank_state\":\"" + String(low ? "LOW" : "NORMAL") + "\",";
  s += "\"tank_raw\":" + String(cfgTankSensor ? digitalRead(PIN_TANK_LOW) : -1) + ",";
  s += "\"tank_pin_mode\":" + String(cfgTankPinMode) + ",";
  s += "\"tank_debounce_ms\":" + String(cfgTankDebounceMs) + ",";
  s += "\"tank_invert\":" + String(TANK_WATER_PRESENT_HIGH ? "false" : "true") + ",";
  s += "\"flow_lpm\":" + String(liveLpm, 3) + ",";
  s += "\"pulses\":" + String(totalPulses) + ",";
  s += "\"total_ml\":" + String(pulsesToMl(coldPulses, CH_COLD) + pulsesToMl(normalPulses, CH_NORMAL)
                              + (simEnable ? pulsesToMl(simPulses, CH_COLD) : 0), 1) + ",";
  s += "\"pulses_cold\":" + String(coldPulses) + ",";
  s += "\"pulses_normal\":" + String(normalPulses) + ",";
  s += "\"pulses_rejected_cold\":" + String(flowRejectedCold) + ",";
  s += "\"pulses_rejected_normal\":" + String(flowRejectedNormal) + ",";
  s += "\"total_ml_cold\":" + String(pulsesToMl(coldPulses, CH_COLD), 1) + ",";
  s += "\"total_ml_normal\":" + String(pulsesToMl(normalPulses, CH_NORMAL), 1) + ",";
  s += "\"ppl\":" + String(pplFor(job.active ? job.channel : CH_COLD), 1) + ",";
  s += "\"ppl_cold\":" + String(pplCold, 1) + ",";
  s += "\"ppl_normal\":" + String(pplNormal, 1) + ",";
  s += "\"tail_comp_ml\":" + String(tailCompRef(job.active ? job.channel : CH_COLD), 1) + ",";
  s += "\"tail_comp_learned\":" + String(tailLearnedRef(job.active ? job.channel : CH_COLD) ? "true" : "false") + ",";
  s += "\"tail_comp_cold\":" + String(tailCompCold, 1) + ",";
  s += "\"tail_comp_normal\":" + String(tailCompNormal, 1) + ",";
  s += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  s += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  s += "\"ip_mode\":\"" + String(netStatic ? "static" : "dhcp") + "\",";
  s += "\"ip_static\":" + String(netStatic ? "true" : "false") + ",";
  s += "\"net_ip\":\"" + netIp + "\",";
  s += "\"net_gw\":\"" + netGw + "\",";
  s += "\"net_mask\":\"" + netMask + "\",";
  s += "\"net_dns\":\"" + netDns + "\",";
  s += "\"host\":\"" HOSTNAME ".local\",";
  s += "\"ap_ssid\":\"" + String(apSsid) + "\",";
  s += "\"sta_ssid\":\"" + staSsid + "\",";
  s += "\"wifi_status\":" + String((int)WiFi.status()) + ",";
  s += "\"wifi_status_name\":\"" + String(wifiStatusName(WiFi.status())) + "\",";
#ifdef ESP8266
  s += "\"rssi\":" + String(WiFi.isConnected() ? WiFi.RSSI() : 0) + ",";
#else
  s += "\"rssi\":" + String(WiFi.isConnected() ? WiFi.RSSI() : 0) + ",";
#endif
  String e = job.error; e.replace("\"", "'");
  s += "\"error\":\"" + e + "\",";
  s += "\"mqtt_enabled\":" + String(mqttEnabled ? "true" : "false") + ",";
  s += "\"mqtt_connected\":" + String(mqtt.connected() ? "true" : "false") + ",";
  s += "\"mqtt_host\":\"" + jsonEsc(mqttHost) + "\",";
  s += "\"mqtt_topic\":\"" + jsonEsc(mqttTopicBase) + "\",";
  s += "\"mqtt_tls\":" + String(mqttTls ? "true" : "false") + ",";
  s += "\"build\":\"" __DATE__ " " __TIME__ "\",";
  s += "\"ota_in_progress\":" + String(otaInProgress ? "true" : "false") + ",";
  s += "\"sim\":" + String(simEnable ? "true" : "false") + ",";
  s += "\"sim_hz\":" + String(simHz, 1) + ",";
  s += "\"flow_timeout\":" + String(cfgFlowTimeout ? "true" : "false") + ",";
  s += "\"tank_sensor\":" + String(cfgTankSensor ? "true" : "false");
  s += "}";
  return s;
}

String netJson() {
  String s = "{";
  s += "\"ok\":true,";
  s += "\"mode\":\"" + String(netStatic ? "static" : "dhcp") + "\",";
  s += "\"ip\":\"" + netIp + "\",";
  s += "\"gw\":\"" + netGw + "\",";
  s += "\"mask\":\"" + netMask + "\",";
  s += "\"dns\":\"" + netDns + "\",";
  s += "\"current_ip\":\"" + WiFi.localIP().toString() + "\",";
  s += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  s += "\"hostname\":\"" HOSTNAME "\",";
  s += "\"mdns\":\"" HOSTNAME ".local\"";
  s += "}";
  return s;
}

// ---------- MQTT telemetry ----------
// Publishes /api/status as JSON and, optionally, accepts commands. Everything
// here is non-blocking apart from the connect attempt itself, which is skipped
// while a dispense/calibration job is running so a dead broker can never stall
// the flow control loop.
String mqttTopic(const char *leaf) {
  return mqttTopicBase + "/" + leaf;
}

void mqttApplyTopics() {
  mqttBase.trim();
  while (mqttBase.startsWith("/")) mqttBase.remove(0, 1);
  while (mqttBase.endsWith("/")) mqttBase.remove(mqttBase.length() - 1);
  if (!mqttBase.length()) mqttBase = MQTT_DEFAULT_BASE;
  if (!mqttDeviceId.length()) {
    uint32_t chip = 0;
#ifdef ESP8266
    chip = ESP.getChipId();
#else
    chip = (uint16_t)(ESP.getEfuseMac() & 0xFFFF);
#endif
    char b[8];
    snprintf(b, sizeof(b), "%04x", (unsigned)(chip & 0xFFFF));
    mqttDeviceId = String(HOSTNAME) + "-" + b;
  }
  mqttTopicBase = mqttBase + "/" + mqttDeviceId;
}

// Split "mqtt://host:1883/path" into a bare host and an optional port. Returns
// false for input that cannot be used as a TCP host.
bool mqttParseHost(String &host, uint16_t &port) {
  host.trim();
  if (host.startsWith("mqtt://")) host.remove(0, 7);
  else if (host.startsWith("tcp://")) host.remove(0, 6);
  int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);
  int colon = host.lastIndexOf(':');
  if (colon > 0) {
    long p = host.substring(colon + 1).toInt();
    if (p < 1 || p > 65535) return false;
    port = (uint16_t)p;
    host = host.substring(0, colon);
  }
  host.trim();
  if (!host.length() || host.length() > 64) return false;
  for (size_t i = 0; i < host.length(); i++)
    if (host[i] == ' ' || host[i] == '"' || host[i] == '\\') return false;
  return true;
}

// Optional TLS for hosted brokers. include/certs.h carries a small public root
// set (Let's Encrypt, DigiCert, Amazon) instead of the 200-certificate ESP32
// bundle, which would cost ~200 kB of flash. A private CA and/or a mutual-TLS
// client certificate can override it over the local API.
bool mqttClockOk() {
#ifdef ESP8266
  return true;  // TLS is ESP32-only; the D1 mini test build stays plain TCP
#else
  return (unsigned long)time(nullptr) > MQTT_CLOCK_MIN_EPOCH;
#endif
}

void mqttStartClock() {
#ifndef ESP8266
  configTime(0, 0, MQTT_NTP_SERVER_1, MQTT_NTP_SERVER_2);
  Serial.println("[MQTT] SNTP sync requested (TLS certificate validation needs the clock)");
#endif
}

// Point PubSubClient at the plain or the TLS socket. Called whenever the MQTT
// transport settings change; the caller disconnects first.
void mqttApplyTransport() {
#ifndef ESP8266
  if (mqttTls) {
    mqttSecure.setHandshakeTimeout(10);
    mqttSecure.setTimeout(MQTT_SOCKET_TIMEOUT_S);
    if (mqttCa.length()) {
      mqttSecure.setCACert(mqttCa.c_str());
      Serial.printf("[MQTT] TLS: custom CA (%u bytes)\n", (unsigned)mqttCa.length());
    } else {
      mqttSecure.setCACert(MQTT_CA_BUNDLE);
      Serial.println("[MQTT] TLS: built-in root CA set (4 roots)");
    }
    if (mqttClientCert.length() && mqttClientKey.length()) {
      mqttSecure.setCertificate(mqttClientCert.c_str());
      mqttSecure.setPrivateKey(mqttClientKey.c_str());
      Serial.println("[MQTT] TLS: mutual-TLS client certificate loaded");
    }
    mqtt.setClient(mqttSecure);
  } else {
    mqtt.setClient(mqttNet);
  }
#endif
}

String mqttJson() {
  String s = "{";
  s += "\"ok\":true,";
  s += "\"enabled\":" + String(mqttEnabled ? "true" : "false") + ",";
  s += "\"connected\":" + String(mqtt.connected() ? "true" : "false") + ",";
  s += "\"host\":\"" + jsonEsc(mqttHost) + "\",";
  s += "\"port\":" + String(mqttPort) + ",";
  s += "\"user\":\"" + jsonEsc(mqttUser) + "\",";
  s += "\"has_pass\":" + String(mqttPass.length() ? "true" : "false") + ",";
  s += "\"base\":\"" + jsonEsc(mqttBase) + "\",";
  s += "\"device\":\"" + jsonEsc(mqttDeviceId) + "\",";
  s += "\"topic_base\":\"" + jsonEsc(mqttTopicBase) + "\",";
  s += "\"interval_ms\":" + String(mqttIntervalMs) + ",";
  s += "\"allow_cmd\":" + String(mqttAllowCmd ? "true" : "false") + ",";
  s += "\"state\":" + String(mqtt.state()) + ",";
  s += "\"publishes\":" + String(mqttPublishCount) + ",";
  s += "\"connects\":" + String(mqttConnectCount) + ",";
  s += "\"tls\":" + String(mqttTls ? "true" : "false") + ",";
  s += "\"clock_ok\":" + String(mqttClockOk() ? "true" : "false") + ",";
  s += "\"has_ca\":" + String(mqttCa.length() ? "true" : "false") + ",";
  s += "\"has_client_cert\":" + String(mqttClientCert.length() ? "true" : "false") + ",";
  s += "\"last_error\":\"" + jsonEsc(mqttLastError) + "\"";
  s += "}";
  return s;
}

void mqttPublishStatus() {
  if (!mqtt.connected()) return;
  String st = statusJson();
  // Retained so a dashboard that connects later immediately sees the last
  // state; the availability topic tells it whether that state is still live.
  if (mqtt.publish(mqttTopic("status").c_str(), st.c_str(), true)) mqttPublishCount++;
}

void mqttPublishEvent(const String &payload) {
  if (mqtt.connected()) mqtt.publish(mqttTopic("event").c_str(), payload.c_str(), false);
}

// Minimal key/value readers for a flat JSON command body:
//   {"cmd":"dispense","channel":"cold","ml":300}
String mqttJsonString(const String &body, const char *key) {
  String pat = "\"" + String(key) + "\"";
  int k = body.indexOf(pat);
  if (k < 0) return "";
  int c = body.indexOf(':', k + pat.length());
  if (c < 0) return "";
  int q1 = body.indexOf('"', c + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

float mqttJsonNumber(const String &body, const char *key) {
  String pat = "\"" + String(key) + "\"";
  int k = body.indexOf(pat);
  if (k < 0) return 0;
  int c = body.indexOf(':', k + pat.length());
  if (c < 0) return 0;
  int i = c + 1;
  while (i < (int)body.length() && body[i] != '-' && body[i] != '.' &&
         (body[i] < '0' || body[i] > '9')) i++;
  int start = i;
  while (i < (int)body.length() && (body[i] == '-' || body[i] == '.' ||
         (body[i] >= '0' && body[i] <= '9'))) i++;
  if (start >= i) return 0;
  return body.substring(start, i).toFloat();
}

// Idempotency for cloud commands: the backend retries with the same "id" until
// it sees an ack. A duplicate id must never dispense twice.
String mqttSeenIds[8];
uint8_t mqttSeenIdx = 0;

bool mqttSeenBefore(const String &id) {
  if (!id.length()) return false;
  for (uint8_t i = 0; i < 8; i++)
    if (mqttSeenIds[i] == id) return true;
  mqttSeenIds[mqttSeenIdx] = id;
  mqttSeenIdx = (mqttSeenIdx + 1) % 8;
  return false;
}

void mqttSendAck(bool ok, const String &cmd, const String &id, Channel ch,
                 float ml, const String &msg, bool dup = false) {
  String ack = "{";
  ack += "\"ok\":" + String(ok ? "true" : "false") + ",";
  ack += "\"cmd\":\"" + jsonEsc(cmd) + "\",";
  if (id.length()) ack += "\"id\":\"" + jsonEsc(id) + "\",";
  ack += "\"channel\":\"" + String(channelName(ch)) + "\",";
  ack += "\"ml\":" + String(ml, 1) + ",";
  ack += "\"msg\":\"" + jsonEsc(msg) + "\"";
  if (dup) ack += ",\"dup\":true";
  ack += "}";
  if (mqtt.connected()) mqtt.publish(mqttTopic("ack").c_str(), ack.c_str(), false);
}

// Accepts "dispense cold 300", "cold 300", "stop", "pause", "resume",
// "status" or the JSON form above. Only called when mqttAllowCmd is on.
void mqttHandleCommand(const String &raw) {
  String body = raw;
  body.trim();
  String lc = body;
  lc.toLowerCase();
  String cmd, chStr, id;
  float ml = 0;

  if (lc.startsWith("{")) {
    cmd = mqttJsonString(body, "cmd");
    chStr = mqttJsonString(body, "channel");
    id = mqttJsonString(body, "id");
    ml = mqttJsonNumber(body, "ml");
    if (ml <= 0) ml = mqttJsonNumber(body, "volume");
  } else {
    String norm = lc;
    norm.replace(",", " ");
    norm.replace(":", " ");
    String tok[4];
    int n = 0, p = 0;
    while (p < (int)norm.length() && n < 4) {
      int q = norm.indexOf(' ', p);
      if (q < 0) q = norm.length();
      String t = norm.substring(p, q);
      t.trim();
      if (t.length()) tok[n++] = t;
      p = q + 1;
    }
    if (n >= 1) cmd = tok[0];
    if (n >= 2) chStr = tok[1];
    if (n >= 3) ml = tok[2].toFloat();
    // "cold 300" / "normal 600" shorthand
    if (cmd == "cold" || cmd == "normal") {
      ml = chStr.toFloat();
      chStr = cmd;
      cmd = "dispense";
    }
  }
  cmd.trim();
  cmd.toLowerCase();

  // Same command id arriving again (backend retry) must not dispense twice.
  if (mqttSeenBefore(id)) {
    mqttSendAck(true, cmd, id, CH_NONE, ml, "duplicate command id ignored", true);
    Serial.printf("[MQTT] cmd id '%s' duplicate -> ignored\n", id.c_str());
    return;
  }

  bool ok = false;
  String msg;
  Channel ch = CH_NONE;

  if (cmd == "dispense" || cmd == "start" || cmd == "pour") {
    ch = parseChannel(chStr);
    if (ch == CH_NONE)
      msg = "unknown channel (cold|normal)";
    else if (!(ml >= 1 && ml <= MAX_VOLUME_ML))
      msg = String("ml must be 1..") + String(MAX_VOLUME_ML);
    else if (startDispense(ch, ml)) {
      ok = true;
      msg = String("started ") + channelName(ch) + " " + String(ml, 0) + " mL";
    } else {
      msg = job.active ? (job.paused ? "job paused" : "already dispensing") : job.error;
      if (!msg.length()) msg = "rejected";
    }
  } else if (cmd == "stop") {
    if (job.active) stopDispense("stopped via MQTT");
    else relaysOff();
    ok = true;
    msg = "stopped";
  } else if (cmd == "pause") {
    ok = pauseDispense();
    msg = ok ? "paused" : (job.active ? "not running" : "no active job");
  } else if (cmd == "resume") {
    ok = resumeDispense();
    msg = ok ? "resumed" : "not paused";
  } else if (cmd == "status") {
    mqttPublishStatus();
    ok = true;
    msg = "status published";
  } else if (!cmd.length()) {
    msg = "empty command";
  } else {
    msg = "unknown command: " + cmd;
  }

  mqttSendAck(ok, cmd, id, ch, ml, msg);
  Serial.printf("[MQTT] cmd '%s' -> %s (%s)\n", cmd.c_str(), ok ? "OK" : "REJECT", msg.c_str());
}

void mqttOnMessage(char *topic, byte *payload, unsigned int len) {
  if (!mqttAllowCmd) return;
  if (len > 256) len = 256;
  char buf[257];
  memcpy(buf, payload, len);
  buf[len] = 0;
  mqttHandleCommand(String(buf));
}

void mqttTryConnect() {
  if (!mqttEnabled || !mqttHost.length() || WiFi.status() != WL_CONNECTED) return;
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  String avail = mqttTopic("availability");
  bool ok;
  if (mqttUser.length())
    ok = mqtt.connect(mqttDeviceId.c_str(), mqttUser.c_str(),
                      mqttPass.length() ? mqttPass.c_str() : nullptr,
                      avail.c_str(), 1, true, "offline");
  else
    ok = mqtt.connect(mqttDeviceId.c_str(), avail.c_str(), 1, true, "offline");
  if (!ok) {
    mqttLastError = String("connect rc=") + mqtt.state();
    Serial.printf("[MQTT] %s:%u connect failed (%s)\n", mqttHost.c_str(), mqttPort,
                  mqttLastError.c_str());
    return;
  }
  mqttConnectCount++;
  mqttLastError = "";
  mqtt.publish(avail.c_str(), "online", true);
  String info = "{\"device\":\"" + mqttDeviceId + "\",\"hostname\":\"" HOSTNAME "\",";
  info += "\"fw\":\"" __DATE__ " " __TIME__ "\",";
  info += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  info += "\"mdns\":\"" HOSTNAME ".local\",";
  info += "\"base_topic\":\"" + mqttTopicBase + "\",";
  info += "\"http_api\":true}";
  mqtt.publish(mqttTopic("info").c_str(), info.c_str(), true);
  if (mqttAllowCmd) mqtt.subscribe(mqttTopic("cmd").c_str(), 1);
  Serial.printf("[MQTT] connected %s:%u as %s, topics %s/#%s\n",
                mqttHost.c_str(), mqttPort, mqttDeviceId.c_str(),
                mqttTopicBase.c_str(), mqttAllowCmd ? ", commands ON" : "");
  mqttPublishStatus();
}

bool mqttRetryNow = false;

void mqttTick() {
  static unsigned long lastTryMs = 0;
  static unsigned long lastPubMs = 0;
  static String lastJobKey;
  static bool lastTank = false;
  static bool lastWifi = false;

  bool wifi = WiFi.status() == WL_CONNECTED;
  if (wifi != lastWifi) {
    lastWifi = wifi;
    Serial.printf("[MQTT] Wi-Fi %s\n", wifi ? "up" : "down");
  }

  if (!mqttEnabled) {
    if (mqtt.connected()) {
      mqtt.publish(mqttTopic("availability").c_str(), "offline", true);
      mqtt.disconnect();
    }
    return;
  }
  if (!wifi) return;

  // TLS certificate validation compares the broker certificate with the system
  // clock; a 1970 clock fails every handshake. Keep requesting SNTP (every
  // 10 s, non-blocking) until the time is plausible, then connect.
  if (mqttTls && !mqttClockOk()) {
    static unsigned long lastNtpMs = 0;
    if (millis() - lastNtpMs > 10000) {
      lastNtpMs = millis();
      mqttStartClock();
    }
    mqttLastError = "waiting for NTP clock (TLS)";
    return;
  }

  if (!mqtt.connected()) {
    if (job.active) return;  // never stall a running dispense
    unsigned long now = millis();
    if (!mqttRetryNow && now - lastTryMs < MQTT_RETRY_MS) return;
    lastTryMs = now;
    mqttRetryNow = false;
    mqttTryConnect();
    if (!mqtt.connected()) return;
    lastPubMs = 0;
  }

  mqtt.loop();

  bool low = readTankLow();
  if (low != lastTank) {
    lastTank = low;
    mqttPublishEvent(String("{\"event\":\"tank\",\"state\":\"") +
                     (low ? "LOW" : "NORMAL") + "\"}");
  }

  String key = String(jobStateName()) + "|" +
               channelName(job.active ? job.channel : CH_NONE) + "|" + job.error;
  unsigned long now = millis();
  bool changed = key != lastJobKey;
  if (changed) {
    lastJobKey = key;
    String ev = "{";
    ev += "\"event\":\"job\",";
    ev += "\"state\":\"" + String(jobStateName()) + "\",";
    ev += "\"channel\":\"" + String(channelName(job.active ? job.channel : CH_NONE)) + "\",";
    ev += "\"job_ml\":" + String(currentJobMl(), 1) + ",";
    ev += "\"target_ml\":" + String(job.targetMl, 1) + ",";
    ev += "\"error\":\"" + jsonEsc(job.error) + "\"";
    ev += "}";
    mqttPublishEvent(ev);
  }
  if (changed || lastPubMs == 0 || now - lastPubMs >= mqttIntervalMs) {
    mqttPublishStatus();
    lastPubMs = now;
  }
}

void mqttLoadPrefs() {
#ifndef ESP8266
  mqttEnabled = prefs.getBool("mq_on", MQTT_DEFAULT_ENABLED);
  mqttHost = prefs.getString("mq_host", MQTT_DEFAULT_HOST);
  mqttPort = prefs.getUShort("mq_port", MQTT_DEFAULT_PORT);
  mqttUser = prefs.getString("mq_user", MQTT_DEFAULT_USER);
  mqttPass = prefs.getString("mq_pass", MQTT_DEFAULT_PASS);
  mqttBase = prefs.getString("mq_base", MQTT_DEFAULT_BASE);
  mqttAllowCmd = prefs.getBool("mq_cmd", MQTT_DEFAULT_ALLOW_CMD);
  mqttTls = prefs.getBool("mq_tls", MQTT_DEFAULT_TLS);
  mqttCa = prefs.getString("mq_ca", "");
  mqttClientCert = prefs.getString("mq_ccert", "");
  mqttClientKey = prefs.getString("mq_ckey", "");
  mqttIntervalMs = prefs.getULong("mq_int", MQTT_DEFAULT_INTERVAL_MS);
#endif
  if (mqttPort < 1) mqttPort = MQTT_DEFAULT_PORT;
  if (mqttIntervalMs < 1000) mqttIntervalMs = MQTT_DEFAULT_INTERVAL_MS;
  mqttApplyTopics();
}

void mqttSavePrefs() {
#ifndef ESP8266
  prefs.putBool("mq_on", mqttEnabled);
  prefs.putString("mq_host", mqttHost);
  prefs.putUShort("mq_port", mqttPort);
  prefs.putString("mq_user", mqttUser);
  prefs.putString("mq_pass", mqttPass);
  prefs.putString("mq_base", mqttBase);
  prefs.putBool("mq_cmd", mqttAllowCmd);
  prefs.putBool("mq_tls", mqttTls);
  if (mqttCa.length()) prefs.putString("mq_ca", mqttCa); else prefs.remove("mq_ca");
  if (mqttClientCert.length()) prefs.putString("mq_ccert", mqttClientCert); else prefs.remove("mq_ccert");
  if (mqttClientKey.length()) prefs.putString("mq_ckey", mqttClientKey); else prefs.remove("mq_ckey");
  prefs.putULong("mq_int", mqttIntervalMs);
#endif
}

void mqttSetup() {
  mqttApplyTopics();
  mqttApplyTransport();
  mqtt.setServer(mqttHost.length() ? mqttHost.c_str() : "invalid.local", mqttPort);
  mqttNet.setTimeout(MQTT_SOCKET_TIMEOUT_S * 1000);  // bounds a dead-host connect
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(mqttOnMessage);
  Serial.printf("[MQTT] %s host='%s:%u' tls=%s base='%s' cmd=%s interval=%lums\n",
                mqttEnabled ? "ENABLED" : "disabled", mqttHost.c_str(), mqttPort,
                mqttTls ? "ON" : "OFF", mqttTopicBase.c_str(),
                mqttAllowCmd ? "ON" : "OFF", mqttIntervalMs);
}

void setupRoutes() {
  static const char* otaHeaderKeys[] = {"X-OTA-Pass"};
  server.collectHeaders(otaHeaderKeys, 1);
#ifdef ESP8266
  server.on("/", [] { server.send(200, "text/html", FPSTR(INDEX_HTML)); });
#else
  server.on("/", [] { server.send_P(200, "text/html", INDEX_HTML); });
#endif
  server.on("/api", HTTP_GET, [] {
    String s = "{\"name\":\"HydroNode\",\"version\":3,\"endpoints\":[";
    s += "\"/api/status\",\"/api/dispense\",\"/api/calrun\",\"/api/pause\",\"/api/resume\",\"/api/stop\",";
    s += "\"/api/wifiscan\",\"/api/wifi\",\"/api/net\",\"/api/cal\",\"/api/config\",\"/api/mqtt\",\"/api/mqtt/ca\",\"/api/mqtt/client\",\"/api/reset-flow\",\"/api/sim\"]}";
    sendApiJson(200, s);
  });
  server.on("/api", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/status", HTTP_GET, [] { sendApiJson(200, statusJson()); });
  server.on("/api/status", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/dispense", HTTP_GET, [] {
    Channel ch = CH_NONE;
    if (server.hasArg("channel")) ch = parseChannel(server.arg("channel"));
    else if (server.hasArg("cold")) ch = server.arg("cold") != "0" ? CH_COLD : CH_NORMAL;
    else if (server.hasArg("r1")) ch = server.arg("r1") != "0" ? CH_COLD : CH_NORMAL;
    else if (server.hasArg("r2")) ch = server.arg("r2") != "0" ? CH_NORMAL : CH_COLD;
    float ml = server.hasArg("ml") ? server.arg("ml").toFloat() : 0;
    if (startDispense(ch, ml)) sendApiText(200, String("Started ") + channelName(ch) + " " + String(ml, 0) + " mL");
    else sendApiText(400, String("Rejected: ") + (job.active ? (job.paused ? "job paused" : "already dispensing") : job.error));
  });
  server.on("/api/dispense", HTTP_OPTIONS, sendApiOptions);

  // Free-run calibration: opens the line with no target; stop it manually,
  // weigh the water, then save PPL = pulses * 1000 / measured_mL.
  server.on("/api/calrun", HTTP_GET, [] {
    Channel ch = server.hasArg("channel") ? parseChannel(server.arg("channel")) : CH_NONE;
    if (startCalibration(ch)) sendApiText(200, String("Calibration run started on ") + channelName(ch) + " — press Stop when done");
    else sendApiText(400, String("Rejected: ") + (job.active ? "device busy" : job.error));
  });
  server.on("/api/calrun", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/pause", HTTP_GET, [] {
    if (pauseDispense()) sendApiText(200, "Paused");
    else sendApiText(400, String("Rejected: ") + (!job.active ? "no active job" :
                     (job.finishing ? "target reached - valve closing" : "already paused")));
  });
  server.on("/api/pause", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/resume", HTTP_GET, [] {
    if (resumeDispense()) sendApiText(200, "Resumed");
    else sendApiText(400, String("Rejected: ") + (!job.active ? "no active job" : (job.error.length() ? job.error : "not paused")));
  });
  server.on("/api/resume", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/stop", HTTP_GET, [] {
    if (job.active) { stopDispense("stopped by user"); sendApiText(200, "Stopped"); }
    else { relaysOff(); sendApiText(200, "Already idle"); }
  });
  server.on("/api/stop", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/wifi", HTTP_GET, [] {
    // ?clear=1 wipes the saved STA credentials and reboots into AP-only.
    if (server.hasArg("clear") && server.arg("clear") != "0") {
      staSsid = "";
      staPass = "";
#ifdef ESP8266
      eeSave();
#else
      prefs.remove("sta_ssid");
      prefs.remove("sta_pass");
      resetStaticIp();
#endif
      Serial.println("[WiFi] credentials cleared via API — rebooting AP-only");
      sendApiText(200, "WiFi cleared, rebooting into AP-only");
      delay(400);
      ESP.restart();
      return;
    }
    String newSsid = server.hasArg("ssid") ? server.arg("ssid") : "";
    newSsid.trim();
    bool ssidChanged = newSsid != staSsid;
    staSsid = newSsid;
    staPass = server.hasArg("pass") ? server.arg("pass") : "";
    if (!staSsid.length()) {
      sendApiText(400, "Missing ssid");
      return;
    }
    // A static address from the old network would make the new link unusable;
    // fall back to DHCP so the router hands out a reachable IP.
    if (ssidChanged && netStatic) {
      resetStaticIp();
      Serial.println("[NET] SSID changed — static IP reset to DHCP");
    }
    Serial.printf("[WiFi] new STA '%s' (pass len %d), saving + reboot\n",
                  staSsid.c_str(), staPass.length());
#ifdef ESP8266
    eeSave();
#else
    prefs.putString("sta_ssid", staSsid);
    prefs.putString("sta_pass", staPass);
#endif
    sendApiText(200, "Saved, rebooting");
    delay(400);
    ESP.restart();
  });
  server.on("/api/wifi", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/wifiscan", HTTP_GET, [] {
    // A saved-but-unreachable SSID makes loop() call WiFi.begin() every 15 s.
    // Each attempt aborts a scan, which is why the list often comes back empty.
    // Pause the retry and detach the STA (credentials stay in NVS) while we scan.
    wifiScanning = true;
    WiFi.setAutoReconnect(false);
    bool wasConnected = WiFi.status() == WL_CONNECTED;
    if (!wasConnected) {
      WiFi.disconnect(false);
      delay(150);
    }
    int n = WiFi.scanNetworks(false, true);  // sync, include hidden
    if (n < 0) {                             // one retry on a failed scan
      WiFi.disconnect(false);
      delay(250);
      n = WiFi.scanNetworks(false, true);
      Serial.printf("[WiFi] scan retry -> %d network(s)\n", n);
    }
    Serial.printf("[WiFi] scan -> %d network(s)\n", n);
    String s = "[";
    bool first = true;
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue; // skip hidden
      ssid.replace("\\", "\\\\");
      ssid.replace("\"", "\\\"");
      if (!first) s += ",";
      first = false;
      s += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
           ",\"ch\":" + String(WiFi.channel(i)) + "}";
    }
    s += "]";
    WiFi.scanDelete();
    // Restore normal STA behaviour and reconnect right away if a saved network
    // exists; waiting for loop()'s 15 s retry leaves the UI without a STA IP.
    WiFi.setAutoReconnect(true);
    wifiScanning = false;
    if (staSsid.length() && WiFi.status() != WL_CONNECTED)
      WiFi.begin(staSsid.c_str(), staPass.c_str());
    sendApiJson(200, s);
  });
  server.on("/api/wifiscan", HTTP_OPTIONS, sendApiOptions);

  // Network mode. GET /api/net reports the config. Add mode=static with
  // ip/gw/mask/dns to save a fixed IPv4, or mode=dhcp to clear it. The new
  // value is applied on boot; add &reboot=1 to restart immediately.
  server.on("/api/net", HTTP_GET, [] {
    if (server.hasArg("mode")) {
#ifndef ESP8266
      String mode = server.arg("mode");
      mode.toLowerCase();
      if (mode != "dhcp" && mode != "static") {
        sendApiJson(400, "{\"ok\":false,\"error\":\"mode must be dhcp or static\"}");
        return;
      }
      if (mode == "static") {
        String ip = server.arg("ip"); ip.trim();
        String gw = server.arg("gw"); gw.trim();
        String mask = server.hasArg("mask") ? server.arg("mask") : "255.255.255.0";
        mask.trim();
        String dns = server.hasArg("dns") ? server.arg("dns") : "";
        dns.trim();
        if (!isUsableStaticIp(ip, gw, mask)) {
          sendApiJson(400, "{\"ok\":false,\"error\":\"Invalid static IP: check IP, gateway and subnet (not 192.168.4.x)\"}");
          return;
        }
        IPAddress d;
        if (dns.length() && !parseIPv4(dns, d)) {
          sendApiJson(400, "{\"ok\":false,\"error\":\"Invalid DNS address\"}");
          return;
        }
        netStatic = true;
        netIp = ip; netGw = gw; netMask = mask; netDns = dns;
        prefs.putBool("net_static", true);
        prefs.putString("net_ip", netIp);
        prefs.putString("net_gw", netGw);
        prefs.putString("net_mask", netMask);
        prefs.putString("net_dns", netDns);
        Serial.printf("[NET] saved static ip=%s gw=%s mask=%s dns=%s\n",
                      netIp.c_str(), netGw.c_str(), netMask.c_str(),
                      netDns.length() ? netDns.c_str() : netGw.c_str());
      } else {
        resetStaticIp();
        Serial.println("[NET] saved DHCP mode");
      }
      bool reboot = server.hasArg("reboot") && server.arg("reboot") != "0";
      sendApiJson(200, netJson());
      if (reboot) {
        delay(400);
        ESP.restart();
      }
      return;
#else
      sendApiJson(400, "{\"ok\":false,\"error\":\"Static IP is only supported on the ESP32 build\"}");
      return;
#endif
    }
    sendApiJson(200, netJson());
  });
  server.on("/api/net", HTTP_OPTIONS, sendApiOptions);

  // MQTT bridge config. No args = read. Any argument is validated, saved to
  // NVS and applied immediately (the client reconnects, deferred if a job is
  // running). The stored password is never sent back — only has_pass.
  server.on("/api/mqtt", HTTP_GET, [] {
    bool changed = false;
    if (server.hasArg("enabled")) {
      mqttEnabled = server.arg("enabled") != "0";
      changed = true;
    }
    if (server.hasArg("allow_cmd")) {
      mqttAllowCmd = server.arg("allow_cmd") != "0";
      changed = true;
    }
    if (server.hasArg("host")) {
      String h = server.arg("host");
      uint16_t p = mqttPort;
      if (h.length() && !mqttParseHost(h, p)) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"Invalid broker host (host or host:port, no spaces)\"}");
        return;
      }
      mqttHost = h;
      mqttPort = p;
      changed = true;
    }
    if (server.hasArg("port")) {
      long p = server.arg("port").toInt();
      if (p < 1 || p > 65535) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"Port must be 1..65535\"}");
        return;
      }
      mqttPort = (uint16_t)p;
      changed = true;
    }
    if (server.hasArg("user")) {
      mqttUser = server.arg("user");
      mqttUser.trim();
      changed = true;
    }
    if (server.hasArg("pass")) {
      mqttPass = server.arg("pass");
      changed = true;
    }
    if (server.hasArg("clearpass") && server.arg("clearpass") != "0") {
      mqttPass = "";
      changed = true;
    }
    if (server.hasArg("base")) {
      String b = server.arg("base");
      b.trim();
      while (b.startsWith("/")) b.remove(0, 1);
      while (b.endsWith("/")) b.remove(b.length() - 1);
      bool bad = b.length() > 64;
      for (size_t i = 0; i < b.length() && !bad; i++)
        if (b[i] == ' ' || b[i] == '#' || b[i] == '+' || b[i] == '"' || b[i] == '\\') bad = true;
      if (bad) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"Invalid topic base (no spaces, #, +, quotes; max 64 chars)\"}");
        return;
      }
      mqttBase = b.length() ? b : MQTT_DEFAULT_BASE;
      changed = true;
    }
    if (server.hasArg("interval")) {
      long v = server.arg("interval").toInt();
      if (v < 1000 || v > 600000) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"Interval must be 1000..600000 ms\"}");
        return;
      }
      mqttIntervalMs = (unsigned long)v;
      changed = true;
    }
    if (server.hasArg("tls")) {
#ifdef ESP8266
      sendApiJson(400, "{\"ok\":false,\"error\":\"TLS is ESP32-only\"}");
      return;
#else
      bool on = server.arg("tls") != "0";
      if (on != mqttTls) {
        mqttTls = on;
        // Follow the usual port when it was still the other default.
        if (on && mqttPort == MQTT_DEFAULT_PORT) mqttPort = MQTT_DEFAULT_TLS_PORT;
        else if (!on && mqttPort == MQTT_DEFAULT_TLS_PORT) mqttPort = MQTT_DEFAULT_PORT;
      }
      changed = true;
#endif
    }
    if (changed) {
      mqttApplyTopics();
      mqttSavePrefs();
      if (mqtt.connected()) {
        mqtt.publish(mqttTopic("availability").c_str(), "offline", true);
        mqtt.disconnect();
      }
      mqttApplyTransport();
      mqtt.setServer(mqttHost.length() ? mqttHost.c_str() : "invalid.local", mqttPort);
      if (mqttTls) mqttStartClock();
      mqttRetryNow = true;
      if (mqttEnabled && !job.active) {
        mqttRetryNow = false;
        mqttTryConnect();
      }
      Serial.printf("[MQTT] cfg saved enabled=%s host='%s:%u' tls=%s base='%s' cmd=%s interval=%lums\n",
                    mqttEnabled ? "ON" : "OFF", mqttHost.c_str(), mqttPort,
                    mqttTls ? "ON" : "OFF", mqttTopicBase.c_str(),
                    mqttAllowCmd ? "ON" : "OFF", mqttIntervalMs);
    }
    sendApiJson(200, mqttJson());
  });
  server.on("/api/mqtt", HTTP_OPTIONS, sendApiOptions);

  // Custom root CA (private broker). POST the PEM as the request body, or
  // ?clear=1 to go back to the built-in root-CA bundle. Only over the LAN.
  server.on("/api/mqtt/ca", HTTP_POST, [] {
#ifndef ESP8266
    if (server.hasArg("clear") && server.arg("clear") != "0") {
      mqttCa = "";
      mqttSavePrefs();
      mqttApplyTransport();
      mqtt.disconnect();
      mqttRetryNow = true;
      sendApiJson(200, "{\"ok\":true,\"has_ca\":false}");
      return;
    }
    String pem = server.arg("plain");
    if (!pem.length() && server.hasArg("ca")) pem = server.arg("ca");
    pem.replace("\\n", "\n");
    pem.trim();
    if (pem.indexOf("BEGIN CERTIFICATE") < 0) {
      sendApiJson(400, "{\"ok\":false,\"error\":\"Body must be a PEM certificate\"}");
      return;
    }
    if (pem.length() > MQTT_MAX_PEM_LEN) {
      sendApiJson(400, "{\"ok\":false,\"error\":\"CA PEM too large\"}");
      return;
    }
    mqttCa = pem;
    mqttSavePrefs();
    mqttApplyTransport();
    mqtt.disconnect();
    mqttRetryNow = true;
    Serial.printf("[MQTT] custom CA saved (%u bytes)\n", (unsigned)mqttCa.length());
    sendApiJson(200, "{\"ok\":true,\"has_ca\":true}");
#else
    sendApiJson(400, "{\"ok\":false,\"error\":\"TLS is ESP32-only\"}");
#endif
  });
  server.on("/api/mqtt/ca", HTTP_OPTIONS, sendApiOptions);

  // Mutual-TLS client certificate + key (AWS IoT, Azure IoT, private brokers).
  // POST form fields cert=<PEM>&key=<PEM>, or ?clear=1 to remove them.
  server.on("/api/mqtt/client", HTTP_POST, [] {
#ifndef ESP8266
    bool clear = server.hasArg("clear") && server.arg("clear") != "0";
    String cert = clear ? "" : server.arg("cert");
    String key = clear ? "" : server.arg("key");
    cert.replace("\\n", "\n");
    key.replace("\\n", "\n");
    cert.trim();
    key.trim();
    if (!clear) {
      if (cert.indexOf("BEGIN CERTIFICATE") < 0 || key.indexOf("PRIVATE KEY") < 0) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"Need cert and key PEM fields\"}");
        return;
      }
      if (cert.length() > MQTT_MAX_PEM_LEN || key.length() > MQTT_MAX_PEM_LEN) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"Certificate or key too large\"}");
        return;
      }
    }
    mqttClientCert = cert;
    mqttClientKey = key;
    mqttSavePrefs();
    mqttApplyTransport();
    mqtt.disconnect();
    mqttRetryNow = true;
    Serial.printf("[MQTT] client certificate %s\n", clear ? "cleared" : "saved");
    sendApiJson(200, String("{\"ok\":true,\"has_client_cert\":") +
                     (mqttClientCert.length() ? "true" : "false") + "}");
#else
    sendApiJson(400, "{\"ok\":false,\"error\":\"TLS is ESP32-only\"}");
#endif
  });
  server.on("/api/mqtt/client", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/cal", HTTP_GET, [] {
    if (server.hasArg("ppl")) {
      float v = server.arg("ppl").toFloat();
      if (v > PPL_MIN && v < PPL_MAX) {
        // ?channel=cold|normal saves that line; no channel keeps the legacy
        // behaviour of writing both lines.
        Channel ch = server.hasArg("channel") ? parseChannel(server.arg("channel")) : CH_NONE;
        if (ch == CH_COLD) pplCold = v;
        else if (ch == CH_NORMAL) pplNormal = v;
        else { pplCold = v; pplNormal = v; }
#ifdef ESP8266
        eeSave();
#else
        if (ch == CH_COLD) prefs.putFloat("ppl_cold", v);
        else if (ch == CH_NORMAL) prefs.putFloat("ppl_norm", v);
        else { prefs.putFloat("ppl_cold", v); prefs.putFloat("ppl_norm", v); }
        prefs.putFloat("ppl", v);  // legacy key, kept for older builds/readers
        // The learned tail is expressed in mL derived from the old PPL, so it
        // must be re-learned after a calibration change.
        tailCompCold = tailCompNormal = VALVE_TAIL_COMP_ML;
        tailCompColdLearned = tailCompNormalLearned = false;
        prefs.remove("tail_cold");
        prefs.remove("tail_norm");
        prefs.remove("tail_ml");
#endif
        sendApiText(200, String("Saved ppl=") + String(v, 1) + " for " +
                         (ch == CH_NONE ? String("both lines") : String(channelName(ch))));
        return;
      }
    }
    sendApiText(400, "Bad ppl (100..50000, ZJ-S201 default 450)");
  });
  server.on("/api/cal", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/config", HTTP_GET, [] {
    bool changed = false;
    if (server.hasArg("flow_timeout")) {
      cfgFlowTimeout = server.arg("flow_timeout") != "0";
#ifndef ESP8266
      prefs.putBool("flow_tmout", cfgFlowTimeout);
#endif
      changed = true;
    }
    if (server.hasArg("tank_sensor")) {
      cfgTankSensor = server.arg("tank_sensor") != "0";
      applyTankConfig();
#ifndef ESP8266
      prefs.putBool("tank_sens2", cfgTankSensor);
#endif
      changed = true;
    }
    if (server.hasArg("tank_pin")) {
      int m = server.arg("tank_pin").toInt();
      if (m < 0 || m > 2) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"tank_pin must be 0 (pulldown), 1 (floating) or 2 (pullup)\"}");
        return;
      }
      cfgTankPinMode = m;
      applyTankConfig();
#ifndef ESP8266
      prefs.putInt("tank_pin", cfgTankPinMode);
#endif
      changed = true;
    }
    if (server.hasArg("tank_debounce")) {
      long ms = server.arg("tank_debounce").toInt();
      if (ms < 200 || ms > 10000) {
        sendApiJson(400, "{\"ok\":false,\"error\":\"tank_debounce must be 200..10000 ms\"}");
        return;
      }
      cfgTankDebounceMs = (unsigned long)ms;
#ifndef ESP8266
      prefs.putULong("tank_deb", cfgTankDebounceMs);
#endif
      changed = true;
    }
    if (server.hasArg("tail_ml")) {
      float v = server.arg("tail_ml").toFloat();
      if (v >= 0 && v <= 150) {
        tailCompCold = tailCompNormal = v;
        tailCompColdLearned = tailCompNormalLearned = v > 0;
#ifndef ESP8266
        prefs.putFloat("tail_cold", v);
        prefs.putFloat("tail_norm", v);
#endif
        changed = true;
      }
    }
    String s = "{";
    s += "\"flow_timeout\":" + String(cfgFlowTimeout ? "true" : "false") + ",";
    s += "\"tank_sensor\":"  + String(cfgTankSensor  ? "true" : "false") + ",";
    s += "\"tank_pin_mode\":" + String(cfgTankPinMode) + ",";
    s += "\"tank_debounce_ms\":" + String(cfgTankDebounceMs) + ",";
    s += "\"tank_raw\":" + String(cfgTankSensor ? digitalRead(PIN_TANK_LOW) : -1) + ",";
    s += "\"flow_timeout_ms\":" + String(FLOW_TIMEOUT_MS) + ",";
    s += "\"dispense_timeout_ms\":" + String(DISPENSE_TIMEOUT_MS) + ",";
    s += "\"valve_tail_ms\":" + String(VALVE_TAIL_MS) + ",";
    s += "\"tail_comp_ml\":" + String(tailCompCold, 1) + ",";
    s += "\"tail_comp_cold\":" + String(tailCompCold, 1) + ",";
    s += "\"tail_comp_normal\":" + String(tailCompNormal, 1) + ",";
    s += "\"tail_comp_learned\":" + String((tailCompColdLearned && tailCompNormalLearned) ? "true" : "false");
    s += "}";
    if (changed) Serial.printf("[CFG] config updated: flow_timeout=%s tank_sensor=%s tank_pin=%d\n",
                               cfgFlowTimeout ? "ON" : "OFF", cfgTankSensor ? "ON" : "OFF",
                               cfgTankPinMode);
    sendApiJson(200, s);
  });
  server.on("/api/config", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/reset-flow", HTTP_GET, [] {
#ifdef ESP8266
    noInterrupts();
    flowPulseCountCold = 0;
    flowPulseCountNormal = 0;
    flowRejectedCold = 0;
    flowRejectedNormal = 0;
    interrupts();
#else
    portENTER_CRITICAL(&flowMux);
    flowPulseCountCold = 0;
    flowPulseCountNormal = 0;
    flowRejectedCold = 0;
    flowRejectedNormal = 0;
    portEXIT_CRITICAL(&flowMux);
#endif
    flowLastSeenCount = 0;
    simPulses = 0; simPulseFrac = 0;
    sendApiText(200, "Flow counter reset");
  });
  server.on("/api/reset-flow", HTTP_OPTIONS, sendApiOptions);

  server.on("/api/sim", HTTP_GET, [] {
    // LAN dummy sensor input. Examples:
    // /api/sim?enable=1&hz=15&tank=ok    (2 L/min flow, tank normal)
    // /api/sim?tank=low                  (abort test, keep-alive)
    // /api/sim?addpulses=225             (exact 500 mL burst)
    // /api/sim?enable=0                  (back to real sensors)
    if (server.hasArg("enable")) {
      simEnable = server.arg("enable") != "0";
      Serial.printf("[SIM] %s\n", simEnable ? "ENABLED (LAN)" : "disabled, real sensors");
    }
    if (server.hasArg("hz")) {
      simHz = constrain(server.arg("hz").toFloat(), 0, 1000);
    }
    if (server.hasArg("tank")) {
      String t = server.arg("tank"); t.toLowerCase();
      simTankLowRx = (t == "low" || t == "1" || t == "alarm");
      Serial.printf("[SIM] tank -> %s\n", simTankLowRx ? "LOW" : "ok");
    }
    if (server.hasArg("addpulses")) {
      long n = constrain(server.arg("addpulses").toInt(), 0, 60000);
      simPulses += (unsigned long)n;
      Serial.printf("[SIM] +%ld pulses (total sim %lu)\n", n, simPulses);
    }
    if (server.hasArg("reset")) { simPulses = 0; simPulseFrac = 0; }
    simLastRxMs = millis();
    if (simLastAccumMs == 0) simLastAccumMs = millis();
    String r = String("{\"sim\":") + (simEnable ? "true" : "false") +
               ",\"hz\":" + String(simHz, 1) +
               ",\"tank_low\":" + (simTankLowRx ? "true" : "false") +
               ",\"sim_pulses\":" + String(simPulses) + "}";
    sendApiJson(200, r);
  });
  server.on("/api/sim", HTTP_OPTIONS, sendApiOptions);

  server.on("/update", HTTP_POST, [] {
    if (!otaAuthorized()) {
      server.sendHeader("Connection", "close");
      server.send(401, "text/plain", "Unauthorized");
      return;
    }
    bool ok = otaOk && !Update.hasError();
    String msg = ok ? "OK" : "FAIL";
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", msg);
    if (ok) {
      delay(500);
      ESP.restart();
    }
  }, handleOtaUpload);
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);
#ifdef ESP8266
  Serial.println("\n=== Dispenser (D1 mini test) Cold/Normal + ZJ-S201 + XKC-Y25 ===");
#else
  Serial.println("\n=== ESP32 Dispenser: Cold/Normal + ZJ-S201 + XKC-Y25 ===");
#endif

  pinMode(PIN_RELAY_COLD, OUTPUT);
  pinMode(PIN_RELAY_NORMAL, OUTPUT);
#if !defined(ESP8266)
  // Pump outputs use LEDC PWM (0..255). Pre-load the inactive level and the
  // OFF duty before attaching, so GPIO25/26 cannot twitch the MOSFETs during
  // boot or while flashing.
  digitalWrite(PIN_PUMP_COLD, PUMP_ACTIVE_LOW ? HIGH : LOW);
  digitalWrite(PIN_PUMP_NORMAL, PUMP_ACTIVE_LOW ? HIGH : LOW);
  pinMode(PIN_PUMP_COLD, OUTPUT);
  pinMode(PIN_PUMP_NORMAL, OUTPUT);
  ledcSetup(PUMP_LEDC_CH_COLD, PUMP_PWM_FREQ_HZ, PUMP_PWM_RES_BITS);
  ledcSetup(PUMP_LEDC_CH_NORMAL, PUMP_PWM_FREQ_HZ, PUMP_PWM_RES_BITS);
  ledcWrite(PUMP_LEDC_CH_COLD, pumpDuty(false));
  ledcWrite(PUMP_LEDC_CH_NORMAL, pumpDuty(false));
  ledcAttachPin(PIN_PUMP_COLD, PUMP_LEDC_CH_COLD);
  ledcAttachPin(PIN_PUMP_NORMAL, PUMP_LEDC_CH_NORMAL);
#endif
  relaysOff();

  pinMode(PIN_LED_BOARD, OUTPUT);
  // Boot self-test: plain blink first (harmless on WS2812), then RGB sweep for WS2812.
  for (int i = 0; i < 2; i++) {
    ledWrite(PIN_LED_BOARD, LED_BOARD_ACTIVE_LOW, true);
    delay(150);
    ledWrite(PIN_LED_BOARD, LED_BOARD_ACTIVE_LOW, false);
    delay(150);
  }
#if !defined(ESP8266) && LED_BOARD_IS_WS2812
  neopixelWrite(PIN_LED_BOARD, 64, 0, 0); delay(250);
  neopixelWrite(PIN_LED_BOARD, 0, 64, 0); delay(250);
  neopixelWrite(PIN_LED_BOARD, 0, 0, 64); delay(250);
  neopixelWrite(PIN_LED_BOARD, 0, 0, 0);
  Serial.printf("[LED] WS2812 RGB on GPIO%d\n", PIN_LED_BOARD);
#else
  Serial.printf("[LED] plain GPIO%d active-%s\n", PIN_LED_BOARD, LED_BOARD_ACTIVE_LOW ? "LOW" : "HIGH");
#endif

  pinMode(PIN_FLOW_SENSOR_COLD, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW_SENSOR_COLD), onFlowPulseCold, FALLING);
#if FLOW_SENSOR_COUNT > 1
  pinMode(PIN_FLOW_SENSOR_NORMAL, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW_SENSOR_NORMAL), onFlowPulseNormal, FALLING);
#endif

#ifndef ESP8266
  prefs.begin("dispenser", false);
  // Per-line PPL. Older firmware stored one shared "ppl"; that value is used as
  // the starting point for both lines until each one is calibrated.
  float legacyPpl = prefs.getFloat("ppl", FLOW_PULSES_PER_L);
  if (!(legacyPpl > PPL_MIN && legacyPpl < PPL_MAX)) legacyPpl = FLOW_PULSES_PER_L;
  // A shared value far from the ZJ-S201 nominal (e.g. 10,000 saved from a noisy
  // test) is not a useful seed and makes every later test target absurd, so
  // fall back to nominal and let the user calibrate per line.
  if (legacyPpl > FLOW_PULSES_PER_L * 3.0f || legacyPpl < FLOW_PULSES_PER_L / 3.0f)
    legacyPpl = FLOW_PULSES_PER_L;
  pplCold   = prefs.getFloat("ppl_cold", legacyPpl);
  pplNormal = prefs.getFloat("ppl_norm", legacyPpl);
  if (!(pplCold   > PPL_MIN && pplCold   < PPL_MAX)) pplCold   = FLOW_PULSES_PER_L;
  if (!(pplNormal > PPL_MIN && pplNormal < PPL_MAX)) pplNormal = FLOW_PULSES_PER_L;
  // Learned pump-off → valve-close drain volume (mL), per line. The old shared
  // "tail_ml" key seeds both until each line has completed a dispense.
  float legacyTail = prefs.getFloat("tail_ml", VALVE_TAIL_COMP_ML);
  if (!(legacyTail >= 0 && legacyTail <= 150)) legacyTail = VALVE_TAIL_COMP_ML;
  tailCompColdLearned   = prefs.isKey("tail_cold");
  tailCompNormalLearned = prefs.isKey("tail_norm");
  tailCompCold   = prefs.getFloat("tail_cold", legacyTail);
  tailCompNormal = prefs.getFloat("tail_norm", legacyTail);
  if (!(tailCompCold >= 0 && tailCompCold <= 150)) {
    tailCompCold = VALVE_TAIL_COMP_ML; tailCompColdLearned = false;
  }
  if (!(tailCompNormal >= 0 && tailCompNormal <= 150)) {
    tailCompNormal = VALVE_TAIL_COMP_ML; tailCompNormalLearned = false;
  }
  // One-shot credential wipe (USB recovery). The NVS marker makes it happen
  // only once per WIFI_CLEAR_TOKEN value, so a later re-provision survives.
  if (strlen(WIFI_CLEAR_TOKEN) &&
      prefs.getString("wifi_cleared", "") != WIFI_CLEAR_TOKEN) {
    prefs.remove("sta_ssid");
    prefs.remove("sta_pass");
    resetStaticIp();  // the old LAN address is not valid for the new network
    prefs.putString("wifi_cleared", WIFI_CLEAR_TOKEN);
    Serial.printf("[WiFi] saved STA credentials wiped (token '%s') — AP-only\n",
                  WIFI_CLEAR_TOKEN);
  }
  staSsid = prefs.getString("sta_ssid", STA_SSID_DEFAULT);
  staPass = prefs.getString("sta_pass", STA_PASS_DEFAULT);
  cfgFlowTimeout = prefs.getBool("flow_tmout", FLOW_TIMEOUT_ENABLED);
  // "tank_sens2": older builds stored "tank_sens"=false while the sensor was
  // disabled, which would otherwise keep the now-wired sensor off forever.
  cfgTankSensor  = prefs.getBool("tank_sens2", TANK_SENSOR_ENABLED);
  cfgTankPinMode = prefs.getInt("tank_pin", TANK_PIN_MODE_DEFAULT);
  if (cfgTankPinMode < 0 || cfgTankPinMode > 2) cfgTankPinMode = TANK_PIN_MODE_DEFAULT;
  cfgTankDebounceMs = prefs.getULong("tank_deb", TANK_DEBOUNCE_DEFAULT_MS);
  if (cfgTankDebounceMs < 200 || cfgTankDebounceMs > 10000)
    cfgTankDebounceMs = TANK_DEBOUNCE_DEFAULT_MS;
  // Optional static IPv4 (validated again in applyStaticIp()).
  netStatic = prefs.getBool("net_static", false);
  netIp     = prefs.getString("net_ip", "");
  netGw     = prefs.getString("net_gw", "");
  netMask   = prefs.getString("net_mask", "255.255.255.0");
  netDns    = prefs.getString("net_dns", "");
#else
  eeLoad();
  Serial.printf("[CFG] loaded ppl=%.0f ssid='%s'\n", pplCold, staSsid.c_str());
#endif

  // MQTT bridge settings (NVS on ESP32; defaults per boot on the D1 mini).
  mqttLoadPrefs();

  // Tank pin setup must come after the saved setting is loaded, otherwise a
  // stored "tank_sens2=1" would be ignored and GPIO13 would stay unconfigured.
  applyTankConfig();
  if (cfgTankSensor) {
    Serial.printf("[TANK] sensor ON GPIO%d (input mode %d), water present=%s, currently %s\n",
                  PIN_TANK_LOW, cfgTankPinMode, TANK_WATER_PRESENT_HIGH ? "HIGH" : "LOW",
                  readTankLow() ? "LOW" : "NORMAL");
  }

  uint32_t chip = 0;
#ifdef ESP8266
  chip = ESP.getChipId();
#else
  chip = (uint16_t)(ESP.getEfuseMac() & 0xFFFF);
#endif
  snprintf(apSsid, sizeof(apSsid), "%s%04X", AP_SSID_PREFIX, chip & 0xFFFF);

  staSsid.trim();
  // staPass intentionally NOT trimmed (spaces can be valid)

  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
#ifdef ESP8266
  WiFi.hostname(HOSTNAME);
#else
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
#endif

  // Static IPv4 must be configured before WiFi.begin(). The AP always stays on
  // 192.168.4.1, so a wrong static address can still be fixed over the AP.
  if (netStatic) {
    if (applyStaticIp()) {
      Serial.printf("[NET] static ip=%s gw=%s mask=%s dns=%s\n",
                    netIp.c_str(), netGw.c_str(), netMask.c_str(),
                    netDns.length() ? netDns.c_str() : netGw.c_str());
    } else {
      Serial.println("[NET] saved static IP unusable — using DHCP");
      netStatic = false;
      netIp = netGw = netDns = "";
      netMask = "255.255.255.0";
    }
  } else {
    Serial.println("[NET] DHCP (router-assigned)");
  }

  // ESP32 AP+STA must share one channel. Scan for the router first
  // and start our AP on the same channel, else STA connect fails
  // when router is not on channel 1.
  int apChannel = 1;
  if (staSsid.length()) {
    Serial.printf("[WiFi] scanning for STA '%s'...\n", staSsid.c_str());
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == staSsid) {
        apChannel = WiFi.channel(i);
        Serial.printf("[WiFi] found '%s' ch=%d rssi=%d enc=%d\n",
                      staSsid.c_str(), apChannel, WiFi.RSSI(i), WiFi.encryptionType(i));
        break;
      }
    }
    WiFi.scanDelete();
    if (apChannel < 1 || apChannel > 13) apChannel = 1;
    if (n == 0) Serial.println("[WiFi] scan found nothing — router may be 5GHz-only / off / too far");
  }
  WiFi.softAP(apSsid, AP_PASSWORD, apChannel);
  Serial.printf("[WiFi] AP %s ch=%d ip=%s\n", apSsid, apChannel, WiFi.softAPIP().toString().c_str());

  if (staSsid.length()) {
    Serial.printf("[WiFi] STA '%s' connecting (pass len %d)...\n", staSsid.c_str(), staPass.length());
    WiFi.begin(staSsid.c_str(), staPass.c_str());
    unsigned long t0 = millis();
    wl_status_t last = WL_IDLE_STATUS;
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
      wl_status_t st = WiFi.status();
      if (st != last) {
        last = st;
        Serial.printf("[WiFi] status: %d (%s)\n", (int)st, wifiStatusName(st));
      }
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] STA CONNECTED ip=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
      wl_status_t st = WiFi.status();
      Serial.printf("[WiFi] STA FAILED: %d (%s). Check: 2.4GHz SSID? correct password? "
                    "router DHCP free? Will retry in loop.\n", (int)st, wifiStatusName(st));
    }
  } else {
    Serial.println("[WiFi] no STA saved — AP-only. Set via /api/wifi?ssid=&pass=");
  }
  Serial.printf("[WiFi] AP=%s (%s) | STA=%s %s\n", apSsid, WiFi.softAPIP().toString().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.isConnected() ? "CONNECTED" : "not-connected");

  setupRoutes();
  server.begin();
  setupOTA();
  setupMDNS();
  mqttSetup();
  Serial.println("[HTTP] open http://192.168.4.1/");
#if FLOW_SENSOR_COUNT > 1
  Serial.printf("[CFG] flow cold=GPIO%d ppl=%.1f | normal=GPIO%d ppl=%.1f\n",
                PIN_FLOW_SENSOR_COLD, pplCold, PIN_FLOW_SENSOR_NORMAL, pplNormal);
#else
  Serial.printf("[CFG] flow=GPIO%d | ppl=%.1f\n", PIN_FLOW_SENSOR_COLD, pplFor(CH_COLD));
#endif
#if TANK_SENSOR_ENABLED
  Serial.printf("[CFG] tank pin=GPIO%d compile-default=ON\n", PIN_TANK_LOW);
#else
  Serial.printf("[CFG] tank pin=GPIO%d compile-default=OFF\n", PIN_TANK_LOW);
#endif
#if !defined(ESP8266)
  Serial.printf("[CFG] pump cold=GPIO%d normal=GPIO%d | PWM %d Hz, %d-bit, ON=%u/255 active-%s\n",
                PIN_PUMP_COLD, PIN_PUMP_NORMAL, PUMP_PWM_FREQ_HZ, PUMP_PWM_RES_BITS,
                (unsigned)PUMP_PWM_DUTY_ON, PUMP_ACTIVE_LOW ? "LOW" : "HIGH");
#endif
  Serial.printf("[CFG] tank_sensor=%s flow_timeout=%s (runtime)\n",
                cfgTankSensor ? "ON" : "OFF", cfgFlowTimeout ? "ON" : "OFF");
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  simAccumulate();  // LAN dummy pulses (no-op unless /api/sim enabled)

  // STA auto-retry: original code tried once at boot then gave up forever.
  // Retry every 15 s when saved SSID exists and we're not connected.
  static unsigned long lastWifiTry = 0;
  if (!wifiScanning && staSsid.length() && WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiTry > 15000) {
    lastWifiTry = millis();
    Serial.printf("[WiFi] retry '%s'... (last status %d %s)\n",
                  staSsid.c_str(), (int)WiFi.status(), wifiStatusName(WiFi.status()));
    WiFi.disconnect(false);
    delay(100);
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }

  static unsigned long lastRateCalc = 0;
  static unsigned long lastRateCount = 0;
  unsigned long nowMs = millis();
  if (nowMs - lastRateCalc >= 1000) {
    unsigned long c = flowPulses();
    unsigned long dp = (c >= lastRateCount) ? (c - lastRateCount) : 0;
    float freqHz = dp / ((nowMs - lastRateCalc) / 1000.0f);
    liveLpm = freqHz * 60.0f / pplFor(job.active ? job.channel : CH_COLD);
    lastRateCount = c;
    lastRateCalc = nowMs;
  }

  bool tankLow = readTankLow();

  if (job.active) {
    float ml = currentJobMl();
    unsigned long now = flowPulsesForChannel(job.channel);
    if (!job.paused && now != flowLastSeenCount) {
      flowLastSeenCount = now;
      job.lastPulseMs = millis();
    }

    if (job.calibrate) {
      // Free run: no target, the user stops it when enough water was collected.
      if (tankLow) stopDispense("tank LOW - stopped, refill");
      else if (!job.paused && millis() - job.startMs > CAL_RUN_MAX_MS)
        stopDispense("calibration time limit - press STOP sooner");
    }
    else if (job.finishing) {
      // Pump is already OFF; keep the solenoid open until the tail expires.
      if (tankLow) stopDispense("tank LOW - stopped, refill");
      else if (millis() - job.finishStartMs >= VALVE_TAIL_MS) stopDispense();
    }
    else if (tankLow) stopDispense("tank LOW - stopped, refill");
    else if (ml >= job.targetMl - tailCompClamped(job.channel, job.targetMl)) beginValveTail();
    else if (!job.paused && millis() - job.startMs > DISPENSE_TIMEOUT_MS)
      stopDispense("timeout — stopped");
    else if (!job.paused && cfgFlowTimeout && millis() - job.lastPulseMs > FLOW_TIMEOUT_MS && ml < job.targetMl - 5)
      stopDispense("no flow — check valve/pump");
  }
  updateLeds();
  mqttTick();  // telemetry publish / broker reconnect (no-op when disabled)
#ifdef ESP8266
  delay(5);
  yield();
#else
  delay(5);
#endif
}
