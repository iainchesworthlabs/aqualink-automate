# Home Assistant add-on — design

*Design snapshot (2026-07-08). A point-in-time plan for shipping Aqualink Automate as a
Home Assistant **add-on** (the store rebranded "Apps" in the 2026.x releases — same
mechanism). When the add-on ships, fold the user-facing parts into
[mqtt-home-assistant.md](../mqtt-home-assistant.md) and [INSTALL.md](../INSTALL.md), and
treat this file as the reconciled record.*

> **RECONCILED 2026-07-08 (same day, after option review):** two decisions below changed
> once we reviewed the full option surface and the UI-exposure model:
> 1. **Ingress was pulled into Phase 1** (was "direct port now, ingress later"). The
>    "surfaced through Home Assistant, not LAN-open, HA-authenticated" behaviour requires
>    ingress, which serves the UI under a per-session path prefix. The frontend used
>    root-absolute URLs everywhere, so a **base-path shim** (`assets/web/scripts/base-path.js`,
>    relies on the app's hash routing to derive the prefix client-side, no backend change)
>    plus relativised asset refs now ship. Config uses `ingress: true` **plus** an opt-in
>    (null-default) `ports:` mapping for direct LAN access.
> 2. **Defaults now defer to Home Assistant.** The app's own auth (ingress provides it),
>    history (HA Recorder), and scheduler (HA automations) are **off by default**, each an
>    opt-in toggle. `log_level` is surfaced (stdout → the add-on Log tab). Preferences +
>    equipment cache are always persisted to `/data`.
>
> The tier tables and phasing further down predate this and are kept as the original plan.

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

## Options form → flag mapping (as shipped, Phase 1)

Assembled by `homeassistant/aqualink-automate/run.sh` (bashio → argv → the base image's
`docker-entrypoint.sh`).

| Add-on option | App flag / behaviour | Notes |
|---|---|---|
| `serial_mode` = `usb` + `serial_port` (device picker, `uart: true`) | `--serial-port` | Mutually exclusive with the network transport. |
| `serial_mode` = `network` + `remote_serial_port` + `remote_protocol` | `--remote-serial-port` + `--rfc2217` / `--rawtcp` / `--no-rfc2217` | `plain` → `--no-rfc2217`. |
| `mqtt_mode` = `auto` | `--mqtt` (+ broker from the Supervisor services API) | Zero-config; from `services: mqtt:want`. |
| `mqtt_mode` = `manual` + `mqtt_host/port/username/password/tls` | `--mqtt --mqtt-*` | External broker HA does not manage. |
| `home_assistant_discovery` | `--home-assistant` | Ignored when `mqtt_mode: disabled`. |
| `log_level` | `--debug` / `--trace` (else default) | Console sink → the add-on Log tab. |
| `enable_history` | `--history-db /data/history.db` (else off) | Off by default → HA Recorder. |
| `enable_scheduler` | `--schedules-file /data/schedules.json` (else off) | Off by default → HA automations. |
| `pool_configuration`, `jandy_device_type`, `jandy_device_id` | `--pool-configuration` / `--jandy-device-*` | Advanced; defaults suit most. |
| _(not surfaced)_ | `--preferences-file /data/preferences.json`, `--equipment-cache-file /data/equipment-cache.json` | Always persisted to `/data`. |
| _(not surfaced)_ | `MATTER_ENABLED=false`, `PUID/PGID=0`, `--address 0.0.0.0 --http-port 80 --disable-https` | Fixed by the add-on. Auth stays off — ingress authenticates. |

## CI / release touches

- **Add-on lint job** — run the official `home-assistant/actions` add-on linter over
  `homeassistant/` (cheap; catches manifest/schema drift). Add to [ci.yml](../../.github/workflows/ci.yml).
- **Version lock-step** — IMPLEMENTED. `scripts/sync-homeassistant-addon-version.ps1` is
  the single writer/checker for `config.yaml` `version` + `build.yaml` build_from tags.
  `-Version <v>` rewrites both (run in release prep); `-Check` verifies internal
  consistency. The `Home Assistant Add-on` CI job runs `-Check` on every add-on change;
  `release.yml`'s `resolve-version` runs `-Check -Version <release>` so a real release
  **fails fast** unless the add-on was bumped to the release version.
- **Arch gap (flagged, not silent):** CI builds `amd64` + `arm64` only. Older 32-bit Pis
  (Pi 3 / `armv7`/`armhf`) are common HA hosts and are **unsupported** until we add an
  `armv7` image to the release matrix (extra glibc-floor validation). Deferred by decision
  above — recorded here so it stays a conscious choice.

## Phasing (reconciled 2026-07-08)

Ingress moved from Phase 2 into Phase 1 (see the reconciliation banner at the top), so the
numbering below supersedes the original three-phase plan.

### Phase 1 — headless + MQTT + ingress — **CODE COMPLETE** (branch `feat/home-assistant-addon`, PR #96)

`uart`/remote serial, `services: mqtt:want` auto-discovery, the options form, ingress
(+ opt-in LAN port), HA-deferring defaults, the base-path frontend refactor, the version
lock-step, and docs. Remaining **close-out** gates before it is truly shipped:

- **Live HAOS install pass** — the three seams unverifiable without a Supervisor: ingress
  proxy end-to-end, the MQTT services-API handshake, and `uart` device mapping.
- **First real add-on image build** — validates the bashio tarball URL/version and that
  `SUPERVISOR_TOKEN` reaches bashio on our non-s6 image (no `with-contenv`).
- **`icon.png` / `logo.png`** store art.
- Green CI, un-draft, merge.

### Phase 2 — release integration & install robustness

- **Version lock-step automation — DONE** (`scripts/sync-homeassistant-addon-version.ps1`
  + CI `-Check` + release `-Check -Version`; see CI/release touches above).
- **Prebuilt add-on image — IMPLEMENTED (pending first publish).** `release.yml` job
  `homeassistant-addon-publish` builds the wrapper per-arch (single-arch image per HA
  arch) FROM this release's app image + bashio + run.sh, pushes
  `ghcr.io/iainchesworth/aqualink-automate/homeassistant-{aarch64,amd64}:<version>`
  (+ floating `latest`/`edge`), and attests provenance. `config.yaml` now carries
  `image: …/homeassistant-{arch}` so the Supervisor pulls instead of building on-device.
  **One-time op:** the two new GHCR packages default to PRIVATE on first push — make them
  PUBLIC once so anonymous Supervisor pulls work (same as the app image). Until the first
  release publishes them, comment out `image:` to fall back to local-build for testing.
- **Add-on options translations — DONE.** `homeassistant/aqualink-automate/translations/en.yaml`
  gives every option a friendly label + help text (and the port label). The
  `Home Assistant Add-on` CI job enforces coverage: each locale must document exactly the
  `config.yaml` `options:` (configuration:) and `ports:` (network:) keys — no missing, no
  extra — mirroring the web UI's i18n key guard. Add a locale = drop in another
  `translations/<lang>.yaml`.
- **Release channel — DONE (stable + edge, generated).** The repository exposes two
  add-ons: `aqualink-automate` (stable channel; tracks the latest stable release) and
  `aqualink-automate-edge` (`stage: experimental`; tracks the latest prerelease). Both
  pull the **same** `homeassistant-{arch}` image family — only the `version` pointer
  differs — so no extra images. The edge folder is **generated** from the stable one by
  `scripts/gen-homeassistant-edge-addon.ps1` (identity + version overrides only; run.sh /
  Dockerfile / translations copied verbatim), and the `Home Assistant Add-on` CI job
  regenerates + fails on any diff, so the channels cannot drift by hand. Channel versions
  are independent: `sync-homeassistant-addon-version.ps1 -Channel {stable|edge} -Version`
  bumps one channel (the generator preserves the edge version across regenerations), and
  `release.yml` checks the channel matching the release's prerelease-ness. **Pre-1.0:** no
  stable release exists yet, so the stable channel is also marked `experimental` and both
  currently track the newest release; flip stable to `stage: stable` at the first
  non-prerelease release.

### Phase 3 — hardware reach & security hardening

- **`armv7`/`armhf` image** — 32-bit Pi support; needs a release-matrix entry + glibc-floor
  validation.
- **AppArmor profile — AUTHORED, STAGED (not yet enforced).** One tailored profile at
  `homeassistant/aqualink-automate/apparmor.txt.draft` covers both the add-on and the
  standalone image (same serial + network + exec surface; broad reads, writes confined to
  `/data`/`/tmp`/home). Shipped as `.draft` so the Supervisor does NOT auto-enforce it —
  the Supervisor loads `apparmor.txt` in enforce mode with no complain toggle, so it must
  be validated on a real HAOS box first (rename → regenerate edge → tune against
  `dmesg | grep DENIED`). The generator rewrites the profile name to `aqualink_automate_edge`
  for the edge channel so the two don't collide on one host. Standalone use is opt-in via
  `docs/SECURITY.md` (Docker's `docker-default` stays the default). **Activate only after
  the baseline HAOS install pass.**
- **Opt-in LAN-port auth** — offer app `auth-mode` as a toggle when the direct (non-ingress)
  port is enabled, so that path is not left unauthenticated. (Still open.)
- **Health / watchdog under ingress — DONE.** `config.yaml` sets
  `watchdog: "http://[HOST]:80/api/health"` — the Supervisor reaches the container over the
  internal hassio network on the fixed container port 80 (NOT the unset `[PORT:80]` host
  mapping), hitting the app's unauthenticated `/api/health` liveness probe directly (no
  ingress prefix). The image's Docker `HEALTHCHECK` stays as belt-and-braces for standalone
  runs. **Confirm the `[HOST]:<literal-port>` form on the HAOS pass.**

### Phase 4 — optional / advanced

- **Matter inside the add-on** — needs `host_network: true` and reconciling with HA's own
  Matter controller. Niche (MQTT discovery already covers HA).
- **Native HACS integration** — the genuinely MQTT-free path (a Python component over the
  REST/WS API). Slickest UX, but a separate deliverable in another repo/language.

## Resolved / open items

- **Multi-arch manifest vs per-arch tags** — RESOLVED for Phase 1: local-build `build.yaml`
  `build_from` references the multi-arch manifest tag; Docker resolves the arch. Revisit if
  Phase 2 switches to a prebuilt `image:`.
- **AppArmor posture** — deferred to Phase 3 (above).
- **Config-file mode** (`--config` into `/data`) vs argv assembly — still open; argv is the
  Phase 1 choice.
