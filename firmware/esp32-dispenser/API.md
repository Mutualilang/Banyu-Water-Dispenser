# ESP32 Dispenser API

Frontend integration guide for the ESP32 water dispenser.

The device serves a small HTTP REST API on port 80. The built-in web UI uses the
same API, so any tablet, phone, desktop app, or separate web frontend on the
same network can use it.

## 1. Connect

Use one of these base URLs:

| Network | Base URL | Notes |
|---|---|---|
| Same router/LAN | `http://<STA-IP>` | Example: `http://192.168.8.62` |
| Same router/LAN (mDNS) | `http://esp32-dispenser.local` | Resolves while the device is connected to the router |
| Device AP | `http://192.168.4.1` | AP SSID: `Dispenser-C34C`, password: `12345678` |

The STA IP can change (DHCP). Prefer the mDNS name, or save a fixed IPv4 with
`GET /api/net?mode=static&ip=…&gw=…&mask=…&dns=…` (applied after a reboot).
Read the current value from `/api/status` (`ip` field) or from the device web
page. If the tablet cannot open the page, confirm both devices are on the same
network and that router client isolation is disabled.

The dispenser does **not** need a public/internet address. Keep the tablet on
the shop router so it has internet for QRIS, and let it call the dispenser at
its private LAN address (or `esp32-dispenser.local`). Do not port-forward this
API: it has no authentication.

The API is HTTP only. A frontend served over HTTPS cannot call it directly
because browsers block mixed content; serve the frontend over HTTP or use a
proxy.

## 2. CORS and browser access

All `/api/*` endpoints send:

```http
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type
Access-Control-Max-Age: 86400
```

`OPTIONS` requests are handled for every API path, so cross-origin frontends can
use preflight requests if needed.

Recommended polling rate: **1 request per second**. The ESP32 web server handles
requests sequentially; do not hammer it with many parallel requests.

## 3. Quick start

```js
const BASE = 'http://192.168.8.62'; // or http://192.168.4.1 in AP mode

async function getStatus() {
  const res = await fetch(`${BASE}/api/status`, { cache: 'no-store' });
  if (!res.ok) throw new Error(`Status failed: ${res.status}`);
  return res.json();
}

async function start(channel, ml) {
  const res = await fetch(
    `${BASE}/api/dispense?channel=${encodeURIComponent(channel)}&ml=${ml}`,
    { cache: 'no-store' }
  );
  const text = await res.text();
  if (!res.ok) throw new Error(text);
  return text;
}

async function pause() {
  const res = await fetch(`${BASE}/api/pause`, { cache: 'no-store' });
  return res.text();
}

async function resume() {
  const res = await fetch(`${BASE}/api/resume`, { cache: 'no-store' });
  return res.text();
}

async function stop() {
  const res = await fetch(`${BASE}/api/stop`, { cache: 'no-store' });
  return res.text();
}

// Example: dispense 500 mL from the normal line, then poll until finished.
await start('normal', 500);

const timer = setInterval(async () => {
  const s = await getStatus();
  console.log(s.job_state, s.job_ml, s.job_remaining_ml, s.flow_lpm);

  if (!s.job_active) {
    clearInterval(timer);
    console.log('finished:', s.error || 'target reached');
  }
}, 1000);
```

## 4. API reference

### `GET /api`

Discovery endpoint.

Response: `application/json`

```json
{
  "name": "HydroNode",
  "version": 3,
  "endpoints": [
    "/api/status",
    "/api/dispense",
    "/api/calrun",
    "/api/pause",
    "/api/resume",
    "/api/stop",
    "/api/wifiscan",
    "/api/wifi",
    "/api/net",
    "/api/cal",
    "/api/reset-flow",
    "/api/sim"
  ]
}
```

### `GET /api/status`

Returns the complete live state. This is the main endpoint for a frontend.

Response: `application/json`

