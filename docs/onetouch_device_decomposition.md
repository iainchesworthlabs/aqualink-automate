# Decomposing `OneTouchDevice` — a composable, testable, SOLID target

**Status:** Recommendation / analysis snapshot (2026-07-06). No code has been changed.
**Scope:** `src/jandy/devices/onetouch_device.{h,cpp}`, its sibling translation units
under `src/jandy/devices/onetouch/`, and the navigation stack it drives
(`src/jandy/navigation/`).

SonarCloud flags the header with **cpp:S1448** (107 methods, limit 35) and
**cpp:S1820** (39 fields, limit 20). It is the largest class in the codebase. This
document maps the class's responsibilities, proposes a delegation-based
decomposition into injected collaborators, calls out the DRY / implicit-state
cleanups, and gives a phased, behaviour-preserving migration plan with the
per-step risk and blast radius.

> **Read this first — the one constraint that shapes everything.**
> `CommandDispatcher::DispatchToCapable` (see `command_dispatcher.h`) discovers
> actuation capabilities by `dynamic_cast<Cap*>(&device)` over the *registered*
> `Interfaces::IDevice` objects in the `EquipmentHub`. The capability mixins
> (`DeviceActuator`, `SetpointController`, `ChlorinatorController`,
> `SpaSwitchConfigurator`, `ControllerScheduleWriter`, `CommandHistory`) **must
> stay implemented on the single registered `OneTouchDevice` object**, or the
> dispatcher stops finding them. Therefore the decomposition is **delegation-based**:
> `OneTouchDevice` remains the thin capability *façade* and `IDevice`, and forwards
> the real work to collaborators. We do **not** move the capability inheritance onto
> a helper (that would require registering the helper too — a larger, riskier change,
> out of scope here).

A second enabling fact: the kernel is **single-threaded and cooperative** (see the
kernel-architecture notes). Every `*_ProcessStep` runs on the `poll()` loop. So
collaborators can hold plain references/back-pointers and there is **no locking to
reason about** — extraction is a pure structural move, not a concurrency change.

---

## 1. Current responsibilities

The class already lives in **four** translation units, but they are all members of
**one** class — which is exactly why Sonar still counts 107 methods and 39 fields
against the single header. The physical file split has been done; the *type* split
has not.

| # | Responsibility cluster | Where it lives today | Representative members | State it owns |
|---|---|---|---|---|
| **A** | **Wire-message ingest** (RS-485 → screen updates → tick) | `onetouch_messageprocessors.cpp` | `Slot_OneTouch_{Ack,MessageLong,Probe,Status,Clear,Highlight,HighlightChars,ShiftLines,DisplayUpdate,Unknown}` (10) | `m_HighlightedLine`, `m_AckType_ToSend`, `ScreenMode` |
| **B** | **Operating-state machine / tick dispatch** | `onetouch_device.cpp` | `ProcessControllerUpdates()` ×2, `WatchdogTimeoutOccurred`, `AttemptFaultRecovery`, `ConvertNavKeyCommand` | `m_OpState`, `m_ScrapingStallCounter`, `m_FaultRecoveryStatusCount`, `m_KeyCommand_ToSend`, `m_AckType_ToSend` |
| **C** | **Startup discovery / menu crawl** | `onetouch_device.cpp` | `Scraping_ProcessStep_StartUp`, `Scraping_ProcessStep`, `ReportMenuSurvey`, `ValidateDiscoveredEquipment`, `DataHubHasSeededAuxLabels` | `m_SpiderEngine` (crawl), `m_MenuSurveyResult`, `m_ScrapingStallCounter` |
| **D** | **Page processors** (screen → DataHub, read path) | `onetouch_pageprocessors.cpp` | 35 × `PageProcessor_*`, `PublishControllerSchedules` | `m_ControllerSchedules`, `m_ControllerScheduleGroup`, `m_ControllerScheduleStore` |
| **E** | **Status-line processors** (Equipment Status page) | `onetouch_statusprocessors.cpp` | 9 × `StatusProcessor_*`, `StatusProcessor_ShouldSkipLineProcessing` | none (pure → DataHub) |
| **F** | **Actuation goals** (keypad write path — the bloat centre) | `onetouch_device.cpp` | `ActuateDevice`/`Actuation_ProcessStep`, `SetPool/SpaSetpoint`+`QueueValueEdit`+`ValueEdit_ProcessStep`, `SetChlorinatorBoost`+`Boost_ProcessStep`, `SetSpaSwitchAssignment`+`SpaSwitchEdit_ProcessStep`, `Create/Delete/EditControllerProgram`+`QueueScheduleWrite`+`ControllerScheduleWrite_ProcessStep`, `SetpointRefresh_ProcessStep` | **~24 fields** (see §3) + 6 phase enums + 3 goal structs |
| **G** | **Screen-reading helpers** (shared by F) | `onetouch_device.cpp` | `DisplayedValue`, `DisplayedFunctionOnRow`, `FindLineStartingWith`, `SanitiseFunctionText`, `DisplayedTime`, `DisplayedDays` | none (pure functions of the page) |
| **H** | **Identity / diagnostics / test seams** | `onetouch_device.cpp` | ctor, dtor, `DescribeDiagnostics`, `EnableChlorinatorSetpointRefresh`, `AvailableFunctions`, 7 × `*ForTest` | `m_ProfilingDomain` |

