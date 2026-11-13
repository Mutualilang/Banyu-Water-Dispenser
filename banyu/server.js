// Banyu local server: zero dependencies, needs only Node 18+.  Run: node server.js
const http = require('http'), fs = require('fs'), path = require('path');
// Settings file: put your Xendit secret key in settings.env (same folder as this file), then restart.
for (const f of ['settings.env', '.env']) {
  try { for (const l of fs.readFileSync(path.join(__dirname, f), 'utf8').split(/\r?\n/)) {
    const m = l.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*?)\s*$/);
    if (m && !l.trim().startsWith('#') && m[2] && !(m[1] in process.env)) process.env[m[1]] = m[2].replace(/^["']|["']$/g, '');
  } } catch (e) { /* file is optional */ }
}

// ===== SETTINGS (placeholders: fill in when you have the real values) =====
const CONFIG = {
  PORT: process.env.PORT || 3000,
  PAYMENT_MODE: process.env.PAYMENT_MODE || '',   // '' = automatic: 'xendit' when a real key is set, otherwise 'mock'
  XENDIT_SECRET_KEY: process.env.XENDIT_SECRET_KEY || '<XENDIT_SECRET_KEY_HERE>',
  PAYMENT_FLOW: process.env.PAYMENT_FLOW || 'invoice',   // 'invoice' = send customer to Xendit's own payment page; 'qr' = show the QR on the kiosk
  XENDIT_PAYMENT_METHODS: process.env.XENDIT_PAYMENT_METHODS || 'QRIS',   // what Xendit's page offers. Default QRIS only. 'ALL' = everything enabled on your account
  XENDIT_BASE: process.env.XENDIT_BASE || 'https://api.xendit.co',   // we ask Xendit about payments, so no public callback URL is needed
  DISPENSER_MODE: process.env.DISPENSER_MODE || '',   // '' = automatic: 'esp32' when ESP32_BASE_URL is set, otherwise the tablet fakes the pour
  ESP32_BASE_URL: (process.env.ESP32_BASE_URL || '').replace(/\/+$/, ''),   // e.g. http://192.168.31.171 (set it in settings.env)
};
// ===== ADMIN LOGIN (placeholder) =====
// NOTE: for now the dashboard accepts ONLY the keywords listed in ADMIN_KEYWORDS (settings.env, comma separated).
// There are no user accounts and no database behind it yet. Anyone who knows one of the keywords gets in.
// Later this is meant to be replaced by real admin accounts in a database (see README, "Admin login").
const crypto = require('crypto');
const KEYWORDS_SET = !!(process.env.ADMIN_KEYWORDS || 'riccibau').trim();
const ADMIN_KEYWORDS = (process.env.ADMIN_KEYWORDS || 'banyuadmin').split(',').map(k => k.trim()).filter(Boolean);
const sha = x => crypto.createHash('sha256').update(String(x)).digest();
const KEYWORD_HASHES = ADMIN_KEYWORDS.map(sha);
const keywordOk = k => KEYWORD_HASHES.map(h => crypto.timingSafeEqual(h, sha(k))).some(Boolean);   // constant-time compare
const SESSION_MS = 12 * 3600 * 1000;
const SECRET = process.env.SESSION_SECRET || sha('banyu-session|' + ADMIN_KEYWORDS.join('|')).toString('hex');
const sign = exp => exp + '.' + crypto.createHmac('sha256', SECRET).update(String(exp)).digest('hex');
const failed = new Map();                                     // ip -> { n, until }: slows down keyword guessing
function cookies(req) { const o = {}; (req.headers.cookie || '').split(';').forEach(c => { const i = c.indexOf('='); if (i > 0) o[c.slice(0, i).trim()] = c.slice(i + 1).trim(); }); return o; }
function isAdmin(req) {
  const t = cookies(req).banyu_session || '', i = t.indexOf('.');
  if (i < 1 || !(+t.slice(0, i) > Date.now())) return false;
  const good = sign(+t.slice(0, i));
  return t.length === good.length && crypto.timingSafeEqual(Buffer.from(t), Buffer.from(good));
}

// ===== XENDIT (QRIS). Test keys start with xnd_development_, live keys with xnd_production_ =====
const XKEY = CONFIG.XENDIT_SECRET_KEY, HAS_KEY = /^xnd_(development|production)_/.test(XKEY);
const KEY_PROBLEM = HAS_KEY ? '' : XKEY.startsWith('xnd_public') ? 'that is the PUBLIC key; the SECRET key (xnd_development_...) is needed'
  : (!XKEY || XKEY.startsWith('<')) ? 'no secret key found in settings.env' : 'the key does not look like a Xendit secret key';
