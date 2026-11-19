#pragma once
#include <Arduino.h>

// ---------- Pin map: user spec (ESP32 final) ----------
#ifdef ESP8266
  // Wemos D1 mini TEMP-TEST mapping (safe boot pins only):
  // D1=GPIO5 cold, D2=GPIO4 normal, D5=GPIO14 flow, D6=GPIO12 tank
  #define PIN_RELAY_COLD       5    // D1 - ColdWater
  #define PIN_RELAY_NORMAL     4    // D2 - NormalWater
  #define PIN_FLOW_SENSOR_COLD 14   // D5 - ZJ-S201 pulse (single-sensor fallback)
  #define PIN_FLOW_SENSOR_NORMAL -1
  #define FLOW_SENSOR_COUNT    1
  #define PIN_TANK_LOW         12   // D6 - XKC-Y25-V OUT
  #define PIN_PUMP_COLD        -1   // no pump output on the D1 mini test build
  #define PIN_PUMP_NORMAL      -1
#else
  #define PIN_RELAY_COLD       16   // Relay 1 - ColdWater
  #define PIN_RELAY_NORMAL     17   // Relay 2 - NormalWater
  #define PIN_FLOW_SENSOR_COLD 18   // ZJ-S201 pulse - ColdWater
  #define PIN_FLOW_SENSOR_NORMAL 4  // ZJ-S201 pulse - NormalWater
  #define FLOW_SENSOR_COUNT    2
  #define PIN_TANK_LOW       13
  #define PIN_PUMP_COLD       26    // NFET module IN1 - pump ColdWater
  #define PIN_PUMP_NORMAL     25    // NFET module IN2 - pump NormalWater
#endif
#define PIN_RELAY_1  PIN_RELAY_COLD
#define PIN_RELAY_2  PIN_RELAY_NORMAL
#define RELAY_ACTIVE_LOW   false

// ---------- Pump outputs (NMOS channel module) ----------
// The module has its gate resistor and pull-down built in and expects a PWM
// signal on IN (0..255), not a static level. The pins are therefore driven by
// the ESP32 LEDC peripheral: PUMP_PWM_DUTY_ON (255 = 100% duty) = pump ON,
// 0 = pump OFF. Each pump follows its own line: it runs only while that
// channel's relay is dispensing, and is dropped on pause/stop/abort/OTA.
// GPIO25/26 are free at boot and are not strapping pins. The module pull-down
// holds the pump OFF while the ESP32 is in reset or flashing.
// Frequency: 1000 Hz matches a typical Arduino analogWrite module input and is
// safe for opto-isolated inputs. Raise to 20000 for an inaudible tone on a
// direct MOSFET module. Resolution 8 bits gives the 0..255 range.
#define PUMP_LEDC_CH_COLD   0     // LEDC channel 0..15, free for the pump
#define PUMP_LEDC_CH_NORMAL 1
#define PUMP_PWM_FREQ_HZ    1000
#define PUMP_PWM_RES_BITS   8
#define PUMP_PWM_DUTY_ON    255   // 100% duty
#define PUMP_PWM_MAX        ((1u << PUMP_PWM_RES_BITS) - 1u)
// Set PUMP_ACTIVE_LOW to true only for an opto-isolated module that turns the
// output ON when IN is driven LOW (the duty is then inverted).
#define PUMP_ACTIVE_LOW     false

// Tank level sensor (XKC-Y25-V) is wired and enabled.
// OUT goes through a level shifter: HIGH (5V -> 3.3V) = water present,
// LOW = tank LOW.  0 = tank input ignored and always treated as NORMAL.
// The Config tab in the web UI can override this at runtime (stored in NVS).
#define TANK_SENSOR_ENABLED 1

