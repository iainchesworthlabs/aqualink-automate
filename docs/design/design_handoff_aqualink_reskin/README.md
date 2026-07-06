# Handoff: Aqualink Web UI — Calm-Premium Reskin + Behaviour Parity

## Overview

This package is the design for a full refresh of the **aqualink-automate** web UI (the
single-page Alpine.js app served by the C++ web server from `assets/web/`). It covers:

- A new **calm-premium** visual language (color, type, depth, motion) applied across every view.
- A restructured information architecture (Dashboard · Detailed · Trends · Schedules · Settings ·
  About · Diagnostics).
- New/expanded views: a dedicated **Trends** page, a unified **Schedules** page (app + controller
  programs), an enriched **Diagnostics** with a device-detail modal registry, a **System Health**
  panel, and a header **alerts** surface.
- **Behavioural parity** work — honest command state, system-state gating, staleness, connection
  status — documented and prototyped, plus the remaining backend gaps in
  `BEHAVIOUR-GAP-SWEEP.md`.

The reskin was designed **against the real source** (`iainchesworth/aqualink-automate`, `main` +
`feat/schedules-unified-view`): its Alpine stores/views, `assets/web/api/swagger.yaml`, and web
components. Values, states, and endpoint semantics below are grounded in that source.

## Merge notes (combining the two worktrees)

The reskin and the **Schedules** feature were developed in parallel worktrees. Combine them **before**
applying the reskin, and treat the overlap carefully so nothing is built twice:

- **Schedules originates from `feat/schedules-unified-view`**, not from the reskin. That branch
  already implements the whole schedule model in `assets/web/scripts/views/schedules-view.js`
  against `/api/schedules` + `/api/controller/schedules`: the unified App + Controller model, list +
  24h timeline, add/edit/delete CRUD, per-row enable, day-of-week bitmask, the 7 action types,
  client-side conflict detection, monitor-only gating, and the **503 / pending_capture /
  unsupported** states.
- **The reskin's Schedules page is a restyle of that existing logic — do NOT re-implement the
  schedule model.** Reuse the branch's `schedules-view` store/computeds and REST wiring; apply only
  the new visual language and the list/timeline **layout** shown in this design (segmented
  List/Timeline toggle, conflict banner + per-row flags, App/Controller badges, inline form, program
  bars + point markers, day pills, now-marker).
- **Single source for gating.** Both the branch's `schedules-view` (`monitorOnly` getter) and the
  reskin's system-state machine gate mutations. After merge, drive both from the one `system-store`
  (`commandsEnabled` / `disableReason`) — don't keep two gating paths.
- **Conflict detection** is client-side in both today. Do it once; see sweep **§E4** for the option
  to move it server-side.
- **Nav/routing.** The reskin adds a dedicated **Trends** item and reorders the nav (Diagnostics
  last). Slot the branch's Schedules route into the reskin's nav order rather than the branch's.
- **Reskin-only (layer on after merge):** the dedicated Trends page, the Diagnostics device-detail
  modal + registry, the header alerts surface, the System Health panel, the command-state visuals,
  body configuration, and the heater three-state. None of these come from the Schedules branch.

## Authentication — login screen included, modelled on `auth.js`

The prototype now includes a **token-based login screen** and wires the header logout control,
faithful to `assets/web/scripts/auth.js` (WS5). Per `swagger.yaml`, “Authentication is **optional and
disabled by default**.”