if (!CONFIG.PAYMENT_MODE) CONFIG.PAYMENT_MODE = HAS_KEY ? 'xendit' : 'mock';
const XTEST = !HAS_KEY || XKEY.startsWith('xnd_development');
const lastLog = {};
function logThrottled(m) { const t = Date.now(); if (!lastLog[m] || t - lastLog[m] > 30000) { lastLog[m] = t; console.error(m); } }   // same message at most every 30s
const NET_HINT = { ENOTFOUND: 'cannot look up api.xendit.co: no internet or DNS problem on this computer', ETIMEDOUT: 'timed out: no internet, or a firewall/VPN is blocking it',
  ECONNREFUSED: 'connection refused', ECONNRESET: 'connection dropped (weak Wi-Fi, VPN or antivirus?)', EAI_AGAIN: 'DNS problem: no internet on this computer',
  CERT_HAS_EXPIRED: 'certificate error: check this computer\'s date and time', UNABLE_TO_VERIFY_LEAF_SIGNATURE: 'certificate error: antivirus or network is intercepting HTTPS' };
async function xendit(method, p, body) {
  try { return await xenditCall(method, p, body); }
  catch (e) { if (e.name === 'TimeoutError') throw new Error('cannot reach Xendit (timeout): ' + NET_HINT.ETIMEDOUT);
    const c = e.cause && (e.cause.code || e.cause.message); if (c) throw new Error(`cannot reach Xendit (${c}): ${NET_HINT[c] || 'check this computer\'s internet connection'}`); throw e; }
}
async function xenditCall(method, p, body) {
  const r = await fetch(CONFIG.XENDIT_BASE + p, { signal: AbortSignal.timeout(10000), method, body: body ? JSON.stringify(body) : undefined,
    headers: { Authorization: 'Basic ' + Buffer.from(XKEY + ':').toString('base64'), 'Content-Type': 'application/json', ...(p.startsWith('/qr_codes') ? { 'api-version': '2022-07-31' } : {}) } });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(`Xendit ${r.status} ${j.error_code || ''} ${j.message || ''}`.trim());
  return j;
}
// Every 2s: ask Xendit whether recent orders were paid. Also catches payments that land just AFTER the kiosk gave up.
setInterval(async () => {
  if (CONFIG.PAYMENT_MODE !== 'xendit') return;
  for (const o of db.orders) {
    const inv = !!o.xendit_invoice_id, age = Date.now() - new Date(o.created_at);
    if ((!inv && !o.xendit_qr_id) || o.xendit_payment_id || age > (inv ? 900000 : 300000)) continue;
    if (o.status === 'awaiting_payment' && age > (inv ? 310000 : 90000)) { setStatus(o, 'expired'); save(); }
    if (!['awaiting_payment', 'expired', 'cancelled'].includes(o.status)) continue;
    try {
      let paidId = null;
      if (inv) { const r = await xendit('GET', `/v2/invoices/${o.xendit_invoice_id}`); if (['PAID', 'SETTLED'].includes(r.status)) paidId = r.id; }
      else { const r = await xendit('GET', `/qr_codes/${o.xendit_qr_id}/payments`), list = Array.isArray(r) ? r : (r.data || []);
             const ok = list.find(x => ['SUCCEEDED', 'COMPLETED'].includes(x.status)); if (ok) paidId = ok.id; }
      if (!paidId) continue;
      o.xendit_payment_id = paidId;
      if (o.status === 'awaiting_payment') setStatus(o, 'paid');
      else { o.status = 'paid'; o.late_payment = true; o.paid_at = now(); o.ended_at = null; }   // paid after we gave up: refund candidate
      save();
    } catch (e) { logThrottled('[xendit poll] ' + e.message); }
  }
}, 2000);

if (!CONFIG.DISPENSER_MODE) CONFIG.DISPENSER_MODE = CONFIG.ESP32_BASE_URL ? 'esp32' : 'mock';
const PRICES = { 300: 2000, 600: 2500, 900: 5000 };   // server decides prices, not the tablet
const TERMINAL = ['completed', 'stopped', 'expired', 'cancelled'];
const STATUSES = ['awaiting_payment', 'paid', 'dispensing', ...TERMINAL];
const DB_FILE = path.join(__dirname, 'data', 'db.json');
fs.mkdirSync(path.join(__dirname, 'data'), { recursive: true });
let db = fs.existsSync(DB_FILE) ? JSON.parse(fs.readFileSync(DB_FILE, 'utf8')) : { seq: 0, orders: [], alerts: [] };
const save = () => fs.writeFileSync(DB_FILE, JSON.stringify(db));
const now = () => new Date().toISOString();
const pad = (n, l) => String(n).padStart(l, '0');
const ymd = d => `${d.getFullYear()}${pad(d.getMonth() + 1, 2)}${pad(d.getDate(), 2)}`;