// ---------- Status LED: on-board only, no external parts ----------
// ESP32 Relay x2 (ESP32-WROOM-32E) board: onboard LED is GPIO23, plain active-HIGH.
// Set LED_BOARD_IS_WS2812 to 1 only if the board has a WS2812/NeoPixel RGB LED.
// Patterns: STA connected = slow blink (500 ms); AP-only = steady ON
#ifdef ESP8266
  #define PIN_LED_BOARD        2
  #define LED_BOARD_ACTIVE_LOW true
  #define LED_BOARD_IS_WS2812  0
#else
  #define PIN_LED_BOARD        23
  #define LED_BOARD_ACTIVE_LOW false
  #define LED_BOARD_IS_WS2812  0
#endif

// ZJ-S201: 1-30 L/min, ~450 pulses/L. F(Hz) = 7.5 * Q(L/min) ±10%
#define FLOW_PULSES_PER_L  450.0f
#define FLOW_FREQ_PER_LPM  7.5f
// Ignore flow edges closer together than this (electrical chatter/EMI gate).
// Keep this SHORT: a high-resolution sensor can legitimately pulse at several
// kHz (12,600 pulses/L at 2 L/min is already ~420 Hz), and a wide gate clips
// real pulses and makes the count depend on time instead of volume.
// 200 us = 5 kHz max. Raise only if the rejected counters prove noise.
#define FLOW_MIN_PULSE_US  200UL
#define PIN_TANK_HIGH      -1
#define TANK_WATER_PRESENT_HIGH true
#define TANK_LEVEL_ACTIVE_LOW false
// Debounce for the tank input. A float switch in a moving tank can drop for a
// moment (slosh, refill splash); the raw state must persist this long before it
// latches LOW or returns to NORMAL. Runtime-adjustable in Config -> Sensor
// (200..10000 ms); 2000 ms is a good value for a mechanical float.
#define TANK_DEBOUNCE_DEFAULT_MS 2000UL

// Input mode for the tank pin (runtime-selectable in Config -> Sensor):
//   0 = INPUT_PULLDOWN : disconnected reads LOW = "tank LOW" (fail-safe, default)
//   1 = INPUT          : floating. Use this when a level shifter/divider already
//                        defines the level; the internal pull-down can drag a
//                        weak HIGH (~2.5 V from a 10k/BSS138 module) below the
//                        ESP32 threshold, so the pin permanently reads LOW.
//   2 = INPUT_PULLUP   : open-collector / NPN sensors (they only pull low).
//                        Disconnected reads HIGH = "water present" (fail-open);
//                        combine with inverted logic if the sensor is active-low.
#define TANK_PIN_MODE_DEFAULT 0

// Optional analog level sensor - disabled in this design.
#define PIN_TANK_ANALOG    -1
#define TANK_EMPTY_ADC     500
#define TANK_FULL_ADC      3000

// ---------- Safety limits ----------
// ZJ-S201 min 1 L/min => 1000mL ≈ 60s. 90s timeout leaves margin.
#define DISPENSE_TIMEOUT_MS   90000UL
#define MAX_VOLUME_ML         1000
// Target reached: pump stops first, the solenoid stays open this long so the
// line can drain/equalize, then the valve closes (Cold and Normal alike).
#define VALVE_TAIL_MS         500UL
// Valve-tail compensation. Water that passes the sensor while the pump is off
// and the valve is still open is counted, but it lands in the cup only after
// the target was reached. The pump is therefore stopped this many mL early.
// 0 = no compensation (first dispense overshoots); the firmware learns the
// real drain volume from each completed dispense and stores it in NVS, so from
// the second run on the cup lands on target. Set a measured value here to seed
// it. Reported as tail_comp_ml in /api/status.
#define VALVE_TAIL_COMP_ML    0.0f
// Safety cap: never compensate by more than this fraction of the target.
#define VALVE_TAIL_COMP_MAX_FRAC 0.25f
// Safety cap for a free-run calibration test (no volume target); the user is
// expected to press STOP well before this.
#define CAL_RUN_MAX_MS        180000UL
// 5 s no-flow stop: pump on but no pulses = empty tank, blocked line or a
// disconnected flow sensor. Set to 0 only while testing/calibrating sensors.
#define FLOW_TIMEOUT_ENABLED  1
#define FLOW_TIMEOUT_MS       5000UL