**Real auth model (from `auth.js`):** access-**token** based (not username/password). A module-scope
transport owns the token (session/local storage), attaches `Authorization: Bearer <token>` to same-
origin `/api` calls, and rides the token as a `bearer.<token>` **WebSocket subprotocol** (browsers
can't set an auth header on a WS upgrade). An Alpine `auth` store probes `/api/auth/check`: **200**
(no token configured) → app runs, no login, **no logout control**; **401** → `showLogin`, the login
overlay. `login()` stores the token and re-checks; a bad token → “That token was not accepted.”; an
empty field → “Please enter an access token.” `logout()` clears the token, disconnects the WS, and
re-shows login. A 401 from any `/api` call drops the stored token and re-surfaces login.

**What the design provides:**
- A full-screen **login overlay** (brand, “Enter access token” heading, monospace **token** input,
  **Remember this device** toggle → local vs session storage, **Sign in** button with busy state,
  inline error line). See `screenshots/12-login.png`. It renders over the app shell (z-index 200)
  on the calm-premium background glow.
- The header **Log out** button is now wired and **only shown when auth is required**
  (`authShowLogout`) — hidden in the default no-auth deployment.
- Three states are exercised via the **Authentication** tweak (`authState`): **Auth disabled**
  (default — no login, no logout control), **Signed in** (logout control visible; logout →
  login overlay), **Login required** (overlay shown on load).

**To implement:** bind the overlay to the `auth` store (`showLogin`, `token`, `remember`, `error`,
`busy`), the header logout to `auth.store.logout()`, and gate the app shell behind
`auth.store.ready`. Match the copy/validation strings above. The prototype's `doLogin` mock accepts
any non-empty token; the real flow validates via `/api/auth/check`. (Formerly gap **A13**.)

## Progressive Web App — preserve installability + offline (do not regress)

> Added post-validation (D4). The existing app is an **installable PWA**; this design did not
> originally mention it. Preserving it is a hard requirement of the reskin.

The current app ships: `assets/web/scripts/pwa.js` (service-worker registration + an update →
reload flow), `assets/web/sw.js` (a **network-first** shell/asset cache — SW cache version is
bumped on asset changes), `manifest.json` (name, icons, theme-color, standalone display), and the
Apple `apple-mobile-web-app-capable` / `apple-mobile-web-app-status-bar-style` / `apple-touch-icon`
meta tags in `index.html`.

**To implement:** the reskinned `index.html` must keep the `<link rel="manifest">` and the Apple
meta tags; `pwa.js` and `sw.js` stay wired unchanged (only the SW asset manifest / cache version is
updated for the new files); keep the SW-update→reload behaviour. The calm-premium `theme-color`
should match `--bg` (dark) so the standalone status bar blends in. Do **not** ship the reskin as a
plain single page — that would silently drop installability and the offline shell.

## About the design files

The bundled files are a **design reference created in HTML**, not production code to copy verbatim.
`Aqualink.dc.html` is a self-contained prototype (a "Design Component" — plain HTML + inline styles
+ a small vanilla-JS logic class, rendered by `support.js`). It shows the intended **look and
behaviour**; it is not the shipping implementation.

**The task is to recreate these designs in the existing app environment** — the vanilla
**Alpine.js + ES-module** front end in `assets/web/`, using its established patterns (Alpine stores,
per-view modules, the existing web components, `app.css` / `components.css`). Do **not** introduce a
build step or a new framework; the app is deliberately no-build, plain HTML + CSS + ES modules with
vendored Alpine/Chart.js.

## Fidelity

**High-fidelity.** Final colors, typography, spacing, depth, and motion are specified. Recreate the
UI pixel-faithfully using the codebase's own CSS (translate the inline styles in the prototype into
`app.css` / `components.css` custom-property-driven rules). The prototype's inline styles are the
source of truth for exact spacing/sizing; the token table below is the source of truth for color,
type, radius, shadow, and motion.

---

## How the prototype maps to the real codebase

The prototype is one file, but it is organised to mirror the app's structure. Map it as follows:

| Prototype construct | Real codebase target |
|---|---|
| `class Component` **state** (`heaters`, `equip`, `poolSet`, `cmd`, `toasts`, `monitorOnly`, `appSchedules`, `logOverrides`, …) | Alpine **stores** (`pool-store`, `stats-store`, `system-store`, `ws-store`, plus a `toast-store`, `alerts-store`) — bind to the real WS/REST feeds instead of the seeded literals. |
| **command-state engine** (`_runCmd`, `_setCmd`, `cmd{}`, `_cmdRing`) | `pool-store.toggleButton / adjustSetpoint / toggleSpaMode` + `_setCommandState` / `getCommandState` (already present) — the prototype models their sending→applied/rejected/timedout/blocked lifecycle. |
| **system-state machine** (`_sysState`, `_commandsEnabled`, `_disableReason`) | `system-store` (`ServiceMode > Starting > Disconnected > MonitorOnly > Ready`, `commandsEnabled`, `disableReason`). |
| `renderVals()` blocks per view | the existing per-view modules: `dashboard-view`, `diagnostics-view`, `trends-view`, `schedules-view`, `settings-view`, `about-view`. |
| Chemistry gauges / SWG / device cards | the existing web components `chemistry-gauge.js`, `chlorinator-control.js`, `device-card.js`, `equipment-button.js` — restyle these, don't replace their logic. |
| `data-props` (see "Configurable states") | real backend signals (availability flags, system state, connection) — the props are the prototype's stand-ins for live data, not user settings. |

