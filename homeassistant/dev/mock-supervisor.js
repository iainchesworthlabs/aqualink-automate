// Minimal mock of the Home Assistant Supervisor API for local add-on smoke testing.
//
// bashio::config does NOT read /data/options.json — it calls the Supervisor at
// http://supervisor/addons/self/options/config (Bearer SUPERVISOR_TOKEN) and uses the
// `.data` field. bashio::services queries /services/<name>. This server answers just
// those two endpoints so run.sh + the app can run without a real Supervisor.
//
// options.json is read live on every request, so you can edit it and `docker compose
// restart addon` to exercise a different option set without restarting this server.
const http = require('http');
const fs = require('fs');

const OPTIONS_FILE = process.env.OPTIONS_FILE || '/harness/options.json';

function ok(res, data) {
  res.setHeader('Content-Type', 'application/json');
  res.end(JSON.stringify({ result: 'ok', data }));
}

http
  .createServer((req, res) => {
    console.log('[mock-supervisor]', req.method, req.url);
    if (req.url === '/addons/self/options/config') {
      return ok(res, JSON.parse(fs.readFileSync(OPTIONS_FILE, 'utf8')));
    }
    // MQTT service discovery (used when the add-on option mqtt_mode = auto).
    if (req.url === '/services/mqtt') {
      return ok(res, {
        host: process.env.MQTT_HOST || 'mqtt',
        port: Number(process.env.MQTT_PORT || 1883),
        ssl: false,
        username: process.env.MQTT_USERNAME || '',
        password: process.env.MQTT_PASSWORD || '',
      });
    }
    return ok(res, {});
  })
  .listen(80, () => console.log('[mock-supervisor] listening on :80 (options:', OPTIONS_FILE + ')'));
