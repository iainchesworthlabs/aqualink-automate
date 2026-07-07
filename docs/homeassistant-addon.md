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
  **`amd64`**. 32-bit `armv7`/`armhf` (Pi 3 and older 32-bit HAOS) is not yet built.
- An RS-485 link to the panel: a USB-RS485 adapter on the Home Assistant host, or a
  serial-over-ethernet adapter on the LAN. See
  [RS-485 connectivity](hardware-rs485-connectivity.md) for wiring.

## Install

1. **Settings → Add-ons → Add-on Store**.
2. **⋮ → Repositories**, add `https://github.com/iainchesworth/aqualink-automate`.
3. Install **Aqualink Automate**, configure it, then **Start**.

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
- **Web UI** — reachable on the mapped host port (default `8129`) via the add-on's
  **Open Web UI** button. `api_auth_token` optionally protects it. (A later release
  embeds the UI in the Home Assistant sidebar via ingress.)

## Notes

- The bundled **Matter** bridge is disabled in the add-on — Home Assistant is already a
  Matter controller, and equipment is surfaced through MQTT discovery instead.
- The add-on version tracks the application release version; the image it runs is the
  matching published release.
