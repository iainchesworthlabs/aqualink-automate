# Changelog

## 0.12.0-beta.4

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
