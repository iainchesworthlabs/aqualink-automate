# Feature implementation plan

*For a maintainer or implementing agent. This is the internal workstream (WS) tracker for the
post-mirror feature set. All seven workstreams have shipped; this file is now a record of what
was built, not an open backlog. The two genuinely-open follow-ups (WS1 verification, capture-gated
items) are flagged below.*

Seven workstreams extended aqualink-automate from a live observe-and-command mirror into a
system with memory (history), a clock (scheduling), and a voice (alerting), plus security and
polish items. Each workstream was independently implemented in its own worktree/branch and
lists its own acceptance criteria. Merge order is at the end.

## Status at a glance

The user-facing surfaces these workstreams shipped (auth token, history API, scheduler API) are
documented in [usage-and-api.md](usage-and-api.md). The e2e-ui jobs that exercise them on every
push live in [ci-cd.md](ci-cd.md) and in `.github/workflows/ci.yml`.

| WS | Area | Status | Notes |
|---|---|---|---|
| WS1 | Chemistry tests + OneTouch AquaPure status processors | **DONE** (one OPEN check) | AquaRite regression tests landed (`test/unit/devices/test_devices_aquarite.cpp`, `test/unit/messages/test_aquarite_message_*.cpp`). OPEN: whether the OneTouch status processors match real screen lines is still capture-gated/unverified. |
| WS2 | History / SQLite persistence + API + trends UI | **DONE** | `src/core/history/`, `WebRoute_History`, `--history-db`, swagger `/api/history/*`, `e2e/trends.spec.ts`. `sqlite3` is a bundled runtime dependency. |
| WS3 | Fault detection + alerting | **DONE** | `src/core/alerting/` (`AlertMonitor`, `webhook_sink`), HA problem entities via `ha_discovery`, `options_alerting_options`. |
| WS4 | Scheduler | **DONE** | `src/core/scheduling/`, `WebRoute_Schedules`, `--schedules-file`, swagger `/api/schedules*`, `e2e/schedules.spec.ts`. |
| WS5 | UI authentication | **DONE** | WebSocket subprotocol auth, `GET /api/auth/check`, login overlay, `e2e/auth.spec.ts`. The transport gap is fixed; the UI works with a token set. |
| WS6 | Prometheus `/metrics` | **DONE** | `WebRoute_Metrics`, swagger `/metrics`. |
| WS7 | PWA polish | **DONE** | `assets/web/manifest.json`, `assets/web/sw.js`, `e2e/pwa.spec.ts`. |

**Note:** the per-WS sections below preserve the original design notes for reference. Where a
section's "current state" describes a broken or missing behavior, that text describes the state
*before* the workstream shipped — the per-WS **Status** header records the as-delivered status.
Use the table above and each WS's Status line as the source of truth for what exists today.

**Audience:** an implementing agent working cold. Everything needed is in this file, the
referenced source files, and the project skills. When a WS touches an area covered by a
project skill (`cmake-build-system`, `jandy-protocol`, `kernel-architecture`,
`mqtt-home-assistant`, `webui-best-practices`, `testing-best-practices`, `device-navigation`,
`backend-observability`), consult that skill before writing code.

---

## 0. Ground rules (apply to every workstream)

These are non-negotiable project conventions; violating them has broken builds/merges before.

1. **Build/test invocation (Windows).** `cmake`/`ctest`/`ninja` are NOT on PATH in agent
   shells. In ONE PowerShell command, dot-launch the VS dev shell first:
   ```powershell
   # The VS install path below is environment-specific (this box has VS 18 Community at
   # this location). Adjust the path to your own Visual Studio edition/version/install — it
   # is NOT a repo-portable constant.
   & "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
   ```
   then build with preset `config-windows-msvc-debug` (NOT the LLVM debug preset). The test
   exe also needs the dev shell (debug-CRT DLLs) and must be run **from
   `build/<preset>/test/`** as cwd so `test/fixtures/*.cap` resolve.
