# MQTT and Home Assistant integration

*For pool owners running a release build who want to publish telemetry, send commands, and auto-discover entities in Home Assistant. This is the deep-dive companion to the [configuration reference](configuration.md); the bare option table lives there.*

This guide covers the MQTT control and telemetry surface: how to connect, the complete topic scheme, command and response topics, and Home Assistant auto-discovery — including a worked `Filter Pump` example and copy-paste mosquitto and Home Assistant setup.

**On Home Assistant OS / Supervised?** The [Home Assistant add-on](homeassistant-addon.md) wires all of this up for you — it auto-discovers the broker through the Supervisor and enables discovery by default, so you can skip the manual MQTT setup below.

## Overview

MQTT is **off by default**. It is opt-in via the `--mqtt` flag; with MQTT disabled the integration is never created. Once enabled, the application connects to your broker and publishes:

- System status, version, and equipment summary.
- Pool temperatures, chemistry, circulation, and configuration.
- Per-body temperatures and a per-device state for every pump, heater, chlorinator, and auxiliary.
- Statistics (message counts, bandwidth, latency, serial health).
- A consolidated alert document.

It also subscribes to a command topic tree so you can change setpoints, toggle devices, drive the chlorinator, and switch circulation modes.

Home Assistant auto-discovery is a separate opt-in on top of MQTT (`--home-assistant`, which requires `--mqtt`). When enabled, the application publishes a single retained discovery payload that registers one Home Assistant device with all of its entities — temperature sensors, setpoints, chemistry sensors, mode sensors, alert binary sensors, and one entity per discovered pool device.

**Note:** Every option below has a config-file equivalent. The config-file key is the option's long name **without** the leading dashes (for example `--mqtt-host` becomes `mqtt-host` in the file). The config file is a flat INI with no sections. See [INSTALL.md](INSTALL.md) for running with a config file or under Docker.

## Connection and CLI options

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--mqtt` | | bool | `false` | Enable the MQTT client. All other MQTT options are inert without this. |
| `--mqtt-host` | | string | `localhost` | Broker hostname or IP address. |
| `--mqtt-port` | | uint16 | `1883` | Broker port. Switches to `8883` automatically when `--mqtt-tls` is set and no port was given explicitly (see below). |
| `--mqtt-tls` | | bool | `false` | Enable TLS/SSL for the broker connection. |
| `--mqtt-ca-cert` | | string | (none) | CA certificate file used to verify the broker. |
| `--mqtt-client-cert` | | string | (none) | Client certificate for mutual TLS. Requires `--mqtt-client-key`. |
| `--mqtt-client-key` | | string | (none) | Client private key for mutual TLS. Requires `--mqtt-client-cert`. |
| `--mqtt-tls-skip-verify` | | bool | `false` | Skip broker certificate verification. Not for production (see [Security](#security)). |
| `--mqtt-client-id` | | string | auto | MQTT client identifier. Auto-generated as `aqualink-<8 hex chars>` when empty. |
| `--mqtt-username` | | string | (none) | Username for broker authentication. |
| `--mqtt-password` | | string | (none) | Password. Sent only alongside a username (see below). |
| `--mqtt-prefix` | | string | `aqualink` | Base topic prefix prepended to every published and subscribed subtopic. |
| `--mqtt-status-interval` | | uint32 | `5` | Seconds between system, pool, and device status publishes. |
| `--mqtt-stats-interval` | | uint32 | `10` | Seconds between statistics publishes. |
| `--mqtt-on-change` | | bool | `true` | Publish immediately on data changes (250 ms debounce) in addition to the periodic interval. |
| `--home-assistant` | | bool | `false` | Enable Home Assistant MQTT discovery. Requires `--mqtt`. |
| `--ha-discovery-prefix` | | string | `homeassistant` | Discovery topic prefix Home Assistant listens on. |
| `--ha-device-id` | | string | derived | Home Assistant device identifier. Requires `--home-assistant`. Defaults to `aqualink_<slug(prefix)>`. |

### TLS port switch

If you set `--mqtt-tls` and do **not** pass `--mqtt-port` explicitly, the default port changes from `1883` to `8883`. Passing `--mqtt-port` always wins.

```bash
# TLS on, port left implicit -> connects on 8883
aqualink-automate --mqtt --mqtt-host broker.example.com --mqtt-tls

