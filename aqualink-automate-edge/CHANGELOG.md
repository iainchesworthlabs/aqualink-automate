# Changelog

## 0.16.0-beta.1

- **Lights now show up in MQTT and Home Assistant.** The separate colour-light controller (not the aux relay that switches it) publishes its own status sensor and mode, read-only — a light can't be actuated directly, so it no longer shows as a broken toggle either here or in the web UI.
- **A spa spillover on AUX 3 is recognised again (and AUX 1 is no longer mistaken for a cleaner).** The panel's S1 option DIP-switch byte was being read as a single flag instead of bit-by-bit, so the spillover rewrite never fired on any panel, while a spillover-only panel had its AUX 1 wrongly rewritten instead.
- **Shutting down can no longer abort the process on a failed diagnostic log.** A handful of destructors logged their own teardown unguarded; an out-of-memory or formatting failure there would abort instead of exiting cleanly.

## 0.15.0-beta.1

- **Force an auxiliary present or absent, overriding live detection.** The Settings page's Device Names card gains an "Other aux slots" tab, grouped by power centre, so you can manually correct a relay that never replies on the wire or undo a misdetected phantom — no equipment-cache editing required.
- **Clear every auto-detected auxiliary and re-run discovery** from a new Diagnostics "Auxiliary Discovery" card, for recovering from a bad detection state without a restart.
- **A repeated equipment command no longer fails with a false "Service Unavailable".** Setting the chlorinator output (or toggling a device, changing circulation/heater/setpoint mode) while an earlier command was still landing on the panel could report a hard failure even though the first command was still in flight and would go on to succeed. These now report a clear, transient "busy, retry shortly" instead.
- **A clearer warning when Home Assistant gets a new device identity.** If the add-on generates a fresh device ID (fresh install, host migration, or switching between the stable and edge channels), the log now explains that previous Aqualink entities are orphaned and how to remove them, instead of leaving you to work it out from stale "offline" entities.

## 0.14.0-beta.1

- **Chlorinator control on iAQ (AqualinkTouch) panels works again — and fast.** Commands were accepted unconditionally and fired a fixed sequence of button presses that could land on the wrong screen, so a percentage change could report success while the panel never received it. The app now walks to the chlorinator page verifying each step instead of guessing.
- **Pool and spa chlorinator outputs are now set independently.** Home Assistant gains a **Spa Output** number beside the existing setpoint (renamed **Pool Output**), and that control's value template no longer snaps back to 0 whenever the cell is idle.
- **Phantom auxiliaries no longer appear.** Auxiliary discovery is now bounded by the model the panel itself reports, so a single-power-centre panel no longer grows a full set of auxiliaries that don't exist. Existing phantom entries are cleaned up automatically.
- **Captures now land somewhere you can reach.** On-demand captures are written to the add-on's `app_config` map — browsable with the Samba or File editor add-ons — instead of inside the container, where they were lost on restart or update.
- **The SWG tile explains a 0% reading**, with a new **Output State** sensor (and **Target %**) so automations can tell "idle" from "off" or "broken" instead of guessing from the percentage.
- **Captures can be downloaded from the web UI's Diagnostics page** — no shell on the host needed.

## 0.13.0-beta.1

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
