// Optional MQTT telemetry for banyu/server.js.
//
// The ESP32 firmware can publish its /api/status JSON to a broker:
//   <base>/status        retained full status JSON
//   <base>/event         job / tank transitions
//   <base>/availability  "online" / "offline" (retained LWT)
//
// This module subscribes read-only. It never publishes commands: start, pause,
// resume and stop stay on the local HTTP API. The `mqtt` package is loaded only
// when this is enabled, so the base server still runs with zero packages.
function loadMqtt() {
  return require('mqtt');   // throws a normal MODULE_NOT_FOUND if not installed
}
function safeUrl(u) {
  try { const x = new URL(u); x.username = ''; x.password = ''; return x.toString(); }
  catch { return null; }
}

function createMqttTelemetry(opts = {}) {
  const url = String(opts.url || '').trim();
  const base = String(opts.base || '').trim().replace(/\/+$/, '');
  const username = String(opts.username || '');
  const password = String(opts.password || '');
  const clientId = String(opts.clientId || 'banyu-server');
  const log = typeof opts.log === 'function' ? opts.log : () => {};
  const onStatus = typeof opts.onStatus === 'function' ? opts.onStatus : () => {};
  const onEvent = typeof opts.onEvent === 'function' ? opts.onEvent : () => {};
  const onAvailability = typeof opts.onAvailability === 'function' ? opts.onAvailability : () => {};

  const state = {
    enabled: false, connected: false, url: null, base: '',
    availability: null, last_status_at: null, last_live_status_at: null,
    last_event: null, last_error: null, connects: 0, reconnects: 0,
  };
  state.enabled = !!(url && base);
  state.url = safeUrl(url);
  state.base = base;

  const topics = { status: base + '/status', event: base + '/event', availability: base + '/availability' };
  let client = null, started = false, lastErr = '', lastErrAt = 0;

  function warn(m) {
    const t = Date.now();
    if (m !== lastErr || t - lastErrAt > 30000) { lastErr = m; lastErrAt = t; log(m); }   // same error at most every 30s
  }

  function handle(topic, buf, packet) {
    const text = buf.toString('utf8');
    if (topic === topics.status) {
      let st;
      try { st = JSON.parse(text); } catch { state.last_error = 'status topic is not JSON'; return; }
      if (!st || typeof st !== 'object' || Array.isArray(st)) { state.last_error = 'status topic is not an object'; return; }
      state.last_status_at = Date.now();
      const retain = !!(packet && packet.retain);
      if (!retain) state.last_live_status_at = Date.now();
      onStatus(st, { retain });
    } else if (topic === topics.event) {
      let ev;
      try { ev = JSON.parse(text); } catch { ev = text; }
      state.last_event = ev;
      onEvent(ev);
    } else if (topic === topics.availability) {
      const a = text.trim().toLowerCase();
      state.availability = a === 'online' || a === 'offline' ? a : text.trim();
      onAvailability(state.availability);
    }
  }

  function start() {
    if (!state.enabled || started) return info();
    started = true;
    let mqtt;
    try { mqtt = loadMqtt(); }
    catch (e) {
      state.last_error = "the optional 'mqtt' package is not installed (run 'npm install' in banyu/)";
      log(state.last_error);
      return info();
    }

    const options = { clientId, clean: true, keepalive: 30, connectTimeout: 10000, reconnectPeriod: 5000 };
    if (username) options.username = username;
    if (password) options.password = password;

    try { client = mqtt.connect(url, options); }
    catch (e) { state.last_error = e.message; log('cannot start: ' + e.message); return info(); }

    client.on('connect', () => {
      state.connected = true; state.connects++; state.last_error = null;
      log(`connected ${state.url || 'broker'} base ${base}`);
      client.subscribe([topics.status, topics.event, topics.availability], { qos: 0 }, (err) => {
        if (err) { state.last_error = err.message; warn('subscribe failed: ' + err.message); }
        else log(`subscribed ${base}/status, /event, /availability (read-only)`);
      });
    });
    client.on('reconnect', () => { state.reconnects++; });
    client.on('close', () => { state.connected = false; });
    client.on('offline', () => { state.connected = false; });
    client.on('error', (e) => { state.last_error = e.message; warn('error: ' + e.message); });
    client.on('message', handle);
    return info();
  }

  function stop() {
    started = false;
    if (client) { try { client.end(true); } catch { /* ignore */ } client = null; }
    state.connected = false;
  }

  function info() { return { ...state }; }

  return { start, stop, info };
}

module.exports = { createMqttTelemetry };
