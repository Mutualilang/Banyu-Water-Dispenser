# Banyu local kiosk + dashboard

No hosting, no domain, no npm install. You only need Node 18+ (https://nodejs.org) on one always-on device
that shares the Wi-Fi with the tablet (an old laptop or mini PC works).

## Run
    node server.js

- Kiosk (open on the tablet):  http://<that-device's-IP>:3000
- Dashboard (any phone/PC on the same Wi-Fi):  http://<that-device's-IP>:3000/dashboard.html
- "Add sample data" on the dashboard fills it with fake orders. "Clear sample data" removes only those.
- Data is saved in data/db.json. Back this file up. "Download CSV" exports everything for Excel.
- Give that device a fixed IP in your router so the tablet's bookmark never breaks.

## What happens now (all dummy)
The kiosk still simulates payment and pouring itself, but reports every step to the server:
awaiting_payment -> paid -> dispensing -> completed / stopped (with ml poured), plus expired / cancelled.
The water-shortage button posts to /api/alerts. "Paid, no water" = paid but never started (refund candidate).

## Xendit (QRIS) - test mode
1. In the Xendit dashboard switch to TEST mode, Settings > API Keys, create a SECRET key with Money-In = WRITE.
   It starts with xnd_development_  (the "public" key xnd_public_... is not used here).
2. Open settings.env (same folder as server.js) in Notepad, paste the key after XENDIT_SECRET_KEY= , save.
   (No quotes, no spaces. Never share this file.)
3. Stop the server (Ctrl+C) and run it again: node server.js
   The first lines it prints MUST say "PAYMENT: Xendit TEST ...". If they say "DEMO MODE", the key was not found and
   the kiosk will show the demo payment page; the message tells you why. The dashboard also shows a yellow banner.
4. On the payment screen, "(Demo) Tandai pembayaran berhasil" asks Xendit to simulate a customer paying.
   With a live key (xnd_production_...) that button disappears and only real payments count.

Payment flow (PAYMENT_FLOW, default "invoice"): after choosing volume + temperature the tablet is sent to XENDIT'S OWN
payment page, pays there (in test mode Xendit lets you simulate the payment), and is sent back to the kiosk, which
resumes at the "Pengisian" screen. Set PAYMENT_FLOW=qr to show a QR on the kiosk instead.
XENDIT_PAYMENT_METHODS (settings.env) defaults to QRIS only. Use ALL to offer everything enabled on your account.
The return address is this server's own address as the tablet opened it (e.g. http://192.168.1.20:3000).
If Xendit refuses that address, the server retries without redirects; the payment still works and is recorded,
but the tablet then stays on Xendit's page and must be returned to the kiosk by hand.

(QR flow) the server creates the QR with our order number + metadata, then asks Xendit every 2s if it was paid.
No public URL or callback needed. A payment that arrives after the QR expired is flagged "Paid, no water" (refund candidate).
Only tested against a stand-in that follows Xendit's docs, not the real Xendit yet: check the first real test run.
If the console prints "[xendit create] ..." the message after it says what Xendit objected to.

## Dispenser (ESP32, from API.md)
- Set ESP32_BASE_URL in settings.env (e.g. http://192.168.31.171). Empty = demo: the tablet fakes the pour.
- The server startup prints "DISPENSER: ESP32 at ..." and later "[dispenser] ONLINE / OFFLINE". This PC must be on the dispenser's Wi-Fi.
- Only the SERVER talks to the dispenser. Flow: paid -> customer presses Start -> server calls /api/dispense with the order's
  volume + temperature -> server reads /api/status (1s during a pour, 5s otherwise) -> final ml is saved when the job ends.
  The device closes the valve by itself at the target. If it refuses (tank low, busy) the order stays "Paid, no water" -> refund.
- Stop on the kiosk = pause; "Ya" resumes; "Tidak"/timeout stops. Orders store ordered vs actual ml, seconds taken, and the % difference;
  the dashboard flags completed pours that are off by more than 5%.
- Alerts: tank LOW for 20s -> one "Water is running low" alert (auto-clears after 20s of NORMAL); no reply for 30s -> "Dispenser not responding".
  The kiosk's "Air habis" button still works. Kiosk refuses new orders while the tank is LOW, the dispenser is offline or busy.
- Kiosk: after 90s untouched on the temperature screen it returns to the start; after 10 minutes idle at the start it reloads.

## Test it with no hardware
    node fake-dispenser.js          (pretend dispenser on http://localhost:8080)
and put ESP32_BASE_URL=http://localhost:8080 in settings.env. Trigger the low-water alert with
http://localhost:8080/api/sim?tank=low (and ?tank=ok to clear). FLOW_ML_S=15 sets its speed.
The stand-in follows the written API, not the real firmware: re-test everything on the real device.

## Questions for the workshop
1. What exactly drives tank_low (which sensor, where mounted, does it flicker while the chamber refills)?
2. Does pumping into the top chamber ever make the dispenser report busy or low in between orders?
3. Real pour time for 300/600/900 ml with the new valves (90s device timeout). Does /api/sim feed the pour counter?
4. Does job_ml stay at the final value after a job? Check calibration (ppl) with a measuring cup.
5. Change the AP password (default 12345678); set a fixed IP for the dispenser in the router; turn on the no-flow safety.
6. Is the doc up to date with the firmware (the first readout showed "tank_sensor": false, the doc does not list that field)?

## Admin login (placeholder)
The dashboard (and every admin-only API: orders list, CSV, stats, alerts, cold controls, sample data) needs a login.
**For now it only accepts the keywords listed in `ADMIN_KEYWORDS` in settings.env** (comma separated, default `banyu-admin`).
There are no user accounts and no database behind it yet: anyone who knows a keyword gets in. Change the default, keep it private, restart after editing.
Login lasts 12 hours (kept in memory, so a server restart logs everyone out), 5 wrong tries = 1 minute lockout.
The kiosk tablet does NOT log in; it can only create/pay/pour its own orders. Serving this over the internet later needs HTTPS (then add `Secure` to the cookie in server.js)
and real admin accounts in a database (Postgres/SQLite fits the member system better than MongoDB) instead of keywords.

## Cold water estimator
The cold tank holds ~2 L and refills with warm water after every cold sale. The server keeps a "cold level" estimate: each cold sale subtracts its ml,
and the level recovers at `COLD_RECOVER_ML_MIN` (default 22 ml/min). It is recomputed from the order timestamps, so restarts lose nothing. Sample orders are ignored.
- Cold (level >= 1500): normal. Cool (1000 to 1500): still sold, colder than room temperature. Below 1000: cold is blocked (kiosk button greyed with a "back in ~N min" note, and the server refuses cold orders) until the level climbs back to 1250.
- The dashboard "Cold water" panel shows the level, state, when the streak started, minutes until cold is back, and the last cold sales.
- Admin controls: Automatic / Force on / Force off, and "Machine just switched on" (level 0) / "Tank is cold and full" (level 2000) so the estimate can be corrected.
- All numbers are first guesses (settings.env, COLD_*). Calibrate with a thermometer at the cold outlet.
- Only cold sales that have started pouring count; a cold order that is paid but not started yet is not subtracted.

## Not built yet
Receipts/refund tooling, live-key hardening, hosting, real admin accounts and the member system.