2. **Worktree per workstream.** Create `C:\aa-wt\<ws-name>` via
   `git worktree add C:\aa-wt\<n> -b feature/<n> develop`. Drop an empty
   `deps/vcpkg/.vcpkg-root` marker file so the root CMakeLists skips submodule init, then add
   an in-worktree `CMakeUserPresets.json` preset `wt` inheriting `config-windows-msvc-debug`
   with `toolchainFile` pointing at `R:/aqualink-automate/deps/vcpkg/scripts/buildsystems/vcpkg.cmake`,
   `binaryDir` = `${sourceDir}/build/wt`, and `ENABLE_CLANG_TIDY: OFF`. Do NOT put the
   worktree on R: (disk-tight) or build with full parallelism if R: is involved.
3. **Commit discipline (shared/contended repo).** Always commit with an explicit pathspec
   (`git commit -m "..." -- <paths>`), `git add` every NEW file explicitly first (pathspec
   commits silently skip untracked files — this has broken clean checkouts twice), and
   re-grep your edits immediately before committing. **Never add `Co-Authored-By` or any
   attribution trailer** to commits or PR text.
4. **Swagger sync.** Any route/schema/status-code change MUST update
   `assets/web/api/swagger.yaml` in the same branch.
5. **Options workflow.** New CLI/config options follow the six-step pipeline documented in
   `CLAUDE.md` (settings struct field + constructor default → `make_appoption` with matching
   `->default_value` → guarded `Process()` write → `Validate()` conflicts → read via
   `settings.Get<>()` → test under `test/unit/options/`). Config-file keys are the option
   long names; no extra code needed for them.
6. **CMake platform isolation.** No `$<PLATFORM_ID:...>`/`$<CXX_COMPILER_ID:...>` in src/
   CMakeLists; platform specifics belong in `cmake/toolchains/*.toolchain.cmake`.
7. **Kernel threading model.** The app is single-threaded cooperative on one
   `io_context`. New services run on that context via asio timers/handlers — no extra
   threads, no blocking waits in handlers. State hubs (`DataHub`, `EquipmentHub`,
   `StatisticsHub`, `PreferencesHub`) are resolved through `HubLocator`; outbound state
   changes propagate via `boost::signals2` hub events (see
   `src/core/kernel/hub_events/` — config events for temperature/chemistry/button-state and
   the equipment-hub system status-change event already exist and are the intended tap
   points for WS2/WS3).
8. **Testing discipline.** Every bug fix gets a regression test. Never delete tests to work
   around production code. The full suite (≈1207 cases / 4655 assertions at time of
   writing) must pass before merge. Use `MockReplayHarness`
   (`test/unit/support/unit_test_mockreplayharness.h`) for wire→DataHub assertions and
   `test/unit/support/unit_test_protocolmessagebuilder.h` for checksummed frames. UI-facing
   workstreams add Playwright e2e specs (`e2e/*.spec.ts`; config boots the real app on a
   replay fixture — see the repo `playwright.config.ts` and `docs/RECORD_REPLAY.md`).
9. **Route registration anchor.** HTTP routes register in
   `src/aqualink-automate.cpp` (the `HTTP::Routing::Add` block, ~lines 569–605) inside the
   `web_settings_result` block.
   Several workstreams add lines there — keep each addition on its own line, alphabetised
   with the existing `HTTP::Routing::Add` calls, to make merges trivial.

---

## WS1 — Chemistry path regression tests + OneTouch AquaPure status-processor verification

**Status: DONE (one OPEN check).** Regression tests landed
(`test/unit/devices/test_devices_aquarite.cpp`, `test/unit/messages/test_aquarite_message_*.cpp`).
OPEN: whether the OneTouch status processors match real Equipment Status screen lines remains
capture-gated and unverified — see task 2 below.

**Size:** small. **Risk:** low. **Dependencies:** none.

### Current state
Two historical AquaRite bugs are ALREADY FIXED — do not re-fix:
- `src/jandy/devices/aquarite_device.cpp:77-78` — `AquariteMessage_PPM` is registered
  **unfiltered** (PPM frames go SWG→Master, destination 0x00, so device-id filtering on
  0x50 would drop them; the comment in code explains this).
- `aquarite_device.cpp:176` (inside `Slot_Aquarite_PPM`, line 170) — the PPM handler calls
  `ReportedSaltConcentration(...)` (correct setter), not the generating-level setter.

What remains open is a historical observation that the OneTouch screen-scrape status
processors `StatusProcessor_AquaPurePercentage`
(`src/jandy/devices/onetouch/onetouch_statusprocessors.cpp:393`) and
`StatusProcessor_CheckAquaPure` (`:490`) **never matched against live screen content**. They
have since been reworked; whether they now match real Equipment Status page lines is
unverified.

