# Running as a Home Assistant add-on

*For pool owners who run **Home Assistant OS** or **Supervised** and want Aqualink
Automate managed by Home Assistant — installed from the add-on store, configured from
a form, run and updated by the Supervisor — without touching Docker themselves. For
the MQTT/entity details this feeds into, see [MQTT and Home Assistant integration](mqtt-home-assistant.md).*

## What this is

A Home Assistant **add-on** is a container the **Supervisor** runs on your behalf. You
add this project as a custom repository, click **Install**, fill in a settings form,
and Home Assistant handles the rest — pulling the image, starting it, restarting it on
failure, and applying updates. The add-on is a thin wrapper around the same multi-arch
image the project already publishes; it does not rebuild the application.

The add-on files live in this repository under
[`homeassistant/`](../homeassistant/); the design rationale is in
[docs/design/homeassistant-addon.md](design/homeassistant-addon.md).

## Requirements and scope

- **Home Assistant OS** or **Home Assistant Supervised**. Add-ons require the
  Supervisor, so **HA Container** and **HA Core** cannot install them — on those, run
  the application with the project's [`docker-compose.yml`](../docker-compose.yml) or a
  native package instead (see [INSTALL.md](INSTALL.md)).
- A host architecture of **`aarch64`** (64-bit Pi 4/5, most HAOS installs) or
  **`amd64`**. 32-bit `armv7`/`armhf` (Pi 3 and older 32-bit HAOS) is **not supported** —
  the add-on won't appear in the store on those hosts. Use a 64-bit host (Pi 4/5, a
  64-bit OS on a Pi 3, or an x86 box).
- An RS-485 link to the panel: a USB-RS485 adapter on the Home Assistant host, or a
  serial-over-ethernet adapter on the LAN. See
  [RS-485 connectivity](hardware-rs485-connectivity.md) for wiring.

## Install

1. **Settings → Add-ons → Add-on Store**.
2. **⋮ → Repositories**, add `https://github.com/iainchesworth/aqualink-automate`.
3. Install **Aqualink Automate**, configure it, then **Start**.

The repository offers two channels: **Aqualink Automate** (stable) and **Aqualink
Automate (Edge)** (beta — tracks the newest prerelease). Install whichever you prefer;
Edge is for trying the latest before it's stable. Don't run both against the same panel
at once — they'd contend for the serial device and MQTT topics. *(Pre-1.0, no stable
release exists yet, so both are marked experimental and track the newest release.)*

## Configuration

The options form maps directly onto the application's settings (full reference:
[Configuration reference](configuration.md)). Key choices:

- **Serial** — `serial_mode: usb` shows a device picker (choose a `/dev/serial/by-id/…`
  entry so it survives reboots) mapped to `--serial-port`; `serial_mode: network` takes
  a `host:port` (`--remote-serial-port`) with `remote_protocol` selecting
  `rfc2217` / `rawtcp` / `plain`.
- **MQTT** — `mqtt_mode: auto` (recommended) discovers the broker Home Assistant already
  uses (e.g. the Mosquitto add-on) through the Supervisor, so you enter **no** MQTT
  credentials. `manual` exposes the broker fields; `disabled` turns MQTT off.
  `home_assistant_discovery` publishes auto-discovery so your equipment appears as
  Home Assistant entities.
- **Web UI** — served through Home Assistant **ingress**: it appears in the sidebar and
  via **Open Web UI**, secured by your Home Assistant login and **not exposed on the
  LAN**. Because ingress provides authentication, the app's own auth is left off. For
  direct LAN access (e.g. a wall tablet), assign a host port to `80/tcp` in the add-on's
  **Network** panel — that port is unauthenticated, so firewall it.
- **Deferring to Home Assistant** — the app's own auth, history, and scheduler are
  **off by default**: Home Assistant provides login (via ingress), Recorder/History,
  and automations. `enable_history` / `enable_scheduler` turn the app's versions on
  (persisted under `/data`). UI preferences and an equipment cache are always persisted
  under `/data`.
- **Logging** — `log_level` (`info` / `debug` / `trace`); output appears in the add-on's
  **Log** tab (the app logs to stdout).

## Notes

- The bundled **Matter** bridge is disabled in the add-on — Home Assistant is already a
  Matter controller, and equipment is surfaced through MQTT discovery instead.
- The add-on version tracks the application release version; the image it runs is the
  matching published release.