# TLS on, explicit port -> connects on 8884
aqualink-automate --mqtt --mqtt-host broker.example.com --mqtt-tls --mqtt-port 8884
```

### Dependency rules

These are enforced at startup; violating one is a configuration error that stops the program:

- `--mqtt-client-cert` and `--mqtt-client-key` are mutually required — set both or neither.
- `--home-assistant` requires `--mqtt`.
- `--ha-device-id` requires `--home-assistant`.

### Auth and password handling

- The password is sent **only when a username is also set**. A password configured without `--mqtt-username` is omitted from the connection and a warning is logged.
- If a password is set but `--mqtt-tls` is `false`, a warning logs that the credentials will be transmitted in cleartext.

### Derived identifiers

- When `--mqtt-client-id` is empty, the client id is generated as `aqualink-<8 hex chars>`.
- When `--ha-device-id` is empty, it is derived as `aqualink_<slug of the topic prefix>`. With the default prefix `aqualink`, the device id is `aqualink_aqualink`.

## Security

**Security:** MQTT credentials are cleartext without TLS. When you set `--mqtt-username` and `--mqtt-password` but leave `--mqtt-tls` off, the username and password cross the network unencrypted. For any broker reachable beyond localhost, enable `--mqtt-tls` so credentials and telemetry are protected.

**Security:** `--mqtt-tls-skip-verify` disables certificate validation and is not for production. With it set, the client establishes TLS but does not verify the broker's identity, so the connection is vulnerable to interception. Use it only for local testing against a self-signed broker.

When TLS verification is left on (the default once `--mqtt-tls` is set), the client:

- Negotiates TLS 1.2 as the minimum, disabling SSLv2, SSLv3, TLS 1.0, and TLS 1.1.
- Loads the system trust store (plus `--mqtt-ca-cert` when supplied).
- Performs peer verification with RFC 6125 hostname checking and sends SNI.

## Topic scheme

Every topic is built by prepending `<prefix>/` to a subtopic, where `<prefix>` is `--mqtt-prefix` (default `aqualink`). An empty prefix produces the bare subtopic. The examples below assume the default prefix `aqualink`.

### State topics (retained)

These carry current state and are published **retained**, so a subscriber that connects later immediately receives the last known value.

| Topic | Contents |
| --- | --- |
| `aqualink/system/status` | `{ "online": true, "uptime_seconds": ... }` |
| `aqualink/system/version` | Application version and build date. |
| `aqualink/system/equipment` | `model_number`, `firmware_revision`. |
| `aqualink/pool/temperatures` | `air`, `pool`, `spa`, `freeze_protect`, `pool_setpoint`, `spa_setpoint`. |
| `aqualink/pool/chemistry` | `orp.value_mv`, `ph.value`, `salt.value_ppm`. |
| `aqualink/pool/circulation` | `mode`, `spa_mode`, `clean_mode`, `pool_configuration`, optional `active_body`, optional `timeout_remaining_seconds`. |
| `aqualink/pool/configuration` | `pool_type`, `system_board`, `equipment_mode`, `date`, `time`. |
| `aqualink/body/{body}/temperature` | One per body of water: `current`, `setpoint`, `is_active`. `{body}` is the lowercased body name. |
| `aqualink/device/{slug}` | Full JSON status blob for one device (see below). |
| `aqualink/ha/{category}_{slug}` | Short single-string state for one device (see below). *(only when `--home-assistant` is enabled)* |
| `aqualink/status/availability` | `online` / `offline` availability. Retained, and used as the Last Will (see below). *(only when `--home-assistant` is enabled)* |
| `aqualink/alert/state` | Consolidated alert document with a boolean per alert condition. |

### Statistics topics (not retained)

Statistics are published on the `--mqtt-stats-interval` cadence and are **not retained** — they are point-in-time samples, so a late subscriber waits for the next interval rather than receiving a stale snapshot.

| Topic | Contents |
| --- | --- |
| `aqualink/statistics/messages` | Per-message-type counts. |
| `aqualink/statistics/bandwidth` | Read/write byte totals and utilisation averages. |
| `aqualink/statistics/latency` | Serial read/write and message-processing latency snapshots. |
| `aqualink/statistics/serial` | Error rate, overflow/underflow, transmission failures, write-queue depth. |

### Availability and Last Will

The availability topic and the MQTT Last Will are only set up when Home Assistant discovery is enabled (`--home-assistant`). With `--mqtt` alone there is no Last Will registered and the availability topic is never published.

When `--home-assistant` is enabled, `aqualink/status/availability` is retained and carries `online` or `offline`. It doubles as the MQTT Last Will: the broker is told to publish `offline` (retained) if the connection drops ungracefully. The application publishes `online` on every successful connect, so the device shows available in Home Assistant as soon as it connects and unavailable if it disappears.

### Per-device JSON blob and short-string state

Each discovered device produces two retained topics. The `{slug}` is the device label lowercased with spaces, hyphens, and dots collapsed to underscores — so the label `Filter Pump` becomes the slug `filter_pump`.

- `aqualink/device/{slug}` — the **full JSON status blob** for the device, plus a `type` field (`aux`, `heater`, `pump`, or `chlorinator`) and an optional `body_of_water`. Template-based Home Assistant entities (for example the chlorinator's generating-% and boost entities) read this topic.
- `aqualink/ha/{category}_{slug}` — a **short single-string state** namespaced by category, so two devices sharing a label in different categories do not collide. Simple Home Assistant switch and sensor entities read this topic. `{category}` is one of `aux`, `heater`, `pump`, `chlorinator`.

### Worked example: prefix `aqualink`, device `Filter Pump`

With the default prefix and a pump labelled `Filter Pump` (category `pump`, slug `filter_pump`):

| Topic | Retained | Carries |
| --- | --- | --- |
| `aqualink/device/filter_pump` | yes | Full JSON status blob, with `"type": "pump"`. |
| `aqualink/ha/pump_filter_pump` | yes | Short state string (`Running` or `Off`). |
| `aqualink/command/device/filter_pump` | n/a (subscribe) | Accepts `ON` / `OFF`. |

Subscribe to everything under the prefix to watch it live:

```bash
mosquitto_sub -h localhost -t 'aqualink/#' -v
```

## Command and response topics

On connect, the integration subscribes to `aqualink/command/#` (QoS 0). Publish a message to a command topic to act; the command name is the part of the topic after `command/`.

