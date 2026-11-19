# MQTT telemetry bridge

The dispenser can push the same data as `GET /api/status` to any MQTT broker,
so an owner/operator dashboard can consume live data **without polling the
ESP32** and without exposing the device to the internet.

The local HTTP API keeps working exactly as before, so the Android tablet /
Banyu kiosk in `banyu/` needs no change.

- Transport: MQTT 3.1.1 over TCP — **1883 plain** (LAN) or **8883 TLS** (cloud).
- Default state: **disabled**.
- Remote commands: **disabled** by default (see [Commands](#commands)).
- Settings survive reboots (stored in NVS, password never returned by the API).

## 1. Enable it

Web UI → **Config** → **MQTT** card:

| Field | Example |
|---|---|
| Enable MQTT | on |
| Broker | `192.168.1.10` or `xxxx.s1.eu.hivemq.cloud` |
| Port | `1883` (LAN) / `8883` (TLS) |
| Use TLS | on for cloud brokers |
| Username | optional |
| Password | optional (blank = keep the saved one) |
| Topic base | `banyu` |
| Publish interval | `5000` ms |

Or over the API (same network):

```bash
curl "http://192.168.1.50/api/mqtt?enabled=1&host=192.168.1.10&port=1883&base=banyu&interval=5000"
curl "http://192.168.1.50/api/mqtt?user=dispenser&pass=secret"
curl "http://192.168.1.50/api/mqtt"        # read state (password omitted)
```

`GET /api/mqtt` returns `enabled`, `connected`, `host`, `port`, `user`,
`has_pass`, `base`, `device`, `topic_base`, `interval_ms`, `allow_cmd`,
`publishes`, `connects`, `last_error`, `state`, `tls`, `clock_ok`, `has_ca`,
`has_client_cert`.

The device ID is `esp32-dispenser-<4 hex>` (chip ID suffix), so the default
topic prefix is e.g. `banyu/esp32-dispenser-c34c`.

## 1b. TLS / cloud broker

Enable **Use TLS** (or `?tls=1`, which moves the port from 1883 to 8883).
The firmware then:

1. syncs SNTP (`pool.ntp.org`) — certificate validation needs a correct clock;
   `/api/mqtt` shows `clock_ok:false` until it succeeds;
2. validates the broker certificate against a small built-in root set:
   Let's Encrypt ISRG X1/X2, DigiCert Global Root G2, Amazon Root CA 1
   (covers HiveMQ Cloud, EMQX Cloud, most managed brokers);
3. keeps the connection on 8883 with the same topics and credentials.

Test-mode examples:

```bash
# HiveMQ Cloud / EMQX Cloud (public CA, username + password)
curl "http://192.168.1.50/api/mqtt?host=abcd1234.s1.eu.hivemq.cloud&port=8883&tls=1&user=dispenser&pass=secret"

# Private broker with its own CA (upload the PEM as the POST body)
curl -X POST --data-binary @my-ca.pem "http://192.168.1.50/api/mqtt/ca"

# Mutual TLS (AWS IoT / Azure IoT): certificate + private key
curl -X POST --data-urlencode "cert@device-cert.pem" --data-urlencode "key@device-key.pem" \
     "http://192.168.1.50/api/mqtt/client"
```

`POST /api/mqtt/ca?clear=1` and `POST /api/mqtt/client?clear=1` remove the
overrides. PEM strings can also be sent as `ca=`, `cert=`, `key=` form fields
with `\n` escapes. With TLS on, `mqtt:8883` is the expected port; the built-in
roots add ~160 kB of flash (firmware ≈85 % of the OTA slot), which is why the
certificate set is trimmed instead of using the ESP32 200-root bundle.

## 2. Topics

All topics are `QoS 0`. `<base>` is the full prefix reported as `topic_base` in
`/api/mqtt`, e.g. `banyu/esp32-dispenser-c34c`.

| Topic | Retained | Direction | Content |
|---|---|---|---|
| `<base>/availability` | yes | device → broker | `online` / `offline` (LWT) |
| `<base>/status` | yes | device → broker | full `/api/status` JSON |
| `<base>/info` | yes | device → broker | identity, once per connect |
| `<base>/event` | no | device → broker | job / tank transitions |
| `<base>/cmd` | no | broker → device | commands (only if allowed) |
| `<base>/ack` | no | device → broker | result of a command |

### status

Published every `interval_ms` and immediately when the job state changes.
Fields are the same as `GET /api/status` (see `API.md`), plus
`mqtt_enabled`, `mqtt_connected`, `mqtt_host`, `mqtt_topic`.

```json
{
  "job_active": true,
  "job_state": "dispensing",
  "channel": "cold",
  "job_ml": 210.4,
  "job_target_ml": 300.0,
  "job_remaining_ml": 89.6,
  "flow_lpm": 1.98,
  "tank_low": false,
  "tank_state": "NORMAL",
  "total_ml_cold": 12450.0,
  "total_ml_normal": 3810.0,
  "ip": "192.168.1.50",
  "rssi": -61,
  "build": "Nov 17 2026 11:40:00",
  "mqtt_connected": true,
  "...": "..."
}
```

### event

```json
{"event":"tank","state":"LOW"}
{"event":"job","state":"dispensing","channel":"cold","job_ml":0.0,"target_ml":300.0,"error":""}
{"event":"job","state":"idle","channel":"none","job_ml":298.7,"target_ml":300.0,"error":""}
```

Use `event` for notifications (Telegram/WhatsApp bot, Grafana annotations) and
`status` for gauges/tables.

### info

```json
{"device":"esp32-dispenser-c34c","hostname":"esp32-dispenser",
 "fw":"Nov 17 2026 11:40:00","ip":"192.168.1.50",
 "mdns":"esp32-dispenser.local","base_topic":"banyu/esp32-dispenser-c34c",
 "http_api":true}
```

## 3. Commands

Off by default. Enable with **Accept remote commands** in the Config tab, or:

```bash
curl "http://192.168.1.50/api/mqtt?allow_cmd=1"
```

Accepted on `<base>/cmd` — plain text or flat JSON:

```
dispense cold 300
dispense normal 600
cold 300
stop
pause
resume
status
```

```json
{"cmd":"dispense","channel":"cold","ml":300,"id":"order-1234"}
```

Always send a unique `id` for cloud control: the backend retries until it sees
the ack, and the firmware ignores a repeated id so a lost acknowledgement can
never dispense twice.

The reply arrives on `<base>/ack` and echoes the id:

```json
{"ok":true,"cmd":"dispense","id":"order-1234","channel":"cold","ml":300.0,"msg":"started cold 300 mL"}
{"ok":false,"cmd":"dispense","id":"order-1235","channel":"normal","ml":1200.0,"msg":"ml must be 1..1000"}
{"ok":true,"cmd":"dispense","id":"order-1234","channel":"none","ml":0.0,"msg":"duplicate command id ignored","dup":true}
```

Safety rules are unchanged and still enforced: tank LOW blocks a start, only
one job at a time, 90 s timeout, pause/stop drop pump + valve, and OTA is not
reachable over MQTT. Commands are ignored while an upload is running.

> A dispense spends real water. Anyone who can publish to `<base>/cmd` can
> start one. Keep `allow_cmd` off unless the broker is private and
> authenticated, and give the dispenser its own broker user/ACL.

## 4. Test with Mosquitto

```bash
# terminal 1 - subscribe to everything from this device
mosquitto_sub -h 192.168.1.10 -t 'banyu/esp32-dispenser-c34c/#' -v

# terminal 2 - command (only if allow_cmd=1)
mosquitto_pub -h 192.168.1.10 -t 'banyu/esp32-dispenser-c34c/cmd' -m 'cold 300'
```

A minimal Node.js dashboard consumer (the kiosk server itself does not need
this; it keeps using HTTP):

```js
// npm install mqtt
const mqtt = require('mqtt');
const c = mqtt.connect('mqtt://192.168.1.10:1883', {
  username: 'dispenser', password: 'secret'
});
c.subscribe('banyu/esp32-dispenser-c34c/#');
c.on('message', (topic, payload) => {
  const msg = JSON.parse(payload.toString());
  if (topic.endsWith('/status')) console.log(msg.job_state, msg.job_ml, msg.tank_state);
});
```

Python (paho-mqtt) works the same way; subscribe to `.../status` for a live
gauge, `.../availability` to grey out the dashboard when the dispenser is
offline.

## 5. Behaviour and limits

- Reconnect backoff is 10 s. A connect attempt is **skipped while a dispense
  or calibration job is running**, so an unreachable broker cannot stall the
  flow-control loop. Updates resume right after the job ends.
- Status is retained, so a dashboard that connects later gets the last known
  state immediately. Check `availability` before trusting it.
- The password is stored in NVS in clear text (like the Wi-Fi password) and is
  never included in any HTTP or MQTT payload.
- TLS is available for cloud brokers (8883). The built-in root set is trimmed to
  four public CAs; upload your own CA / client certificate for a private broker
  or mutual TLS. Commands are still QoS 0 on the wire, so reliability comes from
  the sender retrying with the same command `id` (the firmware dedupes it).
- Prefer TLS for anything that leaves the LAN. Plain 1883 sends the password and
  the control topic in clear text.
- The ESP32 has one 2.4 GHz radio shared by Wi-Fi and the AP. MQTT adds a few
  kB every interval; keep `interval_ms` at 5000 or higher during service.
- OTA is not exposed over MQTT, and MQTT cannot change calibration, Wi-Fi or
  the network configuration — those stay on the local HTTP API only.
