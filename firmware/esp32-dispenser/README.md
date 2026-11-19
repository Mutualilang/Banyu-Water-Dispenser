# Banyu ESP32 dispenser (working firmware)

PlatformIO + Arduino firmware for the **two-line** Banyu dispenser: one cold
line and one room-temperature line, each with its own relay, pump output and
flow sensor.

This is the only firmware in the repository and the one **`banyu/server.js`
actually talks to**. It serves the local HTTP API the machine server polls and
controls. Flash this for the kiosk to work.

## Wiring (final pin map)

The pin map lives in [`include/config.h`](include/config.h).

| Function | GPIO | Notes |
|---|---|---|
| Relay 1 — ColdWater | 16 | Active-HIGH board. Only one channel ON at a time |
| Relay 2 — NormalWater | 17 | Active-HIGH board |
| Pump driver IN1 (NFET module) — ColdWater | 26 | **PWM 0–255** (LEDC, 1 kHz, 8-bit): `255` = 100% = pump ON, `0` = OFF. Runs only while the cold line dispenses |
| Pump driver IN2 (NFET module) — NormalWater | 25 | Same as IN1, runs only while the normal line dispenses |
| Flow ZJ-S201 ColdWater (yellow) | 18 | `INPUT_PULLUP`, FALLING ISR. Red → 5V, black → GND, yellow → **10k series + 20k to GND** (5V→3.3V divider) |
| Flow ZJ-S201 NormalWater (yellow) | 4 | `INPUT_PULLUP`, FALLING ISR. Same divider |
| Level XKC-Y25-V OUT | 13 | `HIGH` = water present, `LOW` = tank LOW. **5V→3.3V divider** (`OUT -10k- GPIO13 -20k- GND`), input mode `INPUT_PULLDOWN`: unplugged = tank LOW (fail-safe) |
| On-board LED | 23 | No wiring. STA connected = slow blink; AP-only = steady ON. Blinks twice at boot |

ZJ-S201: 1–30 L/min, **~450 pulses/L**, F(Hz) = 7.5 × Q(L/min) ±10%.
Example: 500 mL ≈ 225 pulses; at 2 L/min ≈ 15 Hz, 500 mL ≈ 15 s.

The tank input mode is selectable at runtime (Config → Sensor, or
`/api/config?tank_pin=0|1|2`):

| Value | Mode | Use |
|---|---|---|
| `0` | `INPUT_PULLDOWN` | default, unplugged = tank LOW (fail-safe) |
| `1` | `INPUT` | floating; for a BSS138/divider that defines the level |
| `2` | `INPUT_PULLUP` | open-collector / NPN sensors that only pull low |

`/api/status` exposes `tank_raw` and `tank_pin_mode` so a marginal level is
easy to confirm. The tank-low filter is configurable (`tank_debounce`,
200–10000 ms, default **2000 ms**).

### Pump outputs

`GPIO26` (cold) / `GPIO25` (normal) are driven by LEDC as an 8-bit PWM signal
because the NMOS channel modules expect a PWM input: `ledcWrite(255)` = pump
on, `0` = off. The module's pull-down holds the pumps off while the ESP32 is in
reset. Both are reported as `pump_cold` / `pump_normal` in `/api/status`.

## How the Banyu server uses it

Set the dispenser address in `banyu/settings.env`:

```ini
# IP, or mDNS name if the laptop resolves .local
ESP32_BASE_URL=http://192.168.1.50
# ESP32_BASE_URL=http://esp32-dispenser.local
```

The server then polls `GET /api/status` (every 5 s idle, every 1 s while
pouring) and sends:

| Call | When |
|---|---|
| `GET /api/dispense?channel=cold&ml=300` | customer paid and pressed Start |
| `GET /api/dispense?channel=normal&ml=600` | same, room-temperature line |
| `GET /api/pause` / `GET /api/resume` | customer pauses/resumes |
| `GET /api/stop` | customer stops |

`channel` is `cold` for `dingin` and `normal` for `ruang` (the server maps it
from the order). The status fields the server relies on are:

