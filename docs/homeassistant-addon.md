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

The add-on manifests live at the root of this repository
([`aqualink-automate/`](https://github.com/iainchesworthlabs/aqualink-automate/tree/main/aqualink-automate) and `aqualink-automate-edge/`, with
`repository.yaml`); the design rationale is in
[docs/design/homeassistant-addon.md](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/docs/design/homeassistant-addon.md).

## Requirements and scope

- **Home Assistant OS** or **Home Assistant Supervised**. Add-ons require the
  Supervisor, so **HA Container** and **HA Core** cannot install them — on those, run
  the application with the project's [`docker-compose.yml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/docker-compose.yml) or a
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
2. **⋮ → Repositories**, add `https://github.com/iainchesworthlabs/aqualink-automate`.
3. Install **Aqualink Automate**, configure it, then **Start**.

The repository offers two channels: **Aqualink Automate** (stable) and **Aqualink
Automate (Edge)** (beta — tracks the newest prerelease). Install whichever you prefer;
Edge is for trying the latest before it's stable. Don't run both against the same panel
at once — they'd contend for the serial device and MQTT topics. *(Pre-1.0, no stable
release exists yet, so both are marked experimental and track the newest release.)*

## Configuration

The options form maps directly onto the application's settings (full reference:
[Configuration reference](configuration.md)). Key choices:

- **Serial** — pick the **protocol** (`serial_protocol`): **`usb`** for a local device
  (a USB-RS485 adapter or a built-in UART → `--serial-port`), or **`rfc2217`** / **`rawtcp`**
  for a serial-to-ethernet adapter (→ `--remote-serial-port` with the matching transport;
  `rfc2217` suits most adapters, `rawtcp` is a plain TCP stream). Then set the **device or
  address** (`serial_port`): a path (`/dev/serial/by-id/…`, preferred; or any non-standard
  path) for `usb`, or a **`host:port`** for the network protocols.
- **MQTT** — `mqtt_mode: auto` (recommended) discovers the broker Home Assistant already
  uses (e.g. the Mosquitto add-on) through the Supervisor, so you enter **no** MQTT
  credentials. `manual` exposes the broker fields; `disabled` turns MQTT off.
  `home_assistant_discovery` publishes auto-discovery so your equipment appears as
  Home Assistant entities. A stable, unique device identifier is generated automatically
  on first start and persisted under `/data`, so your equipment stays the **same** Home
  Assistant device across restarts and add-on updates (no `ha-device-id` to set).
- **Web UI** — served through Home Assistant **ingress**: it appears in the sidebar and
  via **Open Web UI**, secured by your Home Assistant login and **not exposed on the
  LAN**. Because ingress provides authentication, the app's own auth is left off. For
  direct LAN access (e.g. a wall tablet), assign a host port to `8099/tcp` in the add-on's
  **Network** panel — that port is unauthenticated by default, so firewall it **or** turn
  on `enable_auth` (with `auth_username`/`auth_password`) to make the app enforce its own
  login there. The admin is bootstrapped on first enable; `/api/health` stays open so the
  watchdog keeps working.
- **Deferring to Home Assistant** — the app's own auth, history, and scheduler are
  **off by default**: Home Assistant provides login (via ingress), Recorder/History,
  and automations. `enable_history` / `enable_scheduler` turn the app's versions on
  (persisted under `/data`). UI preferences and an equipment cache are always persisted
  under `/data`.
- **Logging** — `log_level` (`info` / `debug` / `trace`); output appears in the add-on's
  **Log** tab (the app logs to stdout).
- **Home Assistant companion package** — `install_companion_package` (off by default)
  copies the add-on's bundled [companion blueprints](homeassistant-companion.md) straight
  into Home Assistant's `blueprints/` folder on every start, so they appear under
  **Settings → Automations & Scenes → Blueprints** with no import step. This requests a
  **read-write view of Home Assistant's own configuration directory** — broader than
  anything else the add-on asks for — so it stays opt-in; it only ever adds or updates
  files under `blueprints/` and never touches `configuration.yaml`. The companion
  package is also reachable without it: one-click import from the
  [docs](homeassistant-companion.md#blueprints), or the release zip.

## Serial captures

The web UI's **Diagnostics → Serial Recording** card records raw RS-485 traffic on
demand. In a container the app's default capture directory (`<cwd>/captures`) is
neither persisted nor reachable without a Docker shell, which is useless for the one
workflow that most needs the file afterwards — so `run.sh` pins it:

```
--capture-directory /config/captures
```

`/config` in the container is the add-on's **`app_config`** map, so captures land on
the host at `/app_configs/aqualink_automate/captures` (`/addon_configs/…` before
Supervisor 2026.07). That path is read-write, survives restarts and add-on updates,
and is browsable with the **Samba** or **File editor** add-ons — no Terminal & SSH,
no Protection-mode toggle, no `docker cp`.

`app_config` is used rather than `share` on purpose: it is already mapped (adding a
`share` map would ask the user for a broader, cross-add-on permission this feature
does not need), and captures stay in the add-on's own directory where they cannot
collide with another add-on's files and are removed when the add-on is uninstalled.

The Diagnostics page also lists the captures already on the server and offers a
**Download** button on each, so the usual path off the machine needs neither share
nor shell. Captures over 64 MiB are refused by the download route (the response is
buffered) — copy those from the share instead. Full detail:
[Serial record / replay](RECORD_REPLAY.md).

## Notes

- The bundled **Matter** bridge is disabled in the add-on — Home Assistant is already a
  Matter controller, and equipment is surfaced through MQTT discovery instead.
- The add-on version tracks the application release version; the image it runs is the
  matching published release.
