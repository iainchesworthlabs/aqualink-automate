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
3. Find **Aqualink Automate** in the store and click **Install**.
4. Configure it (below), then **Start**, and **Open Web UI** (or use the **Aqualink**
   sidebar entry).

## The web UI

The UI is served through Home Assistant **ingress**: it appears in the sidebar and via
the add-on's **Open Web UI** button, secured by your Home Assistant login — it is **not
exposed on your LAN**, and the app's own authentication is left off because Home
Assistant provides it.

If you specifically want direct LAN access (e.g. a wall tablet that bypasses HA), set a
host port for `80/tcp` in the add-on's **Network** panel. That direct port is
**unauthenticated** — put it behind your firewall.

## Configuration

### Serial connection

| Option | Description |
|---|---|
| `serial_mode` | `usb` for a local USB-RS485 adapter, `network` for serial-over-ethernet. |
| `serial_port` | (`usb`) Pick your adapter from the device list. Prefer a `/dev/serial/by-id/...` entry — it survives reboots. |
| `remote_serial_port` | (`network`) The adapter's `host:port`, e.g. `192.168.1.50:8899`. |
| `remote_protocol` | (`network`) `rfc2217` (default; most adapters), `rawtcp`, or `plain` (a plain socket). |

### MQTT / Home Assistant

| Option | Description |
|---|---|
| `mqtt_mode` | `auto` (recommended): use the broker Home Assistant already knows about — nothing else to fill in. `manual`: enter the broker details below. `disabled`: no MQTT. |
| `mqtt_host`, `mqtt_port`, `mqtt_username`, `mqtt_password`, `mqtt_tls` | Only used when `mqtt_mode` is `manual`. |
| `home_assistant_discovery` | Publish auto-discovery so your pool devices appear in Home Assistant automatically. |

With `mqtt_mode: auto` and the Mosquitto add-on running, the add-on discovers the
broker through the Supervisor — you do **not** enter any MQTT credentials.

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
