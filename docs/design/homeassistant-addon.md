# Home Assistant add-on — design

*Design snapshot (2026-07-08). A point-in-time plan for shipping Aqualink Automate as a
Home Assistant **add-on** (the store rebranded "Apps" in the 2026.x releases — same
mechanism). This is a roadmap, not a description of shipped behaviour: nothing here exists
in the tree yet. When the add-on ships, fold the user-facing parts into
[mqtt-home-assistant.md](../mqtt-home-assistant.md) and [INSTALL.md](../INSTALL.md), and
treat this file as the reconciled record.*

## Goal

Let a Home Assistant user run Aqualink Automate **without ever touching Docker themselves**.
They add a custom repository URL, click **Install**, fill in a settings form, and Home
Assistant's **Supervisor** pulls, runs, and lifecycle-manages the container for them —
auto-restart, logs, updates, and hardware/MQTT wiring all handled by HA.

The add-on is a **thin wrapper around the images we already publish**
(`ghcr.io/iainchesworth/aqualink-automate`, multi-arch `amd64` + `arm64`, built by
[release.yml](../../.github/workflows/release.yml)). It is *not* a second build of the app.

## What a Home Assistant add-on actually is

An add-on is a Docker container the **Supervisor** manages on the user's behalf. The user
adds a "custom repository" (a Git repo the Supervisor scans), installs an add-on from it,
and the Supervisor does the `docker pull` / `docker run` / restart-on-crash. The user
configures the add-on through a generated **options form**, never a command line.

### Scope constraint — which HA installs this reaches

Add-ons require the **Supervisor**, so they are available only on:

- **Home Assistant OS** (HAOS) — the default appliance install (Pi, NUC, VM).
- **Home Assistant Supervised** — Supervisor on a user-managed Debian host.

They are **not** available on **HA Container** (Home Assistant itself in Docker) or **HA
Core** (bare Python venv), which have no Supervisor. Those users keep using the existing
[docker-compose.yml](../../docker-compose.yml). So the add-on **complements** the current
container path — it does not replace it. HAOS-on-a-Pi is the most common HA deployment, so
this still covers the large majority of users.

## Design decisions (2026-07-08)

| Decision | Choice | Rationale |
|---|---|---|
| Add-on location | **In this repo** under `homeassistant/` | Single source of truth, versioned with the app, CI validates it against the same image it publishes. Users add this repo's URL as a custom repository. |
| Web UI, first cut | **Direct mapped port now; ingress later** | Works against today's image with no frontend change. Ingress (sidebar-embedded, HA auth) needs a base-path refactor of the frontend — deferred to a later phase. |
| Architectures | **`amd64` + `arm64` only for now** | Matches what CI already builds. `armv7` (Pi 3 / 32-bit HAOS) deferred. |
| Matter sidecar | **Off by default in the add-on** | HA is already a Matter controller; add-on host-networking + mDNS is fiddly. HA users get devices via the MQTT discovery path. Advanced toggle can re-enable it. |

## Integration seams — how each app need maps to an add-on feature

Every capability the app requires maps onto a first-class add-on manifest feature. This is
what makes the wrapper thin.

### 1. Serial hardware (the make-or-break seam)

The app supports two transports (see [hardware-rs485-connectivity.md](../hardware-rs485-connectivity.md)):
a local USB-RS485 port (`--serial-port`) and serial-over-ethernet (`--remote-serial-port host:port`).

- **USB-RS485:** the manifest declares `uart: true`. The Supervisor auto-maps
  `/dev/ttyUSB*`, `/dev/ttyACM*`, `/dev/ttyAMA*`, and `/dev/serial/by-id/*` into the
  container **and renders a device dropdown** in the options form. The user picks their
  adapter by name; we pass the stable `/dev/serial/by-id/...` path to `--serial-port`.
- **Serial-over-ethernet:** needs no device mapping at all — the container reaches the
  adapter over the network. We pass the user's `host:port` to `--remote-serial-port`
  (plus `--rfc2217` / `--no-rfc2217` / `--rawtcp` as options).

Both existing transports are covered with no app change.

### 2. MQTT — the standout, zero-config seam

The manifest declares a **service dependency**:

```yaml
services: ["mqtt:want"]
```

When the user runs the standard **Mosquitto broker** add-on, the Supervisor injects the
broker's host / port / username / password / ssl into our container via its Services API
(`http://supervisor/services/mqtt`, authed by the `SUPERVISOR_TOKEN` env). Our wrapper reads
that and flips on `--mqtt --home-assistant` pointed at the injected broker — the user
configures **nothing** for MQTT, and entities auto-discover into HA.

`want` (not `need`) means the add-on still installs if no broker is present; manual-broker
options remain for users pointing at an external broker. Option → flag mapping uses the
existing names from [mqtt-home-assistant.md](../mqtt-home-assistant.md)
(`--mqtt-host`, `--mqtt-port`, `--mqtt-username`, `--mqtt-password`, `--mqtt-tls`, …).

### 3. Web UI — direct port now, ingress later

- **Phase 1 (direct port):** the manifest maps a port (`ports:`) and declares a `webui:`
  link. Works with the app exactly as it is today.
