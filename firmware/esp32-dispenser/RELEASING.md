# Releasing Banyu ESP32 firmware

The firmware lives in `firmware/esp32-dispenser/`. Releases are GitHub releases
with two binary assets plus the flashing notes and checksums.

## Versioning

- Tag: `firmware-vMAJOR.MINOR.PATCH` (example: `firmware-v3.0.1`).
- Release title: `Banyu ESP32 firmware vX.Y.Z (HydroNode)`.
- The firmware also reports an internal `HydroNode` version in `src/main.cpp`
  (`"name":"HydroNode","version":3` in the `/api` info). Update it when that
  API/version changes; it does not have to match the tag.
- Tag the **final** commit. Docs added after the tag are not in the release
  source archive.

## 1. Build

```bash
pio run -d firmware/esp32-dispenser
```

Production settings to check before building:

- `FLOW_TIMEOUT_ENABLED 1` in `include/config.h` (5 s no-flow stop).
- The pin map is the two-line board: relays 16/17, pumps 26/25, flow 18/4,
  tank 13.
- Test on the real board: dry run first, then a measured pour.

Build output:

```
firmware/esp32-dispenser/.pio/build/esp32dev/firmware.bin   # OTA / application image
```

## 2. Make the first-flash image

The OTA image is application-only. The first USB flash also needs the
bootloader, partition table and `boot_app0`. Merge them into one 4 MB image:

```bash
BUILD=firmware/esp32-dispenser/.pio/build/esp32dev
BOOT_APP0=$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin

python "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --chip esp32 merge_bin \
  -o "$BUILD/firmware_merged.bin" \
  --flash_mode dio --flash_freq 40m --flash_size 4MB \
  0x1000 "$BUILD/bootloader.bin" \
  0x8000 "$BUILD/partitions.bin" \
  0xe000 "$BOOT_APP0" \
  0x10000 "$BUILD/firmware.bin"
```

On Windows use `%USERPROFILE%` instead of `$HOME`. The output should say
`ready to flash to offset 0x0`.

## 3. Package the assets

Use a clean folder, not the repository (binaries are attached to the release,
not committed):

```bash
BUILD=firmware/esp32-dispenser/.pio/build/esp32dev
V=3.0.1
OUT=release/firmware-v$V
mkdir -p "$OUT"

cp "$BUILD/firmware.bin"        "$OUT/banyu-esp32-v$V.bin"
cp "$BUILD/firmware_merged.bin" "$OUT/banyu-esp32-v$V-merged.bin"
cp firmware/esp32-dispenser/FLASHING.md "$OUT/FLASHING.md"
```

Update the version strings inside `FLASHING.md`, then write
`$OUT/RELEASE_NOTES.md` (use the previous release as a template: version,
changes, wiring warning, OTA and first-flash steps).

Checksums:

```bash
cd "$OUT"
sha256sum *.bin > SHA256SUMS.txt
```

Windows PowerShell:

```powershell
Get-ChildItem *.bin | ForEach-Object {
  $h = (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower()
  "$h  $($_.Name)"
} | Out-File SHA256SUMS.txt -Encoding ascii
```

## 4. Tag and push

```bash
git tag -a firmware-v3.0.1 -m "Banyu ESP32 firmware v3.0.1"
git push origin firmware/esp32-dispenser
git push origin firmware-v3.0.1
```

## 5. Create the GitHub release

With GitHub CLI:

```bash
gh release create firmware-v3.0.1 \
  --title "Banyu ESP32 firmware v3.0.1 (HydroNode)" \
  --notes-file release/firmware-v3.0.1/RELEASE_NOTES.md \
  release/firmware-v3.0.1/banyu-esp32-v3.0.1.bin \
  release/firmware-v3.0.1/banyu-esp32-v3.0.1-merged.bin \
  release/firmware-v3.0.1/FLASHING.md \
  release/firmware-v3.0.1/SHA256SUMS.txt
```

Without `gh`: GitHub -> Releases -> **Draft a new release** -> choose the tag ->
use the release notes -> upload the four files -> **Publish release**.
The release must be public and the latest.

## Checklist

- [ ] `pio run -d firmware/esp32-dispenser` succeeds
- [ ] `FLOW_TIMEOUT_ENABLED` and the pin map match the hardware
- [ ] tested on the real two-line board (dry run + measured pour)
- [ ] tag, release title and asset names use the same version
- [ ] OTA image (`banyu-esp32-vX.Y.Z.bin`) and merged image (`...-merged.bin`) present
- [ ] `FLASHING.md` and `SHA256SUMS.txt` uploaded
- [ ] downloaded the assets back and verified the checksums
- [ ] release is public, not a draft, and marked Latest
- [ ] release notes warn: two-line board only; OTA password `12345678`; do not
      upload the merged image through the web UI

## Notes

- OTA from the dispenser web UI: Firmware card -> `banyu-esp32-vX.Y.Z.bin` ->
  password -> Upload & Reboot. An active pour is stopped and relays are off.
- First USB flash: `esptool.py --chip esp32 --port COM6 write_flash 0x0 banyu-esp32-vX.Y.Z-merged.bin`.
- Do not commit `.pio/` or the release binaries; attach them to the release.
- `firmware/esp32-dispenser/FLASHING.md` is the source for the asset. Bump its
  version strings for every release.
