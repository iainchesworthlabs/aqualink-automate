# Aqualink Automate

Control and monitor your Jandy/Zodiac (and Pentair) pool equipment over RS-485, and
surface it in Home Assistant automatically through MQTT discovery — no Docker, no
command line. The Supervisor runs and manages the container for you.

## Requirements

- **Home Assistant OS** or **Supervised** (add-ons need the Supervisor; HA Container
  and HA Core cannot install add-ons — use the project's `docker-compose.yml` there).
- An RS-485 connection to your pool control panel, either:
  - a **USB-RS485 adapter** plugged into the Home Assistant host, or
  - a **serial-over-ethernet adapter** reachable on your network.
- For automatic Home Assistant entities: the **Mosquitto broker** add-on (or any MQTT
  broker configured in Home Assistant).

## Installation

1. In Home Assistant, go to **Settings → Add-ons → Add-on Store**.
2. Open the **⋮** menu (top-right) → **Repositories**, and add:
   `https://github.com/iainchesworthlabs/aqualink-automate`
3. Find **Aqualink Automate** in the store and click **Install**. The add-on **pulls a
   prebuilt image** (per architecture), so installation is a quick download — it does
   not compile anything on your device.
4. Configure it (below), then **Start**, and **Open Web UI** (or use the **Aqualink**
   sidebar entry).

## The web UI

The UI is served through Home Assistant **ingress**: it appears in the sidebar and via
the add-on's **Open Web UI** button, secured by your Home Assistant login — it is **not
exposed on your LAN**, and the app's own authentication is left off because Home
Assistant provides it. Ingress is **only** for the web UI (browser access); nothing else
talks to the add-on through it — MQTT, serial, and Home Assistant discovery all work
independently.

The direct LAN port is a **separate, optional** thing. By default it's left unmapped, so
in the **Network** panel you'll only see it under **"Show disabled ports"** — that's
expected, and the sidebar UI works fine without it. If you *also* want to reach the UI
directly on your LAN (e.g. a wall tablet that bypasses HA), set a host port for
`8099/tcp` there. That direct path is **unauthenticated by default** — either firewall
it, or turn on **Require web UI login** (below) so the app enforces its own login.

### Require web UI login

| Option | Description |
|---|---|
| `enable_auth` | Off by default (ingress provides login). Turn on to require an app login — mainly for a published direct LAN port, or if you want app-level users. |
| `auth_username` / `auth_password` | The administrator created the first time login is enabled. **Set a password of at least 12 characters** (required when `enable_auth` is on — a shorter one is rejected at startup). Users are stored under `/data`, so this account survives restarts; changing `auth_password` here later does **not** change the existing account — manage it from the app instead. |

With login enabled, the UI requires signing in over **both** ingress and the direct port.
The `/api/health` liveness endpoint stays open, so the add-on watchdog keeps working.

## Configuration

### Serial connection

Two fields: pick the **`serial_protocol`**, then enter the **`serial_port`** (the device or address):

| `serial_protocol` | `serial_port` to enter |
|---|---|
| **`usb`** — a local USB-RS485 adapter or built-in UART | A device path — **prefer `/dev/serial/by-id/usb-…`** (it survives reboots) or `/dev/ttyUSB0`. Non-standard paths work too; just type them. |
| **`rfc2217`** — a network serial adapter (telnet transport; suits most) | Its **`host:port`**, e.g. `192.168.1.50:8899` or `adapter.example.com:9001`. |
| **`rawtcp`** — a network serial adapter (plain TCP stream) | Its **`host:port`**, as above. |

USB adapters are mapped into the container automatically (`uart`).

*Finding a device path:* **Settings → System → Hardware** lists the host's serial devices, or use the Terminal add-on (`ls -l /dev/serial/by-id/`).

### MQTT / Home Assistant

| Option | Description |
|---|---|
| `mqtt_mode` | `auto` (recommended): use the broker Home Assistant already knows about — nothing else to fill in. `manual`: enter the broker details below. `disabled`: no MQTT. |
| `mqtt_host`, `mqtt_port`, `mqtt_username`, `mqtt_password`, `mqtt_tls` | Only used when `mqtt_mode` is `manual` (`auto` gets them from the Supervisor). |
| `home_assistant_discovery` | Publish auto-discovery so your pool devices appear in Home Assistant automatically. |

With `mqtt_mode: auto` and the Mosquitto add-on running, the add-on discovers the
broker through the Supervisor — you do **not** enter any MQTT credentials.

The add-on generates a stable, unique Home Assistant device identifier on first start
and stores it under `/data`, so your equipment stays the **same** device across restarts
and updates — there is no device ID to configure.

**TLS with certificates.** When the broker uses TLS, place the certificate files in Home
Assistant's **`/ssl`** share (via Samba / the File editor) and give just the **filename**:

| Option | Description |
|---|---|
| `mqtt_tls_skip_verify` | Skip broker certificate verification. Quick for a self-signed broker, but insecure — prefer a CA below. |
| `mqtt_ca_cert` | Filename in `/ssl` of a CA cert that signs the broker (for a private/self-signed broker). |
| `mqtt_client_cert` / `mqtt_client_key` | Filenames in `/ssl` for mutual TLS — set **both or neither**. |

### App features that default to Home Assistant's

These are **off by default** because Home Assistant already provides them; turn one on
only if you want the app's own version (it is then persisted under `/data`):

| Option | Description |
|---|---|
| `enable_history` | App-side time-series history. Off by default — Home Assistant's Recorder/History is the expected source. |
| `enable_scheduler` | App-side schedules. Off by default — use Home Assistant automations/schedules. |

### Other

| Option | Description |
|---|---|
| `log_level` | `info` (default), `debug`, or `trace`. Logs appear in the add-on's **Log** tab. |
| `log_to_file` | Off by default. Also writes a rotating, timestamped JSON log file to the add-on's config folder — see [Debugging a live add-on](#debugging-a-live-add-on). |
| `log_channels` | Advanced, blank by default. Raise individual areas without turning everything to `debug`, e.g. `devices=debug,navigation=debug` — see [Debugging a live add-on](#debugging-a-live-add-on). |
| `pool_configuration` | `auto` (default), or force `pool-only` / `spa-only` / `combo` / `dual`. |
| `jandy_device_type` | Advanced: what the software emulates on the bus. `auto` (default) stands up the full default set — OneTouch + IAQ + Serial Adapter — and suits most panels. Choose a single type only to restrict emulation to that one device. |
| `jandy_device_id` | Advanced: optional bus-address override for a single chosen type. Blank uses the type's default (OneTouch → 0x41); ignored when the type is `auto`. |

Your UI preferences and an equipment cache (for an instant dashboard after a restart)
are always persisted under `/data` — no configuration needed.

### Home Assistant companion package (optional)

The add-on ships with a set of ready-made Home Assistant blueprints, a helpers
package, and a dashboard — see the [companion package
docs](https://iainchesworthlabs.github.io/aqualink-automate/homeassistant-companion/)
for what's in it. There are three ways to get them, and only the last needs the
add-on's own settings:

- Import a blueprint with one click from the docs page.
- Download `aqualink-automate-homeassistant-companion-<version>.zip` from a
  [GitHub release](https://github.com/iainchesworthlabs/aqualink-automate/releases).
- Turn on **`install_companion_package`** below.

| Option | Description |
|---|---|
| `install_companion_package` | Off by default. When on, the add-on copies its bundled blueprints straight into Home Assistant's `blueprints/` folder on every start, so they show up under **Settings → Automations & Scenes → Blueprints** with no import step. **This requests a read-write view of Home Assistant's own configuration directory** — broader than anything else this add-on asks for — so it stays opt-in. It only ever adds or updates files under `blueprints/`; it never touches `configuration.yaml` or anything else in your config, and re-running it (a restart, an update) just re-syncs the current files. |

## Recording the RS-485 bus

The web UI's **Diagnostics** page can record the raw serial traffic to a `.cap` file —
useful when reporting a problem with equipment the app does not yet decode correctly.

Captures are written to this add-on's own configuration folder, which you can browse
from Home Assistant:

```
/app_configs/<add-on slug>/captures/
```

(`/addon_configs/<add-on slug>/captures/` on Home Assistant releases before
Supervisor 2026.07.) The slug is shown on the add-on's **Info** page; for a custom
repository it carries a repository prefix, for example
`13e07eae_aqualink_automate_edge`. The folder is reachable with the **Samba** or
**File editor** add-ons, survives restarts and add-on updates, and needs no
Terminal/SSH access.

You do not have to go looking for the file, though: the **Serial Recording** card
lists the captures already saved and gives each one a **Download** button, so you can
save a capture straight to the device you are browsing from. Very large captures
(over 64 MiB) cannot be downloaded that way — copy those from the folder above.

## Debugging a live add-on

You do not need to stop the add-on or run anything on another machine. Use these in
order — each step adds detail and costs a little more:

1. **The Log tab** (or the add-on's *Log* download). Quiet by default: routine web
   requests are only logged at `debug`.
2. **Raise one area, live.** The web UI's **Diagnostics → Log Levels** card sets a
   level per channel and applies it immediately, with no restart. It resets to your
   configured levels when the add-on restarts. Keep the whole add-on at `info` and raise
   only what you need — `devices` shows command handling (e.g. a chlorinator change),
   `navigation` and `scraping` show the OneTouch/iAQ menu walking.
3. **Make a level stick across restarts** with `log_channels`, e.g.
   `devices=debug,navigation=debug`. Don't set `log_level: debug` for a long session: the
   per-frame `serial`, `protocol` and `messages` channels scroll the Log tab's short
   window in seconds.
4. **Keep a long, timestamped log** with `log_to_file`. It writes
   `logs/aqualink-automate.log` (JSON, one record per line, each with a `ts`) in the
   same folder as the captures, rotated and bounded to about 50 MB. Unlike the Log tab,
   it is not limited to a short window and survives restarts.
5. **Record the wire.** For anything involving what the panel actually did, record the
   RS-485 bus from **Diagnostics → Serial Recording** (above). Start it *before* you
   trigger the behaviour, note the clock time you did so, and stop it afterwards. The
   `.cap` file holds every byte exchanged regardless of log level, so it is the ground
   truth when the logs and the equipment disagree.

When reporting a problem, attach the capture and the relevant slice of the log file,
and say what you did and at what time.

## Notes

- The Matter bridge that the container image can run is **disabled** in this add-on —
  Home Assistant is already a Matter controller, and your equipment is exposed through
  MQTT discovery instead.
- Full serial-wiring guidance and the complete option/topic reference live in the
  [project documentation](https://github.com/iainchesworthlabs/aqualink-automate/tree/main/docs).