Treat `Aqualink.dc.html` as the **visual + behavioural spec**; treat the real Alpine
stores/views/components as **where the behaviour already lives** — the job is largely restyling +
wiring the parity behaviours the sweep calls out.

---

## Design tokens (exact values)

All colors are OKLCH, theme-switched via `data-theme` on the root. Two accent variants ship beyond
the default.

### Color — dark (default)
```
--bg:            oklch(0.165 0.014 250)      page background (has a soft radial glow, see below)
--bg-glow:       oklch(0.24 0.035 215)
--surface-1:     oklch(0.205 0.015 250)      cards
--surface-2:     oklch(0.235 0.016 250)      insets / controls
--surface-3:     oklch(0.275 0.017 250)      raised chips / segmented tracks
--nav-bg:        oklch(0.18 0.014 250 / 0.78) sticky nav (backdrop-blur)
--border:        oklch(0.36 0.018 250 / 0.55)
--border-strong: oklch(0.42 0.02 250 / 0.7)
--text:          oklch(0.96 0.006 250)
--text-dim:      oklch(0.745 0.012 250)
--text-faint:    oklch(0.57 0.014 250)
--accent:        oklch(0.745 0.105 200)       teal (default)
--accent-soft:   oklch(0.745 0.105 200 / 0.15)
--spa:           oklch(0.77 0.105 42)         spa/warm accent
--good:          oklch(0.78 0.12 162)         green  (ok / applied / heating)
--warn:          oklch(0.83 0.11 82)          amber  (enabled / blocked / warning)
--bad:           oklch(0.68 0.16 25)          red    (fault / rejected / conflict)
--shadow-md:     0 1px 0 oklch(1 0 0 / .045) inset, 0 12px 32px -18px oklch(0 0 0 / .75)
--shadow-lg:     0 1px 0 oklch(1 0 0 / .06) inset, 0 28px 64px -26px oklch(0 0 0 / .85)
```

### Color — light (`[data-theme="light"]`)
```
--bg: oklch(0.975 0.006 240)   --surface-1: oklch(1 0 0)   --surface-2: oklch(0.985 0.005 240)
--surface-3: oklch(0.96 0.006 240)   --nav-bg: oklch(1 0 0 / 0.8)
--border: oklch(0.905 0.008 240)   --border-strong: oklch(0.84 0.01 240)
--text: oklch(0.26 0.02 250)   --text-dim: oklch(0.46 0.018 250)   --text-faint: oklch(0.605 0.015 250)
--accent: oklch(0.56 0.12 205)   --accent-soft: oklch(0.56 0.12 205 / 0.1)
--spa: oklch(0.6 0.13 40)   --good: oklch(0.58 0.13 162)   --warn: oklch(0.66 0.13 78)   --bad: oklch(0.56 0.17 25)
--shadow-md: 0 1px 2px oklch(0 0 0 / .04), 0 14px 30px -20px oklch(0.4 0.05 250 / .35)
--shadow-lg: 0 2px 6px oklch(0 0 0 / .05), 0 30px 64px -30px oklch(0.4 0.05 250 / .4)
```

### Accent variants (set `data-accent` on root)
```
teal   (default)  --accent: oklch(0.745 0.105 200)   [dark] / oklch(0.56 0.12 205) [light]
azure             --accent: oklch(0.7 0.115 238)
aqua              --accent: oklch(0.78 0.1 178)
violet            --accent: oklch(0.66 0.13 292)
```
`--accent-soft` is always the accent at 0.15 alpha (0.1 in light).

### Page background glow
The app shell paints a fixed radial glow behind content:
`background: radial-gradient(1200px 600px at 50% -5%, var(--bg-glow), transparent 60%), var(--bg);`