```json
{
  "job_active": false,
  "job_paused": false,
  "job_calibrate": false,
  "job_state": "idle",
  "channel": "none",
  "pump_cold": false,
  "pump_normal": false,
  "job_ml": 0.0,
  "job_target_ml": 0.0,
  "job_remaining_ml": 0.0,
  "tank_low": false,
  "tank_state": "NORMAL",
  "flow_lpm": 0.000,
  "pulses": 0,
  "total_ml": 0.0,
  "pulses_cold": 0,
  "pulses_normal": 0,
  "pulses_rejected_cold": 0,
  "pulses_rejected_normal": 0,
  "total_ml_cold": 0.0,
  "total_ml_normal": 0.0,
  "ppl": 450.0,
  "ppl_cold": 450.0,
  "ppl_normal": 450.0,
  "tail_comp_ml": 0.0,
  "tail_comp_cold": 0.0,
  "tail_comp_normal": 0.0,
  "tail_comp_learned": false,
  "ip": "192.168.8.62",
  "ap_ip": "192.168.4.1",
  "ip_mode": "dhcp",
  "ip_static": false,
  "net_ip": "",
  "net_gw": "",
  "net_mask": "255.255.255.0",
  "net_dns": "",
  "host": "esp32-dispenser.local",
  "ap_ssid": "Dispenser-C34C",
  "sta_ssid": "GLORY_DEV",
  "wifi_status": 3,
  "wifi_status_name": "CONNECTED",
  "rssi": -55,
  "error": "",
  "build": "Oct 20 2026 23:44:33",
  "ota_in_progress": false,
  "sim": false,
  "sim_hz": 0.0
}
```

#### Fields

| Field | Type | Meaning |
|---|---|---|
| `job_active` | boolean | A job exists and is either dispensing or paused |
| `job_paused` | boolean | The active job is paused; relays are off |
| `job_state` | string | `idle`, `dispensing`, `paused`, `finishing` (target reached: pump off, solenoid still open for the 0.5 s valve tail), or `calibrating` (free-run calibration) |
| `job_calibrate` | boolean | The active job is a free-run calibration test (no volume target) |
| `channel` | string | `cold`, `normal`, or `none` when idle |
| `pump_cold` | boolean | ColdWater pump output (GPIO26) is energized |
| `pump_normal` | boolean | NormalWater pump output (GPIO25) is energized |
| `job_ml` | number | Live volume counted for the current/last job |
| `job_target_ml` | number | Requested volume for the current/last job |
| `job_remaining_ml` | number | Remaining volume for an active/paused job; `0` when idle |
| `tank_low` | boolean | Tank-low state used by the safety logic |
| `tank_state` | string | `LOW` or `NORMAL` |
| `flow_lpm` | number | Live flow rate in liters per minute |
| `pulses` | integer | Total flow pulses since boot/reset (both sensors + simulator) |
| `total_ml` | number | Total volume derived from `pulses` |
| `pulses_cold` | integer | Raw ColdWater sensor pulses since boot/reset (hardware only) |
| `pulses_normal` | integer | Raw NormalWater sensor pulses since boot/reset (hardware only) |
| `pulses_rejected_cold` | integer | Cold edges rejected by the noise gate (`FLOW_MIN_PULSE_US`) |
| `pulses_rejected_normal` | integer | Normal edges rejected by the noise gate |
| `total_ml_cold` | number | ColdWater volume derived from `pulses_cold` and `ppl_cold` |
| `total_ml_normal` | number | NormalWater volume derived from `pulses_normal` and `ppl_normal` |
| `ppl` | number | PPL of the active line (Cold when idle); legacy field |
| `ppl_cold` | number | ColdWater calibration: pulses per liter |
| `ppl_normal` | number | NormalWater calibration: pulses per liter |
| `tail_comp_ml` | number | Learned valve-tail drain (mL) the pump stops early by, for the active line |
| `tail_comp_cold` | number | Same, ColdWater line |
| `tail_comp_normal` | number | Same, NormalWater line |
| `tail_comp_learned` | boolean | `tail_comp_*` came from a completed dispense (not the config seed) |
| `ip` | string | Current STA IP, or `0.0.0.0` if not connected |
| `ap_ip` | string | AP address (always `192.168.4.1`) |
| `ip_mode` | string | `dhcp` or `static` |
| `ip_static` | boolean | Static IPv4 is configured |
| `net_ip`, `net_gw`, `net_mask`, `net_dns` | string | Saved static address (empty when DHCP) |
| `host` | string | mDNS name (`.local`) to reach the API |
| `ap_ssid` | string | Device AP SSID |
| `sta_ssid` | string | Saved router SSID |
| `wifi_status` | integer | Arduino `wl_status_t` value |
| `wifi_status_name` | string | Human-readable WiFi state |
| `rssi` | integer | WiFi signal strength in dBm |
| `error` | string | Last job/result message |
| `build` | string | Firmware compile timestamp |
| `ota_in_progress` | boolean | Firmware update is running |
| `sim` | boolean | LAN sensor simulator is enabled |
| `sim_hz` | number | Simulated flow frequency |