function newOrder(kioskId, volume, temp, when, fake) {
  db.seq++;
  const d = when || new Date();
  return { id: `BNY-${ymd(d)}-${pad(db.seq, 4)}`, kioskId, volume, temp, channel: temp === 'dingin' ? 'cold' : 'normal',
    price: PRICES[volume], status: 'awaiting_payment', dispensed_ml: 0, created_at: d.toISOString(),
    paid_at: null, started_at: null, ended_at: null, fake: !!fake };
}
function setStatus(o, status, ml) {
  if (TERMINAL.includes(o.status)) return false;           // finished orders never change
  o.status = status; const t = now();
  if (status === 'paid') o.paid_at = t;
  if (status === 'dispensing' && !o.started_at) o.started_at = t;
  if (TERMINAL.includes(status)) { o.ended_at = t; if (ml != null) o.dispensed_ml = Math.min(ml, o.volume); }
  return true;
}
const withFlags = o => {
  const age = (Date.now() - new Date(o.paid_at || o.created_at)) / 60000;
  return { ...o, attention: (o.status === 'paid' && age > 2) || (o.status === 'dispensing' && age > 10),
    qc: o.status === 'completed' && o.variance_pct != null && Math.abs(o.variance_pct) > 5 };   // poured amount off by more than 5%
};
const paidStatuses = ['paid', 'dispensing', 'completed', 'stopped'];

// ===== COLD WATER ESTIMATOR =====
// The cold tank holds ~2 L. Every cold sale pulls water out and the tank refills with warm water, which the chiller then has
// to cool again. We model it as a "cold level": each cold sale subtracts its ml, and the level climbs back at a steady rate.
// Nothing is stored except the sales themselves: the level is recomputed from the order timestamps, so a restart loses nothing.
// The numbers below are first guesses. Calibrate them with a thermometer at the cold outlet (see README).
db.cold = db.cold || { override: 'auto', baseline: null };   // override: 'auto' | 'on' | 'off'. baseline: { at, level } set by the admin buttons
const numEnv = (k, d) => (process.env[k] !== undefined && process.env[k] !== '' && !isNaN(+process.env[k])) ? +process.env[k] : d;
const COLD = { CAP: numEnv('COLD_CAPACITY_ML', 2000), RATE: numEnv('COLD_RECOVER_ML_MIN', 22),   // 2 L in ~1.5 h = ~22 ml/min
  COLD_ABOVE: numEnv('COLD_ABOVE_ML', 1500), LOCK_BELOW: numEnv('COLD_LOCK_BELOW_ML', 1000), REENABLE_AT: numEnv('COLD_REENABLE_AT_ML', 1250) };
function coldModel(at = Date.now()) {
  const windowMs = (COLD.CAP / COLD.RATE + 30) * 60000, wStart = at - windowMs, b = db.cold.baseline;
  const base = b && +new Date(b.at) > wStart ? { t: +new Date(b.at), level: b.level } : { t: wStart, level: COLD.CAP };   // older than a full recovery = tank is back to full
  const ev = db.orders.filter(o => o.temp === 'dingin' && !o.fake && ['dispensing', 'completed', 'stopped'].includes(o.status))
    .map(o => ({ id: o.id, t: +new Date(o.started_at || o.paid_at || o.created_at), ml: o.status === 'dispensing' ? o.volume : (o.dispensed_ml || 0) }))
    .filter(e => e.ml > 0 && e.t > base.t && e.t <= at).sort((a, b2) => a.t - b2.t);
  let level = base.level, t = base.t, disabled = level < COLD.LOCK_BELOW, since = level < COLD.CAP ? base.t : null;
  const advance = to => { level = Math.min(COLD.CAP, level + (to - t) / 60000 * COLD.RATE); t = to;
    if (disabled && level >= COLD.REENABLE_AT) disabled = false;   // hysteresis: back on at REENABLE_AT, not right at the lock line
    if (level >= COLD.CAP) since = null; };
  for (const e of ev) { advance(e.t); if (since === null) since = e.t; level = Math.max(0, level - e.ml); if (level < COLD.LOCK_BELOW) disabled = true; }
  advance(at);
  const autoState = disabled ? 'disabled' : level >= COLD.COLD_ABOVE ? 'cold' : 'cool';
  const ov = db.cold.override;
  const state = ov === 'off' ? 'disabled' : ov === 'on' ? (autoState === 'disabled' ? 'cool' : autoState) : autoState;
  const mins = target => Math.max(0, Math.ceil((target - level) / COLD.RATE));
  return { level: Math.round(level), capacity: COLD.CAP, state, auto_state: autoState, override: ov || 'auto', enabled: state !== 'disabled',
    thresholds: { cold_above: COLD.COLD_ABOVE, lock_below: COLD.LOCK_BELOW, reenable_at: COLD.REENABLE_AT }, recover_ml_per_min: COLD.RATE,
    back_in_min: autoState === 'disabled' ? mins(COLD.REENABLE_AT) : 0,   // minutes until cold sales resume (auto mode)
    cold_again_in_min: level >= COLD.COLD_ABOVE ? 0 : mins(COLD.COLD_ABOVE), full_in_min: mins(COLD.CAP),
    tracking_since: since ? new Date(since).toISOString() : null,   // first cold sale of the current streak; empty once the tank has fully recovered
    last_set: b ? { at: b.at, level: b.level } : null, recent: ev.slice(-6).reverse().map(e => ({ id: e.id, time: new Date(e.t).toISOString(), ml: Math.round(e.ml) })) };
}
const coldAvailable = () => coldModel().enabled;

