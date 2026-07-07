# Changelog

## 0.12.0-beta.4

- Initial Home Assistant add-on (Phase 1: headless + MQTT).
- Wraps the published `ghcr.io/iainchesworth/aqualink-automate` image (`aarch64` +
  `amd64`).
- USB-RS485 (device picker via `uart`) and serial-over-ethernet transports.
- Zero-config MQTT: auto-discovers the Home Assistant broker via the Supervisor
  services API, with Home Assistant MQTT discovery on by default.
- Web UI exposed on a mapped host port (sidebar ingress is a later phase).