### Typography
- **Display / numerals:** `Bricolage Grotesque` (opsz 12–96, weights 400/500/600/700). Used for
  hero temperatures, gauge values, big stats, card/section titles. Tabular numerals
  (`font-variant-numeric: tabular-nums`) on all live numbers.
- **UI / body / labels:** `Hanken Grotesk` (400/500/600/700). Used for nav, labels, table text,
  body copy.
- **Mono:** `ui-monospace, 'SF Mono', Menlo, monospace` for raw hex/JSON diagnostics, file paths.
- **Eyebrow labels** (section headers): Hanken 600, 12px, `letter-spacing:.14em`, uppercase,
  `--text-faint`. Sub-eyebrows use `.10em`–`.12em`.
- Hero temperature: Bricolage 600, 48px, `letter-spacing:-.02em`, unit at 24px `--text-dim`.
- Gauge value: Bricolage 600, 32px. Big stat: 44px.

### Radius scale
```
cards / panels:        20–24px
rows / inset controls: 16–18px
chips / small controls: 9–13px
pills / toggles:       99px (full)
```

### Motion (all gated behind the `motion` flag / prefers-reduced-motion)
```
@keyframes floatIn   from{opacity:0;translateY(12px)} to{…}     card entrance (~.4s)
@keyframes modalPop  from{opacity:0;translateY(14px) scale(.97)} modal enter
@keyframes modalFade / fadeIn                                    overlay fade
@keyframes cmdspin   360° spinner                                pending toggle knob
@keyframes cmdPulse  opacity 1↔.45                               pending / actively-heating pulse
@keyframes toastIn   translateX(16px)→0                          toast enter
```
Gauge arcs animate their `stroke-dasharray` in on mount:
`transition: stroke-dasharray 1.1s cubic-bezier(.22,1,.36,1)`. Bars animate `width` at
`1s cubic-bezier(.22,1,.36,1)`. Toggles: knob `transform .26s cubic-bezier(.34,1.4,.5,1)`.

### Layout
- Content max-width **1180px**, centered, `padding: 30px 24px 72px`.
- Sticky top nav: brand + route buttons (active = `--surface-3` bg + `--border`), right cluster
  (connection chip · status pill · alerts bell · theme toggle · monitor toggle · logout).
- Nav order: **Dashboard · Detailed · Trends · Schedules · Settings · About · Diagnostics**
  (Diagnostics intentionally last).

---

## Screens / views

Exact spacing, copy, and per-element styling live in `Aqualink.dc.html` (search the view's
`<sc-if value="{{ isX }}">` block). Summaries:

### Dashboard
- **Hero summary strip:** Pool / Spa / Air temperature tiles (accent left-bars; Spa uses `--spa`),
  plus a status column (operational pill + Pump / Heater / Chlorinator status dots). Body tiles
  respect body configuration (see below). *The Pump/Heater/Chlorinator "active" derivation already
  lives in `components/pool-graphic.js` (keyword-matched over the button list) — reuse it to drive
  the status dots; don't re-derive ad hoc.*
- **Circulation + Temperature Setpoints** (two-column). Circulation shows the active body; in a
  combo system a Pool/Spa segmented switch, in single-body a static body label. Temperature
  Setpoints is the **single home for per-body setpoints** (Pool / Spa rows with −/+ steppers,
  whole-degree quantized). *Setpoints are per body, independent of heating mechanism.*
- **Water Chemistry:** three semicircular gauges (pH / ORP / Salt) with Good/Okay/Bad bands, plus
  the **SWG** card (actual generating % vs configured target, health badge, Set button enabled only
  when the target differs, Boost with quick/full picker). Whole block gated on `hasSwg`.
- **Heater:** rows for Solar / Pool / Spa heat. Each has an On/Off toggle and a **three-state
  operating badge**: **Off** (toggle off), **Enabled** (on, body at/above setpoint — amber), or
  **Heating** (on, body below setpoint — green with a pulsing dot). A read-only **TEMP2**
  maintenance row appears for single-body pool systems.
- **Equipment Controls:** 2-col grid of device tiles (icon, name, sub-status, toggle). Drag to
  reorder. Non-controllable devices should render read-only (see gap A8).