function stats() {
  const today = ymd(new Date()), O = db.orders;
  const t = O.filter(o => ymd(new Date(o.created_at)) === today);
  const paid = t.filter(o => paidStatuses.includes(o.status));
  const done = O.filter(o => o.status === 'completed').length, stp = O.filter(o => o.status === 'stopped').length;
  const by = (key, vals) => vals.map(v => ({ key: v, orders: paid.filter(o => String(o[key]) === String(v)).length }));
  return { ordersToday: t.length, paidToday: paid.length, revenueToday: paid.reduce((s, o) => s + o.price, 0),
    mlToday: Math.round(paid.reduce((s, o) => s + o.dispensed_ml, 0)),
    completionRate: done + stp ? Math.round(100 * done / (done + stp)) : null,
    attention: O.filter(o => withFlags(o).attention).length, openAlerts: db.alerts.filter(a => !a.resolved).length,
    bottlesSaved: done, byTemp: by('temp', ['ruang', 'dingin']), byVolume: by('volume', [300, 600, 900]),
    fakeCount: O.filter(o => o.fake).length };
}
function seed(n) {
  const r = Math.random;
  for (let i = 0; i < n; i++) {
    const when = new Date(Date.now() - r() * 7 * 864e5), vol = [300, 600, 900][Math.floor(r() * 3)];
    const o = newOrder('banyu-kiosk-01', vol, r() < 0.4 ? 'dingin' : 'ruang', when, true), x = r();
    if (x < 0.06) { setStatus(o, 'expired'); }
    else { setStatus(o, 'paid');
      if (x < 0.10) { if (when < Date.now() - 36e5) { /* stays paid, no water: needs attention */ } }
      else if (x < 0.20) { setStatus(o, 'dispensing'); setStatus(o, 'stopped', Math.round(vol * (0.1 + r() * 0.7))); }
      else { setStatus(o, 'dispensing'); setStatus(o, 'completed', vol); } }
    o.paid_at = o.paid_at && when.toISOString(); o.ended_at = o.ended_at && new Date(+when + 9e4).toISOString();
    db.orders.push(o);
  }
  if (!db.alerts.some(a => a.fake)) db.alerts.push({ id: 'A-demo', type: 'water_shortage', source: 'manual_button', kioskId: 'banyu-kiosk-01', timestamp: now(), resolved: false, fake: true });
}
const csv = () => ['id,created_at,kiosk,volume_ml,temp,price_idr,status,dispensed_ml,paid_at,ended_at,xendit_payment_id,dispenser_ml,duration_s,variance_pct,sample']
  .concat(db.orders.map(o => [o.id, o.created_at, o.kioskId, o.volume, o.temp, o.price, o.status, o.dispensed_ml, o.paid_at || '', o.ended_at || '', o.xendit_payment_id || '', o.dispenser_ml ?? '', o.duration_s ?? '', o.variance_pct ?? '', o.fake].join(','))).join('\n');