// ---------- WiFi defaults ----------
#define AP_SSID_PREFIX   "Dispenser-"
#define AP_PASSWORD      "12345678"
#define STA_SSID_DEFAULT ""
#define STA_PASS_DEFAULT ""
#define HOSTNAME         "esp32-dispenser"

// ---------- MQTT telemetry (optional) ----------
// Off by default; configure at runtime in the web UI (Config -> MQTT) or with
// GET /api/mqtt?...  The device then publishes JSON to
//   <base>/<device>/status       full /api/status snapshot (retained)
//   <base>/<device>/event        job/tank/network transitions
//   <base>/<device>/info         device identity, retained once per connect
//   <base>/<device>/availability "online"/"offline" (retained, MQTT LWT)
// and, only when remote commands are allowed, subscribes to
//   <base>/<device>/cmd          e.g. "dispense cold 300", "stop"
// answering on
//   <base>/<device>/ack
// Plain TCP port 1883 only: keep the broker on the LAN (or reach it through a
// VPN/tunnel). This build has no TLS, so do not point it at a public broker.
#define MQTT_DEFAULT_ENABLED     0
#define MQTT_DEFAULT_HOST        ""
#define MQTT_DEFAULT_PORT        1883
#define MQTT_DEFAULT_USER        ""
#define MQTT_DEFAULT_PASS        ""
#define MQTT_DEFAULT_BASE        "banyu"
// Remote start/stop is a safety-relevant action and the broker may be shared:
// off by default, enable it deliberately in the Config tab.
#define MQTT_DEFAULT_ALLOW_CMD   0
#define MQTT_DEFAULT_INTERVAL_MS 5000UL
// Broker reconnect backoff. A failed attempt is only made while the dispenser
// is idle, so it can never stall a running dispense.
#define MQTT_RETRY_MS            10000UL
#define MQTT_SOCKET_TIMEOUT_S    2
// Big enough for the 1.2 kB /api/status JSON in one publish.
#define MQTT_BUFFER_SIZE         2048

// ---------- MQTT over TLS (cloud brokers) ----------
// HiveMQ Cloud, EMQX Cloud, AWS IoT, Azure IoT and most hosted brokers only
// expose TLS (8883). Set "Use TLS" in Config -> MQTT (or /api/mqtt?tls=1) and
// the device uses the ESP32 built-in root-CA bundle, which covers the public
// Let's Encrypt / DigiCert / Amazon chains. A custom CA (private broker) or a
// client certificate (AWS IoT / Azure) can be uploaded over the local API; see
// docs/MQTT.md. Certificate validation needs a correct clock, so the device
// syncs SNTP before the first TLS connect.
#define MQTT_DEFAULT_TLS          0
#define MQTT_DEFAULT_TLS_PORT     8883
#define MQTT_NTP_SERVER_1         "pool.ntp.org"
#define MQTT_NTP_SERVER_2         "time.cloudflare.com"
// Anything after 2023-11-14: a 1970 clock means SNTP has not answered yet.
#define MQTT_CLOCK_MIN_EPOCH      1700000000UL
#define MQTT_MAX_PEM_LEN          3800

// ---------- WiFi credential recovery (USB) ----------
// One-shot wipe of the saved STA SSID/password on the next boot, so the device
// comes up AP-only even when the saved credentials are wrong.
// Change the token (e.g. "clear2") to wipe again; set to "" to disable.
#define WIFI_CLEAR_TOKEN "clear2"

// ---------- OTA (WiFi firmware update) ----------
// Same password for web upload and PlatformIO espota upload.
#define OTA_PASSWORD     AP_PASSWORD
#define OTA_HOSTNAME     HOSTNAME