Notes:

- `job_ml` is live and is not a separate frozen result. After STOP, use
  `pulses`/`total_ml` if you need the raw counter, or record `job_ml` when
  `job_active` becomes false.
- A job counts only its selected channel's flow sensor: ColdWater `GPIO18`,
  NormalWater `GPIO4`. `pulses`/`total_ml` combine both sensors plus the
  simulator, while `pulses_cold`/`pulses_normal` expose the raw hardware
  counters and do not include simulated pulses.
- While paused, `job_active` stays `true`, `job_paused` is `true`, and
  `job_remaining_ml` is preserved. Pause time is excluded from the dispense
  timeout.
- `tank_low` mirrors the XKC-Y25-V input on GPIO13: `HIGH` = water present,
  `LOW` = tank LOW, debounced 500 ms. The Config tab
  (`/api/config?tank_sensor=0|1`) can enable/disable it at runtime.
  `tank_pin_mode` (`0` pull-down, `1` floating, `2` pull-up) and `tank_raw`
  (`1`/`0`, `-1` when disabled) help diagnose a weak level; see
  `/api/config?tank_pin=0|1|2`. `tank_debounce_ms` (default 2000) sets the
  low-pass filter; `/api/config?tank_debounce=200..10000` changes it.
- `error` is the last event, for example `stopped by user`, `timeout - stopped`,
  `tank LOW - stopped, refill`, or `no flow - check valve/pump`.

### `GET /api/dispense`

Starts a dispense job.

Query parameters:

| Parameter | Required | Values |
|---|---|---|
| `channel` | yes* | `cold` or `normal` |
| `ml` | yes | `> 0` and `<= 1000` |

\* Legacy parameters are also accepted: `cold=1`, `r1=1`, `r2=1`. Prefer
`channel`.

Success: HTTP `200`, `text/plain`

```text
Started normal 500 mL
```

Rejected: HTTP `400`, `text/plain`

```text
Rejected: already dispensing
Rejected: Select Cold or Normal first
Rejected: Bad volume
Rejected: Tank LOW - refill to start
Rejected: OTA update in progress
```

Rules:

- Only one job can run at a time.
- Tank LOW blocks a new job when the tank sensor is enabled.
- The selected relay turns on; the other relay stays off.
- The job stops automatically at the target, on timeout, or on tank LOW.
- At the target the **pump stops first**, the solenoid stays open for
  `VALVE_TAIL_MS` (500 ms), then the valve closes (`job_state: "finishing"`).
- The pump stops `tail_comp_ml` early so the tail drain lands the cup on
  target. The value is learned from each completed dispense (NVS), reported in
  `/api/status`, and can be overridden with
  `/api/config?tail_ml=<0..150>`. Saving a new PPL resets it to re-learn.

### `GET /api/pause`

Pauses the active job. Both relays turn off immediately, but the job keeps its
channel, target, counted volume, and remaining volume.

Response: `text/plain`

```text
Paused
```

Rejected: HTTP `400`, `text/plain`

```text
Rejected: no active job
Rejected: already paused
```

### `GET /api/resume`