### Protocol facts needed
- `AQUARITE_Percent` (0x11): Master→SWG(0x50), ~1/sec, `byte[4]` = percentage
  (0=off, 0–100 normal, 101=boost, 255=service).
- `AQUARITE_PPM` (0x16): SWG→Master(0x00), ~1/sec, `byte[4]` = PPM÷100, `byte[5]` =
  `AquariteStatuses` enum value.
- These flow only while pump AND chlorinator are on.

### Tasks
1. **Regression tests for the AquaRite wire→DataHub chemistry path** (locks in the two
   fixes): via `MockReplayHarness`, feed checksummed `AQUARITE_Percent` and `AQUARITE_PPM`
   frames (note PPM's destination is 0x00) and assert:
   - the chlorinator auxillary exists in `DataHub` with `LabelTrait == "AquaPure"`;
   - salt PPM lands via `ReportedSaltConcentration` (and the DataHub trait the
     chemistry card reads), NOT the generating level;
   - generating % lands via the Percent path;
   - `GET /api/equipment` JSON surfaces the salt/chemistry values (route handler test or
     serializer-level assertion — match existing test style in `test/unit/`).
2. **Verify the OneTouch status processors against real screen content.** Inspect the
   Equipment Status page lines produced by `test/fixtures/iaq_onetouch_startup.cap` (replay
   it through the harness with an emulated OneTouch) and any line formats asserted in
   existing tests. Determine whether the hint/regex matching in
   `StatusProcessor_AquaPurePercentage` / `StatusProcessor_CheckAquaPure` /
   `StatusProcessor_SaltPPM` (if present) actually fires on those lines.
   - If they match: add unit tests pinning the real line formats so future edits can't
     silently break them. Done.
   - If they don't match: fix the matching (detector hints vs actual spacing/case — note
     the project rule that detector text and edge-label text can differ in spacing), with
     regression tests using the REAL line strings from the fixture, not invented ones.
3. No swagger change expected (no route changes). If the equipment JSON schema gains a
   field, update swagger accordingly.

### Acceptance
- New tests fail when either historical bug is re-introduced (verify by temporarily
  reverting the registration line locally), pass on current code.
- Status-processor behaviour against the fixture is documented in the test names/comments.
- Full suite passes.

---

## WS2 — History / persistence (SQLite time-series + history API + trends UI)

**Status: DONE.** Shipped as `src/core/history/` (`HistoryService`), `WebRoute_History`, options
`--history-db` / `--history-retention-days` / `--history-flush-seconds`, swagger `/api/history/*`,
the Trends UI, and `e2e/trends.spec.ts` (run by the `E2E — history enabled` job via
`AQUALINK_HISTORY_DB`). `sqlite3` is already a declared `vcpkg.json` dependency and a bundled
runtime library (`cmake/CPackConfig.cmake`); it does NOT need adding.

**Size:** large. **Risk:** medium. **Dependencies:** none (merged after the small WSs).

### Goal
Before this workstream, all state was ephemeral in-memory and restart lost it. WS2 added an
embedded SQLite-backed sampler recording temperatures, chemistry (pH/ORP/salt/SWG %), and
button/device state transitions, a read API with downsampling, and a trends UI.

### Design
- **New directory** `src/core/history/` (own `CMakeLists.txt`, added to the core library
  exactly like sibling dirs — see `src/core/CMakeLists.txt` for the pattern).
- **`HistoryService`** (plain service, not a hub): constructor takes `HubLocator&` +
  settings; subscribes to the existing hub events:
  `data_hub_config_event_temperature`, `data_hub_config_event_chemistry`,
  `data_hub_config_event_button_state_change`
  (`src/core/kernel/hub_events/data_hub_config_event_*.h`) and
  `equipment_hub_system_event_status_change`. Follow how `MqttIntegration`
  (`src/core/mqtt/mqtt_integration.cpp`) subscribes — same mechanism.
- **Sampling policy:** write-on-change with a per-series minimum interval (default 60 s)
  to bound row rate, PLUS a periodic heartbeat sample every 5 min per active series so
  charts have points during steady state. Numeric series store value rows; state series
  (buttons/devices) store transition rows only (no heartbeat).
