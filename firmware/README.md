# Firmware

Two ESP32 firmware projects live here. They are **not** interchangeable.

| Folder | What it is | Used by the Banyu server? |
|---|---|---|
| [`esp32-dispenser/`](esp32-dispenser/) | Production firmware for the two-line dispenser (relay + pump + flow sensor per line). Serves the local **HTTP** API (`/api/status`, `/api/dispense`, `/api/pause|resume|stop`) and the web UI/OTA. Final pin map from the hardware spec. | **Yes.** `banyu/server.js` talks to this over `ESP32_BASE_URL`. |
| [`banyu-esp32/`](banyu-esp32/) | Earlier **MQTT** prototype (one pump + two valves, one flow sensor). Publishes/consumes `banyu/<id>/...` topics. | **No.** The server has no MQTT client yet. Keep for the future cloud path. |

## Which one to flash

For the kiosk to pour, flash **`esp32-dispenser/`**:

```bash
pio run -d firmware/esp32-dispenser -t upload --upload-port COM6
```

Then set in `banyu/settings.env`:

```ini
ESP32_BASE_URL=http://192.168.1.50
# or http://esp32-dispenser.local if mDNS works on the machine's computer
```

Wiring, build, calibration and the exact API contract:
[`esp32-dispenser/README.md`](esp32-dispenser/README.md).