Resumes a paused job on the same channel and continues toward the original target.
The paused duration is excluded from the dispense timeout. A resume is rejected
while the tank is LOW.

Response: `text/plain`

```text
Resumed
```

Rejected: HTTP `400`, `text/plain`

```text
Rejected: no active job
Rejected: not paused
Rejected: Tank LOW — refill to resume
```

### `GET /api/stop`

Stops the current job and turns both relays off.

Response: `text/plain`

```text
Stopped
```

or, when nothing is running:

```text
Already idle
```

### `GET /api/calrun`

Starts a **free-run calibration** test: the selected line's valve and pump open
with **no volume or pulse target**, so the reference volume is set by weighing
the collected water instead of by the stored PPL. It ends on `/api/stop` or the
`CAL_RUN_MAX_MS` safety cap (180 s).

Query parameters:

| Parameter | Required | Values |
|---|---|---|
| `channel` | yes | `cold` or `normal` |

Response: `text/plain`

```text
Calibration run started on cold — press Stop when done
```

or HTTP `400` with the rejection reason (busy, tank low, OTA in progress).

Calibration procedure: start → let the line prime → collect ~900 mL → stop →
`PPL = pulses × 1000 / actual_mL` → save with `/api/cal?channel=…&ppl=…`.

### `GET /api/wifiscan`

Scans for nearby 2.4 GHz WiFi networks.

Response: `application/json`

```json
[
  { "ssid": "GLORY_DEV", "rssi": -55, "ch": 6 },
  { "ssid": "AnotherNetwork", "rssi": -72, "ch": 11 }
]
```

Hidden SSIDs are skipped. Scanning can block the API briefly.

### `GET /api/wifi`

Saves router credentials and reboots the ESP32.

Query parameters:

| Parameter | Required |
|---|---|
| `ssid` | yes |
| `pass` | no |
| `clear` | no — `1` wipes the saved credentials and reboots into AP-only |

Responses: `text/plain`

```text
Saved, rebooting
```

or HTTP `400`:

```text
Missing ssid
```

Recovery: `GET /api/wifi?clear=1` removes `sta_ssid`/`sta_pass` from NVS, so the
device boots AP-only (`Dispenser-XXXX`, `http://192.168.4.1`) even if the saved
network is unreachable or the password is wrong. It also resets a saved static
IPv4 back to DHCP. Use this when you need to join a different network.

Saving a **different SSID** also resets the static IPv4 to DHCP, because an
address from the old network would otherwise be unreachable.

After this call, expect the device to disconnect and reconnect to the router.
Read the new IP from the router or reconnect to the device AP and check the web
page.

### `GET /api/net`

Reads or changes the STA IPv4 mode. Changes are persisted in NVS and applied on
the next boot; add `reboot=1` to restart immediately.

Query parameters:

| Parameter | Required | Meaning |
|---|---|---|
| `mode` | no | `dhcp` or `static`; omit to only read the config |
| `ip` | for `static` | e.g. `192.168.1.50` |
| `gw` | for `static` | Router/gateway, must be in the same subnet |
| `mask` | no | Defaults to `255.255.255.0` |
| `dns` | no | Defaults to the gateway |
| `reboot` | no | `1` restarts after saving |

Responses: `application/json`

```json
{
  "ok": true,
  "mode": "static",
  "ip": "192.168.1.50",
  "gw": "192.168.1.1",
  "mask": "255.255.255.0",
  "dns": "",
  "current_ip": "192.168.1.50",
  "ap_ip": "192.168.4.1",
  "hostname": "esp32-dispenser",
  "mdns": "esp32-dispenser.local"
}
```

Invalid values (bad address, gateway outside the subnet, or `192.168.4.x` which
collides with the device AP) return HTTP `400` and are not saved. The device AP
stays available for recovery even when the static address is wrong.

### `GET /api/mqtt`

Reads or changes the optional MQTT telemetry bridge. Settings are persisted in
NVS and applied immediately; the client disconnects and reconnects (the
reconnect is deferred while a dispense/calibration job is running).

Query parameters (all optional; omit all to only read):