- **Schema:**
  ```sql
  CREATE TABLE series  (id INTEGER PRIMARY KEY, key TEXT UNIQUE NOT NULL, unit TEXT);
  CREATE TABLE samples (series_id INTEGER NOT NULL REFERENCES series(id),
                        ts INTEGER NOT NULL,         -- unix seconds, UTC
                        value REAL NOT NULL);
  CREATE INDEX idx_samples_series_ts ON samples(series_id, ts);
  ```
  Series keys are stable strings: `temp/pool`, `temp/spa`, `temp/air`, `chem/ph`,
  `chem/orp`, `chem/salt_ppm`, `swg/percent`, `device/<slug>/state`, …
- **Write path:** single-threaded io_context — open with WAL + `synchronous=NORMAL`;
  buffer samples in memory and flush in one transaction on an asio timer (default every
  10 s) so the main loop never blocks on fsync. Flush on clean shutdown.
- **Retention:** daily purge timer deleting rows older than `--history-retention-days`.
- **Dependency:** `"sqlite3"` is in `vcpkg.json` and bundled at install time (see
  `cmake/CPackConfig.cmake`); consume it with `find_package(unofficial-sqlite3 CONFIG REQUIRED)`
  per vcpkg usage docs. The C API is wrapped in a small RAII helper inside `src/core/history/`
  (statement/transaction guards); no ORM.

### Options (follow ground rule 5)
New area `src/core/options/options_history.{h,cpp}` (or `src/core/history/options/` if
matching subsystem-local precedent — check where `options_web` lives and mirror it):
- `--history-db <path>` — empty default ⇒ history disabled entirely (service not built).
- `--history-retention-days <n>` — default 90.
- `--history-flush-seconds <n>` — default 10.
Validate: retention/flush must be > 0.

### API (swagger update REQUIRED)
New `WebRoute_History` in `src/core/http/` — a single route (`/api/history/series`) with two
behaviours selected by the optional `?key=` query param (series keys contain `/`, so the key
travels as a query parameter, not a path segment):
- `GET /api/history/series` (no `key`) → `[{key, unit, first_ts, last_ts, count}]`.
- `GET /api/history/series?key=<key>&from=<unix>&to=<unix>&max_points=<n>` → bucket-averaged
  downsample server-side (simple time-bucket mean is sufficient; cap `max_points` ≤ 2000,
  default 500). 404 unknown key, 400 bad range, 503 if history disabled.
Register both in `aqualink-automate.cpp` (ground rule 9) only when history is enabled —
or always register and return 503 when disabled (pick the latter; simpler and
self-describing).

### UI
- New "Trends" nav view: `assets/web/scripts/views/trends-view.js` + a `history-store.js`
  Alpine store fetching the REST API. **Chart.js is already used** in
  `diagnostics-view.js` — reuse the same instance-outside-Alpine-proxy pattern documented
  in that file's header comment (Chart.js inside Alpine's reactive proxy causes infinite
  recursion).
- Range selector (1 h / 24 h / 7 d / 30 d), series toggles for temps + chemistry + SWG %.
- Hide/disable the view gracefully when `/api/history/series` returns 503.

### Tests
- Unit: sampler throttle (change-driven + heartbeat + min interval), bucket downsampling,
  retention purge, RAII sqlite wrapper. Use a temp-file or `:memory:` DB.
- Options tests under `test/unit/options/`.
- Integration: replay a fixture through `MockReplayHarness` with history enabled at a temp
  path; assert rows exist for `temp/*` and that the route returns them.
- Playwright: trends view renders with the replay fixture running.

### Acceptance
- With `--history-db` unset, behaviour is byte-identical to today (no DB file, routes 503).
- With it set: samples persist across restart; retention purge works; API + UI render.
- Full suite passes; swagger documents both routes and the 503 contract.

---

## WS3 — Fault detection + alerting (HA problem entities, webhook, UI surfacing)

**Status: DONE.** Shipped as `src/core/alerting/` (`AlertMonitor`, `webhook_sink`), HA problem
entities via `src/core/mqtt/ha_discovery.{h,cpp}`, and options `options_alerting_options`
(`--alert-salt-low-ppm` / `--alert-webhook-url` / `--alert-comms-timeout-seconds`).

**Size:** medium. **Risk:** medium. **Dependencies:** none (textual merge overlap with WS2
in registration blocks only).

