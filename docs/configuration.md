# Configuration and CLI reference

*For pool owners and operators running a release build of Aqualink Automate. This is the authoritative reference for every command-line flag and config-file key. Building from source lives in [INSTALL.md](INSTALL.md).*

Aqualink Automate is configured entirely at startup, through command-line flags and an optional configuration file. There is no runtime settings file that the app writes back to; what you pass at launch is what it runs with.

This page documents:

- [How options are provided](#how-options-are-provided)
- [Config file format and example](#config-file-format-and-example)
- The 13 option areas, one section each
- [Non-obvious behaviors and gotchas](#non-obvious-behaviors-and-gotchas)
- [Conflict and dependency matrix](#conflict-and-dependency-matrix)
- [Worked examples](#worked-examples)

## How options are provided

You can set every option two ways: on the command line, or as a key in a configuration file. The two are merged at startup.

### Command line vs config file

- **Command line:** options use their long name with a leading `--` (for example `--mqtt-host 192.168.1.100`). Six options also have a short form: `-c -d -h -v -s -r`.
- **Config file:** select a file with `--config <path>` (or `-c <path>`). Keys are the option **long name without dashes** (for example `mqtt-host`, not `--mqtt-host`). This long-name-equals-config-key equivalence holds for every option in this document.

### Precedence: the command line wins

Command-line arguments are parsed first; the config file is read second and merged into the same option set. Merging is **first-write-wins** — a value already set on the command line is never overwritten by the config file. So if an option appears in both places, the command-line value is used.

### Flat-INI config format

The config file is a flat INI file: one `key = value` per line, no `[sections]`. See [Config file format and example](#config-file-format-and-example) for the exact rules.

### Flag truthiness

Some options are *flags* — they take no value on the command line (for example `--debug`, `--mqtt`, `--disable-https`). In a config file, write them as `key = <truthy-or-falsy>`:

- **Truthy** (`true`, `yes`, `on`, `1`, case-insensitive) enables the flag.
- **Anything else** (including `false`, `no`, `0`, or an empty value) omits the flag entirely, leaving it at its default.

Non-flag options always take a value in the config file (`mqtt-host = 192.168.1.100`).

### Unknown arguments vs unknown keys

The two input paths treat unrecognized entries differently:

- **An unknown command-line argument is fatal.** The app logs the unrecognized argument and exits without starting.
- **An unknown config-file key only produces a warning.** The app logs `Config file: unrecognized option '<key>'` and continues. This means a typo in a config key silently has no effect — check the log if a setting does not take.

**Important:** If you pass `--config <path>` but the file does not exist, parsing fails and the app exits. A *missing* `--config` flag is fine (the app just runs with defaults and command-line flags).

### Inspecting the live option set

Every registered option, with its long name, short name, and description, is exposed at runtime as JSON at `GET /api/diagnostics/options`. Use it to confirm exactly which options a given build accepts.

## Config file format and example

Rules, as parsed:

- One `key = value` pair per line. Whitespace around the key and value is trimmed.
- Blank lines are ignored.
- Lines beginning with `#` are comments and are ignored.
- A line with no `=`, or with an empty key, is skipped with a warning.
- Keys are option long names without the leading dashes. There are no sections.
- Flag keys follow the [truthiness](#flag-truthiness) rule above.

Four ready-to-copy example files ship in the repo. Start from the one that matches your wiring, or from the complete reference if you want to see every option in one place:

| File | Use case |
| --- | --- |
| [examples/config-full.conf](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/examples/config-full.conf) | **Complete reference** — every option, grouped and commented with its default. Copy it and uncomment what you need. |
| [examples/config-serial.conf](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/examples/config-serial.conf) | A physical RS-485 serial port on the host. |
| [examples/config-network.conf](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/examples/config-network.conf) | A remote serial port over a serial-to-Ethernet adapter (RFC2217). |
| [examples/config-docker.conf](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/examples/config-docker.conf) | Running inside a Docker container (includes Matter and host-networking notes). |

A minimal physical-serial config looks like this:

```ini
# config.conf — physical RS-485 on the host
serial-port = /dev/ttyUSB0
baudrate = 9600

jandy-device-type = onetouch
jandy-device-id = 0x41

address = 0.0.0.0       # bind the web UI on all interfaces (see the security note below)
disable-https = true    # serve plain HTTP only
```

Run it with:

```bash
aqualink-automate --config config.conf
```

## App options

General application options. Lives in the `Application` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--config` | `-c` | string | none | Path to the configuration file. If supplied and missing, startup fails. |
| `--debug` | `-d` | flag | off | Enable debug-level logging. Conflicts with `--trace`. |
| `--trace` | | flag | off | Enable trace-level logging (more verbose than debug). Conflicts with `--debug`. |
| `--help` | `-h` | flag | off | Print help and exit. |
| `--version` | `-v` | flag | off | Print the version and exit. |
| `--version-detail` | | flag | off | Print detailed version info, including the git commit, and exit. |

`--help`, `--version`, and `--version-detail` print and exit immediately. They are checked *before* option validation, so `--help` still works even if the rest of the command line has a conflict (for example `--debug --trace --help`).

## Serial and connectivity options

How the app reaches the RS-485 bus — a physical port or a network bridge. Lives in the `Serial` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--serial-port` | `-s` | string | platform-specific (see below) | Physical serial port for the Aqualink connection. |
| `--baudrate` | | uint16 | `9600` | Serial port baud rate. |
| `--remote-serial-port` | `-r` | string | none | Remote serial port as `host:port` (a serial-to-Ethernet bridge). |
| `--rfc2217` | | flag | on | Use the RFC2217 transport on the remote port (the default for remote ports). |
| `--no-rfc2217` | | flag | off | Disable RFC2217 on the remote port; use a plain socket instead. |
| `--rawtcp` | | flag | off | Use raw TCP for the remote port. |

**Default serial port is platform-specific:** `/dev/ttyS0` on Linux/macOS, `COM1` on Windows.

Use a *physical* port unless `--remote-serial-port` is set; any non-empty `--remote-serial-port` switches the app to the network transport. RFC2217 is on by default for remote ports — pass `--no-rfc2217` for a plain socket, or `--rawtcp` for raw TCP.

**Note:** The physical serial line is fixed at 9600 baud, no parity, 8 data bits, 1 stop bit, no flow control (9600 8N1). `--baudrate` exists for completeness; standard Aqualink hardware runs at 9600.

Conflicts (each pair is mutually exclusive): `--serial-port` vs `--remote-serial-port`; `--rfc2217` vs `--rawtcp`; `--rfc2217` vs `--no-rfc2217`; `--no-rfc2217` vs `--rawtcp`.

## Web server and security options

The built-in HTTP/HTTPS server that hosts the web UI and the REST/WebSocket API. Lives in the `Web` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--address` | | string | `127.0.0.1` | Network interface to bind. Use `0.0.0.0` to expose on all interfaces. |
| `--http-port` | | uint16 | `80` | HTTP listen port. |
| `--https-port` | | uint16 | `443` | HTTPS listen port. |
| `--disable-content` | | flag | off | Disable serving rendered content; expose APIs only. |
| `--disable-http` | | flag | off | Disable the HTTP server. |
| `--disable-https` | | flag | off | Disable the HTTPS server. |
| `--cert` | | string | exe-relative `ssl/cert.pem` | TLS certificate (PEM). If absent at the default path, a unique self-signed cert is generated on first boot. |
| `--cert-key` | | string | exe-relative `ssl/key.pem` | TLS certificate private key (PEM). Auto-generated (key written `0600`) when absent at the default path; no key is shipped in the package. |
| `--cachain-cert` | | string | none | CA chain certificate (PEM). |
| `--doc-root` | | string | exe-relative `web` asset dir | Directory from which the web UI is served. |
| `--api-auth-token` | | string | none (no auth) | Require a bearer token on every API request and the WebSocket upgrade. Use a long random value (32+ chars); a token under 16 chars triggers a startup warning. |
| `--api-allowed-origin` | | string (repeatable) | none (check off) | Allowed `Origin` for the API/WebSocket. When set, a request/upgrade with a non-listed `Origin` is rejected with HTTP 403. |
| `--api-require-csrf-header` | | flag | off | Require the `X-Requested-With` header on state-changing API requests (CSRF mitigation). |
| `--insecure-no-auth` | | flag | off | Acknowledge an intentionally unauthenticated API on a non-loopback bind; downgrades the open-control-plane startup warning to an informational note. |

**Security:** `--address` defaults to `127.0.0.1` (loopback only) so a fresh install is not exposed to the network. Set `--address 0.0.0.0` deliberately when you want LAN access.

**Default paths are resolved at runtime, relative to the executable**, not printed as a literal string:

- `--cert`, `--cert-key`, and `--doc-root` default to assets bundled next to (or near) the installed binary.
- `--http-port` and `--https-port` have no fixed boost default; the app falls back to 80 and 443 respectively when you do not set them.

**API authentication is off by default** (`--api-auth-token` unset). When you set a token, every API request and the WebSocket upgrade must carry `Authorization: Bearer <token>` or the server answers `HTTP 401`. See [usage-and-api.md](usage-and-api.md) for how the token is sent on the wire.

`--cert` and `--cert-key` are mutually required: set both or neither.

Conflicts: `--disable-http` vs `--http-port`; `--disable-https` vs each of `--https-port`, `--cert`, `--cert-key`, `--cachain-cert`. Because the conflict checker [ignores defaulted options](#conflict-and-dependency-matrix), `--disable-https` is accepted even though `--cert`/`--cert-key` carry default values — only an *explicit* `--cert` collides with it.

## Authentication and identity options

The opt-in identity system — subjects, entitlements, and the policy engine (see [SECURITY.md](SECURITY.md) for the enforcement model and [auth-redesign.md](auth-redesign.md) for the design). Lives in the `Authentication` settings area. Slice 1 ships the framework only: login and user management arrive in a later slice, so the app cannot yet issue tokens itself.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--auth-mode` | | enum | `disabled` | Identity-system posture: `disabled` (historical behaviour — no identity resolution, every policy decision is Permit) or `enabled` (every request resolves to a subject and routes are gated by entitlements; anonymous callers get the deny-by-default Guest group). When `enabled`, the legacy `--api-auth-token` shared-token check is superseded — bearer credentials are interpreted by the subject resolver instead. |
| `--auth-state-dir` | | string | none (platform secure state dir) | Directory for authentication state (JWT signing keys and the user/group/API-key/session/kiosk stores). When unset, an `auth/` subdirectory of the platform's secure state directory is used (the same fallback chain as the generated TLS key). The directory is prepared owner-only; startup fails if no usable secure directory can be prepared. |
| `--jwt-access-ttl` | | uint32 | `15` | Access-token lifetime in minutes. Must be at least 1. |
| `--bootstrap-admin` | | string | none | Username of a first administrator to create at startup. Only fires when `--auth-mode enabled` **and** the user store is empty; idempotent (a no-op once any user exists). The password comes from `--bootstrap-admin-password-file` or `AQUALINK_BOOTSTRAP_ADMIN_PASSWORD` — never a CLI argument. |
| `--bootstrap-admin-password-file` | | string | none | File whose first line (newline-terminated) is the bootstrap administrator's password. Used only with `--bootstrap-admin`. |

An `--auth-mode` value other than `disabled`/`enabled`, or a `--jwt-access-ttl` of `0`, fails validation and stops startup.

**Headless first-admin bootstrap.** `--bootstrap-admin <username>` creates the first administrator without the interactive setup screen — useful for containers and unattended installs. It only acts when `--auth-mode enabled` and no users exist yet; with any user already on file it is silently a no-op (so a stale flag can never mint a second admin). The password is **never** passed on the command line, where it would be visible in the process table. Instead it is read from the first line of the file named by `--bootstrap-admin-password-file`, or, if that is unset, from the `AQUALINK_BOOTSTRAP_ADMIN_PASSWORD` environment variable. If `--bootstrap-admin` is given but no password can be found from either source, no administrator is created and a startup warning is logged. The password must satisfy the same policy as everywhere else (at least 12 characters).

## MQTT and Home Assistant options

Publishing pool state to an MQTT broker, optionally with Home Assistant auto-discovery. Lives in the `MQTT` settings area. This is a summary — for the full topic layout, entity types, and discovery payloads, see the [MQTT and Home Assistant guide](mqtt-home-assistant.md).

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--mqtt` | | flag | off | Enable the MQTT client. |
| `--mqtt-protocol-version` | | enum | `3.1.1` | MQTT wire-protocol version the client speaks: `3.1.1` or `5.0`. |
| `--mqtt-host` | | string | `localhost` | Broker hostname or IP. |
| `--mqtt-port` | | uint16 | `1883` | Broker port (auto-switches to `8883` with `--mqtt-tls` if you do not set a port). |
| `--mqtt-tls` | | flag | off | Enable TLS for the broker connection. |
| `--mqtt-ca-cert` | | string | none | TLS CA certificate path. |
| `--mqtt-client-cert` | | string | none | TLS client certificate path. Requires `--mqtt-client-key`. |
| `--mqtt-client-key` | | string | none | TLS client key path. Requires `--mqtt-client-cert`. |
| `--mqtt-tls-skip-verify` | | flag | off | Skip TLS certificate verification. |
| `--mqtt-client-id` | | string | none (auto-generated) | MQTT client identifier. |
| `--mqtt-username` | | string | none | Broker username. |
| `--mqtt-password` | | string | none | Broker password. |
| `--mqtt-prefix` | | string | `aqualink` | Base topic prefix for all published topics. |
| `--mqtt-status-interval` | | uint32 | `5` | Status publish interval (seconds). |
| `--mqtt-stats-interval` | | uint32 | `10` | Statistics publish interval (seconds). |
| `--mqtt-on-change` | | flag | on | Publish immediately when data changes (in addition to the periodic interval). |
| `--home-assistant` | | flag | off | Enable Home Assistant MQTT discovery. Requires `--mqtt`. |
| `--ha-discovery-prefix` | | string | `homeassistant` | Home Assistant discovery topic prefix. |
| `--ha-device-id` | | string | none (derived from prefix) | Device identifier for HA discovery. Requires `--home-assistant`. |

**Security:** Without `--mqtt-tls`, MQTT credentials (`--mqtt-username` / `--mqtt-password`) travel in cleartext. `--mqtt-tls-skip-verify` disables certificate validation and is not for production — it defeats the point of TLS.

When you enable `--mqtt-tls` and do not also set `--mqtt-port` explicitly, the port switches from 1883 to 8883 automatically.

Dependencies: `--mqtt-client-cert` and `--mqtt-client-key` are mutually required; `--home-assistant` requires `--mqtt`; `--ha-device-id` requires `--home-assistant`.

## Alerting options

Fault detection and notifications. Lives in the `Alerting` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--alert-salt-low-ppm` | | uint32 | `2600` | Salt PPM threshold for the salt-low alert. `0` disables the check. Valid range 0–6000. |
| `--alert-webhook-url` | | string | none | Webhook URL POSTed on every alert transition. Must be an absolute `http://` or `https://` URL. |
| `--alert-comms-timeout-seconds` | | uint32 | `60` | Seconds without a decoded message before the serial-comms-loss alert raises. Must be greater than 0. |

`--alert-salt-low-ppm` outside 0–6000, an `--alert-webhook-url` that is not an absolute http(s) URL, or `--alert-comms-timeout-seconds` of 0 each fail validation and stop startup.

## History options

Time-series persistence for trends. Lives in the `History` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--history-db` | | string | none | SQLite database path. Empty (the default) disables history; the read API returns `503`. |
| `--history-retention-days` | | uint32 | `90` | Days of samples to keep before a daily purge. Must be greater than 0. |
| `--history-flush-seconds` | | uint32 | `10` | Interval at which buffered samples flush to disk. Must be greater than 0. |

History is **off until you set `--history-db`**. With no database path, the history read API returns `503` and nothing is recorded.

## Equipment options

Pool topology and the discovered-equipment cache. Lives in the `Equipment` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--pool-configuration` | | enum | `auto` | Installation type. Accepted: `pool-only`, `spa-only`, `combo`, `dual`, `auto`. |
| `--equipment-cache-file` | | string | none | JSON cache of discovered equipment for an instant dashboard on restart. Empty disables caching. |
| `--temperature-staleness-threshold` | | uint | `600` | Age in seconds after which a reported temperature is flagged stale. |

`--pool-configuration` maps to the internal body model:

| Value | Meaning |
| --- | --- |
| `pool-only` | Single body. |
| `spa-only` | Single body (same model as `pool-only`). |
| `combo` | Dual body, shared equipment. |
| `dual` | Dual body, separate equipment. |
| `auto` | Autodetect from the bus (the default; the configuration is treated as not user-specified). |

## Scheduling and preferences options

Persistence for schedules and user preferences. These are two separate areas (`Scheduling` and `Preferences`).

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--schedules-file` | | string | none | JSON file persisting schedules. Empty disables the scheduler; its CRUD routes return `503`. |
| `--preferences-file` | | string | none | JSON file persisting user/admin preferences. Empty keeps preferences in-memory only. |

With no `--schedules-file`, the scheduler is disabled and the schedule CRUD routes return `503`. With no `--preferences-file`, preferences stay in memory and reset on restart — the preferences API still works for the session.

## Matter options

The Matter (smart-home) bridge sidecar. Lives in the `Matter` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--matter` | | bool | on | Enable the Matter bridge sidecar. Opt-out: `--matter false` (or `matter = false`) turns it off. |
| `--matter-storage-path` | | string | `/data/matter` | Directory the sidecar persists fabric/commissioning state to. |
| `--matter-status-port` | | uint16 | `8099` | Port the sidecar's status/QR server listens on. |
| `--matter-passcode` | | uint32 | `0` | Commissioning passcode (8 digits). `0` lets the sidecar generate and persist one. |
| `--matter-discriminator` | | uint16 | `0` | Commissioning discriminator (0–4095). `0` lets the sidecar generate and persist one. |

**Note:** `--matter` is an opt-out flag, and it behaves unlike the other on/off flags. It takes a value:

- Absent, or bare `--matter`, leaves the bridge **on** (the default).
- `--matter false` (command line) or `matter = false` (config) turns it **off**.

The C++ app does not host the Matter stack itself; it launches a Node.js sidecar. For commissioning and the Docker host-networking requirement, see [MATTER.md](MATTER.md).

## Jandy emulation options

Which Jandy/Zodiac devices the app emulates on the bus. Lives in the `Jandy` settings area.

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--jandy-disable-emulation` | | flag | off | Disable Jandy controller emulation entirely. |
| `--jandy-disable-presence-gating` | | flag | off | Disable RSSA presence-gating (do not auto-suppress an emulated Serial Adapter when a real one is suspected on the bus). |
| `--jandy-auto-startup` | | flag | off | Auto-detect the controller from the bus and choose what to emulate (overrides `--jandy-device-type`). |
| `--jandy-device-type` | | enum list | `onetouch iaq serialadapter` | Space-separated emulation types. Accepted: `rs_keypad`, `onetouch`, `iaq`, `pda`, `serialadapter`, `spasideremote`, `unknown` (case-insensitive). |
| `--jandy-device-id` | | hex list | per-type (see below) | Space-separated hex device IDs (`0xNN`), one per `--jandy-device-type` in order. |
| `--jandy-nav-password` | | string | `""` | 4-digit password for navigating password-protected Jandy menus. |

When `--jandy-device-type` is not given, the app emulates **OneTouch, IAQ, and SerialAdapter** (OneTouch for menu scraping, IAQ for status data, SerialAdapter for commands).

Default device IDs, assigned per type when you do not give an explicit `--jandy-device-id`:

| Type | Default ID |
| --- | --- |
| `onetouch` | `0x41` |
| `iaq` | `0xA1` |
| `rs_keypad` | `0x09` |
| `pda` | `0x61` |
| `serialadapter` | `0x48` |

Both `--jandy-device-type` and `--jandy-device-id` are multi-value: list types space-separated, then list IDs in the same order. Any type without a matching ID gets its default from the table above.

Conflicts and dependencies: `--jandy-device-id` requires `--jandy-device-type`; `--jandy-disable-emulation` conflicts with both `--jandy-device-type` and `--jandy-device-id`.

## Pentair options

The `Pentair` settings area is registered but **contributes no command-line options**. Pentair protocol support is auto-detected from the wire; there is nothing to configure here.

## Developer and replay options

Diagnostics, capture recording, and deterministic replay. Lives in the `Developer` settings area. The full record/replay workflow is in [RECORD_REPLAY.md](RECORD_REPLAY.md).

| Option | Short | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `--dev-mode` | | flag | off | Enable developer mode. Required to gate `--replay-filename`. |
| `--replay-filename` | | string | none | Replay file to source test data from. |
| `--record-serial` | | string | none | Record serial port data to a file for later replay. |
| `--decode-to-master` | | flag | off | Decode and log RS-485 frames addressed to the master (`0x00`); observe-only. |
| `--replay-frame-period` | | uint32 | `15` | Capture-replay inter-frame period in milliseconds. `0` = unpaced (as fast as possible). |
| `--replay-speed` | | double | `1.0` | Replay speed factor scaling `--replay-frame-period` (>1 faster, <1 slower). |
| `--profiler` | | enum | none | Profiling tool. Accepted: `tracy`, `uprof`, `vtune` (case-insensitive). |
| `--loglevel-<channel>` | | enum | none | Set the log level for one logging channel. One option per channel (see below). |

There is one `--loglevel-<channel>` option per logging channel. The 18 channels are:

```
certificates  coroutines  developer   devices     equipment  exceptions
main          messages    mqtt        navigation  options     platform
profiling     protocol    scraping    serial      signals     web
```

Each takes a severity (case-insensitive): `Trace`, `Debug`, `Info`, `Notify`, `Warning`, `Error`, `Fatal`. For example `--loglevel-protocol Debug`.

> **Audit is not a log channel.** The security audit trail (auth decisions,
> privileged actions) is a separate subsystem, not an operational log channel, so
> there is deliberately no `--loglevel-audit`. See
> [Security → Audit trail](SECURITY.md) for where audit events are routed.

### Logging sinks

Where operational log records are delivered is configured by the **Logging**
options. By default (`--log-sinks auto`) the destination is derived from the run
environment: an interactive terminal or a container gets the console; under
systemd the console additionally carries sd-daemon `<N>` priority prefixes so
`journalctl -u aqualink-automate -p warning` works.

| Option | Type | Default | Notes |
|---|---|---|---|
| `--log-sinks` | string | `auto` | `auto` (environment-derived) or a comma-separated list of `console`, `native`, `file`, `journald`. `native` adds the OS-native operational sink (syslog on POSIX, the Windows Event Log); `file` requires `--log-file`; `journald` is the structured native journal sink on Linux when libsystemd is present at runtime (elsewhere it falls back to console with priority prefixes). Under `auto`: `--log-file` implies the file sink, and under systemd (stderr is the journal) with libsystemd available, `journald` is used automatically. |
| `--log-syslog-facility` | enum | `daemon` | POSIX syslog facility for the general `native` sink: `daemon`, `user`, `local0`–`local7` (case-insensitive). Ignored on Windows. The audit sink always uses `authpriv` regardless. |
| `--log-format` | enum | `text` | Record format for the **console and file** sinks: `text` (human) or `json` (one JSON object per line, for container / SIEM pipelines). The native/syslog sink is always message-only. |
| `--log-file` | path | *(unset)* | Write logs to this file (enables the file sink). Rotated and size-bounded; on shutdown the active file is rotated into the kept set. |
| `--log-file-max-size` | bytes | `10485760` (10 MiB) | Rotate the log file when it would exceed this many bytes. |
| `--log-file-max-files` | count | `5` | Keep at most this many rotated log files (oldest deleted). |

Examples: `--log-sinks console,native --log-syslog-facility local0` (console plus
a syslog `local0` operational sink); `--log-file /var/log/aqualink/app.log
--log-format json` (console **and** a rotating JSON file, since `auto` adds the
file sink when a path is given); `--log-sinks console --log-format json` (JSON to
the console only, ideal behind a container log driver).

**Platform-native sinks.** On Linux, when `libsystemd.so.0` is present (it is on any
systemd host), `journald` delivers records to the journal with structured fields
(query with e.g. `journalctl AA_CHANNEL=Web`), and `auto` selects it when running
under systemd. libsystemd is loaded at runtime (`dlopen`), so it is neither a build
nor a hard runtime dependency — if it is absent, logging falls back to the console. On macOS the
native sink uses the unified logging system (`os_log`; view with Console.app or
`log show`). On **Windows**, the Event Log source is registered once, elevated,
with `--register-log-source` (and removed with `--unregister-log-source`); the
running service never needs elevation because the sink itself does not write the
registry. `--install-service` performs this registration too, so a service install is
clean in one step. The executable embeds a message-table resource matching the sink's
event IDs, so once the source is registered Event Viewer renders each entry verbatim;
until that one-time registration, events still appear in the Application log, just
wrapped in a generic description.

### Running as a Windows service

On Windows the application can run as a managed service (the analogue of the Linux
systemd unit): auto-start at boot, survives logoff, restarts on failure, and logs to the
Windows Event Log. Install it from an **elevated** prompt:

```
aqualink-automate.exe --install-service --config "C:\ProgramData\Aqualink Automate\aqualink-automate.conf"
```

`--install-service` registers an auto-starting (delayed) service running as the built-in
least-privilege `NT AUTHORITY\LocalService` account, sets restart-on-failure recovery,
and registers the Event Log source. Any flags you pass alongside `--install-service`
(`--config`, ports, `--mqtt-*`, …) are **baked into the service command line**, mirroring
the systemd unit's `ExecStart` — so that is where the service reads its configuration
from. If you omit `--config`, it defaults to
`%ProgramData%\Aqualink Automate\aqualink-automate.conf`, creating that folder and a
starter file. Use **absolute** paths: a service's working directory is `System32`.

Manage it with the standard tools — `sc start Aqualink-Automate` /
`Start-Service Aqualink-Automate`, `Stop-Service`, `Get-Service`, `sc qc Aqualink-Automate`
(shows the baked-in command line) — and remove it with an elevated
`aqualink-automate.exe --uninstall-service` (which also removes the Event Log source).
Run from a normal console (no `--install-service`) the application behaves exactly as
before — a foreground process that shuts down cleanly on Ctrl-C.

**Important — the big replay gotcha:** `--replay-filename` has hard dependencies. A bare `--dev-mode --replay-filename file.cap` **fails validation**. `--replay-filename` requires *all* of:

- `--dev-mode`
- `--profiler <tool>`
- *every* `--loglevel-<channel>` option (all 18 must be present)

It also conflicts with `--record-serial`. A complete, validation-passing replay invocation is in the [Worked examples](#developer-replay).

## Non-obvious behaviors and gotchas

- **The command line wins over the config file.** Merging is first-write-wins, and the command line is parsed first. A flag on the command line cannot be overridden by the config file.
- **A missing `--config` file is fatal; a missing `--config` flag is fine.** If you point `--config` at a path that does not exist, the app exits.
- **Unknown config keys are silently warned, not fatal.** A typo'd key has no effect and only shows up in the log. Unknown *command-line* arguments, by contrast, stop startup.
- **The real default is the settings-struct default, not always the boost default.** Each option is only written into settings when it is present on the command line or in the config; otherwise the value initialized in the settings struct's constructor applies. For most options these match, but `--serial-port` is a known case: the struct initializes `/dev/ttyUSB0`, yet the option carries a platform default (`/dev/ttyS0` / `COM1`) that wins because the option is always present via its default value.
- **`--matter` is opt-out and value-bearing.** Unlike the on/off flags, it takes a value; use `--matter false` to disable.
- **Disable flags invert their setting.** `--disable-http` and `--disable-https` turn the respective server off; both servers are on by default.
- **Empty-string settings disable subsystems.** `--history-db`, `--schedules-file`, and `--equipment-cache-file` each disable their feature when left empty (the default). History and scheduling routes then return `503`.
- **Defaulted options never trigger conflict or dependency checks.** See the matrix below — a check fires only when the relevant option is explicitly set, not when it is sitting at its default.

## Conflict and dependency matrix

Two validation rules run after parsing:

- **Conflict:** two options that cannot be set together. The check fires only when **both** are present **and neither is defaulted** — so two defaulted options, or one explicit plus one defaulted, never conflict.
- **Dependency:** option A requires option B. The check fires when A is present-and-not-defaulted while B is absent or sitting at its default.

| Area | Type | Options |
| --- | --- | --- |
| App | conflict | `--debug` ↔ `--trace` |
| Serial | conflict | `--serial-port` ↔ `--remote-serial-port` |
| Serial | conflict | `--rfc2217` ↔ `--rawtcp` |
| Serial | conflict | `--rfc2217` ↔ `--no-rfc2217` |
| Serial | conflict | `--no-rfc2217` ↔ `--rawtcp` |
| Web | conflict | `--disable-http` ↔ `--http-port` |
| Web | conflict | `--disable-https` ↔ `--https-port` |
| Web | conflict | `--disable-https` ↔ `--cert` |
| Web | conflict | `--disable-https` ↔ `--cert-key` |
| Web | conflict | `--disable-https` ↔ `--cachain-cert` |
| Web | dependency | `--cert` requires `--cert-key` (and vice versa) |
| MQTT | dependency | `--mqtt-client-cert` requires `--mqtt-client-key` (and vice versa) |
| MQTT | dependency | `--home-assistant` requires `--mqtt` |
| MQTT | dependency | `--ha-device-id` requires `--home-assistant` |
| Jandy | conflict | `--jandy-disable-emulation` ↔ `--jandy-device-type` |
| Jandy | conflict | `--jandy-disable-emulation` ↔ `--jandy-device-id` |
| Jandy | dependency | `--jandy-device-id` requires `--jandy-device-type` |
| Developer | conflict | `--replay-filename` ↔ `--record-serial` |
| Developer | dependency | `--replay-filename` requires `--dev-mode` |
| Developer | dependency | `--replay-filename` requires `--profiler` |
| Developer | dependency | `--replay-filename` requires every `--loglevel-<channel>` |

## Worked examples

### Remote serial over RFC2217

Connect to an Aqualink RS controller through a serial-to-Ethernet bridge, emulating a OneTouch device, with the web UI on all interfaces over plain HTTP:

```bash
aqualink-automate \
  --remote-serial-port 192.168.1.50:8899 \
  --rfc2217 \
  --jandy-device-type onetouch --jandy-device-id 0x41 \
  --address 0.0.0.0 \
  --disable-https
```

The same thing as a config file:

```ini
remote-serial-port = 192.168.1.50:8899
rfc2217 = true
jandy-device-type = onetouch
jandy-device-id = 0x41
address = 0.0.0.0
disable-https = true
```

```bash
aqualink-automate --config config-network.conf
```

### MQTT with Home Assistant discovery

Publish to a broker and advertise entities to Home Assistant:

```bash
aqualink-automate \
  --serial-port /dev/ttyUSB0 \
  --mqtt --mqtt-host 192.168.1.100 --mqtt-port 1883 \
  --mqtt-username pool --mqtt-password secret \
  --home-assistant --ha-discovery-prefix homeassistant
```

**Security:** without `--mqtt-tls`, those credentials cross the network in cleartext. For the full topic and entity reference, see [mqtt-home-assistant.md](mqtt-home-assistant.md).

### Developer replay

`--replay-filename` requires `--dev-mode`, `--profiler`, and every `--loglevel-<channel>`. This invocation passes validation (it mirrors the one the UI end-to-end tests boot with):

```bash
aqualink-automate \
  --dev-mode \
  --replay-filename test/fixtures/sample_session.cap \
  --profiler tracy \
  --http-port 18080 --address 127.0.0.1 --disable-https \
  --doc-root assets/web \
  --jandy-disable-emulation \
  --loglevel-certificates info --loglevel-coroutines info \
  --loglevel-developer info --loglevel-devices info \
  --loglevel-equipment info --loglevel-exceptions info \
  --loglevel-main info --loglevel-messages info \
  --loglevel-mqtt info --loglevel-navigation info \
  --loglevel-options info --loglevel-platform info \
  --loglevel-profiling info --loglevel-protocol info \
  --loglevel-scraping info --loglevel-serial info \
  --loglevel-signals info --loglevel-web info
```

Dropping any one `--loglevel-<channel>`, `--profiler`, or `--dev-mode` makes the command fail validation at startup. See [RECORD_REPLAY.md](RECORD_REPLAY.md) for recording fixtures and the replay workflow.