| Parameter | Required | Meaning |
|---|---|---|
| `enabled` | no | `1`/`0`; `1` starts the bridge |
| `host` | no | Broker host or `host:port`; `mqtt://` prefix and `/path` are stripped |
| `port` | no | `1..65535`, default `1883` (`8883` with TLS) |
| `tls` | no | `1` enables TLS and moves the default port 1883 → 8883 |
| `user` | no | Broker username; empty clears it |
| `pass` | no | Broker password; omitted = keep the saved one |
| `clearpass` | no | `1` erases the saved password |
| `base` | no | Topic base, default `banyu` (no spaces, `#`, `+`, quotes) |
| `interval` | no | Publish interval `1000..600000` ms, default `5000` |
| `allow_cmd` | no | `1` accepts commands on `<base>/<device>/cmd` (default `0`) |

`POST /api/mqtt/ca` uploads a custom root CA (PEM body, `?clear=1` removes it).
`POST /api/mqtt/client` takes `cert=<PEM>` and `key=<PEM>` for mutual TLS
(`?clear=1` removes them). Both only change the MQTT transport; nothing else.

Response: `application/json` — the password is never returned, only `has_pass`.

```json
{
  "ok": true,
  "enabled": true,
  "connected": true,
  "host": "192.168.1.10",
  "port": 1883,
  "user": "dispenser",
  "has_pass": true,
  "base": "banyu",
  "device": "esp32-dispenser-c34c",
  "topic_base": "banyu/esp32-dispenser-c34c",
  "interval_ms": 5000,
  "allow_cmd": false,
  "state": 0,
  "publishes": 42,
  "connects": 1,
  "tls": false,
  "clock_ok": true,
  "has_ca": false,
  "has_client_cert": false,
  "last_error": ""
}
```

Topics, payloads, command syntax and a dashboard example:
[`docs/MQTT.md`](docs/MQTT.md). TLS (8883) is supported for cloud brokers; a
custom CA or mutual-TLS client certificate can be uploaded over the local API.
Remote commands are off by default.

### `GET /api/cal`

Saves the flow calibration value per line. It is persisted in NVS.

Query parameters:

| Parameter | Required | Range |
|---|---|---|
| `ppl` | yes | `100..50000` |
| `channel` | no | `cold` or `normal`; omit to write **both** lines (legacy behaviour) |

Response: `text/plain`

```text
Saved ppl=450.0 for cold
```

or HTTP `400`:

```text
Bad ppl (100..50000, ZJ-S201 default 450)
```

Calibration formula:

```text
PPL = pulses * 1000 / actual_mL
```

Use the pulse count of the line being calibrated (`pulses_cold` or
`pulses_normal`). 450 is only the ZJ-S201 nominal fallback: high-resolution
sensors can legitimately reach 10,000+ pulses/L, so verify by re-running a
dispense and checking the delivered volume (±2 %). If counts do not track
volume, check `pulses_rejected_cold/normal` and the sensor wiring.

### `GET /api/reset-flow`

Resets both hardware flow pulse counters and simulated pulses.

Response: `text/plain`

```text
Flow counter reset
```

### `GET /api/sim`

Test-only LAN sensor simulator. State is RAM-only and clears on reboot.

Query parameters (all optional):

| Parameter | Example | Meaning |
|---|---|---|
| `enable` | `1` / `0` | Enable or disable the simulator |
| `hz` | `15` | Simulated pulse frequency |
| `tank` | `ok` / `low` / `1` / `alarm` | Override tank state |
| `addpulses` | `225` | Add an exact pulse burst |
| `reset` | `1` | Clear simulated pulses |

Response: `application/json`

```json
{
  "sim": true,
  "hz": 15.0,
  "tank_low": false,
  "sim_pulses": 0
}
```

### `POST /update`

Firmware update endpoint used by the built-in OTA page. It is not needed by a
normal frontend.

- Content type: `multipart/form-data`
- File field: `firmware`
- File: `.pio/build/esp32dev/firmware.bin`
- Response: `text/plain` with `OK` or `FAIL`
- On `OK`, the device reboots.

