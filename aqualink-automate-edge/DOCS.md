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
   `https://github.com/iainchesworth/aqualink-automate`
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
| `pool_configuration` | `auto` (default), or force `pool-only` / `spa-only` / `combo` / `dual`. |
| `jandy_device_type`, `jandy_device_id` | Advanced: the identity the software presents on the RS-485 bus. Defaults suit most panels. |

Your UI preferences and an equipment cache (for an instant dashboard after a restart)
are always persisted under `/data` — no configuration needed.

## Notes

- The Matter bridge that the container image can run is **disabled** in this add-on —
  Home Assistant is already a Matter controller, and your equipment is exposed through
  MQTT discovery instead.
- Full serial-wiring guidance and the complete option/topic reference live in the
  [project documentation](https://github.com/iainchesworth/aqualink-automate/tree/main/docs).
