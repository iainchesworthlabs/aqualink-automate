# Aqualink Reskin — Behaviour-Gap Sweep

A bidirectional audit of the reskinned UI (`Aqualink.dc.html`) against the real
`iainchesworth/aqualink-automate` source (Alpine stores/views/components +
`swagger.yaml`). Two directions:

- **▶ UI gaps** — real backend behaviour the reskin doesn't yet represent (close these to reach parity).
- **◀ App features** — UX in the reskin that the backend doesn't yet support (candidates to build server-side, or to drop from the UI).

Grounded in: `pool-store`, `stats-store`, `system-store`, `ws-store`, `settings-view`,
`schedules-view`, `dashboard-view`, `diagnostics-view`, `device-card`, `chlorinator-control`,
`chemistry-gauge`, `equipment-button`, `config/ui-constants`, and `api/swagger.yaml`.

> ## ⚠ Post-validation reconciliation (2026-07-01) — read `../VALIDATION-REPORT.md`
> This sweep was authored against `main`. Validated against the real merged branch, several
> Phase-3 items marked "open" below are **already built**, and the alerts contract here is wrong:
> - **E5 alert firing + webhook — DONE.** Backend emits WS `AlertTransition
>   { condition, state, ts, detail }` (5 conditions; **no** `alert`/`severity`/`title` shape,
>   **no** `device_fault`) and POSTs the webhook. Frontend `alerts-store.js` already consumes it.
> - **E6 body configuration — DONE.** `/api/equipment` exposes `configuration.pool_configuration`
>   + `configuration.bodies[]`. No inference from non-zero temps.
> - **E3 MQTT — DONE** (fields all present, different key names). Matter extras live in the
>   matter.js **sidecar**, not C++.
> - **E1 latency — window is 60s, not 15m**, with only all-time min/max. **Decision: BUILD** the
>   900s window + cumulative block.
> - **Auth, PWA** — fully present in the app; PWA must be preserved (README).
> Decisions D1–D4 and the edit work-list are in `../VALIDATION-REPORT.md §E`.

---

## Implementation status (as built in `Aqualink.dc.html`)

**Phase 1 — parity on existing endpoints — ✅ complete.**
A1 command-state machine · A12 toasts · A2 system-state gating + monitor-only + service-mode
modal · A3 staleness · A4 connection/reconnect indicator · A8 device classification ·
A10 Settings parity · A11 chlorinator seed/feedback · A-prof selector from `available[]` ·
B1 Trends wired to the `/api/history/series` shape + 503 empty state · A13 Log Levels
(`Trace/Debug/Info/Warning/Error`, global + overrides).

**Phase 2 — UI build-out — ✅ complete.**
A5 Schedules (unified App + Controller, list + 24h timeline, inline CRUD, conflict flagging,
503 / pending-capture / unsupported states) · A6 spa-side remote (honest read-only observed
mapping + LED strip) · A7 device-card registry (per-type sections + recent commands) ·
A9 heater dual-setpoint + TEMP2 · B5 alerts surface (auto-clearing) · B7 System Health panel.

**Plus — body configuration (A14 below).**

**Phase 3 — backend decisions — see §E.** After validation the remaining C++ work is small:
**B2/E1 latency windowing (BUILD, accepted)**, B3/E2 recording rotation (optional), B6/E4 schedule
server-side conflicts + one-time (optional). **Already built (no work): B4/E3 MQTT fields, B5/E5
alert firing + webhook, E6 body config.** Matter sidecar extras (E3) are a matter.js task, not C++.

---

## 0. Verified faithful (no change needed)

- **Salt-low threshold (2600 ppm)** — correct. It is a *server-backed alert* preference
  (`/api/preferences` → `alert.salt_low_ppm`), deliberately distinct from the chemistry
  Good/Okay/Bad bands. Not a duplicate of the band logic.