### Goal
React to fault conditions instead of just displaying state. v1 conditions:

| Condition key | Trigger source | Clears when |
|---|---|---|
| `chlorinator_fault` | `ChlorinatorHealthTrait` enters a fault state (e.g. `GeneralFault` — set by `StatusProcessor_CheckAquaPure` and AquaRite status byte) | health returns to OK/Unknown |
| `salt_low` | salt PPM < `--alert-salt-low-ppm` (only evaluate when a fresh PPM sample arrives) | PPM ≥ threshold + hysteresis (+100 ppm) |
| `device_lost_comms` | a device's status becomes `DeviceStatus_LostComms` (watchdog path — see `AquariteDevice::WatchdogTimeoutOccurred` for the pattern) | device status recovers |
| `service_mode` | `DataHub` equipment mode enters Service | mode leaves Service |
| `serial_comms_loss` | no valid protocol message decoded for `--alert-comms-timeout-seconds` (asio timer kicked from the message-received path or StatisticsHub counters) | next valid message |

### Design
- New `src/core/alerting/`: `AlertMonitor` service subscribing to the same hub events as
  WS2 (status-change system event, chemistry config event) plus a comms-timeout timer.
  Maintains per-condition latched state with transitions (`raised`/`cleared`), each
  carrying a timestamp + human-readable detail string.
- **Sinks (v1):**
  1. **MQTT/HA (primary):** publish one HA `binary_sensor` per condition with
     `device_class: problem`, via the existing discovery layer (`src/core/mqtt/
     ha_discovery.{h,cpp}` — follow the established entity-add workflow in the
     `mqtt-home-assistant` skill; topics under the existing prefix scheme, retained state,
     wired into the existing availability topic). HA then handles user notification
     routing — we deliberately do NOT build email/push.
  2. **Webhook (optional):** when `--alert-webhook-url` is set, POST
     `{"condition": "...", "state": "raised"|"cleared", "ts": <unix>, "detail": "..."}`
     on every transition (Boost.Beast async client on the io_context; one retry, then log
     and drop — alerts must never wedge the main loop).
  3. **UI:** broadcast transitions over the existing `/ws/equipment` channel as a new
     message type; surface via the existing `toast-store.js` plus a persistent problem
     badge while any condition is raised. Document the new WS message in whatever place
     the WS protocol is documented (see `webui-best-practices` skill).
- Alert state is in-memory only (no persistence dependency on WS2); on restart, conditions
  re-evaluate naturally from live data.

### Options
New area `options_alerting.{h,cpp}`: `--alert-salt-low-ppm` (default 2600, range
0–6000; 0 disables the salt check), `--alert-webhook-url` (validated URL — needs a
`validate()` overload per the validators pattern in `src/core/options/validators/`),
`--alert-comms-timeout-seconds` (default 60, must be > 0).

### Tests
- Unit: condition latching/hysteresis/clear logic with injected events; webhook payload
  shape; comms-timeout timer with injected clock if the existing code has a pattern for it
  (otherwise short real timeouts in test).
- MQTT: discovery payload + state topic tests following `test/unit/mqtt/` conventions.
- Integration: replay harness — drive a watchdog timeout and assert the
  `device_lost_comms` transition reaches the MQTT hub layer.

### Acceptance
- With no new options set, only the MQTT problem entities and UI toasts are added; no
  webhook traffic. HA discovery payloads validate against HA's documented schema.
- Salt-low respects hysteresis (no flapping at the threshold).
- Full suite passes; swagger updated only if any HTTP surface changed (UI/WS only ⇒ check
  whether the WS message catalogue lives in swagger; if not, document where it does live).

---

## WS4 — Scheduler (app-side time-based automation)

**Status: DONE.** Shipped as `src/core/scheduling/` (`SchedulerService`), `WebRoute_Schedules`,
option `--schedules-file` (`options_scheduling_options`), swagger `/api/schedules` +
`/api/schedules/{uuid}`, the Schedules UI, and `e2e/schedules.spec.ts` (run by the
`E2E — scheduler enabled` job via `AQUALINK_SCHEDULES_FILE`).

**Size:** large. **Risk:** medium. **Dependencies:** none (JSON-file persistence keeps it
independent of WS2; merged last).

