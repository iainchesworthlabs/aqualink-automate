# Usage, HTTP API and WebSocket protocol

*For pool owners running a release build who want to control or monitor the system over HTTP. The machine-readable companion is [swagger.yaml](https://github.com/iainchesworth/aqualink-automate/blob/main/assets/web/api/swagger.yaml); CLI flags live in the [Configuration reference](configuration.md); building from source lives in [INSTALL.md](INSTALL.md).*

This document covers how to run the application, the built-in web UI, the complete HTTP REST API, and the WebSocket protocol the UI uses for live updates. Routes are documented as the code registers them today — the same source of truth `swagger.yaml` is generated against.

## Contents

- [Quickstart: first run](#quickstart-first-run)
- [The web UI](#the-web-ui)
- [Security model](#security-model)
- [HTTP API conventions](#http-api-conventions)
- [Route reference by tag](#route-reference-by-tag)
- [Key request and response examples](#key-request-and-response-examples)
- [WebSocket protocol](#websocket-protocol)
- [Prometheus metrics](#prometheus-metrics)
- [Viewing the API spec](#viewing-the-api-spec)

## Quickstart: first run

The application talks to the AquaLink panel over RS-485 — either a local serial port or a remote serial bridge — and serves a web UI plus REST/WebSocket API. Pick the connection that matches your hardware.

**Connect to a local serial port.** Use this when the RS-485 adapter is plugged into the machine running the app.

```bash
# Linux: an FTDI/CH340-style USB RS-485 adapter shows up as /dev/ttyUSB0
aqualink-automate --serial-port /dev/ttyUSB0
```

```powershell
# Windows: the adapter shows up as a COM port
aqualink-automate.exe --serial-port COM3
```

**Connect to a remote serial bridge.** Use this when the panel is wired to a networked RS-485-over-TCP device (for example a Brainboxes or other RFC 2217 gateway). Pass `--rfc2217` when the bridge speaks the RFC 2217 control protocol.

```bash
aqualink-automate --remote-serial-port 192.168.1.50:9001 --rfc2217
```

**Open the web UI.** The web server binds to `127.0.0.1` by default — reachable only from the same machine. To reach it from your LAN, bind a routable address and choose a port:

```bash
aqualink-automate --serial-port /dev/ttyUSB0 --address 0.0.0.0 --http-port 8080
# Then browse to http://<host-ip>:8080/
```

**Security:** The bind address defaults to `127.0.0.1` and authentication is off by default. Binding to `0.0.0.0` exposes an unauthenticated control surface to your whole network. Turn on a bearer token (see [Security model](#security-model)) before exposing the server, and prefer HTTPS. See the [Configuration reference](configuration.md) for `--address`, `--http-port`, and TLS options.

CLI flags and the optional config file share names: a config-file key is the option's long name without the leading dashes (for example the flag `--http-port` is the key `http-port`).

## The web UI

The UI is a static single-page application built on Alpine.js. The server ships it from the document root (override with `--doc-root`), and your browser drives everything from there — there is no server-side rendering.

On load the page:

1. Loads the vendored `alpine.min.js` and Chart.js (vendored as `chart.umd.min.js`) — no external CDN.
2. Calls `auth.check()`, which issues `GET /api/auth/check`. A `200` means you are allowed in (either auth is disabled or your stored token is valid); a `401` shows the login screen.
3. Runs `fetchInitial` (a batch of REST reads) to paint the current state, then opens the live WebSocket connections.
4. Falls back to polling REST every 30 seconds whenever the WebSocket is down, so the UI keeps updating even if the live channel drops.

The WebSocket client opens `/ws/equipment` and `/ws/equipment/stats`, matching the page protocol (`ws://` for HTTP, `wss://` for HTTPS). It reconnects with exponential backoff from 1 s up to 30 s. When a token is stored it is attached as the `['aqualink', 'bearer.<token>']` WebSocket subprotocols.

## Security model

Authentication is **opt-in** and **off by default**. With no token configured the server behaves exactly as an open, unauthenticated service — every route is reachable without credentials.

To require authentication, set a bearer token via `--api-auth-token <token>` (config key `api-auth-token`). When a token is configured:

- **Registered routes** (everything under `/api`, plus `/metrics`) and any **unmatched `/api/*` path** require an `Authorization: Bearer <token>` header. The token is compared in constant time. A missing or wrong token returns `401 Unauthorized`. An unknown `/api/*` path answers `401` (not `404`) so the existence of routes is not leaked.
- **Static assets are intentionally served without authentication.** This is deliberate: the browser must be able to load `index.html`, scripts, and CSS to render the login screen before the user has a token.
- **`GET /api/health` is intentionally served without authentication.** The liveness probe must be reachable by a container/orchestrator health check without baking in the operator's token. It returns only `{"status":"ok","uptime_seconds":N}` — no sensitive data. The richer `GET /api/health/detailed` is **not** exempt: it exposes internal subsystem state and is gated by the bearer-token policy like every other route.
- **WebSocket upgrades are gated by the same policy.** Browsers cannot set an `Authorization` header on a WebSocket upgrade, so the token rides in the `Sec-WebSocket-Protocol` header as a `bearer.<token>` entry alongside the `aqualink` marker (for example `Sec-WebSocket-Protocol: aqualink, bearer.<token>`). On success the handshake echoes back the `aqualink` subprotocol.

Two further hardening layers are built into the routing layer and are exposed via CLI flags / config keys (see the [Configuration reference](configuration.md)):

- **Origin allow-list.** Set one or more allowed origins with the repeatable `--api-allowed-origin` flag (config key `api-allowed-origin`). When at least one origin is configured, a request or WebSocket upgrade whose `Origin` header is not on the list is rejected with `403 Forbidden`.
- **CSRF header.** Enable `--api-require-csrf-header` (config key `api-require-csrf-header`) so that state-changing methods (`POST`, `PUT`, `DELETE`, …) must send an `X-Requested-With` header or they are rejected with `403 Forbidden`. This check does not apply to the WebSocket upgrade, which is always a `GET`.

**Security:** A token over plain HTTP travels in cleartext. Pair `--api-auth-token` with HTTPS (see the TLS options in the [Configuration reference](configuration.md)) whenever the server is reachable beyond `localhost`.

## HTTP API conventions

- **Base paths.** Application routes live under `/api`. Two endpoints are exceptions: `GET /metrics` lives at the **root** (not under `/api`), and the web UI / static assets are served from `/`.
- **No caching of dynamic responses.** Every registered (dynamic) route response is stamped `Cache-Control: no-store`. Static assets are not, so they cache normally.
- **Request body limit.** HTTP request bodies are capped at **10000 bytes**. A larger body is rejected before your handler runs.
- **Connection limits.** The server accepts at most **1000** concurrent HTTP connections and applies a **30-second** per-operation idle timeout.
- **Method dispatch.** Each route dispatches on the HTTP method. An unsupported method returns `405 Method Not Allowed`. Some routes return a bare `405`; others (the diagnostics routes) return `405` with a JSON body such as `{"error":"Method not allowed. Use GET."}`.
- **Content types.** JSON responses use `application/json`. Validation and method errors are usually `text/plain` or a small JSON `{"error":"..."}` object, depending on the route (noted per route below).

**Note:** In replay/dev mode the system has no live serial adapter, so command (`POST`) routes commonly answer `503` while `PoolConfiguration` is `Unknown`. This is a documented, accepted outcome — treat it as "not ready", not a server fault.

## Route reference by tag

All routes below are registered in a single block when the web server is enabled. URLs are compile-time constants.

### Health

| Method | Path | Success | Notes |
|---|---|---|---|
| GET | `/api/health` | `200` `{"status":"ok","uptime_seconds":N}` | **Liveness** probe for Docker/Kubernetes health checks. **Always unauthenticated** (reachable without the bearer token even when `--api-auth-token` is set); carries no sensitive data. The Docker image ships a `HEALTHCHECK` that polls it. |
| GET | `/api/health/detailed` | `200` JSON / `503` when not ready | **Readiness + diagnostics**: overall readiness, uptime, version, and per-subsystem checks (configuration + equipment validation, equipment count, MQTT connectivity). Returns `200` once the pool configuration is known, `503` while still starting. **Subject to the bearer-token policy** (it exposes internal state). Serial/protocol counters are not duplicated here — see `/metrics`. |

### Version

| Method | Path | Success | Notes |
|---|---|---|---|
| GET | `/api/version` | `200` JSON | App + build metadata and uptime. `git_info.uncommitted_changes` is the build-time working-tree state and means a **tracked source file differed from the commit** — it deliberately ignores exec-bit-only changes, untracked build artifacts, and the bootstrapped `deps/vcpkg` submodule, so a CI-released binary reports `false`. |

### Equipment

| Method | Path | Success | Other codes |
|---|---|---|---|
| GET | `/api/equipment` | `200` JSON | Full state block (temperatures, chemistry, configuration, devices, stats, version). |
| GET | `/api/equipment/devices` | `200` JSON | Devices grouped as `auxillaries`, `heaters`, `pumps`. |
| GET | `/api/equipment/version` | `200` JSON | `fields[]`, `model_number`, `fw_revision`. |
| POST | `/api/equipment/circulation` | `200` JSON | Set circulation mode (`pool`/`spa`/`spillover`). `400` (bad value), `503` (no dispatcher). POST-only; non-`POST` returns a bare `405`. |
| POST | `/api/equipment/heater` | `200` JSON | Enable/disable a heater by body of water (`pool`/`spa`/`solar`). `400` (bad value), `503` (no dispatcher). POST-only; non-`POST` returns a bare `405`. |

### Buttons

| Method | Path | Success | Other codes |
|---|---|---|---|
| GET | `/api/equipment/buttons` | `200` JSON | List of buttons with `controllable` flag. |
| POST | `/api/equipment/buttons` | `200` empty `text/html` | Placeholder (no-op). |
| GET | `/api/equipment/buttons/{button_id}` | `200` JSON | `503` (system not ready), `404` (bad/unknown id). |
| POST | `/api/equipment/buttons/{button_id}` | `200` JSON (toggled) | `503`, `404`, `422`, `400` (see below). |

### Devices

| Method | Path | Success | Notes |
|---|---|---|---|
| GET | `/api/equipment/iaq` | — | POST only (see Setpoints/commands). |
| POST | `/api/equipment/iaq` | `200` JSON | `select_button` (0..255). `503` no dispatcher, `400` bad value. |
| GET | `/api/equipment/spaside-remotes` | `200` JSON | `remotes`, `assignments`, `requested`. |
| POST | `/api/equipment/spaside-remotes` | `200` JSON | `press` / `assign` actions (see below). |

### Setpoints

| Method | Path | Success | Other codes |
|---|---|---|---|
| GET | `/api/equipment/setpoints` | `200` JSON | `pool_setpoint`, `spa_setpoint`. |
| POST | `/api/equipment/setpoints` | `200` JSON | `400` (bad value), `503` (no dispatcher), `500` (dispatch failed). |
| POST | `/api/equipment/chlorinator` | `200` JSON | `400` (bad body), `503` (no dispatcher). |

### Diagnostics

| Method | Path | Success | Notes |
|---|---|---|---|
| GET | `/api/diagnostics/emulated-devices` | `200` JSON | Per-device diagnostics array (emulated). |
| GET | `/api/diagnostics/actual-devices` | `200` JSON | Per-device diagnostics array (bus-discovered). |
| GET / POST | `/api/diagnostics/logging` | `200` JSON | GET returns channels + levels; POST sets a level. |
| GET | `/api/diagnostics/options` | `200` JSON | `options[]` of `{name, short_name, description}`. |
| GET | `/api/diagnostics/mqtt` | `200` JSON | `enabled` + MQTT connection diagnostics. |
| GET | `/api/diagnostics/matter` | `200` JSON | `enabled` + Matter sidecar status (cached). |
| GET / POST | `/api/diagnostics/recording` | `200` JSON | GET returns `{recording, file, bytes}`; POST starts/stops. |
| GET / POST | `/api/diagnostics/profiling` | `200` JSON | GET reports `{enabled, running, backend, available}`; POST controls it (`start`/`stop`/`select`). `400` (bad body), `409` (no backend in this build), `503` (no controller). |

Non-`GET` requests to the read-only diagnostics routes return `405` with a JSON body.

### History

| Method | Path | Success | Other codes |
|---|---|---|---|
| GET | `/api/history/series` | `200` JSON | `503` when history is disabled; `404` unknown key; `400` if `to` < `from`. |

### Schedules

| Method | Path | Success | Other codes |
|---|---|---|---|
| GET | `/api/schedules` | `200` array | `503` when scheduler disabled. |
| POST | `/api/schedules` | `201` JSON | `400` invalid body; `503` disabled. |
| GET | `/api/schedules/{uuid}` | `200` JSON | `404` unknown; `503`; `400` missing uuid. |
| PUT | `/api/schedules/{uuid}` | `200` JSON | `400`, `404`, `503`. |
| DELETE | `/api/schedules/{uuid}` | `204` | `404`, `503`. |
| GET | `/api/controller/schedules` | `200` `{status, schedules[]}` | `503` when the store is unavailable. |

App-side schedules (`/api/schedules`) are point actions the app fires; controller
schedules (`/api/controller/schedules`) are the controller's own built-in
on→off programs, read-only and reported as spans. `status` is `available`,
`pending_capture` (parser not yet wired), or `unsupported`. The web UI merges
both into one timeline. Writing controller schedules is not yet supported.

### Preferences

| Method | Path | Success | Other codes |
|---|---|---|---|
| GET | `/api/preferences` | `200` JSON | `503` when the service is unavailable. |
| PUT | `/api/preferences` | `200` JSON | `400` (invalid JSON or apply failure), `503`. |

### Metrics

| Method | Path | Success | Notes |
|---|---|---|---|
| GET | `/metrics` | `200` `text/plain` | Prometheus exposition. Lives at the **root**, not under `/api`. |

### Authentication

| Method | Path | Success | Notes |
|---|---|---|---|
| GET | `/api/auth/check` | `200` `{"authenticated":true}` | Only reached when already authorized or auth is disabled; the routing layer answers `401` upstream otherwise. |

## Key request and response examples

### GET /api/equipment

Returns the full equipment state. The top-level keys are `temperatures`, `chemistry`, `configuration`, `devices`, `stats`, and `version`. The shape below shows the documented fields.

```json
{
  "temperatures": {
    "pool": { "celsius": 28, "fahrenheit": 82 },
    "spa": null,
    "air": { "celsius": 24, "fahrenheit": 75 },
    "pool_setpoint": { "celsius": 28, "fahrenheit": 82 },
    "spa_setpoint": { "celsius": 38, "fahrenheit": 100 }
  },
  "chemistry": {
    "salt_ppm": 3200,
    "orp_mv": null,
    "ph": null,
    "chlorinator": {
      "generating_percent": 50,
      "duty_cycle": 50,
      "status": "Generating",
      "health": "Ok"
    }
  },
  "configuration": {
    "pool_configuration": "PoolOnly",
    "configuration_source": "DataHub",
    "expected_auxillary_count": 6,
    "expected_power_center_count": 1,
    "validation": {
      "passed": true,
      "expected_auxillaries": 6,
      "discovered_auxillaries": 6,
      "expected_power_centers": 1,
      "discovered_power_centers": 1,
      "anomalies": []
    },
    "bodies": [
      { "id": "Pool", "label": "Pool", "is_active": true,
        "temperature": { "celsius": 28, "fahrenheit": 82 },
        "setpoint": { "celsius": 28, "fahrenheit": 82 } }
    ]
  },
  "devices": { },
  "stats": { },
  "version": { }
}
```

Field notes (these are deliberate, not bugs):

- `salt_ppm` is a raw `uint16`. A value of `0` is emitted as-is, **not** nulled.
- `orp_mv` is `null` when the reported value is exactly `0` (no sensor on the wire).
- `ph` is `null` when the reported value is exactly `0.0`.
- `chlorinator` is `null` when no chlorinator (SWG) has been discovered; otherwise it carries `generating_percent`, `duty_cycle`, `status`, and `health`.
- `validation` is an object once the startup scrape completes, or `null` before then. The spelling `expected_auxillary_count` (and the nested `expected_auxillaries` / `discovered_auxillaries`) is the exact field name in the payload.
- A temperature is either a `{ "celsius": ..., "fahrenheit": ... }` object or `null`.

### GET /api/equipment/buttons

```json
{
  "buttons": [
    {
      "id": "1f3c...uuid",
      "label": "Filter Pump",
      "display_label": "Filter Pump",
      "status": "On",
      "device_type": "FilterPump",
      "controllable": true
    }
  ]
}
```

`controllable` is `false` for `Chlorinator` and `Unknown` device types (they are configurable/informational, not on/off toggles).

### POST /api/equipment/buttons/{button_id} (toggle)

Toggle a single controllable device by its UUID.

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/buttons/1f3c-...-uuid
```

```json
{ "id": "1f3c-...-uuid", "label": "Filter Pump", "status": "Off", "command": "toggled" }
```

Status mapping (from the command dispatcher result):

| Outcome | Status |
|---|---|
| Toggled successfully | `200` (body above) |
| No serial adapter / not ready / null dispatcher | `503` (with `Retry-After`) |
| Device id not found | `404` |
| Device type cannot be mapped to a command | `422` |
| Invalid value for the command | `400` |

`PoolConfiguration == Unknown` (system still starting) yields `503` for both GET and POST. A missing, unparseable, or unknown `button_id` yields `404`.

### POST /api/equipment/setpoints

Set pool and/or spa water-temperature setpoints. Values are in **Celsius**. Each field is validated to be finite and within `-10.0 .. 50.0`; an out-of-range value returns `400` (`text/plain`). Either field may be omitted.

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/setpoints \
  -H 'Content-Type: application/json' \
  -d '{ "pool": 28.0, "spa": 38.0 }'
```

```json
{
  "pool": { "status": "success", "celsius": 28.0 },
  "spa":  { "status": "success", "celsius": 38.0 }
}
```

With no command dispatcher available the whole request returns `503` (`text/plain` "Command dispatcher not available"). If any accepted field's dispatch fails, the overall status is `500` while the per-field JSON still reports each field's individual `status`.

### POST /api/equipment/chlorinator

Set the chlorinator generating percentage and/or its boost mode.

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/chlorinator \
  -H 'Content-Type: application/json' \
  -d '{ "percentage": 60, "boost": false }'
```

- `percentage` is a number in `0 .. 100`.
- `boost` is a boolean.

```json
{
  "percentage": { "status": "success", "value": 60 },
  "boost":      { "status": "success", "value": false }
}
```

**Note:** There are two distinct `503` shapes here. A missing dispatcher returns `text/plain` "Command dispatcher not available". A command that ran but failed returns the per-field JSON body with the mapped status code.

### POST /api/equipment/circulation

Set the circulation mode (which body of water the system circulates).

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/circulation \
  -H 'Content-Type: application/json' \
  -d '{ "mode": "spa" }'
```

`mode` is a string and must be one of `pool`, `spa`, or `spillover`.

```json
{ "mode": "spa", "status": "success" }
```

POST-only: any non-`POST` method returns a bare `405`. A missing/non-string `mode`, or a value outside the allowed set, returns `400` (`text/plain`). With no command dispatcher available the request returns `503` (`text/plain` "Command dispatcher not available"). The HTTP status otherwise follows the command-dispatch result (`400` invalid value, `503` no serial adapter / device not found, `422` unknown equipment type, `500` otherwise).

### POST /api/equipment/heater

Enable or disable a heater, identified by its body of water. `solar` is modelled as the shared heater.

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/heater \
  -H 'Content-Type: application/json' \
  -d '{ "body": "spa", "enable": true }'
```

- `body` is a string and must be one of `pool`, `spa`, or `solar`.
- `enable` is a boolean.

```json
{ "body": "spa", "enable": true, "status": "success" }
```

POST-only: any non-`POST` method returns a bare `405`. A missing/non-string `body` (or a value outside the allowed set) or a missing/non-boolean `enable` returns `400` (`text/plain`). With no command dispatcher available the request returns `503` (`text/plain` "Command dispatcher not available"). The HTTP status otherwise follows the command-dispatch result (as for circulation, above).

### POST /api/equipment/iaq

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/iaq \
  -H 'Content-Type: application/json' \
  -d '{ "select_button": 3 }'
```

`select_button` is an integer in `0 .. 255`. Response: `200 {"select_button":{"status":"success","value":3}}`. A missing dispatcher returns `503`; a bad value returns `400`.

### POST /api/equipment/spaside-remotes

Two actions are supported. **`press`** actuates an emulated remote:

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/spaside-remotes \
  -H 'Content-Type: application/json' \
  -d '{ "action": "press", "address": 32, "button": 1 }'
```

| Outcome | Status |
|---|---|
| Press queued | `200` |
| No remote at that address | `404` |
| Remote is real (observe-only, not emulated) | `409` |
| Button out of range | `400` |

**`assign`** programs a switch button to a function:

```bash
curl -X POST http://127.0.0.1:8080/api/equipment/spaside-remotes \
  -H 'Content-Type: application/json' \
  -d '{ "action": "assign", "switch": 1, "button": 2, "function": "Spa" }'
```

| Outcome | Status |
|---|---|
| Accepted | `200` |
| Invalid switch/button/function | `400` |
| Controller busy | `409` |
| No controller can program assignments | `503` |

**Note:** Two distinct `503` bodies exist. A null controller (dev/replay mode) returns `{"error":"Spa-side remote control is not available in this mode"}`. An `assign` with no capable controller returns `{"error":"No controller can program spa-switch assignments on this system"}`. A missing or unknown `action` returns `400`.

### GET /api/history/series

List all series, or query one. Returns `503` when history is disabled (no history database configured).

```bash
# List every series and its metadata
curl http://127.0.0.1:8080/api/history/series
```

```json
[
  { "key": "temp/pool", "unit": "C", "label": "", "first_ts": 1700000000, "last_ts": 1700090000, "count": 900 },
  { "key": "device/4b8c1e2a-7f3d-4a9b-bc10-2f5e6d7a8b90", "unit": "state", "label": "Pool Light", "first_ts": 1700000000, "last_ts": 1700090000, "count": 18 }
]
```

`label` is the friendly display name. Device series are keyed by the stable
button UUID (`device/<uuid>`) so a device that is renamed after discovery (e.g.
`Aux5` → `Pool Light`) stays a single series; analog series use an empty label.

```bash
# Query a single series within a time window
curl 'http://127.0.0.1:8080/api/history/series?key=pool_temp&from=1700000000&to=1700090000&max_points=500'
```

```json
{ "key": "pool_temp", "from": 1700000000, "to": 1700090000, "max_points": 500,
  "points": [ { "ts": 1700000000, "value": 28.1 } ] }
```

Query parameters: `key` selects a series; an unknown key returns `404`. `from` defaults to `0`, `to` defaults to "now", and `to < from` returns `400`. `max_points` defaults to `500`, clamped to `1 .. 2000`.

## WebSocket protocol

There are two WebSocket endpoints:

| Endpoint | Purpose |
|---|---|
| `/ws/equipment` | Live equipment, chemistry, temperature, status, and alert updates. |
| `/ws/equipment/stats` | Live statistics updates. |

Neither handler processes inbound client messages — the channels are server-to-client only.

### Frame envelope

Every frame is a JSON object with two fields:

```json
{ "type": "<EventType>", "payload": { } }
```

`type` is the name of a `WebSocket_EventTypes` value: `ChemistryUpdate`, `StatisticsUpdate`, `TemperatureUpdate`, `CirculationUpdate`, `SystemStatusChange`, `SystemStateUpdate`, `ButtonStateChange`, `AlertTransition`, or `Unknown`.

### What each endpoint sends

`/ws/equipment` broadcasts:

- `ChemistryUpdate`
- `TemperatureUpdate`
- `CirculationUpdate` — circulation/heater mode changes
- `ButtonStateChange`
- `SystemStatusChange`
- `AlertTransition` — `{ "condition": ..., "state": "raised" | "cleared", "ts": ..., "detail": ... }`

On connect, `/ws/equipment` enqueues exactly one `SystemStateUpdate` so a freshly connected client knows the current state immediately:

```json
{
  "type": "SystemStateUpdate",
  "payload": {
    "state": "ready",
    "pool_configuration": "PoolOnly",
    "equipment_mode": "Normal"
  }
}
```

`state` is one of `ready`, `starting`, or `service_mode`.

`/ws/equipment/stats` sends `StatisticsUpdate` frames.

### Per-type payload fields

These are the fields the UI consumes from each frame's `payload`:

| Event type | Payload fields |
|---|---|
| `TemperatureUpdate` | `pool_temp`, `spa_temp`, `air_temp`, `pool_setpoint`, `spa_setpoint` |
| `ChemistryUpdate` | `ph`, `orp`, `salt_level` |
| `ButtonStateChange` | `button_id`, `status`, `label` (optional) |
| `SystemStateUpdate` | `state`, `pool_configuration`, `equipment_mode` |
| `SystemStatusChange` | `status_type`, `source_name`, `source_type` |

### Connection limits

- Each connection has an outbound queue capped at **100** messages. When full, the oldest message is dropped (with a rate-limited warning) before the new one is enqueued.
- Inbound WebSocket frames are capped at **64 KiB**.
- The same HTTP limits apply to the upgrade request: a **10000-byte** body limit, at most **1000** concurrent connections, and a **30-second** idle timeout.

### Keepalive and reverse proxies

Both endpoints send a WebSocket protocol-level ping after roughly **30 seconds** of silence (the server runs a 60-second idle timeout with keep-alive pings enabled — see `WebSocketServerTimeout()` in `src/core/http/server/websocket_timeouts.h`). This matters because `/ws/equipment` is change-driven: with the pool in a steady state no data frames flow for minutes, and without the pings a reverse proxy in front of the app would sever the quiet connection at its own idle timeout (nginx defaults `proxy_read_timeout` to 60 s; Cloudflare closes idle WebSockets after ~100 s), causing a spurious "Connection lost — retrying..." toast in the UI. Browsers answer pings automatically — clients do not need to implement anything.

If you run a reverse proxy in front of the application, any WebSocket/upstream idle timeout of **60 seconds or more** works out of the box. A proxy configured tighter than ~30 seconds will still drop quiet connections. A peer that stops answering pings is torn down by the server within the 60-second idle timeout.

## Prometheus metrics

`GET /metrics` returns Prometheus exposition text with content type `text/plain; version=0.0.4; charset=utf-8`. It is at the root, not under `/api`. A token-protected deployment requires the bearer token for `/metrics` too.

Series emitted:

| Metric | Type | Labels |
|---|---|---|
| `aqualink_build_info` | gauge (always `1`) | `version`, `commit` |
| `aqualink_serial_read_bytes_total` | counter | — |
| `aqualink_serial_write_bytes_total` | counter | — |
| `aqualink_messages_total` | counter | — |
| `aqualink_message_errors_total` | counter | `kind` (`checksum`, `deserialization`, `invalid_packet_format`, `generator`, `overlapping_packets`, `buffer_overflow`) |
| `aqualink_serial_errors_total` | counter | `kind` (`overflow`, `underflow`, `transmission_failure`) |
| `aqualink_latency_microseconds` | gauge | `stage` (`serial_read`, `serial_write`, `msg_proc`), `quantile` (`0.01`, `0.5`, `0.95`, `0.99`) |

A minimal scrape config:

```yaml
scrape_configs:
  - job_name: aqualink-automate
    static_configs:
      - targets: ['127.0.0.1:8080']
    # If a bearer token is configured:
    # authorization:
    #   credentials: '<token>'
```

## Viewing the API spec

The machine-readable OpenAPI spec is [swagger.yaml](https://github.com/iainchesworth/aqualink-automate/blob/main/assets/web/api/swagger.yaml). It is served as a **static asset**, not by a dedicated route handler, reachable at:

```http
GET /api/swagger.yaml
```

Because it is a static asset, it is served as raw YAML and (like all static assets) without authentication.

To browse it as interactive Swagger UI, use the Docker `docs` profile:

```bash
docker compose --profile docs up
# Swagger UI is then at http://localhost:8080
```

See [INSTALL.md](INSTALL.md) for the Swagger UI docs profile and other deployment details.

## Related documentation

- [Configuration reference](configuration.md) — `--api-auth-token`, `--address`, `--http-port`, and the TLS flags that control this server.
- [MQTT and Home Assistant](mqtt-home-assistant.md) — the MQTT control surface, an alternative to this HTTP API.
- [swagger.yaml](https://github.com/iainchesworth/aqualink-automate/blob/main/assets/web/api/swagger.yaml) — the machine-readable companion to this reference.
- [INSTALL.md](INSTALL.md) — installation and the Swagger UI docs profile.