- **Chemistry bands** (pH / ORP / Salt) — match `CHEMISTRY_BAND_DEFAULTS` exactly
  (Salt good 3500–4000, okay 2700–3500, bad <2700 / >4500; pH 7.4–7.6 / 7.2–7.8 / 7.0–8.0;
  ORP 700–750 / 650–700 / 650–800).
- **SWG / chlorinator** — actual *generating %* vs configured *target setpoint*, boost,
  and health→band mapping are conceptually correct (see §A11 for the missing seed/feedback nuance).
- **Profiling card** *(reworked this pass)* — now models the real `/api/diagnostics/profiling`
  shape: pick a backend, pause/resume the running profiler. (One refinement remains — §A-prof.)

---

## A. ▶ UI GAPS — real behaviour to add to the reskin

### A1. Command-state machine *(highest priority)*
The real app never treats an HTTP 200 as success. Every mutating command runs a state
machine: **sending → applied / rejected / timedout / blocked**, with the *authoritative*
confirmation coming from a WebSocket event (`ButtonStateChange`) or a re-poll
(`bodies[].is_active` for circulation), **not** the POST response (whose body is the
*pre-toggle* state). See `pool-store.toggleButton / adjustSetpoint / toggleSpaMode`,
`_setCommandState`, `getCommandState`, `COMMAND_CONFIRM_TIMEOUT_MS`.

- Reskin today: toggles/sliders flip instantly and optimistically.
- Add: per-control **pending** state (disabled + spinner), **applied** flash,
  **rejected/timedout** feedback, and a **toast** per outcome. This is the core
  "honest control" behaviour and underpins several items below.

### A2. System state machine + command gating
`system-store` derives one effective state with priority
**ServiceMode > Starting > Disconnected > MonitorOnly > Ready**; `commandsEnabled`
is true only in **Ready**; `disableReason` gives the explanation; a one-shot, dismissible
**Service-Mode modal** fires on entry; **Monitor-only** is a user toggle persisted to
`localStorage` that disables all mutating controls.

- Reskin today: static "Ready" pill; the monitor-screen icon is decorative.
- Add: real state derivation, **disable + reason** on every control when not Ready,
  the service-mode modal, and wire the header monitor icon to the **Monitor-only** toggle.

### A3. Telemetry staleness
`pool-store` keeps per-value timestamps, `_stalenessThreshold = 60s`, `isStale(field)`,
and `dashboard-view` ticks every 1s to render `ageText` ("just now / Ns / Nm ago") and
mark stale values. Sensors that report `0`/`--` are treated as *unknown*.

- Reskin today: a single "Last update 21:05:08" string.
- Add: per-tile freshness + stale styling; treat `0`/`--` as unknown (the gauges already
  imply this — make it real).

### A4. WebSocket connectivity + reconnect
`ws-store` runs two sockets (`/ws/equipment`, `/ws/equipment/stats`) with exponential-backoff
reconnect (1s→30s), a "Connection lost — retrying…" toast, and `connected`/`statsConnected`
flags that drive the **Disconnected** state and the diagnostics **LIVE** badges.

- Reskin today: no connection surface; the "LIVE" badge is cosmetic.
- Add: a connection indicator that drives Disconnected, and key the diagnostics live badges to `statsConnected`.

### A5. Schedules — full CRUD *(✅ built from `feat/schedules-unified-view`)*
`schedules-view` + `/api/schedules` implement: list with **per-row enable toggle**,
**add/edit form** (name, day-of-week bitmask Mon–Sun chips, `time_local`, action type,
target/value), **delete-with-confirm**, **monitor-only gating**, and a **503 → "unavailable"**
state. Action types: `button_on/off/toggle`, `pool_setpoint`, `spa_setpoint`,
`chlorinator_percent`, `circulation_mode`. The branch further **merges controller programs**
(`/api/controller/schedules`, read-only on→off spans) with app schedules, flags **conflicts**
(app OFF/toggle inside a controller ON window), and offers a **24h timeline** day-view.