### Goal
A clock-driven engine executing existing dispatcher commands: "pump on 08:00 weekdays",
"SWG to 70 % at 09:00", "spa setpoint 38 at 17:00 Sat/Sun".

### Design
- New `src/core/scheduling/`:
  - **Model** `Schedule`: `{uuid, name, enabled, days_of_week (bitmask Mon–Sun),
    time_local "HH:MM", action}`. `action` is a tagged variant:
    `button_on|button_off|button_toggle (target: device uuid or label)`,
    `pool_setpoint|spa_setpoint (value)`, `chlorinator_percent (value)`,
    `circulation_mode (mode string)` — exactly mirroring `ICommandDispatcher`
    (`src/jandy/devices/command_dispatcher.h`) capabilities; do not invent new command
    types.
  - **`SchedulerService`**: asio steady timer ticking once per minute; on each tick,
    compute "did any enabled schedule's local wall-clock HH:MM + day match the minute we
    just entered" and fire. Recomputing from current local time each tick makes DST
    handling automatic (a skipped 02:30 during spring-forward simply doesn't fire; an
    ambiguous fall-back time fires once — document this in the swagger description).
    Guard against double-fire within the same minute (remember last-fired minute per
    schedule).
  - **Clock injection:** take a `std::function<std::chrono::system_clock::time_point()>`
    (or small clock interface) so unit tests are deterministic.
  - **Execution:** resolve `ICommandDispatcher` via `HubLocator`. If the system is in
    Service mode, SKIP the action and log a warning (and, if WS3 is merged, this is
    visible via the service_mode condition anyway). Log every fire at Info.
- **Persistence:** JSON file at `--schedules-file <path>` (empty default ⇒ scheduler
  disabled). Load at boot (tolerate and log invalid entries individually rather than
  rejecting the file); save on every mutation via write-temp-then-rename (atomic). Use
  nlohmann-json (already a dependency).

### Options
`options_scheduling.{h,cpp}`: `--schedules-file <path>` only (keep v1 minimal).

### API (swagger update REQUIRED)
Two route classes in the same `src/core/http/webroute_schedules.h`: the collection route
`WebRoute_Schedules` (`/api/schedules`) and the separate item route `WebRoute_Schedule`
(`/api/schedules/{uuid}`).
- `GET /api/schedules` → full list.
- `POST /api/schedules` → create (server assigns uuid), 201 + entity.
- `PUT /api/schedules/{uuid}` → replace, 200 + entity.
- `DELETE /api/schedules/{uuid}` → 204.
- All: 400 on validation error (bad time format, unknown action type, value out of the
  same ranges the dispatcher enforces — setpoints 1–104, chlorinator 0–100), 503 when
  scheduler disabled. Schema fully described in swagger including the action variant.

### UI
- New "Schedules" view + store: list with enable toggles, add/edit form (day-of-week
  chips, time picker, action selector populated from `/api/equipment/buttons` +
  fixed action types), delete with confirm. Reuse the command feedback (Applied/Rejected)
  visual language from the dashboard.
- Respect monitor-only mode (disable mutating controls client-side, consistent with
  existing behaviour).

### Tests
- Unit: next-fire/minute-match logic across day boundaries, day masks, DST notes
  (deterministic via injected clock); JSON round-trip including rejection of malformed
  entries; validation bounds.
- Integration: schedule fires → dispatcher invoked (assert via the wire-bytes capture
  pattern in `test/integration/flows/` or a mock dispatcher).
- Options tests; Playwright spec creating + listing a schedule against the replay app
  (commands may 503 in replay mode — that is documented/accepted; assert the schedule CRUD
  itself, not command success).

### Acceptance
- `--schedules-file` unset ⇒ identical behaviour to today; routes return 503.
- Schedules persist across restart; fire at the right local time; Service mode suppresses.
- Full suite passes; swagger complete.

---

## WS5 — UI authentication (login + browser-compatible WebSocket auth)

**Status: DONE.** The WebSocket-subprotocol auth transport, the login overlay, `GET /api/auth/check`
(swagger documented), and the token-attaching fetch/WS helpers all shipped. The
e2e-ui `identity` jobs (`AQUALINK_AUTH_MODE=enabled`) run `e2e/auth.spec.ts` (and, since Wave B,
`admin.spec.ts` / `guest.spec.ts`) against an identity-enabled build; the earlier WS5 token-gated
`auth.spec` was superseded by the identity system. The web UI works (login → live WebSocket). The shipped auth surface is
documented in [usage-and-api.md](usage-and-api.md).