Cluster **F** is the reason the class is a god-class: **six near-identical per-tick
phase machines**, each carrying its own `m_XInProgress` + `m_PendingX` +
`m_XStepCount` (+ `m_XPhase`) quartet of fields, all serialising on the **single
shared `Navigator` / cursor / screen / one-key-per-tick output**.

---

## 2. The good patterns already in the tree (extend these, don't invent new ones)

The maintainer has already demonstrated the target style twice. The decomposition
should look like more of the same, not a new paradigm:

- **`ChlorinatorSetpointRefresh`** (`chlorinator_setpoint_refresh.h`) — the *policy*
  ("when should we re-scrape") is extracted into a header-only, device-free struct
  with a pure `Evaluate(...)` taking the gating booleans + a clock, unit-testable in
  isolation. The *mechanism* (the menu walk) stays in the device. **This is the model
  for every goal below.**
- **`onetouch_schedule_parser`** (`Devices::OneTouch::Parse*`) — the read-path decode
  is a set of free functions over a `ScreenDataPage` (or raw lines), tested without a
  device (`test_jandy_onetouch_schedule_parser.cpp`).
- **`Navigator` / `SpiderEngine` / `MenuModel` / `VisitPolicy`** — already injected
  collaborators with clean seams and their own unit tests.

---

## 3. Implicit state that should become explicit

The field count (39) is dominated by **six parallel copies of the same goal
bookkeeping**:

```
Toggle       : m_PendingActuationLabel, m_ActuationInProgress, m_ActuationStepCount
ValueEdit    : m_PendingValueEdit, m_ValueEditPhase, m_ValueEditInProgress, m_ValueEditStepCount
Boost        : m_PendingBoost, m_BoostPhase, m_BoostInProgress, m_BoostStepCount
SpaSwitch    : m_PendingSpaSwitchEdit, m_SpaSwitchEditPhase, m_SpaSwitchEditInProgress,
               m_SpaSwitchEditStepCount, m_PickerFirstSeenFunction, m_SpaSwitchCursorStuck
ScheduleWrite: m_PendingScheduleWrite, m_ScheduleWritePhase, m_ScheduleWriteInProgress,
               m_ScheduleWriteStepCount, m_ScheduleWriteFieldStep
Refresh      : m_RefreshState, m_RefreshInProgress, m_RefreshStepCount
```

Two invariants are enforced *implicitly* over these fields:

1. **"One goal at a time on the shared keypad."** Today this is a hand-maintained
   OR of **eleven** booleans in `GoalInProgress()`. There is no single "active goal"
   object — the mutual exclusion is a convention that every new goal must remember to
   join (miss a term and two cursor walks interleave and corrupt each other).
2. **"Who drives the keypad this tick."** `m_KeyCommand_ToSend` is written as a
   side-effect from **35 sites** across the file; arbitration is purely the call
   order inside `ProcessControllerUpdates`'s `NormalOperation` arm.

**Make both explicit:** a single `std::unique_ptr<IKeypadGoal> m_ActiveGoal` (or a
`std::variant`) owned by a coordinator, so `GoalInProgress() == (m_ActiveGoal !=
nullptr)` by construction, and the "one key per tick" output is a single sink handed
to the active goal. This collapses ~24 fields to ~2 and makes the invariant
impossible to violate by omission.

---

## 4. DRY targets (duplication to remove *before/while* extracting)

Verified duplication across cluster F (and one read-path case):

