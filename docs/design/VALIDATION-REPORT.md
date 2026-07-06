# UI Design Package — Completeness Validation (Phase 1)

Validation of `design_handoff_aqualink_reskin/` against the **actual merged branch**
(`feat/ui-redesign` = `develop` + `feat/schedules-unified-view`). The design was authored against
`main` + `feat/schedules-unified-view`; this report reconciles it against what is really in the code.

**Overall:** the package is high quality and its self-audit (`BEHAVIOUR-GAP-SWEEP.md`) is ~90%
accurate. The corrections below fall into: (A) genuine **design omissions** — real integrations the
reskin doesn't represent; (B) **contract reconciliations** — design assumptions that disagree with
the real wire/API; (C) **backend-reality corrections** — Phase-3 items the sweep lists as "open"
that are actually already built; (D) **decisions** for you.

Evidence for every backend claim was read from C++ source, not swagger.

---

## A. Design omissions — real integrations the reskin doesn't represent

### A1. PWA / installability / offline shell — **MISSING from the design** (highest-value omission)
The real app is an installable PWA: `assets/web/scripts/pwa.js` (SW registration + update-reload),
`assets/web/sw.js` (asset cache + offline shell), `manifest.json`, and Apple
`apple-mobile-web-app-capable` / `apple-touch-icon` meta tags in `index.html`. The design package
(`Aqualink.dc.html`, README, screenshots) never mentions any of this.
**Risk:** a from-scratch reskin silently drops installability + offline. **Action:** add a section to
the package: preserve `manifest.json` link, the Apple meta tags, `pwa.js` SW registration, the
network-first SW (see memory: SW is v2 network-first), and the SW-update→reload behaviour. This is
the closest analogue to the "login is missing" example — a whole integration absent from the design.

### A2. Accent-colour selector has no home
The design introduces four accent variants (teal/azure/aqua/violet, `data-accent`) and calls accent a
"genuine setting", but no Settings control is shown to pick it, and no persistence is specified
(theme uses a `theme` localStorage key via `theme-store`; there is no accent store today).
**Action:** add an accent picker to Settings (client pref, mirror the theme-store pattern) — or drop
accents to a single fixed accent if not wanted.

### A3. Pool-graphic derivation must be carried over
`assets/web/scripts/components/pool-graphic.js` is **not** a visual schematic — it's the accessor
that derives *pump / heater / chlorinator active* from button-label keyword matching, feeding the
dashboard status dots. The design's hero "status column" (Pump/Heater/Chlorinator dots) needs that
same derivation. **Action:** note in the package that the hero dots reuse `pool-graphic` logic; don't
re-derive it ad hoc.

---

## B. Contract reconciliations — design assumptions that disagree with the real code

### B1. Alerts: bind to the REAL `AlertTransition` contract, not the invented `alert` shape (important)
The sweep (§B5/E5) says `alerts-store.js` "was not present in the import" and specs a **new** event:
`alert { id, condition, severity, title, detail, raised_at, cleared_at? }`.
**Reality:** `assets/web/scripts/stores/alerts-store.js` already exists AND the backend already fires
alerts. On the wire it is:

```
{ type: "AlertTransition", payload: { condition, state: "raised"|"cleared", ts, detail } }
```

Conditions are a fixed set of **5**: `chlorinator_fault`, `chlorinator_warning`, `salt_low`,
`service_mode`, `serial_comms_loss` (backend `src/core/alerting/alert_monitor.cpp`,
`alert_condition.h`; WS emit in `websocket_equipment.cpp`; webhook in `webhook_sink.cpp`).
**Action:** the design's alerts bell/dropdown must derive severity + title **client-side** from those
5 conditions (as `alerts-store` already does via `ALERT_LABELS`) and bind to `AlertTransition`
`{condition,state,ts,detail}`. Drop the invented `alert`/`severity`/`title`/`raised_at` shape from
the package. There is **no** manual-ack (correct — the design already says auto-clear only).