Inbound payloads are parsed as JSON. If a payload is not valid JSON, the raw string is wrapped as `{ "raw": "<payload>" }` — which is why the plain-text commands below (a bare number, or `ON` / `OFF`) work as well as JSON. Numeric values are range-checked and rejected when out of range.

| Command topic | Payload | Effect |
| --- | --- | --- |
| `aqualink/command/status` | (any) | Republish all status topics. |
| `aqualink/command/refresh` | (any) | Force a status refresh; publishes a response. |
| `aqualink/command/device` | JSON `{ "device_id": ..., "action": ... }` | Toggle a device by UUID or label. This command **always toggles**; the `action` value is ignored (echoed back in the response only). For explicit on/off, use `command/device/{slug}` with `ON`/`OFF`. |
| `aqualink/command/device/{slug}` | `ON` or `OFF` | Drive one discovered device on/off. |
| `aqualink/command/setpoint` | JSON `{ "target": "pool"\|"spa", "temperature": <celsius> }` | Set a body's temperature. Always Celsius (stable API contract). |
| `aqualink/command/setpoint/pool` | plain number | Set the pool setpoint. Interpreted in the `temperature_units` display preference's unit (Celsius by default) — these are the HA number entities' command topics and must match their declared unit. |
| `aqualink/command/setpoint/spa` | plain number | Set the spa setpoint. Same unit rule as `setpoint/pool`. |
| `aqualink/command/chlorinator/percentage` | number `0`–`100` | Set the chlorinator output percentage. |
| `aqualink/command/chlorinator/boost` | `ON` or `OFF` | Enable or disable chlorinator boost. |
| `aqualink/command/circulation/mode` | `pool`, `spa`, or `spillover` | Set the circulation mode. |

**Note:** The `device/{slug}`, `chlorinator/*`, and per-device commands are registered once the matching pool devices are discovered on the wire. Until a device is seen, its command topic has no handler.