- Built: the whole unified surface — merged chronological list (App/Controller badges),
  inline new/edit form (day chips, action picker with target/value branch), row enable toggles,
  delete, conflict banner + per-row flags, the 24h timeline (program bars + point markers + now
  line), and the **503 / pending-capture / unsupported** empty states. Mutations gated when
  commands aren't enabled.
- ▶ Backend note: conflict detection is client-side today — see §E4 for moving it server-side.

### A6. Spa-side remote keypad (Diagnostics)
`SpasideRemote` device cards join the rich `/api/equipment/spaside-remotes` feed:
**LED states** (on/blink/off), **key groups by switch**, **momentary press injection**
(emulated remotes only), and **inline per-key function assignment** from a strict dropdown
(programmed over the bus). See `diagnostics-view.pressSpasideButton / setSpasideAssignment / spasideKeyGroups`.

- Reskin today: the device modal shows generic info, no keypad/LED/press/assign UI.
- Add: the keypad with LED indicators, press buttons (emulated), and the assign-function editor.

### A7. Device-card detail registry (Diagnostics)
`device-card.js` has a per-type REGISTRY: type-specific "what it's doing" summary, detail
sections, and a live-activity pulse for **OneTouch** (navigator/spider-engine), **IAQ**
(command queue), **SerialAdapter** (decoder), **PDA** (scrape state), **Keypad**, **SpasideRemote**;
plus an **emulation-posture badge** (Active emulator / Passive decoder / Real device),
operating-state badges, a **recent-commands** list (Success/err outcomes), and raw JSON.

- Reskin today: the modal covers some of this generically.
- Add: mirror the per-type sections + recent-commands feed + emulation/operating-state badges.

### A8. Equipment classification (Dashboard)
`dashboard-view` splits buttons into **heaters** (own setpoint+toggle block),
**controllable** switches (`b.controllable === true`), and read-only **other** devices
(non-controllable, e.g. sensors); the **chlorinator** is surfaced only in Water Chemistry.
Icons come from `device_type` then `DEVICE_KEYWORDS` label matching.