**Size:** medium. **Risk:** medium (touches the security path). **Dependencies:** none.

### The problem this workstream solved
`--api-auth-token` enforces `Authorization: Bearer <token>` on routes AND WebSocket
upgrades (`EvaluateSecurity` in `src/core/http/server/routing/routing.cpp:184-251`,
constant-time compare at ~210-235; the 401 + `WWW-Authenticate: Bearer` response is built
in `MakeSecurityResponse` at ~100-101. The WebSocket-subprotocol bearer-extraction helper
is `WebSocketSubprotocolTokenMatches` at ~148-178). Before WS5, the UI had no login and never
attached the header — and **browsers cannot set arbitrary headers on WebSocket upgrades**, so
even a token-aware UI could not authenticate `/ws/*`, leaving the web UI unusable whenever a token
was set. WS5 closed that transport gap (see the design below); it is the as-delivered behavior.

### Design
1. **WS auth transport:** accept the token on WebSocket upgrades via the
   `Sec-WebSocket-Protocol` header: client requests protocols
   `aqualink, bearer.<token>`; server constant-time-compares the `bearer.` entry and, on
   success, completes the handshake echoing protocol `aqualink` (required — a server
   selecting none while the client offered some causes browser-side close). On failure,
   reject pre-upgrade with 401. Do NOT use a query parameter (tokens leak into logs).
   Header auth continues to work unchanged for non-browser clients. Never log the token.
2. **Static assets stay unauthenticated** (index.html/scripts/css must load to show the
   login screen). Verify how `StaticFileHandler` interacts with `SecurityConfig` today
   (`routing.cpp` — the enforcement may already exempt static; if it doesn't, exempt it,
   and state in swagger/docs that only `/api` + `/ws` are protected).
3. **`GET /api/auth/check`** — returns 200 if the request is authorised (or if auth is
   disabled), 401 otherwise. Used by the login screen to validate a token. Swagger it.
4. **UI:** on app start, call `/api/auth/check`; on 401 show a full-screen login overlay
   (token input + "remember on this device" choosing sessionStorage vs localStorage).
   Attach the token to every fetch (one helper — audit all fetch call sites in
   `assets/web/scripts/`) and to WS connects via the subprotocol above (`ws-store.js`).
   Global 401 handler → clear stored token → show login. Add a logout control.
5. When auth is disabled (no token configured), the UI behaves exactly as today — the
   check returns 200 and no login is ever shown.

### Tests
- Unit: routing tests for the subprotocol path (valid/invalid/missing token; correct
  protocol echo), `/api/auth/check` semantics, static exemption.
- Playwright: launch the replay app WITH `--api-auth-token` — assert login appears, wrong
  token rejected, right token lands on dashboard with a LIVE WebSocket (this is the
  regression test for the transport gap), reload stays logged in, logout works. Keep the
  existing no-auth Playwright specs running without a token to prove the default path.

### Acceptance
- With a token set, the full UI (including live updates) works after login; without, no
  behaviour change. Token never appears in logs at any level.
- Full suite + both Playwright modes pass; swagger updated (`/api/auth/check`, auth
  description for WS subprotocol).

---

## WS6 — Prometheus `/metrics` endpoint

**Status: DONE.** Shipped as `WebRoute_Metrics` serving `GET /metrics` (swagger documented,
`text/plain` exposition).

**Size:** small. **Risk:** low. **Dependencies:** none. Good first merge after WS1.

### Design
- New `WebRoute_Metrics` serving `GET /metrics` (root level, Prometheus convention; NOT
  under `/api`) with `Content-Type: text/plain; version=0.0.4`. Hand-format the exposition
  text — no new dependency.
- Source: `StatisticsHub` (`src/core/kernel/statistics_hub.{h,cpp}`) — inspect what it
  actually holds and export at minimum:
  - `aqualink_serial_read_bytes_total` / `aqualink_serial_write_bytes_total` (counters)
  - `aqualink_messages_total{direction=...}` and error counters
    (checksum/deserialization/overflow) as `aqualink_message_errors_total{kind=...}`
  - latency percentiles as gauges: `aqualink_latency_microseconds{stage="serial_read|serial_write|msg_proc",quantile="0.01|0.5|0.95|0.99"}`
  - `aqualink_build_info{version=...,commit=...} 1`
  - `aqualink_devices{state="active|lost"}` gauge from the device registry if cheaply
    available.