// ===== DISPENSER (ESP32, see API.md). We ask it for status: every 1s during a pour, every 5s otherwise =====
const TANK_LOW_MS = (+process.env.TANK_LOW_SECONDS || 20) * 1000, OFFLINE_MS = (+process.env.OFFLINE_SECONDS || 30) * 1000;
const disp = { online: false, lastOk: Date.now(), status: null, lowSince: 0, okSince: 0 };
async function esp(p) {
  const r = await fetch(CONFIG.ESP32_BASE_URL + p, { signal: AbortSignal.timeout(3000), cache: 'no-store' });
  return { ok: r.ok, text: (await r.text()).trim() };
}
function raiseAlert(type, source) {   // one open alert per kind, never a flood
  if (db.alerts.some(a => a.type === type && a.source === source && !a.resolved)) return;
  db.alerts.push({ id: 'A-' + Date.now(), type, source, kioskId: 'banyu-kiosk-01', timestamp: now(), resolved: false }); save();
}
function clearAlert(type, source) {
  let ch = false; db.alerts.forEach(a => { if (a.type === type && a.source === source && !a.resolved) { a.resolved = true; ch = true; } });
  if (ch) save();
}
function availability() {   // never sell water we cannot pour
  if (CONFIG.DISPENSER_MODE !== 'esp32') return { ok: true };
  if (!disp.status || Date.now() - disp.lastOk > 12000) return { ok: false, reason: 'dispenser offline' };   // tolerates one missed reading
  if (disp.status.tank_low) return { ok: false, reason: 'tank low' };
  if (disp.status.job_active) return { ok: false, reason: 'busy' };
  return { ok: true };
}
function onStatus(st) {
  const t = Date.now();
  if (st.tank_low) { disp.okSince = 0; disp.lowSince = disp.lowSince || t; if (t - disp.lowSince >= TANK_LOW_MS) raiseAlert('water_shortage', 'esp32_sensor'); }
  else { disp.lowSince = 0; disp.okSince = disp.okSince || t; if (t - disp.okSince >= TANK_LOW_MS) clearAlert('water_shortage', 'esp32_sensor'); }
  const o = db.orders.find(x => x.status === 'dispensing');
  if (!o) return;
  if (st.job_active) {
    o.dispensed_ml = Math.min(st.job_ml, o.volume); o.last_active_ml = st.job_ml; o.paused = st.job_paused; save();
  } else if (t - (o.dispense_started_ms || 0) > 1500) {   // job over: job_ml is NOT frozen by the device, so record it now
    const ml = Math.max(o.last_active_ml || 0, st.job_ml || 0), err = st.error || '';
    const ok = ml >= o.volume * 0.95 && !/stopped|timeout|tank|no flow/i.test(err);
    o.dispenser_ml = ml; o.dispenser_error = err; o.duration_s = Math.round((t - o.dispense_started_ms) / 1000);
    o.variance_pct = Math.round((ml - o.volume) / o.volume * 1000) / 10; o.paused = false;
    setStatus(o, ok ? 'completed' : 'stopped', ml); save();
  }
}
const ESP_HINT = { TimeoutError: 'no reply: the ESP is probably not on this network, or its IP changed', UND_ERR_CONNECT_TIMEOUT: 'no reply: the ESP is probably not on this network, or its IP changed',
  ETIMEDOUT: 'no reply: the ESP is probably not on this network, or its IP changed', EHOSTUNREACH: 'no route to the ESP: it is not on this network (check its Wi-Fi), or the IP is wrong',
  ENETUNREACH: 'this computer is not on the ESP\'s network', ECONNREFUSED: 'the address answers but nothing listens there: wrong IP?', ECONNRESET: 'connection dropped mid-way: weak signal or the ESP restarted',
  SyntaxError: 'got an answer that is not the dispenser status: wrong address?' };
function connEvent(type) {   // keep a diary of every drop, with what the ESP last reported, to find the cause
  const st = disp.status || {}, t = Date.now();
  const e = { time: now(), event: type, rssi: st.rssi, wifi: st.wifi_status_name, job_state: st.job_state, pump_on: !!(st.pump_cold || st.pump_normal), flow_lpm: st.flow_lpm };
  if (type === 'offline') disp.downSince = disp.lastOk;
  if (type === 'online' && disp.downSince) { e.down_s = Math.round((t - disp.downSince) / 1000); disp.downSince = 0; }
  db.dispenserLog = (db.dispenserLog || []).concat(e).slice(-200); save();
  console.log('[dispenser]', type.toUpperCase(), type === 'offline' ? `(last reading: signal ${e.rssi} dBm, ${e.job_state}${e.pump_on ? ', PUMP ON' : ''})` : e.down_s != null ? `(was down ${e.down_s}s)` : '');
}
let polling = false;
async function pollDispenser() {
  if (CONFIG.DISPENSER_MODE !== 'esp32' || polling) return;   // never two requests at once: the ESP32 serves them one by one
  polling = true;
  try {
    const st = JSON.parse((await esp('/api/status')).text);
    disp.status = st; disp.lastOk = Date.now(); disp.fails = 0; disp.lastErr = null;
    if (!disp.online) { disp.online = true; connEvent('online'); }
    clearAlert('dispenser_offline', 'server'); onStatus(st);
  } catch (e) {
    const c = (e.cause && e.cause.code) || e.name; disp.fails = (disp.fails || 0) + 1; disp.lastErr = c;
    logThrottled(`[dispenser] cannot reach ${CONFIG.ESP32_BASE_URL} (${c}): ${ESP_HINT[c] || 'unknown network problem'}`);
    if (disp.online && disp.fails >= 2) { disp.online = false; connEvent('offline'); }   // one missed reply is tolerated
    if (Date.now() - disp.lastOk > OFFLINE_MS) raiseAlert('dispenser_offline', 'server');
  } finally { polling = false; }
}
let loopTimer;
const schedule = () => { clearTimeout(loopTimer); loopTimer = setTimeout(kick, db.orders.some(o => o.status === 'dispensing') ? 1000 : 5000); };
async function kick() { clearTimeout(loopTimer); await pollDispenser(); schedule(); }   // also called right after every command, so progress shows at once
kick();