### Detailed
Live-system-state cards (Temperatures, Water Chemistry, Circulation, Heating, System) as
key/value tables. Body-config aware: single-body collapses to the one body + Air, and Circulation
shows **Body** rather than **Mode**. Gated cards (Water Chemistry / Heating) follow equipment
availability.

### Trends
Dedicated page (faithful to `trends-view`). Combined multi-band chart — temperature line band,
water-chemistry band, equipment-runtime gantt band — with a **hover scrubber** (all-series tooltip
readout), **grouped series toggles** (Temperatures / Water Chemistry / Equipment), a **range
selector** (1h/6h/24h/7d/30d), and **per-series stat cards** (min/max/avg, runtime %). Chemistry
band gated on chlorinator. **History-disabled (503)** empty state when `historyAvailable` is false.

### Schedules
Unified surface (from `feat/schedules-unified-view`): **App schedules** (`/api/schedules`, editable)
merged with read-only **Controller programs** (`/api/controller/schedules`). Two views via a
segmented toggle:
- **List** — merged chronological rows with App/Controller badges, per-row enable toggle +
  edit/delete on app rows, Read-only on controller rows; a **conflict banner** + per-row flags when
  an app OFF/toggle lands inside a controller program's ON window on a shared day.
- **Timeline** — 24-hour day view: day pills (today highlighted), grouped target rows, controller
  program bars + app-action point markers (conflict-red), axis + legend + "now" marker.
- Inline **new/edit form**: name, enabled, 7 day chips (Mon–Sun bitmask), time, action type
  (`button_on/off/toggle`, `pool_setpoint`, `spa_setpoint`, `chlorinator_percent`,
  `circulation_mode`) with a target dropdown or value input.
- Empty/disabled states: **503** (app scheduler off), **pending_capture** / **unsupported**
  (controller programs). Mutations gated when commands aren't enabled.

### Diagnostics (dense, technical)
System Health panel (from `/api/health/detailed`: readiness header + Configuration / Equipment /
MQTT subsystem cards) · Serial Port Utilization tiles · Bandwidth (Read + Write area graphs, window
selector) · Communication Latency cards (p1/p50/p95/p99 + windowed min/max/mean/samples + since-
restart) · Serial Health · Message Errors · Message Statistics (with a modal) · Log Levels (global
+ per-channel overrides, search, Configure modal — levels `Trace/Debug/Info/Warning/Error`) · MQTT
Broker card · Serial Recording (filename + backed **bytes-written**; size/rotation marked planned)
· **Emulated Devices** and **Actual Devices** grids.
- **Device detail modal:** per-type sections (SerialAdapter decoder, OneTouch navigator/spider,
  IAQ command queue, PDA scrape, SpaSide observed keypad + LED strip), an emulation-posture badge
  (Active emulator / Passive decoder / Suppressed), a **Recent Commands** list, the numbered panel
  **screen**, and raw JSON.

### Settings
Server-backed prefs (temperature units, salt-low threshold, comms timeout, webhook URL, history
retention) + client prefs (**theme + accent pickers** — accent = teal/azure/aqua/violet, persisted
client-side in a small `accent-store` mirroring `theme-store`; the accent tweak has no other home,
so it belongs here), device display-name overrides + "show Aux id" toggle, chemistry target
ranges (Good/Okay/Bad zone bar + inputs), Matter (pairing QR + fields, or unavailable), Profiling
(backend selector gated on `available[]`, pause/resume, or unavailable). Save shows saved-flash +
disconnected-error path. *(Post-validation D1: Matter + Profiling **stay in Settings** — they call
`/api/diagnostics/matter` + `/api/diagnostics/profiling` from here; they are **not** moved to the
Diagnostics view.)*

### About
Software information + build/system tables. (The old marketing "Features" list was removed.)

---

## Interactions & behaviour

These are the **honest-control** behaviours the reskin adds; they are the heart of the parity work.

- **Command-state machine.** No mutating control treats an HTTP 200 as success. Every toggle /
  stepper / Set runs **sending → applied / rejected / timedout / blocked**, with the authoritative
  confirmation coming from a WS event or re-poll (never the POST body). While pending: control
  disabled, spinner/pulse, badge "Switching…"; on resolve: applied flash (green ring, ~1.1s) or
  rejected/timedout/blocked styling (~1.7s) + a toast. See `_runCmd` / `_setCmd` / `_cmdRing`.