- Subject to the same `SecurityConfig` bearer enforcement as other routes (Prometheus
  scrapers support bearer auth natively). Always registered — read-only, no flag needed.
- Document the route in swagger (text/plain response).

### Tests
- Unit: exposition format snapshot test (stable ordering), counter passthrough from a
  populated StatisticsHub, 401 when token set and absent.

### Acceptance: route serves valid exposition (verify with `promtool check metrics` text
rules manually or a format unit test); full suite passes; swagger updated.

---

## WS7 — PWA polish

**Status: DONE.** Shipped as `assets/web/manifest.json`, `assets/web/sw.js`, and
`e2e/pwa.spec.ts`.

**Size:** small. **Risk:** low. **Dependencies:** merge after WS5 (the service worker must
not cache auth-gated API responses; coordinate cache strategy with the login flow).

### Design
- `assets/web/manifest.json` (name, short_name, icons 192/512 — generate simple ones from
  the existing favicon/branding in `assets/web`, theme/background colours matching the
  UI's dark theme variables), `<link rel="manifest">` + `theme-color` meta in
  `index.html`.
- `assets/web/sw.js`: cache-first for the static shell (html/js/css/icons) with a
  version-stamped cache name (bump from the app version constant if accessible at build
  time, else hand-bump), **network-only for `/api/*` and never intercept `/ws/*`**.
  Register in index.html, HTTPS-or-localhost only (service workers require it).
- No offline command queueing, no push — explicitly out of scope.

### Tests
- Playwright: manifest fetches 200 and parses; service worker registers; an `/api`
  request is NOT served from cache (assert via response headers or SW absence on that
  path).

### Acceptance: Lighthouse "installable" criteria met locally; no behaviour change to API
traffic; full suite + Playwright pass.

---

## Explicitly EXCLUDED (capture-gated — do not attempt)

These require live hardware captures that do not exist yet; synthetic guesses are
forbidden. They are listed so nobody "helpfully" implements them:
- Jandy light colour-mode byte encoding (programs/shows).
- `Chem_Analyzer` (0x84–0x87) device/message support beyond the existing id-layer
  recognition.
- Pentair payload field-offset reconciliation and any new Pentair device types
  (IntelliValve etc.) — framing/checksum/generator layers are complete; only field offsets
  are pending real captures.

---

## Orchestration & merge order (historical record)

All seven workstreams have merged into `develop`. This section records the order they landed in,
kept for context; it is not an open plan. Each WS was developed on its own branch/worktree per
ground rule 2 and merged sequentially, rebasing each branch on the live tip immediately before
merging (the repo sees concurrent sessions; the atomic
`git update-ref refs/heads/develop <new> <old>` compare-and-swap was used when landing without a
checkout, re-verifying files survived afterwards).

Order (small/additive → large; later items absorbed the registration-block merge noise):

1. **WS1** — tests/verification only; zero conflict surface.
2. **WS6** — one route + swagger.
3. **WS7** — assets only (can also trail WS5; if WS5 is delayed, ship WS7 with a
   network-only-for-everything-dynamic SW and revisit).
4. **WS3** — alerting (new dir + MQTT entities + one options area).
5. **WS5** — UI auth (routing core touch; merge before the big UI workstreams so their
   Playwright specs are written against the final auth reality).
6. **WS2** — history (new dep, new dir, routes, UI view).
7. **WS4** — scheduler (largest UI + API surface; merges last, resolves trivial
   registration-block overlaps with WS2/WS3/WS5).

Expected conflict points are confined to: the `HTTP::Routing::Add` block in
`src/aqualink-automate.cpp`, the options-registry assembly in the same file,
`vcpkg.json` (WS2 only), the nav bar in `index.html`, and `swagger.yaml` — all
line-additive and trivially resolvable if each WS keeps additions alphabetised/one-per-line.

Per-WS definition of done: full unit suite passes from the dev shell; new tests cover the
new behaviour; swagger in sync; Playwright passes where the WS touches the UI; commits use
explicit pathspecs with all new files `git add`ed; no attribution trailers.