### B2. `device_fault` alert does not exist (and is deliberately excluded)
The design lists `device_fault` among alert conditions. The backend intentionally omits it
(`alert_condition.h` documents `device_lost_comms` is out of v1 — no per-device status hub signal
exists yet). **Action:** remove `device_fault` from the design's alert list, or move it to the
explicit "backend-pending" bucket.

### B3. MQTT / Matter / recording field names need mapping (data is there)
- **MQTT** (`webroute_diagnostics_mqtt.cpp`) returns everything the card wants but with different
  keys: `reconnects`→`reconnect_attempts`; `ha_discovery{enabled,prefix}`→ flat
  `home_assistant_enabled` + `ha_discovery_prefix`; `broker`→ `broker_host` + `broker_port`.
  **Action:** update the design's field contract to the real keys. No backend work.
- **Serial recording** (`webroute_diagnostics_recording.cpp`) returns `{recording, file, bytes}`.
  `bytes` is available now — the design's "surface bytes-written, mark size/rotation backend-pending"
  is correct; just confirm the key is `bytes`.

---

## C. Backend-reality corrections — Phase-3 items the sweep marks "open" that are already built

### C1. §E6 body configuration — **already exposed** (close it)
`webroute_equipment.cpp` emits `configuration.pool_configuration` (enum) **and**
`configuration.bodies[]` (`{id, label, is_active, temperature, setpoint}`). The UI does **not** need
to infer single-body from non-zero temps. **Action:** mark A14/E6 as DONE (read `pool_configuration`
+ `bodies[]`); remove the "build (small)" backend ask.

### C2. §B5/E5 alert firing + webhook — **already built** (close it, except `device_fault`)
`AlertMonitor` is a live service (wired in `aqualink-automate.cpp`), evaluates on hub signals + a
1–10s comms timer, latches edges, emits `AlertTransition`, and `WebhookSink::Post` POSTs
`{condition,state,ts,detail}` (http/https, TLS-verified, 1 retry) to the runtime `webhook_url`.
**Action:** E5 is not open work — only the frontend binding (B1) and optional `device_fault`.

### C3. §B4/E3 MQTT half — **already complete** (naming only, see B3). Matter half still open but in the
**matter.js sidecar** (`matter-bridge/`), not C++: missing `exposed_endpoints[]`, `node_id`,
`vendor_id`, `spec_version`, `commissioning_open`. **Action:** relocate the E3 Matter ask to the
sidecar; MQTT needs no backend work.

### C4. §B2/E1 latency — genuinely open, but the UI overstates today's data
`LatencyPercentileTracker` is a **60-second** sliding window (not 15m) emitting a single block per
channel (`p1/p50/p95/p99/min/max/mean_us/sample_count`) plus `alltime_min/max_us` only (no cumulative
percentiles). The design's cards show "Last 15m" + a "Since restart" set — more than exists.
**Action / decision (D2):** either relabel the card to "last 60s" + alltime min/max (simplify), or
build the 900s window + full cumulative block (E1 spec).

### C5. §B3/E2 recording rotation, §E4 schedule server-side conflicts / one-time — still open backend
choices (unchanged by this validation). Auth (README) is **fully implemented and matches** — closed.

---

## D. Decisions for you (drive the next design-package iteration)

- **D1. Profiling / Matter placement (IA).** The design puts Matter + Profiling under **Settings**;
  the real app + all their endpoints (`/api/diagnostics/*`) live under **Diagnostics**, and the
  sweep's own §C recommends moving them. Move Profiling (and Matter) into Diagnostics, or keep in
  Settings?
- **D2. Latency card (C4).** Simplify to the real 60s window + alltime min/max, or schedule the E1
  backend build (900s window + cumulative)?
- **D3. "Detailed" view.** The design adds a net-new **Detailed** view (the real app has 6 views, no
  Detailed). All its data already exists in `pool-store`, so no backend gap — keep it, or fold it
  into Dashboard?