- **Toasts.** Info/warn/error/success, stacked bottom-right, auto-dismiss ~4.2s. One per command
  outcome and for mode changes.
- **System-state gating.** Effective state precedence **ServiceMode > Starting > Disconnected >
  MonitorOnly > Ready**; commands enabled only in Ready. A prominent full-width **banner** under
  the nav when not Ready (with reason + action), a header **status pill** + **connection chip**, a
  one-shot dismissible **Service-Mode modal**, and **Monitor-only** as a header toggle that disables
  all mutating controls.
- **Staleness.** When the feed goes quiet/disconnected, readings dim, the hero shows a **Stale**
  pill, and a freshness chip appears; `0`/`--` sensor values are treated as unknown.
- **Alerts.** Header bell with a live count; dropdown lists active alerts by severity,
  **auto-clearing** when the condition resolves (no manual ack — the API has none).
  **Wire to the real contract (post-validation):** the backend already fires alerts as WS
  `AlertTransition` events `{ condition, state: "raised"|"cleared", ts, detail }` over `/ws/equipment`
  (`alerts-store.js` already consumes this). There are exactly **5 conditions** —
  `chlorinator_fault`, `chlorinator_warning`, `salt_low`, `service_mode`, `serial_comms_loss` —
  and severity/title/icon are derived **client-side** from the condition (see `alerts-store`
  `ALERT_LABELS`), not carried on the event. There is **no** `alert`/`severity`/`title`/`raised_at`
  event shape and **no** `device_fault` condition (deliberately excluded — no per-device status
  signal exists yet). Bind the bell/dropdown to `Alpine.store('alerts')`.
- **Heater three-state** and **SWG seed/changed/feedback**, **schedule conflict flagging**,
  **log-level global+overrides**, **profiler availability gating** — as described per view.

---

## State management

In the real app these live in Alpine stores bound to the WS/REST feeds. The prototype's `state`
enumerates what's needed: theme/accent/motion, current view; per-body setpoints; `heaters`,
`equip[]` (with order for drag-reorder), `swg`/`swgConfigured`; `cmd{}` (per-key command state) +
`toasts[]`; `monitorOnly`, `serviceDismissed`; schedules (`appSchedules[]`, `ctrlSchedules[]`,
`ctrlStatus`, form + view + selected day); `logGlobal` + `logOverrides{}`; boost timer;
Matter/Profiling/recording flags; modal + alert-dropdown open state. Data fetching = the existing
WS (`/ws/equipment`, `/ws/equipment/stats`) + REST endpoints in `swagger.yaml`.

## Configurable states (prototype `data-props`)

These props are **stand-ins for live backend signals**, not user settings — use them to see every
state. Wire each to the real signal:

- `startTheme` / `accent` / `motion` — real user prefs (theme + accent are genuine settings).
- `hasSwg`, `hasSolarHeat`, `hasPoolHeat`, `hasSpaHeat`, `hasSpillway`, `swgHealth`, **`bodyConfig`**
  (`Pool + Spa` / `Pool only` / `Spa only`) — **installed-equipment / body configuration** from
  `/api/equipment`. Body config drives all dual pool/spa UI.
- `matterAvailable`, `profilingAvailable`, `profBackendsAvail`, `historyAvailable`,
  `schedulesAvailable` — **subsystem availability** (compiled-in / configured), drive the
  "unavailable"/503 states.
- `systemState` (`Operational`/`Starting up`/`Service mode`), `connection`
  (`Connected`/`Reconnecting`/`Offline`), `dataFreshness` (`Live`/`Stale`) — **live system/WS
  state**.
- `cmdOutcome` (`Confirm`/`Reject`/`Time out`) — simulates command confirmation; in production the
  outcome comes from the WS confirmation, not a prop.
- `chemAlert` — simulates an active chemistry alert; in production comes from the alerts feed.
- `authState` (`Auth disabled` / `Signed in` / `Login required`) — stands in for the `auth` store's
  probe of `/api/auth/check`; drives the logout control + login overlay.

---

## App gaps to close — see `BEHAVIOUR-GAP-SWEEP.md`

The bundled `BEHAVIOUR-GAP-SWEEP.md` is a **bidirectional audit** against the real source and the
implementation plan. Status:

- **Phase 1 (parity on existing endpoints) — ✅ prototyped.** Command-state machine + toasts,
  system-state gating + monitor-only + service modal, staleness, connection/reconnect, Settings
  parity, chlorinator seed/feedback, device classification, profiler-from-`available[]`, Trends
  wired to the `/api/history/series` shape, Log Levels. → **Wire these prototyped behaviours to the
  real stores/endpoints.**
- **Phase 2 (UI build-out, endpoints exist) — ✅ prototyped.** Schedules (unified + timeline +
  CRUD), spa-side remote (honest read-only), device-card registry, heater TEMP2 + three-state,
  alerts surface, System Health panel, body configuration.
- **Phase 3 (backend decisions) — ◀ mostly closed after validation.** See **§E** of the sweep and
  `../VALIDATION-REPORT.md` for the verified status. Corrected picture:
  - **E1** latency windowed-vs-cumulative — **BUILD accepted (D2).** Reality: the tracker is a **60s**
    window with only all-time min/max (no cumulative percentiles). Widen to a 900s window + add a
    full cumulative histogram, and split the stats JSON into `window{secs:900,…}` + `cumulative{…}`.
  - **E2** serial-recording size cap + rotation — **still open (optional).** `bytes` written is
    already returned; surface it and mark size/rotation backend-pending until built.
  - **E3** MQTT — ✅ **DONE** (all fields present; only key names differ — see sweep §E3). Matter
    extras (`exposed_endpoints[]`, `node_id`, `vendor_id`, `spec_version`, `commissioning_open`)
    live in the **matter.js sidecar**, not the C++ route.
  - **E4** schedule action completeness + server-side conflict detection + optional one-time — open.
  - **E5** alert firing + webhook — ✅ **DONE.** `AlertMonitor` evaluates + emits `AlertTransition`
    and `WebhookSink` POSTs the webhook. Only the frontend binding (see Alerts above) remains; the
    `device_fault` condition is intentionally out of scope.
  - **E6** body configuration — ✅ **DONE.** `/api/equipment` already exposes
    `configuration.pool_configuration` + `configuration.bodies[]`; read it directly.

Read the sweep in full — it is the authoritative gap list and includes the exact endpoint/event
shapes to build.

---

## Files

- **`Aqualink.dc.html`** — the design prototype (all views, modals, tokens, behaviours). Open in a
  browser to interact; use the Tweaks/props panel to exercise every state. Source of truth for exact
  layout and styling.
- **`support.js`** — the tiny runtime that renders the prototype. Required only to *run* the
  prototype; **not** something to port into the app.
- **`BEHAVIOUR-GAP-SWEEP.md`** — the bidirectional behaviour audit + phased implementation plan +
  Phase-3 backend specs. Read alongside this README.
- **`screenshots/`** — reference captures of the key views/states (dark unless noted):
  `01-dashboard-dark` · `02-dashboard-chemistry-swg` · `03-dashboard-heater-equipment`
  · `04-dashboard-light` · `05-trends` · `06-schedules-list` (with conflict banner)
  · `07-schedules-timeline` · `08-diagnostics` (System Health) · `09-diagnostics-devices`
  · `11-settings` · `12-login`. These are visual references; `Aqualink.dc.html` is the source of truth (open it
  to see every state, incl. the device-detail/log-level/service-mode modals, which are fixed
  overlays that don't screenshot cleanly).
- Reference source (not bundled): `github.com/iainchesworth/aqualink-automate` — `assets/web/`
  (Alpine stores/views/components) and `assets/web/api/swagger.yaml`.

## Notes for the implementer

- Keep the app **no-build**: plain HTML + CSS + ES modules, vendored Alpine/Chart.js. Port inline
  styles into `app.css` / `components.css` as custom-property-driven rules; keep the token block as
  `:root` / `[data-theme="light"]` exactly as above.
- The prototype's seeded numbers (temps, counts, device lists) are **placeholders** — bind to the
  real feeds.
- Respect `prefers-reduced-motion`; all motion is already gated behind a `motion` flag in the design.
- Icons are inline line-art SVG (stroke `currentColor`) — reuse the app's existing icon sprite;
  match stroke-width ~1.7–2.