### Responses

The `refresh`, `device`, and `setpoint` commands publish a response to `aqualink/response/{command}` as JSON (the `status` command republishes all status topics but publishes no response):

```json
{
  "command": "setpoint",
  "status": "success",
  "timestamp": 1718409600,
  "target": "pool",
  "temperature": 28.5
}
```

The `status` field reflects the dispatch outcome (for example `success`, `device_not_found`, `no_serial_adapter`, `unknown_equipment_type`, `invalid_value`).

## Home Assistant discovery

When `--home-assistant` is set, the application publishes a single retained discovery payload to:

```
<ha-discovery-prefix>/device/<ha-device-id>/config
```

With defaults this is `homeassistant/device/aqualink_aqualink/config`. This is the single-device, bundled-components form: one payload registers the whole device and all its entities. Its top-level keys are:

- `dev` — the device object.
- `o` — the origin (this application).
- `availability` — points at `aqualink/status/availability` with `online` / `offline`.
- `cmps` — the components map; each member is one entity carrying a `p` (platform) field.

### Device object

The `dev` object identifies the single Home Assistant device:

- `identifiers`: `[<ha-device-id>]`
- `name`: `aqualink-automate`
- `manufacturer`: `Jandy / Zodiac`
- `sw_version`: the project version
- `model` and `hw_version`: filled from the panel's equipment model and firmware when known.

Each entity's `unique_id` follows the pattern `<ha-device-id>_<suffix>`.

### Static entity types

These entities are always published:

| Entity | Platform | Notes |
| --- | --- | --- |
| Pool / Spa / Air / Freeze Protect temperatures | `sensor` | `device_class: temperature`, unit degC. |
| Pool / Spa setpoint | `number` | Follows the `temperature_units` preference: min 15, max 41, step 0.5, °C (default), or min 59, max 106, step 1, °F. Command on `command/setpoint/{pool,spa}`, interpreted in the declared unit. Changing the preference republishes discovery so HA picks up the new unit/range. (Temperature *sensors* stay declared °C — their payload is Celsius and HA converts sensors to its own unit system.) |
| ORP | `sensor` | `device_class: voltage`, unit mV. |
| pH | `sensor` | `device_class: ph`. |
| Salt Level | `sensor` | unit ppm. |
| Circulation Mode | `sensor` | From `pool/circulation`. |
| Equipment Mode | `sensor` | From `pool/configuration` (Normal / Service / TimeOut). |
| Spa Mode | `binary_sensor` | `payload_on: true` / `payload_off: false`. |
| Clean Mode | `binary_sensor` | `payload_on: true` / `payload_off: false`. |
| Circulation Mode (select) | `select` | Dual-body systems only. Options `Pool`, `Spa`, `Spillover`. Command on `command/circulation/mode`. |
| Uptime | `sensor` | `device_class: duration`, `entity_category: diagnostic`. |
| Alerts (6) | `binary_sensor` | `device_class: problem`. See below. |

### Alert binary sensors

Six `binary_sensor` entities (`device_class: problem`, `payload_on: true` / `payload_off: false`) read the consolidated document at `aqualink/alert/state`:

| Key | Display name |
| --- | --- |
| `chlorinator_fault` | Chlorinator Fault |
| `chlorinator_warning` | Chlorinator Warning |
| `salt_low` | Salt Low |
| `service_mode` | Service Mode |
| `serial_comms_loss` | Serial Comms Loss |
| `temperature_stale` | Temperature Stale |

`temperature_stale` raises when the filter pump is running but the active body's water temperature has not refreshed within `--temperature-staleness-threshold`; a reading going stale while the pump is off is expected (no flow past the sensor) and never raises.

### Dynamic device entities

One entity (or a small set) is published per discovered pool device:

| Device kind | Entity | State values |
| --- | --- | --- |
| Pump | `switch` | `state_on: Running`, `state_off: Off`. |
| Auxiliary | `switch` | `state_on: On`, `state_off: Off`. |
| Heater | `sensor` | `Off` / `Heating` / `Enabled`. |
| Chlorinator | `switch` + extras | `state_on: On`, `state_off: Off`, plus the entities below. |