- **D4. PWA (A1).** Confirm we preserve installability/offline (recommended) so the reskin doesn't
  regress it.

---

## Net status of the design package

- **Visually / behaviourally:** complete and high-fidelity; no *view* is missing except the
  cross-cutting PWA integration (A1) and the accent-picker control (A2).
- **Backend readiness is better than the sweep claims:** body-config (E6), alert firing + webhook
  (E5), MQTT fields (E3-mqtt), and auth are **already done**. Truly-open backend items reduce to:
  latency windowing (E1, if not simplified), recording rotation (E2), Matter sidecar fields (E3-matter),
  server-side schedule conflicts / one-time (E4), and the `device_fault` alert.
- **Must-fix in the package before implementation:** A1 (PWA), B1 (alerts → `AlertTransition`),
  B2 (`device_fault`), B3 (field names), and the C-series status corrections so we don't build
  backend work that already exists.

---

## E. Resolutions (Phase-1 decisions — authoritative for implementation)

| # | Decision | Resolution | Consequence for the package |
|---|----------|-----------|------------------------------|
| D1 | Profiling / Matter placement | **Keep in Settings** (design as-drawn; §C recommendation declined) | Settings view calls `/api/diagnostics/profiling` + `/api/diagnostics/matter` directly. Do **not** move them to Diagnostics. Accent picker (A2) also lives in Settings. |
| D2 | Latency card | **BUILD the 15m/cumulative backend** (E1) | Real C++ work: widen `LatencyPercentileTracker` to a 900s window (or add a 2nd ring) + a full cumulative histogram; split the stats JSON into `window{secs:900,…}` + `cumulative{…}` per channel. UI cards stay as-drawn ("Last 15m" + "Since restart"). This is now an accepted backend task, not a simplify. |
| D3 | "Detailed" view | **Keep** | Ship the net-new live key/value system-state view. All data is already in `pool-store`; no backend gap. |
| D4 | PWA | **Preserve** | Reskin must keep `manifest.json` link, Apple `apple-mobile-web-app-*` meta, `pwa.js` SW registration, the network-first `sw.js` offline shell, and SW-update→reload. Add a "preserve PWA" requirement to the package. |

### Package edits these resolutions imply (the Phase-2 iteration work-list)
1. **Add a PWA section** to the package (README): the reskin's `index.html` must retain the manifest
   link + Apple meta; `pwa.js`/`sw.js` unchanged; document the offline shell + update-reload. *(D4/A1)*
2. **Rewrite the alerts spec** to the real contract: `AlertTransition {condition,state,ts,detail}`,
   5 conditions (`chlorinator_fault/warning`, `salt_low`, `service_mode`, `serial_comms_loss`),
   severity/title derived client-side (as `alerts-store` already does). Remove the invented `alert`
   event and `device_fault`. *(B1/B2)*
3. **Fix the diagnostics field names** (MQTT `reconnect_attempts` / `home_assistant_enabled` /
   `ha_discovery_prefix` / `broker_host` / `broker_port`; recording `bytes`). *(B3)*
4. **Restatus the backend work** in `BEHAVIOUR-GAP-SWEEP.md`: E6 (body config), E5 (alert firing +
   webhook), E3-MQTT, and auth are **DONE**. Remaining open: **E1 (build — accepted)**, E2 (recording
   rotation), E3-Matter (**sidecar**), E4 (server-side conflicts / one-time), `device_fault`. *(C1–C5)*
5. **Add an accent picker to Settings** + an accent client-pref store mirroring `theme-store`. *(A2)*
6. **Note the pool-graphic derivation** feeds the hero status dots — reuse it, don't re-derive. *(A3)*

*Backend items to schedule (not blocking the UI reskin, but now committed): E1 latency windowing.
E2/E3-Matter/E4/device_fault remain optional/deferred unless separately prioritised.*
