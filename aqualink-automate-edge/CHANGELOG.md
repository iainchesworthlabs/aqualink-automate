# Changelog

## 0.13.0-beta.1 (Edge only)

- **Repository moved.** The add-on now lives at `https://github.com/iainchesworthlabs/aqualink-automate` (was `iainchesworth/aqualink-automate`). If you added it as a custom repository, remove the old entry and add the new URL, then reinstall/update to keep receiving updates — the old repository will not publish further releases.
- **Home Assistant companion package.** A ready-to-use bundle for this integration — automation blueprints (activity scenes, spa readiness, and more), a script blueprint, a helpers package, and a pre-built dashboard. Enable the opt-in **Install companion package** option (off by default) to have it synced into Home Assistant's `blueprints/` folder automatically on every start/update, or install it manually from `/homeassistant/` on your running instance.

## 0.12.0-beta.9

- **The add-on now reaches "Running", and the Watchdog no longer kill-loops it.** The container health probe inherited from the app image still pointed at the app's default port `80`, but the add-on runs the app on `8099` (since 0.12.0-beta.6) — so the container never reported healthy: the add-on sat in "Starting" forever and, with **Watchdog** enabled, was restarted every ~80 seconds. The probe now targets the add-on's real port. If you disabled the Watchdog toggle to work around this, it is safe to turn back on.
- **Storage map renamed `addon_config` → `app_config`**, following the Supervisor's
  add-ons→apps rename (silences the "uses legacy map type 'addon_config'" deprecation
  warning logged on every store refresh). Same mount, same paths — but the new name
  needs Supervisor 2026.07.1 (2026-07-08) or newer; an older Supervisor will not list
  or update the add-on until it self-updates.

## 0.12.0-beta.8

- **Manual MQTT mode is configurable again.** The broker fields (`mqtt_host`, `mqtt_username`, `mqtt_password`) now appear in the options form — Home Assistant hides optional fields that carry no default, so `mqtt_mode: manual` previously had nowhere to enter the broker. An empty host in manual mode is now rejected with a clear message.
- **Jandy emulation defaults to `auto`.** `jandy_device_type` now defaults to `auto`, standing up the full default device set (OneTouch + IAQ + Serial Adapter) instead of a single OneTouch device (which suppressed IAQ status and Serial Adapter commands). Pick a specific type to restrict emulation to one device; `jandy_device_id` is now an optional per-type bus-address override (blank = the type's default, e.g. OneTouch → 0x41).

## 0.12.0-beta.7

- **Serial is one "Serial protocol" field.** Pick `usb`, `rfc2217`, or `rawtcp`, then enter the device path or `host:port`. Both fields are required, so Home Assistant always shows them (beta.6's optional field was hidden from the form). `plain` is dropped — it is the same transport as `rawtcp`.
- **Automatic device identity.** A stable, unique Home Assistant device id is generated on first start and stored under `/data`, so your equipment stays the same device across restarts and updates — nothing to configure.

## 0.12.0-beta.6

- **Serial is now one field.** `serial_port` takes either a device path (`/dev/serial/by-id/…`, or any non-standard path) or a network `host:port`, auto-detected — no more mode selector. This also fixes a "Device '' does not exist" error when saving.
- **MQTT TLS certificates.** New `mqtt_tls_skip_verify` and `mqtt_ca_cert` / `mqtt_client_cert` / `mqtt_client_key` options (filenames in Home Assistant's `/ssl` share).
- Direct-port and internal port moved from `80` to **`8099`**; the add-on gains a read-write `addon_config` mount.
- **Conformance with the current Home Assistant app schema:** modern `map` syntax, optional fields carry no default, and the image build uses `BUILD_VERSION` in the Dockerfile (dropped the deprecated `build.yaml`).

## 0.12.0-beta.5

- Initial Home Assistant add-on (Phase 1: headless + MQTT).
- Wraps the published `ghcr.io/iainchesworthlabs/aqualink-automate` image (`aarch64` +
  `amd64`).
- USB-RS485 (device picker via `uart`) and serial-over-ethernet transports.
- Zero-config MQTT: auto-discovers the Home Assistant broker via the Supervisor
  services API, with Home Assistant MQTT discovery on by default.
- Web UI via Home Assistant ingress (sidebar, behind HA login, not LAN-exposed),
  with an opt-in direct LAN port in the Network panel.
- App auth, history, and scheduler default off (defer to HA login / Recorder /
  automations); each is an opt-in toggle. `log_level` surfaced.
- Installs from a **prebuilt per-arch image** (`homeassistant-{arch}`) published by
  CI — a quick download, no on-device build.
- Localised options form: every setting has a friendly label and help text
  (`translations/en.yaml`).
- Two channels: **Aqualink Automate** (stable) and **Aqualink Automate (Edge)**
  (beta, tracks the newest prerelease).
- Liveness watchdog: Home Assistant restarts the add-on if the app stops answering
  its `/api/health` probe (e.g. a hung poll loop).
- Optional **Require web UI login** (`enable_auth`) — protects the UI when you publish
  the direct LAN port; bootstraps an admin on first enable.
- Add-on icon + logo (the app's water-drop mark).
