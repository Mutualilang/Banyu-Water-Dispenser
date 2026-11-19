# Banyu ESP32 firmware v3.0.0 — flashing

Two-line Banyu dispenser firmware: one **cold** line and one **normal**
(room-temperature) line, each with its own relay, pump PWM output and ZJ-S201
flow sensor.

> **Hardware warning:** this is for the two-line board. Pins: relays 16/17
> (active-HIGH), pump PWM 26/25, flow sensors 18/4, tank sensor 13. Do not flash
> it on the old one-pump prototype.

## Which file?

| File | Use |
|---|---|
| `banyu-esp32-v3.0.0.bin` | **OTA / application image.** Use this from the dispenser web UI when the device already runs this firmware. |
| `banyu-esp32-v3.0.0-merged.bin` | **First USB flash.** Full 4 MB image (bootloader + partition table + boot_app0 + app), write at `0x0`. |

Do **not** upload the merged image through the web UI; it expects the
application image only.

## OTA from the web UI (easiest)

1. Open the dispenser: `http://<device-ip>/` or `http://esp32-dispenser.local/`.
2. **Firmware** card → choose `banyu-esp32-v3.0.0.bin` (not the merged file).
3. Password: `12345678` (change `OTA_PASSWORD` in `include/config.h` if you
   changed the AP password).
4. **Upload & Reboot.** Any active pour is stopped and both relays/pumps are
   switched off first.

## First flash over USB

Using esptool:

```bash
esptool.py --chip esp32 --port COM6 write_flash 0x0 banyu-esp32-v3.0.0-merged.bin
```

Or ESP32 Flash Download Tool: one file at address `0x0`.

Advanced (separate images):

| Address | File |
|---|---|
| `0x1000` | `bootloader.bin` |
| `0x8000` | `partitions.bin` |
| `0xe000` | `boot_app0.bin` |
| `0x10000` | `firmware.bin` |

PlatformIO users can also build/flash from source:

```bash
pio run -d firmware/esp32-dispenser -t upload --upload-port COM6
```

## First boot / network

- The board creates AP **`Dispenser-XXXX`** / password **`12345678`**.
- Open `http://192.168.4.1` → Config → Network, join the shop router
  (2.4 GHz), optionally set a static IP.
- On the router LAN it is also at `http://esp32-dispenser.local`.
- In `banyu/settings.env` set `ESP32_BASE_URL` to that IP or mDNS name.

## Safety defaults in this build

- `FLOW_TIMEOUT_ENABLED` is **on**: pump on with no flow for 5 s stops the job.
- Tank LOW (XKC-Y25-V on GPIO13) aborts a running pour and blocks new starts.
- 90 s dispense timeout (paused time excluded).
- Pump off first, valve tail 500 ms, then relay off at target.
- Calibration is per line (`ppl_cold`, `ppl_normal`); see the full README.

## Optional MQTT telemetry

Off by default. The dispenser can publish `/api/status` to a broker
(Config → MQTT); `banyu/server.js` can subscribe read-only (see
`banyu/README.md` → "MQTT telemetry"). Commands remain on the local HTTP API.

## Checksums

See `SHA256SUMS.txt`.

Full source and documentation: `firmware/esp32-dispenser/` in the repository.