Switch entities use `payload_on: ON` / `payload_off: OFF` on their command topic.

The chlorinator's extra entities read its JSON blob at `aqualink/device/{slug}`:

- `sensor` "Generating %" from `generating_percentage` (unit `%`).
- `sensor` "Boost Mode" from `boost_mode`.
- `sensor` "Health" from `chlorinator_health`.
- `number` "Generating Setpoint" — min 0, max 100, command on `command/chlorinator/percentage`.
- `switch` "Boost" — command on `command/chlorinator/boost`.

## Worked setup

This walks you from a fresh broker to discovered entities in Home Assistant.

### 1. Run a mosquitto broker

If you do not already have a broker, the simplest path is a local mosquitto with a minimal config:

```ini
# /etc/mosquitto/conf.d/aqualink.conf
listener 1883
allow_anonymous true     # local testing only; require auth + TLS for anything networked
```

```bash
mosquitto -c /etc/mosquitto/conf.d/aqualink.conf
```

### 2. Start aqualink-automate with MQTT and Home Assistant

```bash
aqualink-automate \
  --serial-port /dev/ttyUSB0 \
  --mqtt --mqtt-host 192.168.1.100 --mqtt-port 1883 \
  --home-assistant --ha-discovery-prefix homeassistant
```

The same settings as a config file (keys are the long names without dashes):

```ini
# Serial
serial-port = /dev/ttyUSB0

# MQTT
mqtt = true
mqtt-host = 192.168.1.100
mqtt-port = 1883

# Home Assistant
home-assistant = true
ha-discovery-prefix = homeassistant
```

```bash
aqualink-automate --config aqualink.conf
```

The shipped [`examples/config-serial.conf`](https://github.com/iainchesworth/aqualink-automate/blob/main/examples/config-serial.conf) and [`examples/config-network.conf`](https://github.com/iainchesworth/aqualink-automate/blob/main/examples/config-network.conf) already contain a working MQTT + Home Assistant block to copy from.

### 3. Enable the Home Assistant MQTT integration

In Home Assistant, add the **MQTT** integration and point it at the same broker (`192.168.1.100:1883`). With the discovery prefix left at the default `homeassistant`, the aqualink device and all its entities appear automatically — no YAML required. The device shows as **aqualink-automate** by **Jandy / Zodiac**.

### 4. Verify and control from the command line

Watch the live tree:

```bash
mosquitto_sub -h 192.168.1.100 -t 'aqualink/#' -v
```

Read the `Filter Pump` short state:

```bash
mosquitto_sub -h 192.168.1.100 -t 'aqualink/ha/pump_filter_pump'
```

Turn the `Filter Pump` on:

```bash
# Plain ON/OFF payload on the per-device command topic
mosquitto_pub -h 192.168.1.100 -t 'aqualink/command/device/filter_pump' -m 'ON'
```

Set the pool setpoint to 28.5 C two equivalent ways:

```bash
# Plain number on the per-target topic
mosquitto_pub -h 192.168.1.100 -t 'aqualink/command/setpoint/pool' -m '28.5'

# JSON on the generic setpoint topic
mosquitto_pub -h 192.168.1.100 -t 'aqualink/command/setpoint' -m '{"target":"pool","temperature":28.5}'
```

## Connection behavior

The MQTT client speaks MQTT 3.1.1 (protocol level 4), QoS 0 only, with a clean session and a 60-second keepalive. The CONNACK timeout is 10 seconds. On disconnect it reconnects with exponential backoff and jitter (initial 5 s, max 300 s). The outbound publish queue is capped at 1000 messages, dropping the oldest when full, with a 1 MB read buffer.

Live MQTT diagnostics — queue depth, reconnect attempts, published and dropped counts, and the last error — are available over HTTP at `GET /api/diagnostics/mqtt`. See [Usage and API](usage-and-api.md) for that endpoint and the HTTP control alternative to MQTT commands.

## See also

- [Configuration reference](configuration.md) — the canonical option table for every flag and config key.
- [Usage and API](usage-and-api.md) — `GET /api/diagnostics/mqtt` and the HTTP control surface.
- [INSTALL.md](INSTALL.md) — running with a config file or under Docker.