- Reskin today: every equipment item is a toggle.
- Add: read-only vs controllable distinction (don't render a working switch on a non-controllable device), and the device_type/keyword icon mapping.

### A9. Heater specifics
Backend exposes a dedicated `POST /api/equipment/heater`, plus `pool_setpoint_2` /
`POOLHT2` **maintenance setpoint** and a read-only `poolHeater2Enabled` for single-body
systems; setpoints are **quantized to whole degrees** (the UI rounds optimistically to match).

- Reskin today: simple solar/pool/spa heater rows.
- Add: TEMP2 maintenance setpoint (where present), whole-degree quantization in the stepper, and per-body heater mapping by label.

### A10. Settings parity
`/api/preferences` carries far more than the reskin shows: `temperature_units`
(Celsius/Fahrenheit), `alert.{salt_low_ppm, comms_timeout_seconds, webhook_url}`,
`history.retention_days`, `ui.chemistryBands`, `label_overrides{}`, `show_aux_id_in_label`.
Chem bands sync to **both** localStorage **and** server; saving shows a validation error
path and a saved-flash.

- Reskin today: salt-low + chem ranges only.
- Add: **temperature-units** toggle, **comms-timeout**, **webhook URL**, **history retention**,
  **device display-name overrides**, **"show Aux id in label"** toggle, plus the server-sync + validation + saved-flash behaviour.

### A11. Chlorinator target seeding & feedback
`chlorinator-control` seeds the target slider from the **live configured setpoint** until
the user grabs it (re-seeds every 1s), enables **Set** only when the target differs,
shows **applied/rejected** feedback, de-underscores the health label, and tracks pool-vs-spa
per-body setpoints.

- Reskin today: static 40% target, Set/Boost present but no seeding/changed/feedback logic.
- Add: seed-until-touched, "changed" gating on Set, and applied/rejected feedback.

### A-prof. Profiling backend list should come from the API
The reskin now hardcodes `['Tracy','Intel VTune','AMD uProf']`. The real endpoint returns
`available[]` (compiled-in backends) plus `enabled`/`running`/`backend`. When this is wired
to a live backend, drive the selector from `available` and disable when empty (the
"compiled without a profiler" state already handles empty).

### A12. Toasts
`toast-store` drives info/warn/error toasts for nearly every action and outcome. The reskin
has none — needed to surface command results (ties to A1).

### A13. Lower-priority parity
- **Auth** (`auth.js`): optional bearer-token auth — login, logout, token as a WS subprotocol.
  **A token-based login screen + wired logout are now in the design** (see README “Authentication”
  and `screenshots/12-login.png`); bind them to the `auth` store. Login is shown on `/api/auth/check`
  401; hidden entirely when auth is disabled (the default).
- **PWA**: `manifest.json` + `sw.js` + `pwa.js` (installable, offline shell). **Decision D4: PRESERVE**
  — the reskin must keep the manifest link, Apple meta tags, SW registration, and offline shell (see
  README "Progressive Web App"). Not optional.
- **Log Levels**: backed by real `/api/diagnostics/logging` + `/api/diagnostics/options` — the reskin's Global+overrides model matches; wire it to those endpoints.

### A14. Body configuration (single-body: pool-only / spa-only) — ✅ built
Aqualink installs are **Combo** (pool + spa, the default) **or single-body**, and single-body is
a user config choice of **pool-only (default)** or **spa-only** (`Pool_Config` / `system_board`
in `/api/equipment`; the real dashboard/detailed views only render the bodies that exist).

- Reskin before: always rendered both Pool *and* Spa everywhere (hero tiles, Circulation
  Pool/Spa switch, Temperature Setpoints, heater rows, Detailed Temperatures/Heating/Circulation)
  — showing "Spa 1 °C" and a pointless Pool/Spa selector on a pool-only board.
- Built: a **Body config** control (`Pool + Spa` / `Pool only` / `Spa only`) now drives every
  dual surface. Single-body drops the unused body's hero tile, replaces the Circulation switch
  with a static body label, shows only the configured body's setpoint + heater rows, and
  collapses Detailed Temperatures/Heating to the one body (Circulation shows **Body** not **Mode**).
  Combo is unchanged.
- ▶ Backend note: expose the body configuration explicitly (e.g. `bodies[]` with `type`) so the
  UI reads it rather than inferring from which temperatures are non-zero.

---

## B. ◀ APP FEATURES — UX in the reskin not (fully) backed by the API

### B1. Trends history — **supported, just unwired** *(was my earlier worry; the API exists)*
`GET /api/history/series` provides exactly what Trends needs: a series list and
server-side **time-bucket-averaged** points (`from`/`to` unix seconds, `max_points` ≤ 2000).
Series include `temp/*`, `chem/*` (e.g. `chem/salt_ppm`), SWG, and **`device/<uuid>`**
state series — the latter backs the **equipment-runtime gantt** (derive on/off intervals
client-side from the state series). Returns **503 when history is disabled** (`--history-db`
not configured).

- Action: wire Trends to `/api/history/series` (it's currently synthetic), derive runtime
  bands from `device/<uuid>` series, and add a **"history disabled"** empty state for 503.
- No backend work required *unless* you want ranges/aggregations the bucket-mean API doesn't cover.

### B2. Diagnostics latency: "Last 15m" vs "Since restart"
The reskin's latency cards show p1/p50/p95/p99 + min/max/mean/samples **windowed to "Last 15m"**
*and* a separate **"Since restart"** set. The real `StatisticsUpdate.latency` exposes a single
set (`p1/p50/p95/p99/min/max/mean_us` + `sample_count`) with **no windowed-vs-cumulative split**.

- Decision: either (a) **build** a rolling-window latency aggregation alongside the cumulative
  one in the backend, or (b) simplify the card to the single set the API actually provides.

### B3. Serial recording: size cap + on-full policy + path
`POST /api/diagnostics/recording` accepts **only** `{action, filename}` (bare `*.cap` basename,
jailed server-side); status returns `{recording, file, bytes}`.

- The reskin's **MAX SIZE** and **ON FULL (rotate/overwrite/stop)** controls are not backed by the API.
- Decision: either **build** size-cap + rotation params into the recording endpoint, or drop those
  controls and instead show **bytes written** (which the API does return).

### B4. MQTT / Matter card fields
`/api/diagnostics/mqtt` and `/api/diagnostics/matter` exist. Confirm they return everything the
reskin shows (MQTT: published count, **queue depth**, **dropped**, **Home-Assistant discovery**;
Matter: `qr_payload`, manual code, **per-endpoint exposure list**). Any field the reskin shows
that isn't in the response is a small backend addition (or should be removed from the UI).

### B5. Alerts surface + delivery — ✅ backend DONE; wire the UI to the real contract
**Corrected (validation):** `alerts-store.js` **exists** and the backend **already fires alerts and
the webhook**. `AlertMonitor` (`src/core/alerting/alert_monitor.cpp`) evaluates conditions on hub
signals + a 1–10s comms timer, latches edges, and emits WS `AlertTransition`; `WebhookSink::Post`
(`src/core/alerting/webhook_sink.cpp`) POSTs the webhook on raise **and** clear.

- **Wire shape (authoritative):** `{ type:"AlertTransition", payload:{ condition, state:"raised"|
  "cleared", ts, detail } }` on `/ws/equipment`. **5 conditions:** `chlorinator_fault`,
  `chlorinator_warning`, `salt_low`, `service_mode`, `serial_comms_loss`.
- **UI action:** bind the header bell/dropdown to `Alpine.store('alerts')`; derive severity + title +
  icon **client-side** from the condition (as `alerts-store` `ALERT_LABELS` does). **Drop** the
  invented `alert { id, severity, title, raised_at, cleared_at }` shape and the `device_fault`
  condition from the design — neither exists (`device_fault` deliberately out of v1: no per-device
  status hub signal). Only the UI binding remains; no backend work.

### B6. Schedules: action completeness + one-time schedules
`/api/schedules` supports recurring day-of-week schedules with the 7 action types in §A5.
AqualinkD-style **one-time / quick schedules** ("run cleaner for 1 hour") are **not** in the
action model — a candidate feature if you want parity with that product. Also verify every
listed action type is fully implemented server-side (the subsystem returns **503** when off).

### B7. Health / metrics surfaces (bonus, already in API)
`/api/health`, `/api/health/detailed`, and a Prometheus `/metrics` endpoint exist but aren't
surfaced anywhere in the reskin. Consider a small **System Health** panel (uptime, subsystem
health) on About or Diagnostics — pure UI, API already there.

---

## C. Information-architecture mismatches

- **Profiling / Matter placement — RESOLVED (D1): keep in Settings.** The endpoints are
  `/api/diagnostics/*`, but the reskin intentionally surfaces Profiling + Matter under **Settings**;
  the Settings view calls those endpoints directly. The §C recommendation to move them is **declined**
  — do not relocate them. (Serial Recording / Log Levels / MQTT status remain in Diagnostics.)
- Nav order now ends with **Diagnostics** (per your request) — fine.

---

## D. Suggested phasing (path forward)

**Phase 1 — parity on existing endpoints (UI-only, no backend work): ✅ complete.**
A1 command-state machine + A12 toasts · A2 system-state gating + monitor-only + service modal ·
A3 staleness · A4 connection/reconnect indicator · A10 Settings parity · A11 chlorinator seed/feedback ·
A8 device classification · A-prof selector from `available` · B1 wire Trends to `/api/history/series` ·
A13 Log Levels → real logging/options endpoints.

**Phase 2 — UI build-out (endpoints exist): ✅ complete.**
A5 Schedules CRUD (+ unified controller programs + timeline) · A6 spa-side keypad (honest read-only) ·
A7 device-card registry detail · A9 heater TEMP2 · B5 alerts surface · B7 health/metrics panel ·
plus A14 body configuration.

**Phase 3 — backend decisions (build or simplify): ◀ mostly closed — see §E.**
**BUILD (accepted):** B2/E1 windowed-vs-cumulative latency (60s→900s + cumulative). **Already built
(no work):** B4/E3 MQTT fields · B5/E5 alert firing + webhook · E6 body config. **Optional / open:**
B3/E2 recording size-rotate · B6/E4 schedule server-side conflicts + one-time · E3 Matter *sidecar*
fields · `device_fault` alert. Auth + PWA already present (PWA must be preserved — D4).

---

## E. Phase-3 decisions (build vs simplify)

Each item: **UI today · API today · decision · spec.** "Build" = a change to the C++ server;
"Simplify" = trim the UI to what the API already provides (keeps the prototype honest).

### E1 (B2) — Latency: "Last 15m" window vs cumulative
- **UI today:** each latency card (Serial Read / Write / Message Processing) shows
  `p1/p50/p95/p99` + `min/max/mean/samples` badged **Last 15m**, plus a **Since restart**
  `min/max` line — matching the existing product screenshot you supplied.
- **API today (corrected):** `StatisticsUpdate.latency` exposes **one block per channel**:
  `p1/p50/p95/p99/min/max/mean_us` + `sample_count` computed over a **60-second sliding window**
  (`LatencyPercentileTracker`, `window_duration = 60s`), **plus `alltime_min_us`/`alltime_max_us`
  only** (no cumulative percentiles). So today's percentiles are *recent (60s)*, not 15m, and the
  "Since restart" line has only min/max.
- **Decision: BUILD (accepted — D2).** The windowed view is the higher-value diagnostic (catches
  recent regressions the lifetime numbers hide), and the real UI already shows it — so compute it
  once on the server. Widen the window ring to **900s** (or add a second 900s ring alongside the
  60s one) and add a **full cumulative histogram** so `cumulative` carries real percentiles.
- **Spec:** emit both blocks per channel —
  ```
  latency.<serial_read|serial_write|message_processing> = {
    window:     { secs:900, p1,p50,p95,p99, min,max,mean_us, sample_count },
    cumulative: { p1,p50,p95,p99, min,max,mean_us, sample_count }
  }
  ```
  Keep a rolling 15-min ring of per-bucket histograms (e.g. 90 × 10s HdrHistogram merges) per
  channel; `cumulative` is the existing lifetime histogram. UI stays as-is once wired.
- **Simplify alternative:** collapse each card to the one cumulative set and drop the window badges.

### E2 (B3) — Serial recording: size cap + on-full policy + path
- **UI today:** Capture file (path), **Max size**, **On full** (Rotate / Stop / Overwrite).
- **API today:** `POST /api/diagnostics/recording {action, filename}` (bare `*.cap` basename,
  jailed server-side); `GET` status returns `{recording, file, bytes}`.
- **Decision: SIMPLIFY now, BUILD later.** `bytes` (written) is returned but not shown — surface
  it; keep the filename field (backed); mark Max-size / On-full as **backend-pending** so the UI
  stops implying capability the server lacks. *(UI edit applied this pass.)*
- **Spec (if building rotation later):** extend the POST to
  `{action, filename, max_bytes?, on_full?: "rotate"|"stop"|"overwrite"}`; status adds
  `{bytes, rotations, policy}`; rotate → `name.1.cap`, `name.2.cap`, …

### E3 (B4) — MQTT / Matter card field completeness
- **UI today (MQTT):** broker host, client id, topic prefix, **Home-Assistant discovery**,
  published count, **queue depth**, **dropped**, reconnects, state.
  **(Matter):** fabric, node id, vendor id, spec, commissioning, **exposed endpoints[]**,
  pairing QR + manual code.
- **API today (verified):** **MQTT is complete** — `webroute_diagnostics_mqtt.cpp` returns
  `enabled, running, connected, state, broker_host, broker_port, tls, protocol_version, topic_prefix,
  client_id, home_assistant_enabled, ha_discovery_prefix, queue_depth, reconnect_attempts, published,
  dropped, last_error`. **Matter** (`webroute_diagnostics_matter.cpp`) returns `enabled, status_port,
  reachable` then **passes through the matter.js sidecar's `/matter/status`** verbatim (typically
  `running, paired, fabrics, qr_payload, manual_code`).
- **Decision — MQTT: NO C++ BUILD.** All data present; the UI just maps the real key names
  (`reconnects`→`reconnect_attempts`; `ha_discovery{enabled,prefix}`→`home_assistant_enabled` +
  `ha_discovery_prefix`; `broker`→`broker_host`+`broker_port`).
- **Decision — Matter: BUILD in the SIDECAR (not C++).** Missing `exposed_endpoints[]`, `node_id`,
  `vendor_id`, `spec_version`, `commissioning_open` must be added to matter.js `/matter/status`; the
  C++ route is a transparent pass-through and needs no change.

### E4 (B6) — Schedule action completeness, conflicts, one-time schedules
- **UI today:** recurring day-of-week schedules with 7 action types; controller programs merged
  read-only; **conflict detection done client-side**; 503 when the scheduler is off.
- **API today:** `/api/schedules` recurring CRUD with the 7 action types; `/api/controller/schedules`
  read-only; no one-time schedules; conflicts not computed server-side.
- **Decision:** (1) **Verify** every action type is honored end-to-end (esp. `circulation_mode`
  and `chlorinator_percent`). (2) **BUILD (optional):** move **conflict detection server-side**
  so all clients agree and the scheduler can reject/warn on save. (3) **BUILD (optional):**
  one-time / countdown schedules for AqualinkD parity.
- **Spec:** conflict check on `POST/PUT /api/schedules` → `409` or a `{warnings:[…]}` body when
  an app action contradicts a controller program on a shared day/window. One-time:
  `{ recurrence: "weekly"|"once", run_at? }` (drop `days_of_week` when `once`).

### E5 (B5 — firing half) — Alert evaluation + webhook delivery — ✅ ALREADY BUILT
- **UI today:** header alerts surface, auto-clearing, no manual ack; prefs carry `salt_low_ppm`,
  `comms_timeout_seconds`, `webhook_url`.
- **API today (verified):** the server **already** evaluates conditions and emits alert events, and
  **already** POSTs the webhook. `AlertMonitor` + `EquipmentHub::AlertTransitionSignal` →
  `websocket_equipment.cpp` `m_AlertSlot`; `WebhookSink::Post` (http/https, TLS-verified, 1 retry).
- **Decision: NO BUILD.** The only remaining work is the **UI binding** (see §B5).
- **Real contract (not the earlier draft):** WS `AlertTransition { condition, state:"raised"|
  "cleared", ts, detail }`; conditions `salt_low | serial_comms_loss | service_mode |
  chlorinator_fault | chlorinator_warning`. Severity/title are **not** on the event (derived
  client-side). Webhook body `{ condition, state, ts, detail }` on raise + clear.
- **Open (optional):** a generic `device_fault` / `device_lost_comms` condition — needs a new
  per-device status hub signal first; explicitly deferred.

### E6 — Explicit body configuration (from A14) — ✅ ALREADY BUILT
- **Verified:** `/api/equipment` (`src/core/http/webroute_equipment.cpp`) already emits an explicit
  `configuration` object: `pool_configuration` (enum name) + `configuration_source`, and
  `configuration.bodies[]` — each `{ id, label, is_active, temperature{…}, setpoint{…} }`, built
  from `DataHub::Bodies()`.
- **Decision: NO BUILD.** The UI reads `pool_configuration` + `bodies[]` directly; it does **not**
  infer single-body from non-zero temperatures.

---

*Generated from a read of the live `main` branch. The reskin is a faithful visual/IA prototype;
the gaps above are behavioural — the wiring needed to make it a working control surface, and the
handful of places where the UI is ahead of the backend.*