- **Phase 2 (ingress):** `ingress: true` embeds the UI *inside the HA sidebar* with HA
  handling authentication — no extra port, no separate login. **Blocked on a frontend
  change:** ingress serves everything under a dynamic path prefix
  (`/api/hassio_ingress/<token>/`), but the frontend currently uses **root-absolute
  paths**. Confirmed in the tree:
  - [assets/web/index.html](../../assets/web/index.html) references `/styles/app.css`,
    `/i18n/en.js`, `/scripts/...`, etc. — all rooted at `/`.
  - `wsUrl()` in [assets/web/scripts/stores/ws-store.js](../../assets/web/scripts/stores/ws-store.js)
    builds `ws://<host>/ws/equipment` off the host root, ignoring any prefix.

  Ingress therefore needs a **base-path mechanism**: an injected `<base>` (or a computed
  prefix) prepended to static-asset, `/api/*`, and WebSocket URLs, derived from the
  document's own location so it works both standalone and under the ingress prefix. This is
  its own unit of work — hence Phase 2.

### 4. Matter — off by default

The image bundles the Matter sidecar (see [docker-entrypoint.sh](../../docker-entrypoint.sh)),
which needs host networking + mDNS (UDP 5540 / 5353). HA is already a Matter controller and
add-on host-networking is awkward, so the add-on ships `MATTER_ENABLED=false` by default. An
advanced option can re-enable it together with `host_network: true` for users who want the
bridge. The idiomatic HA path is MQTT discovery (seam 2), which needs none of this.

### 5. Config plumbing — `/data/options.json` → argv

The options form writes `/data/options.json`. A small **`run.sh`** (using **bashio**, HA's
config helper library — pure bash, so it runs fine on our Ubuntu/glibc image) reads those,
pulls MQTT details from the Services API, assembles the command line, and execs the
**existing** [docker-entrypoint.sh](../../docker-entrypoint.sh). Persistence at `/data`
already matches what the image expects (`/data/matter`, etc.). The wrapper stays thin: it
translates a form into flags and delegates to the entrypoint we already ship.

## Repository layout

```
homeassistant/                      # users add THIS repo's URL as a custom repository
  repository.yaml                   # repo metadata the Supervisor scans
  aqualink-automate/
    config.yaml                     # manifest: arch, options+schema, uart, services, ports, image ref, version
    build.yaml                      # build_from per-arch (or reference the multi-arch manifest)
    Dockerfile                      # FROM the published GHCR image + bashio/jq + run.sh
    run.sh                          # options.json + MQTT service → argv → exec docker-entrypoint.sh
    icon.png / logo.png
    DOCS.md / README.md / CHANGELOG.md
```

`config.yaml`'s `image:` points at the **already-published** GHCR tags, with the add-on
`version` tracking our release version. Installing the add-on = pulling a release we already
build; nothing new compiles. The add-on `Dockerfile` only layers bashio + jq + `run.sh` on
top of that image.

## Options form → flag mapping (initial cut)

| Add-on option | App flag | Notes |
|---|---|---|
| `serial_port` (device dropdown, `uart: true`) | `--serial-port` | Mutually exclusive with remote. |
| `remote_serial_port` (`host:port`) | `--remote-serial-port` | Selects network transport. |
| `rfc2217` / `rawtcp` | `--rfc2217` / `--no-rfc2217` / `--rawtcp` | Remote-only. |
| `mqtt_auto` (bool, default on) | `--mqtt --home-assistant` + injected broker | From `services: mqtt` (Services API). |
| `mqtt_host` / `mqtt_port` / `mqtt_username` / `mqtt_password` / `mqtt_tls` | `--mqtt-*` | Manual broker override when `mqtt_auto` is off. |
| `api_auth_token` | `--api-auth-token` | Optional; ingress (Phase 2) makes it redundant. |
| `matter_enabled` (advanced, default off) | `MATTER_ENABLED` env + `host_network` | Off by default. |
| `log_level` | app log flags | Map to the logging facade. |

Exact option set is finalised during scaffolding against
[docs/configuration.md](../configuration.md).

## CI / release touches

- **Add-on lint job** — run the official `home-assistant/actions` add-on linter over
  `homeassistant/` (cheap; catches manifest/schema drift). Add to [ci.yml](../../.github/workflows/ci.yml).
- **Version lock-step** — the add-on `config.yaml` `version` must track the app release
  version, mirroring the existing version-stamping. A small check (like the release
  `version-check`) fails the build on drift.
- **Arch gap (flagged, not silent):** CI builds `amd64` + `arm64` only. Older 32-bit Pis
  (Pi 3 / `armv7`/`armhf`) are common HA hosts and are **unsupported** until we add an
  `armv7` image to the release matrix (extra glibc-floor validation). Deferred by decision
  above — recorded here so it stays a conscious choice.

## Phasing

1. **Phase 1 — headless + MQTT (the 80% case).** Add-on repo dir, `uart` + remote serial,
   `services: mqtt:want` auto-discovery, options form, UI on a direct mapped port. Ships
   against today's image with **no app code change**.
2. **Phase 2 — ingress.** Frontend base-path refactor (assets + `/api/*` + WebSocket) so the
   UI embeds in the HA sidebar with HA-handled auth.
3. **Phase 3 — polish.** `armv7` image, AppArmor profile, optional Matter toggle, docs +
   cross-links from [mqtt-home-assistant.md](../mqtt-home-assistant.md) and
   [INSTALL.md](../INSTALL.md).

## Open items to resolve during implementation

- Whether to reference the **multi-arch manifest** directly from `config.yaml` `image:` or
  publish per-arch tags (`…-{arch}`) — HA supports both; the manifest path is simpler if the
  Supervisor resolves the arch cleanly.
- Final AppArmor posture (default profile vs a tailored one) for a container that opens a
  serial device and binds an HTTP port.
- Whether the add-on should offer a config-file mode (`--config` into `/data`) in addition to
  argv assembly, for power users.