```bash
curl -f -F "firmware=@.pio/build/esp32dev/firmware.bin" \
  http://192.168.8.62/update
```

## 5. curl examples

```bash
# Discovery (mDNS works while the device is on the router)
curl http://esp32-dispenser.local/api
curl http://192.168.8.62/api

# Full state
curl http://esp32-dispenser.local/api/status

# Read / set the network mode
curl http://esp32-dispenser.local/api/net
curl "http://esp32-dispenser.local/api/net?mode=static&ip=192.168.1.50&gw=192.168.1.1&mask=255.255.255.0&dns=192.168.1.1&reboot=1"
curl "http://esp32-dispenser.local/api/net?mode=dhcp&reboot=1"

# Forget the saved Wi-Fi and boot AP-only
curl "http://esp32-dispenser.local/api/wifi?clear=1"

# Start 300 mL cold
curl "http://192.168.8.62/api/dispense?channel=cold&ml=300"

# Pause and resume, preserving the remaining volume
curl http://192.168.8.62/api/pause
curl http://192.168.8.62/api/resume

# Stop/cancel
curl http://192.168.8.62/api/stop

# Calibrate per line
curl "http://192.168.8.62/api/cal?channel=cold&ppl=450"

# Reset total counter
curl http://192.168.8.62/api/reset-flow

# Check CORS preflight
curl -i -X OPTIONS http://192.168.8.62/api/dispense
```

## 6. Python example

```python
import requests

BASE = "http://192.168.8.62"

status = requests.get(f"{BASE}/api/status", timeout=5).json()
print(status["job_active"], status["flow_lpm"])

r = requests.get(f"{BASE}/api/dispense", params={"channel": "normal", "ml": 500}, timeout=5)
print(r.status_code, r.text)
```

## 7. Safety and security

- The API has **no authentication**. Anyone who can reach the device on the
  network can start or stop water.
- Keep the dispenser on a trusted LAN. Do not port-forward it to the internet.
- The tank sensor is enabled by default (`TANK_SENSOR_ENABLED 1`), but the
  no-flow safety is still disabled for calibration (`FLOW_TIMEOUT_ENABLED 0`).
  Enable it before unattended operation.
- Keep the 90-second dispense timeout and the 1000 mL maximum in mind when
  building UI validation.

## 8. Troubleshooting

| Symptom | Check |
|---|---|
| Browser shows CORS error | Confirm the request goes to `/api/*`; all API paths now return CORS headers |
| Connection refused | Same WiFi/LAN? Correct IP? Device powered? |
| Works on device page but not external UI | HTTPS frontend calling HTTP device, or wrong base URL |
| 400 on dispense | Channel missing, volume outside `1..1000`, tank low, or job already active |
| `/api/wifi` disconnects | Expected: the device reboots after saving credentials |
| Status IP is `0.0.0.0` | STA not connected; join the device AP and configure WiFi |
| `esp32-dispenser.local` does not resolve | mDNS blocked on this Android/WebView; use the IP or DHCP reservation |
| Static IP set but unreachable | Wrong subnet/gateway; recover on the device AP `192.168.4.1` and switch back to DHCP |
| MQTT stays `connected:false` | Broker host/port/credentials, broker listening on the LAN (not only `127.0.0.1`), and `last_error` in `/api/mqtt`; the device retries every 10 s and never during a pour |
| MQTT `connect rc=4/5` | Bad username/password (`4`) or not authorised (`5`) — check the broker ACL for that user |
| No `status` on the dashboard | Subscribe to `<topic_base>/#`; the topic base shown in `/api/mqtt` (`topic_base`), not just `banyu` |

## 9. Related files

- `src/main.cpp` — API routes, status JSON, control logic
- `include/config.h` — pins, calibration, feature flags
- `README.md` — wiring, build, OTA, calibration
- `docs/MQTT.md` — MQTT topics, payloads, commands, dashboard examples
- `dummy-sensor/` — optional LAN simulator for frontend testing