const send = (res, code, body, type = 'application/json') => { res.writeHead(code, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(type === 'application/json' ? JSON.stringify(body) : body); };
const readBody = req => new Promise(r => { let b = ''; req.on('data', c => b += c); req.on('end', () => { try { r(JSON.parse(b || '{}')); } catch { r({}); } }); });
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8', '.png': 'image/png' };

http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x'), p = url.pathname, m = req.method;
  let g;
  // --- who may call what: the kiosk (tablet) stays open, everything admin-only needs the dashboard login ---
  const kioskOpen = (m === 'GET' && (p === '/api/config' || p === '/api/cold/public' || /^\/api\/orders\/[\w-]+$/.test(p)))
    || (m === 'POST' && (p === '/api/orders' || p === '/api/alerts' || /^\/api\/orders\/[\w-]+\/(status|start|pause|resume|stop|simulate-pay)$/.test(p)));
  if (p === '/api/login' && m === 'POST') {
    const ip = req.socket.remoteAddress, f = failed.get(ip) || { n: 0, until: 0 };
    if (f.until > Date.now()) return send(res, 429, { error: 'Too many wrong tries. Wait a minute.' });
    const b = await readBody(req);
    if (typeof b.keyword === 'string' && keywordOk(b.keyword.trim())) {
      failed.delete(ip); const t = sign(Date.now() + SESSION_MS);
      res.setHeader('Set-Cookie', `banyu_session=${t}; HttpOnly; SameSite=Strict; Path=/; Max-Age=${SESSION_MS / 1000}` + (req.headers['x-forwarded-proto'] === 'https' ? '; Secure' : ''));
      return send(res, 200, { ok: true });
    }
    f.n++; if (f.n >= 5) { f.until = Date.now() + 60000; f.n = 0; } failed.set(ip, f);
    return send(res, 401, { error: 'That keyword is not accepted.' });
  }
  if (p === '/api/logout' && m === 'POST') { res.setHeader('Set-Cookie', 'banyu_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0'); return send(res, 200, { ok: true }); }
  if (p === '/api/me' && m === 'GET') return send(res, isAdmin(req) ? 200 : 401, { admin: isAdmin(req) });
  if (p.startsWith('/api/') && !kioskOpen && !isAdmin(req)) return send(res, 401, { error: 'login required' });
  if (p === '/dashboard.html' && !isAdmin(req)) { res.writeHead(302, { Location: '/login.html' }); return res.end(); }
  if (p.startsWith('/api/')) {
    if (m === 'GET' && p === '/api/config') return send(res, 200, { payment: CONFIG.PAYMENT_MODE, dispenser: CONFIG.DISPENSER_MODE, testMode: XTEST, problem: KEY_PROBLEM, flow: CONFIG.PAYMENT_MODE === 'xendit' ? CONFIG.PAYMENT_FLOW : 'mock' });
    if (m === 'GET' && p === '/api/cold/public') { const c = coldModel(); return send(res, 200, { state: c.state, enabled: c.enabled, back_in_min: c.enabled ? 0 : Math.max(c.back_in_min, 1) }); }   // the tablet only needs this much
    if (m === 'GET' && p === '/api/cold') return send(res, 200, coldModel());
    if (m === 'POST' && p === '/api/cold/override') {   // admin: 'auto' = follow the estimator, 'on' = never block cold, 'off' = block cold
      const b = await readBody(req); if (!['auto', 'on', 'off'].includes(b.mode)) return send(res, 400, { error: 'mode must be auto, on or off' });
      db.cold.override = b.mode; save(); return send(res, 200, coldModel());
    }
    if (m === 'POST' && p === '/api/cold/reset') {   // admin: tell the estimator what the tank is like right now (machine just switched on = 0, known cold and full = capacity)
      const b = await readBody(req), lv = Number(b.level_ml); if (!isFinite(lv) || lv < 0 || lv > COLD.CAP) return send(res, 400, { error: 'level_ml must be 0 to ' + COLD.CAP });
      db.cold.baseline = { at: now(), level: lv }; save(); return send(res, 200, coldModel());
    }
    if (m === 'GET' && p === '/api/stats') return send(res, 200, stats());
    if (m === 'GET' && p === '/api/orders.csv') return send(res, 200, csv(), 'text/csv; charset=utf-8');
    if (m === 'GET' && p === '/api/orders') {
      const s = url.searchParams.get('status'); let list = db.orders.map(withFlags);
      if (s === 'attention') list = list.filter(o => o.attention); else if (s) list = list.filter(o => o.status === s);
      return send(res, 200, list.reverse().slice(0, 200));
    }
    if (m === 'POST' && p === '/api/orders') {
      const b = await readBody(req);
      if (!PRICES[b.volume] || !['ruang', 'dingin'].includes(b.temp)) return send(res, 400, { error: 'bad volume or temp' });
      const av = availability(); if (!av.ok) return send(res, 503, { error: 'unavailable', reason: av.reason });
      if (b.temp === 'dingin' && !coldAvailable()) return send(res, 503, { error: 'unavailable', reason: 'cold unavailable' });   // cold tank is running low
      const o = newOrder(b.kioskId || 'banyu-kiosk-01', b.volume, b.temp); db.orders.push(o);
      if (CONFIG.PAYMENT_MODE === 'xendit') {
        try {
          const meta = { kiosk_id: o.kioskId, volume_ml: o.volume, temp: o.temp, channel: o.channel };
          if (CONFIG.PAYMENT_FLOW === 'invoice') {   // Xendit's hosted payment page; it sends the customer back to this kiosk afterwards
            const origin = 'http://' + (req.headers.host || 'localhost:' + CONFIG.PORT);
            const body = { external_id: o.id, amount: o.price, currency: 'IDR', invoice_duration: 300, metadata: meta,
              description: `Banyu ${o.volume} ml, ${o.temp === 'dingin' ? 'cold' : 'room temperature'}`,
              success_redirect_url: `${origin}/?order=${o.id}`, failure_redirect_url: `${origin}/?order=${o.id}&failed=1` };
            if (CONFIG.XENDIT_PAYMENT_METHODS && CONFIG.XENDIT_PAYMENT_METHODS !== 'ALL') body.payment_methods = CONFIG.XENDIT_PAYMENT_METHODS.split(',');
            let inv;
            try { inv = await xendit('POST', '/v2/invoices', body); }
            catch (e) { if (e.message.startsWith('cannot reach')) throw e;   // a network problem is not about redirect URLs
              console.warn('[xendit] invoice rejected with redirect URLs (' + e.message + '), retrying without them');
              delete body.success_redirect_url; delete body.failure_redirect_url; inv = await xendit('POST', '/v2/invoices', body); }
            o.xendit_invoice_id = inv.id; o.invoice_url = inv.invoice_url;
          } else {
            const q = await xendit('POST', '/qr_codes', { reference_id: o.id, type: 'DYNAMIC', currency: 'IDR', amount: o.price,
              expires_at: new Date(Date.now() + 90000).toISOString(), metadata: meta });
            o.xendit_qr_id = q.id; o.qr_string = q.qr_string;
          }
        } catch (e) { console.error('[xendit create]', e.message); setStatus(o, 'cancelled'); o.error = e.message; save(); return send(res, 502, { error: 'payment unavailable' }); }
      }
      save(); return send(res, 201, o);
    }
    if (m === 'POST' && (g = p.match(/^\/api\/orders\/([\w-]+)\/status$/))) {
      const o = db.orders.find(x => x.id === g[1]), b = await readBody(req);
      if (!o) return send(res, 404, { error: 'no such order' });
      if (!STATUSES.includes(b.status)) return send(res, 400, { error: 'bad status' });
      if (CONFIG.PAYMENT_MODE === 'xendit' && b.status === 'paid') return send(res, 403, { error: 'only Xendit can mark an order paid' });
      if (CONFIG.DISPENSER_MODE === 'esp32' && ['dispensing', 'completed', 'stopped'].includes(b.status)) return send(res, 403, { error: 'only the dispenser decides that' });
      const ok = setStatus(o, b.status, b.dispensed_ml); save(); return send(res, ok ? 200 : 409, o);
    }
    if (m === 'GET' && (g = p.match(/^\/api\/orders\/([\w-]+)$/))) {
      const o = db.orders.find(x => x.id === g[1]); return o ? send(res, 200, o) : send(res, 404, { error: 'no such order' });
    }
    if (m === 'GET' && p === '/api/dispenser') {
      const st = disp.status || {};
      return send(res, 200, CONFIG.DISPENSER_MODE !== 'esp32' ? { mode: 'mock' } :
        { mode: 'esp32', online: disp.online, tank_low: !!st.tank_low, tank_state: st.tank_state, job_state: st.job_state, flow_lpm: st.flow_lpm, rssi: st.rssi,
          wifi: st.wifi_status_name, last_seen_s: Math.round((Date.now() - disp.lastOk) / 1000), last_error: disp.lastErr || null,
          drops_24h: (db.dispenserLog || []).filter(e => e.event === 'offline' && Date.now() - new Date(e.time) < 864e5).length,
          log: (db.dispenserLog || []).slice(-10).reverse() });
    }
    if (m === 'POST' && (g = p.match(/^\/api\/orders\/([\w-]+)\/(start|pause|resume|stop)$/))) {   // only the server talks to the dispenser
      const o = db.orders.find(x => x.id === g[1]), act = g[2];
      if (!o) return send(res, 404, { error: 'no such order' });
      if (CONFIG.DISPENSER_MODE !== 'esp32') return send(res, 409, { error: 'dispenser is in demo mode' });
      if (act === 'start' ? o.status !== 'paid' : o.status !== 'dispensing') return send(res, 409, { error: 'wrong state: ' + o.status });
      try {
        const r = await esp(act === 'start' ? `/api/dispense?channel=${o.channel}&ml=${o.volume}` : `/api/${act}`);
        if (!r.ok) { o.dispense_error = r.text; save(); return send(res, 409, { error: r.text }); }   // paid order stays "paid, no water" -> refund
        if (act === 'start') { setStatus(o, 'dispensing'); o.dispense_started_ms = Date.now(); o.last_active_ml = 0; o.dispense_error = null; }
        save(); setTimeout(kick, 300); return send(res, 200, o);
      } catch (e) { o.dispense_error = 'dispenser unreachable'; save(); return send(res, 502, { error: 'dispenser unreachable' }); }
    }
    if (m === 'POST' && (g = p.match(/^\/api\/orders\/([\w-]+)\/simulate-pay$/))) {   // test keys only: Xendit pretends a customer paid
      const o = db.orders.find(x => x.id === g[1]);
      if (!o || !o.xendit_qr_id || CONFIG.PAYMENT_MODE !== 'xendit' || !XTEST) return send(res, 403, { error: 'not available' });
      try { await xendit('POST', `/qr_codes/${o.xendit_qr_id}/payments/simulate`, {}); return send(res, 200, { ok: true }); }
      catch (e) { console.error('[xendit simulate]', e.message); return send(res, 502, { error: e.message }); }
    }
    if (m === 'GET' && p === '/api/alerts') return send(res, 200, db.alerts.slice().reverse().slice(0, 50));
    if (m === 'POST' && p === '/api/alerts') {   // water shortage: from the button now, from the ESP32 sensor later
      const b = await readBody(req); const a = { id: 'A-' + Date.now(), type: b.type || 'water_shortage', source: b.source || 'unknown',
        kioskId: b.kioskId || 'banyu-kiosk-01', timestamp: b.timestamp || now(), resolved: false }; db.alerts.push(a); save(); return send(res, 201, a);
    }
    if (m === 'POST' && (g = p.match(/^\/api\/alerts\/([\w-]+)\/resolve$/))) {
      const a = db.alerts.find(x => x.id === g[1]); if (!a) return send(res, 404, {}); a.resolved = true; save(); return send(res, 200, a);
    }
    if (m === 'POST' && p === '/api/seed') { seed(60); save(); return send(res, 200, stats()); }
    if (m === 'DELETE' && p === '/api/seed') { db.orders = db.orders.filter(o => !o.fake); db.alerts = db.alerts.filter(a => !a.fake); save(); return send(res, 200, stats()); }
    return send(res, 404, { error: 'not found' });
  }
  const file = path.join(__dirname, 'public', p === '/' ? 'index.html' : p);
  if (!file.startsWith(path.join(__dirname, 'public')) || !fs.existsSync(file)) return send(res, 404, 'Not found', 'text/plain');
  send(res, 200, fs.readFileSync(file), MIME[path.extname(file)] || 'application/octet-stream');
}).listen(CONFIG.PORT, '0.0.0.0', () => {
  console.log(`Banyu running. Kiosk: http://localhost:${CONFIG.PORT}  Dashboard: http://localhost:${CONFIG.PORT}/dashboard.html`);
  if (CONFIG.PAYMENT_MODE === 'xendit') console.log(`PAYMENT: Xendit ${XTEST ? 'TEST (no real money)' : 'LIVE (REAL MONEY)'} | flow: ${CONFIG.PAYMENT_FLOW} | methods: ${CONFIG.XENDIT_PAYMENT_METHODS} | key ${XKEY.slice(0, 16)}...`);
  else console.log(`PAYMENT: DEMO MODE. Xendit is NOT connected (${KEY_PROBLEM || 'PAYMENT_MODE=mock'}). Edit settings.env and restart.`);
  console.log(KEYWORDS_SET ? `DASHBOARD LOGIN: ${ADMIN_KEYWORDS.length} keyword(s) accepted (from settings.env)` : 'DASHBOARD LOGIN: WARNING, ADMIN_KEYWORDS is not set in settings.env, so the default keyword "banyu-admin" works. Change it.');
  console.log(`COLD ESTIMATOR: ${COLD.CAP} ml tank, recovers ${COLD.RATE} ml/min, Cold above ${COLD.COLD_ABOVE}, locked below ${COLD.LOCK_BELOW}, back on at ${COLD.REENABLE_AT}`);
  console.log(CONFIG.DISPENSER_MODE === 'esp32' ? `DISPENSER: ESP32 at ${CONFIG.ESP32_BASE_URL} (water-low alert after ${TANK_LOW_MS / 1000}s LOW)` : 'DISPENSER: DEMO MODE (no ESP32_BASE_URL in settings.env): the tablet fakes the pour');
});
