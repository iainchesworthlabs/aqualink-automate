# Matter (smart-home) support

*For pool owners pairing the equipment with Apple Home, Google Home, Alexa or
SmartThings over Matter. CLI/config option semantics live in
[Configuration reference](configuration.md); the Docker host-networking requirement
lives in [INSTALL.md](INSTALL.md#matter-bridge-and-host-networking).*

aqualink-automate exposes the pool equipment to **Apple Home, Google Home, Amazon
Alexa and Samsung SmartThings** over [Matter](https://csa-iot.org/all-solutions/matter/),
so a single QR-code pairing covers every major ecosystem.

Matter is implemented as a **sidecar bridge** ([`matter-bridge/`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/matter-bridge/README.md)):
a separate Node.js process that consumes the existing HTTP/WebSocket equipment API and
presents the equipment as a Matter *Aggregator*. It does **not** link into the C++
build — keeping the C++/MSVC build untouched and the image small. The sidecar depends
on [`@matter/main`](https://www.npmjs.com/package/@matter/main) `^0.12.0` and `ws`, and
requires **Node.js ≥ 20** (the published Docker image ships Node 24, which satisfies
that floor).

```
 Apple Home / Google / Alexa / SmartThings
        │  Matter over IPv6 mDNS (UDP 5540 / 5353)
        ▼
 ┌───────────────────────┐   HTTP + WS    ┌──────────────────────────┐
 │  matter-bridge (node)  │ ─────────────▶ │  aqualink-automate (C++)  │
 │  @matter/main Aggregator│  /api/equipment │  RS-485 ⇄ pool hardware   │
 └───────────────────────┘  /ws/equipment   └──────────────────────────┘
```

## Cross-controller actuation (why this works on any rig)

Commands reach the RS-485 bus through a **capability-routed command dispatcher**:
whichever controller is running advertises what it can do (`DeviceActuator`,
`SetpointController`, `ChlorinatorController`, …) and the dispatcher routes to the
highest-priority one present. Three controllers can actuate equipment and heater
setpoints, in precedence order:

1. **Serial Adapter (RSSA, `0x48`/`0x49`)** — *High*. A direct, stateless command channel.
2. **AqualinkTouch / iAqualink2 (`0x30–33`)** — *Medium*. Presses the on-screen
   `PageButton` matching the device by name (`0x11 + index`) for toggles, and uses the
   value-submit protocol (select field → `0x80` → control-data value) for setpoints +
   the chlorinator. These commands take effect immediately.
3. **OneTouch (`0x40–43`)** — *Low*. Drives the menu via the Navigator (equipment
   toggle = in-place *Select* on the Equipment ON/OFF row; setpoint = arrow-stepping
   the value on the Set Temperature page). Slower, since it must crawl menus.
4. **PDA (`0x60–63`)** — *Lowest*. (Currently advertises nothing → honest `NotSupported`;
   actuation parity is planned.)

The AqualinkTouch outranks the OneTouch because its direct page-button / value-submit
commands take effect immediately, whereas the OneTouch must navigate menus. So a rig
with *any* controller can be controlled, and the most direct channel present wins.

## Device mapping

| Aqualink device                         | Matter device type      | Notes                                    |
| --------------------------------------- | ----------------------- | ---------------------------------------- |
| Pump (single/two-speed)                 | On/Off Plug-in Unit     | on/off (VSP speed not a command yet)     |
| Aux / Cleaner / Spillover / Sprinkler   | On/Off Plug-in Unit     | toggle                                   |
| Light                                   | On/Off Plug-in Unit (v1)| on/off only; dimmable/colour is **planned**, not yet shipped |
| Heater                                  | Thermostat (heat)       | setpoint per body (pool/spa)             |
| Chlorinator (SWG)                       | On/Off + Level Control  | level = generating %; boost = 101        |
| Pool / Spa / Air temperature            | Temperature Sensor      | read-only                                |
| pH / ORP / salt ppm                     | (omitted in v1)         | no faithful Matter type                  |

## Enabling / disabling

Matter is **on by default (opt-out)**. `--matter` is a `value<bool>` with an implicit
`true`, so omitting it or passing a bare `--matter` leaves the bridge on; only an
explicit false turns it off. Config-file keys are the option long name without the
leading dashes (so `matter = false`). See the
[Configuration reference](configuration.md) for the full option semantics.

- App flag: `--matter false` (or `matter = false` in the config file).
- Docker: `-e MATTER_ENABLED=false` (read by the container entrypoint).
- Tunables: `--matter-storage-path` (default `/data/matter`), `--matter-status-port`
  (default `8099`, localhost only), `--matter-passcode`, `--matter-discriminator`
  (passcode/discriminator `0` ⇒ the sidecar generates and persists random values).

## Networking requirement (Docker)

Matter commissioning uses **IPv6 mDNS (UDP 5540 + 5353)**, which Docker's bridge
driver does not forward. The container therefore **must run with host networking**:

```bash
docker run --network host -v aqualink-matter:/data/matter aqualink-automate
# or simply:
docker compose up      # compose sets network_mode: host + a matter-data volume
```

The fabric-state volume (`/data/matter`) makes pairing survive restarts. Host
networking is Linux-only; on Docker Desktop (macOS/Windows) disable Matter with
`MATTER_ENABLED=false`. The Docker setup is covered in full in
[INSTALL.md](INSTALL.md#matter-bridge-and-host-networking).

## Pairing

1. Open the web UI → **Diagnostics → Matter**. When the badge reads **"Ready to
   pair"** the QR code and an 11-digit manual pairing code are shown. (The code is
   also printed in the container logs on first boot.) The badge is backed by the
   `GET /api/diagnostics/matter` status route — see
   [Usage and API](usage-and-api.md#diagnostics).
2. **Apple Home:** Home app → **+** → *Add Accessory* → scan the QR (or *More
   options…* → enter the manual code). Approve the "uncertified accessory" prompt
   (the bridge uses a test vendor ID).
3. **One other ecosystem** (pick one):
   - **Alexa:** Alexa app → *Devices* → **+** → *Add Device* → *Other* → *Matter* →
     scan the QR / enter the code.
   - **Google Home:** Home app → **+** → *Set up device* → *Matter-enabled device*.
   - **SmartThings:** app → **+** → *Add device* → *Partner devices / Matter* →
     scan the QR.
4. Each ecosystem joins its own fabric; the **Matter** panel's "Paired fabrics"
   count increments per ecosystem. Toggle a pool device from the controller app and
   confirm it actuates on the bus, and that controller-side changes reflect back.

## Verification status

**Note:** The table below records the outcome of a specific test run (case counts,
`tsc`-clean, Docker build stages) at the time it was captured, not a continuously
verified guarantee. Treat it as a time-stamped snapshot; re-run the suites for the
current state.

| Check                                                                   | Status |
| ----------------------------------------------------------------------- | ------ |
| C++ unit + integration suites (incl. OneTouch setpoint, dispatcher, fixture replay) | ✅ green (1726 + 101 cases) |
| OneTouch toggle replay driven from `onetouch_equipment_toggle.cap`      | ✅ Navigator emits in-place Select |
| OneTouch setpoint edit model vs `onetouch_setpoint_edit.cap` hardware    | ✅ Select-enter → arrow-step ±1 → Select-commit; Pool=line2/Spa=line3; on-screen units |
| AqualinkTouch (IAQ) aux + setpoint vs `iaq_aux_setpoint.cap` hardware    | ✅ toggle = 0x11+index by name (Pool Light 0x1a, Spillway 0x1c); setpoint = value-submit '1'+value |
| OneTouch chlorinator (% + boost) vs `onetouch_chlorinator.cap` hardware  | ✅ % = Set AquaPure 5% value-steps (Pool=line3); boost start/stop via Boost Pool page |
| Sidecar unit tests (device-map, API client)                             | ✅ green (11 cases) |
| Sidecar typecheck + build (matter.js bridge)                            | ✅ `tsc` clean |
| Sidecar boots → emits a valid commissioning QR + manual code            | ✅ verified |
| `/api/diagnostics/matter` (enabled / opt-out / unreachable-sidecar)     | ✅ verified live on a replay fixture |
| Docker `matter-builder` stage builds + prod-pruned sidecar boots        | ✅ verified (node:24-bookworm-slim) |
| Docker runtime stage (NodeSource + tini on ubuntu) + entrypoint         | ✅ verified: both processes supervised, opt-out, SIGTERM, `--version` |
| Full `docker compose up` of the runtime image                           | ⚙️ runs in CI (`docker-verify`) — the in-container C++ build needs a populated `deps/vcpkg`, which the local reuse-vcpkg worktree intentionally omits |
| Physical pairing from Apple Home + a second ecosystem                   | 🔌 manual — requires hardware on the LAN (steps above) |