| Duplicated thing | Copies | Notes |
|---|---|---|
| `finish(bool ok)` lambda (reset navigator + clear pending + reset phase/in-progress) | 5 (`ValueEdit`, `Boost`, `SpaSwitchEdit`, `ScheduleWrite`, + inline in `Actuation`) | Becomes `IKeypadGoal` teardown (one place). |
| `move_cursor_to(target_line)` lambda | 2 (`SpaSwitchEdit`, `ScheduleWrite`) — **byte-identical** | Move to the shared keypad context / screen helper. |
| `line_text(i)` lambda (`i<Size() ? Sanitise(page[i]) : ""`) | 2 | Same. |
| `equals_ci(a,b)` lambda | 2 (+ ad-hoc `to_lower` in `FindLineStartingWith`, `DisplayedFunctionOnRow`) | One case-insensitive compare in the screen helper. |
| Step-limit backstop `if (++m_XStepCount > LIMIT) finish(false)` | 6 | Becomes a base-`IKeypadGoal` counter. |
| **Queue guard** `!IsEmulationActive() → NotSupported; fault-state → NotSupported; GoalInProgress() → NotSupported` | 5 (`ActuateDevice`, `QueueValueEdit`, `SetChlorinatorBoost`, `SetSpaSwitchAssignment`, `QueueScheduleWrite`) | Collapse to one `TryQueue(goal, desc)` on the coordinator. |
| Digit-run percent/int parse | 2 (`DisplayedValue` vs `PageProcessor_SetAquapure`'s `parse_percent`) | One `ScreenReader::FirstNumber(...)`. |
| **Pool-config decode + body build** | 2 (`PageProcessor_Version` vs `PageProcessor_StartUp`) — **divergent!** `StartUp` calls `ApplyPoolConfiguration`; `Version` uses an older inline `switch`. | This is a latent inconsistency, not just duplication. Unify on `ApplyPoolConfiguration` — but as its **own tested commit** (it is a behaviour change, unlike the rest). |

---

## 5. Target decomposition

The organising idea: **there is exactly one physical keypad, one cursor, one screen,
and one key-per-tick output.** Model that resource honestly as a `KeypadContext`, run
one `IKeypadGoal` against it at a time, and split the read path (screen → DataHub)
from the write path (goal → keypad).

```
                         ┌─────────────────────────────────────────────┐
                         │  OneTouchDevice  (IDevice + capability façade)│
                         │  — stays registered so dynamic_cast finds it  │
                         │                                               │
   RS-485 slots ───────► │  A. MessageRouter (thin; may stay inline)     │
                         │  B. OperatingState machine + watchdog + tick  │
                         │  H. DescribeDiagnostics (aggregates below)    │
                         │  10 capability methods → forward to Runner    │
                         └───────┬───────────────┬──────────────┬────────┘
                                 │               │              │
              read path ◄────────┘        write path           │ startup
                                 │               │              │
                 ┌───────────────▼──┐   ┌────────▼───────────┐  ┌▼──────────────────┐
                 │ OneTouchScraper  │   │ OneTouchGoalRunner │  │ OneTouchStartupCrawl│
                 │ (D + E)          │   │ (F coordinator)    │  │ (C)                 │
                 │  35 PageProc     │   │  m_ActiveGoal:     │  │  SpiderEngine crawl │
                 │  9  StatusProc   │   │   unique_ptr<      │  │  MenuSurvey         │
                 │  schedule pub.   │   │     IKeypadGoal>   │  │  ValidateEquipment  │
                 │  → DataHub       │   │  TryQueue() guard  │  │  → DataHub          │
                 └──────────────────┘   └───────┬────────────┘  └─────────────────────┘
                                                │ Step(KeypadContext&)
                       ┌──────────────┬─────────┼──────────┬──────────────┬───────────┐
                       ▼              ▼         ▼          ▼              ▼           ▼
                  ToggleGoal   ValueEditGoal BoostGoal SpaSwitchGoal ScheduleWrite  SetpointRefresh
                                                                        Goal          Task(read-only)

   Shared seams (injected, pure):
     • KeypadContext  — { const ScreenDataPage& page; uint8_t highlightedLine;
                          Navigator& nav; void QueueKey(KeyCommands); }
     • ScreenReader   — pure fns over ScreenDataPage (cluster G): FirstNumber,
                          FunctionOnRow, FindLineStartingWith, Sanitise, Time, Days,
                          MoveCursorToward(cursor,target)->KeyCommand
     • Navigator / SpiderEngine / MenuModel — UNCHANGED existing collaborators
```

### 5.1 Collaborators and their contracts

| Collaborator | Owns | Interface (sketch) | Testable in isolation via |
|---|---|---|---|
| **`ScreenReader`** (cluster G) — free functions or a stateless struct in `Devices::OneTouch` | nothing | `FirstNumber(page,line)`, `FunctionOnRow(page,line)`, `FindLineStartingWith(page,prefix)`, `Sanitise(raw)`, `Time(page,line)`, `Days(page,line)`, `MoveCursorToward(cursor,target)→optional<KeyCommands>` | a hand-built `ScreenDataPage` — no device, no bus |
| **`KeypadContext`** — the shared-resource seam | nothing (borrows) | value passed to `Step()`: current page, highlighted line, `Navigator&`, a `QueueKey(KeyCommands)` sink | a fake context struct in a Boost.Test case |
| **`IKeypadGoal`** + concrete `ToggleGoal / ValueEditGoal / BoostGoal / SpaSwitchGoal / ScheduleWriteGoal` | its own phase enum + params + step counter (fields lifted off the device) | `Step(KeypadContext&) → {Running, Done, Failed}` | drive `Step()` against a fake context + scripted pages |
| **`SetpointRefreshTask`** (read-only GET) | `ChlorinatorSetpointRefresh` (policy, already extracted) + in-flight crawl | `Evaluate(...)` (exists) + `Step(KeypadContext&)` | policy already unit-tested; mechanism via fake context |
| **`OneTouchGoalRunner`** (cluster F coordinator) | `unique_ptr<IKeypadGoal> m_ActiveGoal` | `TryQueue(make_goal, desc) → ActuationResult` (the *one* queue guard), `Service(KeypadContext&)` (drives active goal, clears on Done/Failed), `Busy()` | inject fake goals; assert one-at-a-time + guard |
| **`OneTouchScraper`** (clusters D + E) | `DataHub&`, `ControllerScheduleStore`, `m_ControllerSchedules`, `m_ControllerScheduleGroup` | the 35 `PageProcessor_*` + 9 `StatusProcessor_*`, registered into the `Screen` processor list (bound to *this* collaborator, not the device) | fixture pages → assert DataHub mutations (existing `test_devices_onetouch_pageprocessors`) |
| **`OneTouchStartupCrawl`** (cluster C) | `SpiderEngine` crawl, `m_MenuSurveyResult` | `Step()` returning a crawl outcome the op-state machine consumes; `Survey()` for diagnostics | replay fixtures via `MockReplayHarness` |

### 5.2 What stays on `OneTouchDevice`

- Construction / destruction, profiling domain, slot registration.
- The **10 capability methods** — now thin: build the concrete goal and hand it to
  `m_Runner.TryQueue(...)` (or forward to the scraper/refresh). They must remain here
  for `dynamic_cast` discovery.
- The **operating-state machine**: `ProcessControllerUpdates`, `WatchdogTimeoutOccurred`,
  `AttemptFaultRecovery`, `ConvertNavKeyCommand`, and the `m_OpState` field + counters.
  In `NormalOperation` the arm becomes `m_Runner.Service(ctx)` + `m_Refresh.Step(ctx)`;
  in `Scraping` it delegates to `m_StartupCrawl.Step()`.
- `DescribeDiagnostics` — aggregates JSON from the collaborators.
- The `*ForTest` seams (some migrate to the collaborators they now belong to).

After Phases 0–4 this is roughly **~22 methods / ~11 fields** (op-state + counters,
the three/four collaborator handles, `m_Navigator`/`m_SpiderEngine`/`m_MenuModel`,
`m_AckType_ToSend`/`m_KeyCommand_ToSend`, `m_HighlightedLine`, `m_ProfilingDomain`) —
comfortably under both Sonar thresholds.

---

## 6. Phased migration plan

Each phase is independently shippable and must leave **build + unit + integration
tests green**. Ordered lowest→highest blast radius. The existing fixtures are the
safety net: `onetouch_equipment_toggle.cap`, `onetouch_setpoint_edit.cap`,
`onetouch_chlorinator.cap`, `iaq_onetouch_startup.cap`, and the flow tests
(`test_flow_onetouch_{toggle,setpoint,chlorinator}.cpp`), plus
`test_devices_onetouch*`, `test_jandy_onetouch_schedule_*`, `test_spider_engine`,
`test_navigation_navigator_edgecases`.

| Phase | Move | Risk | Wire / Nav? | Blast radius | Net effect |
|---|---|---|---|---|---|
| **0. ScreenReader** | Cluster G → pure `ScreenReader` fns; device methods become forwarders; fold `parse_percent` into `FirstNumber` | **Low** | No | Read helpers only | Pure fns unit-tested for the first time; 1 DRY win |
| **1. DRY the state machines** | Introduce `move_cursor_to` / `line_text` / `equals_ci` (via ScreenReader) and a single `TryQueue` guard; collapse the 5 queue guards + duplicated lambdas. *No extraction yet.* | **Low–Med** | Touches write path (mechanical only) | Cluster F internals | Uniform goals → de-risks Phase 3; −a few methods |
| **2. OneTouchScraper** | Clusters D + E → collaborator holding `DataHub&`; re-bind page/status processors to it | **Med** | No (read path) | DataHub publish only | **Biggest S1448 win (~45 methods gone)** + 3 fields |
| **2b. Pool-config unify** | Make `PageProcessor_Version` call `ApplyPoolConfiguration` like `StartUp` | **Med** | No | Body-building | Fixes a real divergence; separate tested commit |
| **3a–3f. Extract goals** | `KeypadContext` + `IKeypadGoal` + `OneTouchGoalRunner`; migrate **one goal per commit**: 3a Toggle → 3b ValueEdit → 3c Boost → 3d SpaSwitch → 3e ScheduleWrite → 3f SetpointRefresh | **Med–High** | **YES — keypad write + navigation** | Actuation per goal | Removes ~24 fields + 6 enums + 3 structs; each goal unit-testable in isolation for the first time |
| **4. OneTouchStartupCrawl** | Cluster C → collaborator; op-state machine calls `Step()` | **Med–High** | **YES — SpiderEngine crawl + op-state** | Startup/discovery | Last: entangled with `m_OpState` transitions |
| **5. MessageRouter** *(optional)* | Cluster A → thin router | **Low** | Ingest only | — | Skip unless still over the limit after 0–4 |

**Phase-3 safety technique:** move each phase-machine `switch` body **verbatim** into
its goal's `Step()`; the *only* change is that field access
(`m_ValueEditPhase`, `m_HighlightedLine`, `m_KeyCommand_ToSend = …`,
`DisplayedPage()`) is rerouted through the goal's own members and the injected
`KeypadContext`. Behaviour is byte-for-byte identical; the flow fixture for that goal
proves it before the commit lands. Do **not** combine goals in one commit — the whole
point is per-goal revertibility on the highest-risk path.

**Highest-risk steps are 3a–3f and 4** (they touch the RS-485 keypad write path and
the Navigator/SpiderEngine). Phases 0–2 are safe structural/read-path moves and should
land first to shrink the header and build confidence in the seams.

---

## 7. Genuinely inherent complexity — leave it alone

Splitting these would add indirection without buying testability or clarity:

- **The operating-state machine** (`ColdStart → StartUp → Scraping → NormalOperation`
  + `ScrapingFaulted` / `FaultHasOccurred`, watchdog degrade, fault-recovery
  hysteresis). This *is* the device's essence and must stay on the device; the
  `AttemptFaultRecovery` hysteresis and the "key only in a Status ACK, V1→V2 handshake"
  wire subtlety are inextricable from `ProcessControllerUpdates`. Keep them glued.
- **The single-keypad serialization.** The goals are mutually exclusive by *physical
  necessity* — one cursor, one screen. `KeypadContext` + one `m_ActiveGoal` models that
  honestly; do not try to make goals "independent" or concurrent.
- **Empty/log-only page processors** (`PageProcessor_MenuHelp`, `_SetTime`, …). They
  exist because a page must be *registered* to be detectable/navigable even when nothing
  is scraped from it. Move them with the scraper; don't delete them.
- **`Navigator`, `SpiderEngine`, `MenuModel`** are already well-bounded collaborators
  (and large in their own right). They are **not** part of this refactor.

---

## 8. Summary

`OneTouchDevice` is a god-class because six structurally-identical keypad phase
machines and two screen-scraping families all live as members of the one type that
must stay registered for capability discovery. The fix is **delegation, not
re-inheritance**: keep the capability façade on the device, and lift

1. the read path into an **`OneTouchScraper`**,
2. the write path into **`IKeypadGoal` goals behind an `OneTouchGoalRunner`** over a
   shared **`KeypadContext`**,
3. the screen-reading primitives into a pure **`ScreenReader`**, and
4. startup discovery into an **`OneTouchStartupCrawl`**,

each mirroring the already-proven `ChlorinatorSetpointRefresh` / schedule-parser
style. The phased plan lands the safe read-path and DRY work first (Phases 0–2), then
migrates the high-risk keypad goals one revertible commit at a time (Phase 3), with the
existing replay fixtures as the behaviour-preserving guard. The result drops the class
under both Sonar limits and makes every previously-untestable phase machine
unit-testable in isolation for the first time.