```json
{
  "job_active": true, "job_paused": false, "job_state": "dispensing",
  "channel": "cold", "job_ml": 240.0, "job_target_ml": 300.0,
  "tank_low": false, "tank_state": "NORMAL", "error": "",
  "flow_lpm": 2.1, "rssi": -58, "pump_cold": true, "pump_normal": false
}
```

The server decides completion from `job_active` going false plus `job_ml` and
`error`; it stores ordered vs actual ml, duration and the % difference. A
completed pour off by more than 5 % is flagged in the dashboard.

`MAX_VOLUME_ML` is 1000 ml here, so the kiosk's 300/600/900 ml all fit. Keep the
two in sync if the kiosk ever offers more.

## Network

- First boot: the board creates AP **`Dispenser-XXXX`** / password
  **`12345678`**. Open `http://192.168.4.1`.
- Config → Network: join the shop router (2.4 GHz only) and optionally set a
  static IP. A DHCP reservation on the router is the easiest.
- On the router LAN the device is also at **`http://esp32-dispenser.local`**
  (mDNS).
- The tablet must join the **router**, not the dispenser AP, so it keeps
  internet for QRIS. The dispenser only needs the local network; do not
  port-forward it (the API has no authentication).

## Build and flash

```bash
pio run -d firmware/esp32-dispenser                 # compile
pio run -d firmware/esp32-dispenser -t upload       # first flash over USB
pio device monitor -d firmware/esp32-dispenser
```

There is no fixed `upload_port`; pass one if needed:

```bash
pio run -d firmware/esp32-dispenser -t upload --upload-port COM6
```

After the first USB flash, OTA works from the web UI (Firmware card, password
`12345678`) or:

```bash
pio run -d firmware/esp32-dispenser -e esp32dev_ota -t upload --upload-port <ip>
```

## Logic

1. Web UI → choose **❄ Cold / 🌡 Normal** → choose **300 / 600 / 900 mL** → **START**.
2. The ESP turns on that line's relay + pump output and counts pulses on that
   line's flow sensor.
3. At target: **pump OFF first**, hold the solenoid open for `VALVE_TAIL_MS`
   (500 ms) so the line drains, then relay OFF. `job_state` reports `finishing`
   during the tail. The pump stops `tail_comp_ml` early so the drained water
   still lands the cup on target; that value is learned from each completed
   dispense.
4. **PAUSE** keeps the remaining volume; **RESUME** continues on the same line.
   PAUSE / STOP / abort drop pump and valve immediately.
5. Safety: tank LOW aborts immediately and blocks new starts; 90 s timeout
   (paused time excluded); 5 s no-flow stop (`FLOW_TIMEOUT_ENABLED 1` in this
   copy); cold/normal mutual exclusion.

## Calibration

Each line has its own PPL (`ppl_cold` on GPIO18, `ppl_normal` on GPIO4) so
calibrate Cold and Normal separately.

Web UI: **Calibration tab** → pick the line → **Start test** → let it prime,
collect ~900 mL → **Stop** → weigh the water → enter mL → **Calculate & save**.
Repeat for the other line, then verify with a 300 mL dispense.

Manual (API): `GET /api/reset-flow`, `GET /api/calrun?channel=cold`, collect,
`GET /api/stop`, read `pulses_cold`, then
`GET /api/cal?channel=cold&ppl=<value>`.

The `450` nominal is only a fallback. Trust the measured figure.

## Optional MQTT telemetry

The firmware can publish `/api/status` to a broker (Config → MQTT), but it is
**off by default**. `banyu/server.js` can optionally subscribe to that telemetry
(see "MQTT telemetry" in `banyu/README.md`); control still goes over the local
HTTP API. The topic scheme is `<base>/status`, `<base>/event` and
`<base>/availability`; full reference: [`MQTT.md`](MQTT.md).

## Notes for this copy

- Pins are unchanged from the final hardware spec.
- `FLOW_TIMEOUT_ENABLED` is **1** here for production (the original bench build
  had it at 0 while sensors were being tested).
- `upload_port` is not pinned to COM6, so the project works on any machine.
- `API.md` in this folder is the full HTTP API reference (web UI, config,
  calibration, OTA, MQTT).
