# Add-on local smoke-test harness

A one-command Docker harness to exercise the add-on's `run.sh` and the app **without a
real Home Assistant** — for catching wrapper / option-translation / boot regressions
before an HAOS install. This folder is not an add-on (no `config.yaml`), so the
Supervisor and the edge generator both ignore it.

## Why it exists

`bashio::config` does **not** read `/data/options.json`; it calls the Supervisor API
(`http://supervisor/addons/self/options/config`, `Bearer $SUPERVISOR_TOKEN`) and uses the
`.data` field, and `bashio::services` queries `/services/<name>`. So a bare container
can't run `run.sh`. [`mock-supervisor.js`](mock-supervisor.js) answers those two endpoints
from [`options.json`](options.json).

## Run it

```bash
cd homeassistant/dev
docker compose up --build
# in another shell:
curl localhost:8099/api/health        # -> {"status":"ok","uptime_seconds":...}
docker compose logs addon             # run.sh translation + app boot
```

Iterate on option branches by editing [`options.json`](options.json) then:

```bash
docker compose restart addon
```

Tear down with `docker compose down -v`.

## What it validates

- The wrapper image builds; bashio + jq install; the base image pulls.
- `run.sh` translates every option (serial, MQTT `auto`/`manual`, `enable_history`,
  `enable_scheduler`, `enable_auth`, `pool_configuration`, `log_level`) into the right
  flags, and the app accepts them and boots.
- `/api/health` (the watchdog target) returns 200; the container reports healthy.
- `/data` persistence (preferences, equipment cache, history DB, auth state) is written.

## Limits (these need real Home Assistant OS)

- **Ingress + the base-path frontend** — needs the Supervisor's ingress proxy.
- **A live MQTT broker + entity discovery** — the mock advertises a broker but runs none.
  Add a `mosquitto` service + set `mqtt_mode: manual`/`auto` to test a real connection.
- **`uart` device mapping** — needs HA + hardware.

## Faithful frontend/flags

`BUILD_VERSION` defaults to the last published release, which **predates newer app
frontend and some flags**. To test this branch faithfully, build a base image from source
and point the harness at it:

```bash
BUILD_VERSION=0.12.0-beta.6 docker compose up --build
```
