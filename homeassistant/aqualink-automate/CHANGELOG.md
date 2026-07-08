# Changelog

## 0.12.0-beta.5

- Initial Home Assistant add-on (Phase 1: headless + MQTT).
- Wraps the published `ghcr.io/iainchesworth/aqualink-automate` image (`aarch64` +
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
