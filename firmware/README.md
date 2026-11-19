# Firmware

One firmware project: [`esp32-dispenser/`](esp32-dispenser/).

It drives the two-line dispenser (one relay, one pump output and one flow
sensor per line) and serves the local **HTTP** API `banyu/server.js` uses:
`/api/status`, `/api/dispense?channel=cold|normal&ml=...`,
`/api/pause|resume|stop`, plus the web UI, calibration and OTA.

## Build and flash

```bash
pio run -d firmware/esp32-dispenser
pio run -d firmware/esp32-dispenser -t upload --upload-port COM6
```

After the first USB flash, OTA works from the web UI or with:

```bash
pio run -d firmware/esp32-dispenser -e esp32dev_ota -t upload --upload-port <ip>
```

## Connect the machine server

In `banyu/settings.env`:

```ini
ESP32_BASE_URL=http://192.168.1.50
# or http://esp32-dispenser.local if mDNS works on the machine's computer
```

Wiring, the final pin map, calibration and the exact API contract:
[`esp32-dispenser/README.md`](esp32-dispenser/README.md).
