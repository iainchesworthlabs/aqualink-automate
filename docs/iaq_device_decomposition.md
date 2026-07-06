# IAQDevice Decomposition — Recommendation

**Status:** Analysis / recommendation only (dated 2026-07-06). No refactor performed.
**Subject:** `src/jandy/devices/iaq_device.h` + `iaq_device.cpp` and the collaborators under
`src/jandy/devices/iaq/`.
**Trigger:** SonarCloud `cpp:S1448` (55 methods > 35) and `cpp:S1820` (37 non-static data
members > 20) on `IAQDevice`.

> This is a design snapshot. Anchors are symbols/URLs, not line numbers (per `CLAUDE.md`).
> Verify against the code before relying on any claim; the write-path clusters especially are
> capture-gated and must not be changed by guesswork.

---

## Implementation status (2026-07-06, branch `claude/iaq-device-decomp`)

Landed, each an independently green + committed step (full test suite `*** No errors detected`):

| Phase | What | Result |
|---|---|---|
| 0 | `Utility::EqualsCaseInsensitive` added; writers' `eq_ci` lambdas migrated to it | done |
| 1 | **`IAQ::PageModel`** extracted (`iaq_page_model.{h,cpp}`) — page id/title/button table + schedule/device-picker/spa-switch-picker rows + `FindButtonByLabel`; new unit test | done |
| 3 | **`IAQ::PageSurvey`** extracted (header-only in `iaq_page_registry.h`) — the 3 survey fields → 1 | done |
| 7 | **`IAQ::SpaSwitchWriter`** extracted over a new **`IAQ::ICommandSink`** seam that `IAQDevice` implements | done |
| 8 | **`IAQ::ScheduleWriter`** (create/delete/edit) extracted over `ICommandSink` (extended with `ArmControlValue`) | done |

**Outcome vs the Sonar rules:**
- **`cpp:S1820` (fields) — SATISFIED.** `IAQDevice` went from **37 → 17** non-static data members (the two write state machines + survey + page clusters collapsed into `m_PageModel`, `m_SpaSwitchWriter`, `m_ScheduleWriter`, `m_PageSurvey`). Under the limit of 20, with margin.
- **`cpp:S1448` (methods) — improved but still above 35, as predicted in §5/§6.** The private helpers that could leave did (`FindPageButtonByLabel`, `SpaSwitchWrite_ProcessStep`, `QueueScheduleWrite`, `ControllerScheduleWrite_ProcessStep`), but the 17 `signals2` slots + ~17 capability overrides + ctor/dtor/state-queries are all methods that *must* stay on `IAQDevice`. Driving it ≤35 needs the separate **data-driven slot-registration table** (out of scope here — a deliberate, individually-reviewed follow-up).

**Not yet done (optional / lower value):** phase 2 (status publisher), phase 4 (schedule reader), phase 5b (relocate the command-channel fields into an `IAQCommandChannel`), phase 6 (actuator), phase 9 (cross-device `PollGoalRunner` shared with OneTouch). None are needed for S1820; they are cpp-complexity / DRY / testability improvements. The `ICommandSink` seam now exists, so the actuator and a full command-channel extraction are straightforward follow-ups.

---

## 1. Why the class is over the threshold

`IAQDevice` is the 0x33 (AqualinkTouch) / 0xA3 (iAqualink2 cloud) virtual-device handler. It
simultaneously plays **eleven** capability roles (its base list: `JandyController`,
`Restartable`, `Screen`, `Emulated`, `Describable`, `ChlorinatorController`, `PageNavigator`,
`DeviceActuator`, `SetpointController`, `SpaSwitchConfigurator`, `CommandHistory`,
`ControllerScheduleWriter`) **and** owns the mutable state for six unrelated concerns:

- decoded live-page UI state (page id, title, on-screen button table, table rows),
- a poll-ACK command channel + control-data handshake,
- status ingest → DataHub publishing,
- a spa-switch-assignment write state machine,
- a controller-schedule read path,
- a controller-schedule write state machine (create/delete/edit),
- a one-shot start-up page survey.

**Important:** the files already under `src/jandy/devices/iaq/` do *not* reduce the count. Only
`iaq_schedule_parser.{h,cpp}` and `iaq_page_registry.{h,cpp}` are genuine collaborators (free
functions in namespace `Devices::IAQ`, and both already have standalone unit tests —
`test_jandy_iaq_schedule_parser.cpp`, `test_jandy_iaq_page_registry.cpp`). By contrast
`iaq_messageprocessors.cpp` and `iaq_statusprocessors.cpp` are **partial-class file splits**:
they define `IAQDevice::` member functions in separate translation units. Every method and field
there still belongs to `IAQDevice`, so Sonar still counts them. Reducing the metric requires
moving state and behaviour onto *new types*, not into new `.cpp` files.

---

## 2. Current responsibilities — clustered

The 55 methods and 37 fields group into nine cohesive clusters. "Method / field" names are the
actual symbols in `iaq_device.h`.

### A. Device shell — identity, lifecycle, operating state *(keep on IAQDevice)*
- **Methods:** ctor, dtor, `ProcessControllerUpdates()`, `ProcessControllerUpdates(bool)` (the
  dispatch spine), `WatchdogTimeoutOccurred`, `IsInNormalOperation`, `IsFaulted`,
  `IsNotPresent`, `DescribeDiagnostics`.
- **Fields:** `m_OpState`, `m_HasReceivedData`, `m_HasReceivedMainStatus`, `m_ProfilingDomain`.
- The irreducible core: the capability facade, the `OperatingStates` machine, slot registration,
  and the per-poll orchestration that fans out to every other collaborator.

### B. Message-slot ingest *(keep as thin adapters; delegate bodies)*
- **Methods:** the 17 `Slot_IAQ_*` handlers (`AuxStatus`, `CommandReady`, `ControlReady`,
  `Heartbeat`, `MainStatus`, `MessageLong`, `OneTouchStatus`, `PageButton`, `PageContinue`,
  `PageEnd`, `PageMessage`, `PageStart`, `Poll`, `Probe`, `StartUp`, `TableMessage`,
  `TitleMessage`).
- Each is a `boost::signals2` slot registered in the ctor via `m_SlotManager`. They must remain
  on `IAQDevice` (the `SlotManager` binds `this`), but their *bodies* should shrink to: update the
  relevant collaborator, then `ProcessControllerUpdates(); Restartable::Kick();`. The 17 slots are
  the single biggest contributor to the method count and the hardest to remove — see §6.

### C. Live-page UI model *(extract → `IAQPageModel`)*
- **Methods:** `FindPageButtonByLabel`.
- **Fields:** `m_CurrentPageId`, `m_CurrentPageTitle`, `m_PageButtons` (+ `PageButtonInfo`),
  `m_ScheduleRows`, `m_DevicePickerRows`, `m_SpaSwitchPickerRows`, `m_StatusPage`, `m_TableInfo`,
  `m_SM_PageUpdate`, `m_SM_TableUpdate`.
- The decoded "what is on screen right now" blackboard. Written by cluster B, read by clusters
  E/F/G/H. This is the shared mutable state that makes the writers untestable in isolation today.

### D. Status ingest → DataHub / Screen *(extract → `IAQStatusPublisher`)*
- **Methods:** `ProcessMainStatus`, `ProcessAuxStatus`, `RenderStatusScreen`,
  `RenderCloudLinkScreen`.
- Already file-isolated in `iaq_statusprocessors.cpp`. Pure read-path: decodes a message and
  writes bodies/temps/setpoints/heaters/auxes into the `DataHub`, then renders the `Screen`.

### E. Direct-command actuation *(extract → `IAQActuator`)*
- **Methods:** `QueueChlorinatorPercentage`, `QueueChlorinatorBoost`, `SetChlorinatorPercentage`,
  `SetChlorinatorBoost`, `ActuatePageButton`, `SelectPageButton`, `ActuateDevice`,
  `SetPoolSetpoint`, `SetSpaSetpoint`, `QueueSetpoint`, `AvailableFunctions`.
- The capability implementations that translate a request into a fixed poll-ACK command sequence
  (`m_CommandQueue`) + optional control-data value. Depend on cluster C (button lookup) and F/C
  command channel, not on the write state machines.

### F. Command channel + control-data handshake *(extract → `IAQCommandChannel` / `IAQCommandSink`)*
- **Methods:** `QueueCommand`, `Signal_ControlDataResponse`, and the send block inside
  `ProcessControllerUpdates(bool)`.
- **Fields:** `m_PendingCommand`, `m_CommandQueue`, `m_AwaitingControlReady`, `m_ControlDataValue`.
- The single wire-write seam: whatever ends up here is what rides the `Signal_AckMessage` poll
  ACK. Every actuator and both writers poke these fields directly today.

### G. Controller-schedule read *(extract → `IAQScheduleReader`)*
- **Methods:** `PublishSchedulePage`.
- **Fields:** `m_ControllerScheduleStore`, `m_ScheduleRows` (shared with C), `m_CurrentPageTitle`
  (shared with C).
- On `PageEnd` of page `0x28`, parse accumulated `m_ScheduleRows` (via `IAQ::ParseScheduleRow`)
  into `ControllerScheduleStore`. Read-only; already backed by `test_jandy_iaq_schedule_read.cpp`.

### H. Controller-schedule write state machine *(extract → `IAQControllerScheduleWriter`)*
- **Methods:** `CreateControllerProgram`, `DeleteControllerProgram`, `EditControllerProgram`,
  `QueueScheduleWrite`, `ControllerScheduleWrite_ProcessStep`.
- **Fields:** `m_PendingScheduleWrite` (+ `ScheduleWriteGoal`/`ScheduleWriteOp`/
  `ScheduleWritePhase`), `m_ScheduleWritePhase`, `m_ScheduleWritePollCount`,
  `m_ScheduleWriteSettleCount`, `m_ScheduleWriteScrollCount`, `m_ScheduleProgramAdded`,
  `m_ScheduleDeviceClicked`, `m_ScheduleTimeFieldOpened`, `m_DevicePickerRows` (shared with C).
- A per-poll goal servicer that drives the Program pages. Backed by
  `test_jandy_iaq_schedule_write.cpp`. **Highest risk** (wire + schedule write path).

### I. Spa-switch write state machine *(extract → `IAQSpaSwitchWriter`)*
- **Methods:** `SetSpaSwitchAssignment`, `SpaSwitchWrite_ProcessStep`.
- **Fields:** `m_PendingSpaSwitchWrite` (+ `SpaSwitchWriteGoal`/`SpaSwitchWritePhase`),
  `m_SpaSwitchWritePhase`, `m_SpaSwitchRowSelected`, `m_SpaSwitchWritePollCount`,
  `m_SpaSwitchScrollCount`, `m_SpaSwitchSettleCount`, `m_SpaSwitchFirstPickerSeen`,
  `m_SpaSwitchPickerRows` (shared with C).
- Structurally a twin of cluster H. Backed by `test_devices_iaq_spaswitch_config.cpp`. **High
  risk** (wire).

### J. Start-up page survey *(extract → `IAQPageSurvey`)*
- **Methods:** `EnablePageSurvey`, `MaybeStartPageSurvey`.
- **Fields:** `m_PageSurveyEnabled`, `m_PageSurveyDone`, `m_PageSurveyRegistry`.
- Small, self-contained, emulated-only; already uses `IAQ::BuildSurveyCommandSequence`.

---

## 3. Target decomposition

Composition over inheritance: `IAQDevice` keeps clusters A + B (it *must* — capability facade and
`SlotManager`-bound slots), and **holds** the rest as member collaborators. The two moving pieces
that make everything else testable are a shared **page model** (data) and a **command sink**
(interface).

```
                         ┌────────────────────────────────────────────┐
                         │ IAQDevice  (clusters A + B)                 │
                         │  - capability facade (11 mixins)            │
                         │  - OperatingStates machine, watchdog        │
                         │  - 17 Slot_IAQ_* adapters                   │
                         │  - implements IAQCommandSink                │
                         └───┬───────────┬───────────┬───────────┬─────┘
             writes │        │ owns       │ owns      │ owns      │ owns
        ┌───────────▼──┐  ┌──▼─────────┐ ┌▼────────┐ ┌▼────────┐ ┌▼──────────────┐
        │ IAQPageModel │  │IAQStatus   │ │IAQActu- │ │IAQSpa   │ │IAQController  │
        │ (cluster C)  │  │Publisher(D)│ │ator (E) │ │Switch   │ │ScheduleWriter │
        │  page id,    │  │            │ │         │ │Writer(I)│ │ (H)           │
        │  title,      │  └─────┬──────┘ └────┬────┘ └────┬────┘ └──────┬────────┘
        │  buttons,    │        │ DataHub&    │ sink      │ model&      │ model&
        │  rows,       │◀───────┴─────────────┴───────────┴─ read ──────┘
        │  screen SMs  │        ┌──────────────┐   ┌──────────────────┐
        └──────────────┘        │IAQSchedule   │   │ IAQPageSurvey (J)│
              ▲ read            │Reader (G)    │   └──────────────────┘
              └─────────────────┴──────────────┘
                          write → CommandSink → Signal_AckMessage (poll ACK)
```

### Key seams (the DI-friendly interfaces)

**`IAQCommandSink` — the wire-write boundary.** Both writers and the actuator currently mutate
`m_PendingCommand`, `m_AwaitingControlReady`, `m_ControlDataValue` directly. Replace with:

```cpp
class IAQCommandSink {
public:
    virtual ~IAQCommandSink() = default;
    virtual void IssueCommand(uint8_t cmd) = 0;         // set the next poll-ACK command
    virtual void ArmControlValue(std::string value) = 0; // "1"+HH:MM / "1"+percent handshake
    virtual void QueueSequence(std::span<const uint8_t>) = 0; // multi-step command queue
};
```

`IAQDevice` (or a small `IAQCommandChannel` it owns) implements it. A **fake sink** in tests
records the emitted bytes — so a writer's `ProcessStep` becomes a pure function of
`(IAQPageModel, IAQCommandSink&, DataHub*)` and is unit-testable with a hand-built page model,
**no bus and no MockReplayHarness required for the state-machine logic**.

**`IAQPageModel` — the decoded-page blackboard.** A plain value type owning cluster C's fields
plus `FindPageButtonByLabel`. Cluster B writes it (`SetPageId`, `SetTitle`, `UpsertButton`,
`SetScheduleRow`, `SetDevicePickerRow`, `SetSpaSwitchPickerRow`, `Clear`); clusters E/F/G/H/I read
it. This is the single change that unlocks isolation of the writers.

### Collaborator contracts

| New type | Owns (fields from) | Public surface | Depends on | Testability |
|---|---|---|---|---|
| `IAQPageModel` | C | page id/title getters, button table, row maps, `FindPageButtonByLabel` | — | Pure data; direct unit tests |
| `IAQStatusPublisher` | D | `ProcessMainStatus(msg)`, `ProcessAuxStatus(msg)` | `DataHub&`, `Screen&` (or a render callback) | MockReplayHarness + DataHub asserts |
| `IAQActuator` | E | the capability methods (chlorinator/setpoint/aux/page-button) | `IAQCommandSink&`, `const IAQPageModel&` | Fake sink + fake page model |
| `IAQCommandChannel` | F | `IAQCommandSink` impl + `Drain(is_poll) → Signal_AckMessage` | `Signal_AckMessage` callback | Fake ack callback |
| `IAQScheduleReader` | G | `OnPageEnd(model, store)` / `Publish()` | `const IAQPageModel&`, `ControllerScheduleStore*` | Existing `..._schedule_read` test |
| `IAQControllerScheduleWriter` | H | `Create/Delete/Edit(...)`, `Active()`, `ProcessStep(model, sink, hub)` | model, sink, `DataHub*` | Fake sink; existing `..._schedule_write` test + captures |
| `IAQSpaSwitchWriter` | I | `Queue(...)`, `Active()`, `ProcessStep(model, sink, hub)` | model, sink, `DataHub*` | Fake sink; `..._spaswitch_config` test + captures |
| `IAQPageSurvey` | J | `Enable(registry)`, `MaybeStart(model, sink)` | `IAQCommandSink&` | Pure; direct unit tests |

After this, `IAQDevice` retains ~clusters A+B (the ctor, dtor, 17 slots, op-state queries,
`ProcessControllerUpdates`, `WatchdogTimeoutOccurred`, `DescribeDiagnostics`) and **~8 member
collaborators + 4 op-state fields** — comfortably under both Sonar thresholds, and every extracted
unit is independently testable.

---

## 4. Duplication to DRY up (and implicit state to surface)

### 4.1 Two near-identical goal-servicer harnesses *inside* IAQDevice
`SpaSwitchWrite_ProcessStep` and `ControllerScheduleWrite_ProcessStep` each hand-roll the same
scaffolding:

- an overall poll backstop (`m_*WritePollCount > *_POLL_LIMIT → finish(false)`),
- a settle counter (`m_*SettleCount` dwell after each command),
- a local `issue(cmd)` lambda (set pending + reset settle),
- a local `finish(ok)` teardown (reset phase + counters + pending),
- a local `eq_ci` case-insensitive compare,
- a "one goal at a time" busy/passive guard (`!IsEmulationActive()` / `!IsEmulated()` +
  `m_Pending*.has_value() || !m_CommandQueue.empty() || m_AwaitingControlReady`).

Factor the mechanical part into a small reusable **`PollGoalRunner`** (settle/backstop/`issue`/
`finish` over an `IAQCommandSink`). The *navigation policy* (which command advances which phase)
stays per-writer — that is the genuinely different, capture-derived part. Do **not** try to unify
the phase enums themselves.

### 4.2 Cross-device duplication with OneTouchDevice
`OneTouchDevice` (`onetouch_device.{h,cpp}`, ~2150 LOC, the sibling god-class being analysed
separately) has the **same** shape: `Actuation_ProcessStep`, `ValueEdit_ProcessStep`,
`Boost_ProcessStep`, `SpaSwitchEdit_ProcessStep`, `ControllerScheduleWrite_ProcessStep`,
`SetpointRefresh_ProcessStep`, each with its own `finish()`, `m_PendingScheduleWrite`,
`ScheduleWritePhase`, and a duplicated case-insensitive compare (3 occurrences confirmed).

Shared abstractions that could serve both, ordered by confidence:

1. **`Utility::EqualsCaseInsensitive` (or `std::ranges` helper)** — replace every `eq_ci` in both
   devices. *(First confirm one doesn't already exist in `utility/string_manipulation.*`; if not,
   add + unit-test it.)* Lowest-risk, highest-frequency win.
2. **`PollGoalRunner`** (settle/backstop/issue/finish) — shared by all IAQ **and** OneTouch
   per-poll goals. Medium confidence: the IAQ gates on a raw page id; OneTouch gates on the
   `Navigator`/`SpiderEngine`. The *runner* is the thin common part; the gating is not.
3. **A busy/passive precondition object** (`RejectIfBusyOrPassive`) — the guard repeated across
   `Create`/`Delete`/`Edit`/`SetSpaSwitchAssignment` on both devices.

⚠️ **Do the cross-device abstraction LAST**, after both decompositions land independently.
Generalising a navigation abstraction across the page-id world (IAQ) and the menu-crawl world
(OneTouch) prematurely is a classic over-abstraction trap — see §5.

### 4.3 Implicit state to make explicit
- The "busy" predicate is an ad-hoc boolean expression evaluated in four places. Promote to a
  single `bool IsPanelBusy() const` on the command channel.
- `m_CurrentPageTitle` / `m_ScheduleRows` are cleared on `PageStart` **and** read by both the
  reader and the writer — an implicit lifetime coupling. `IAQPageModel::Clear()` makes the
  invalidation point explicit and single-sourced.
- The write goals encode "which sub-step already fired" as scattered booleans
  (`m_ScheduleProgramAdded`, `m_ScheduleDeviceClicked`, `m_ScheduleTimeFieldOpened`,
  `m_SpaSwitchRowSelected`). These are really sub-phases; folding them into the phase enum (or a
  small per-phase struct owned by the writer) removes cross-cutting flags from the class.

---

## 5. Inherent complexity — where NOT to split

- **The 17 `Slot_IAQ_*` handlers cannot leave `IAQDevice`.** They are `signals2` slots bound to
  `this` in the ctor via `m_SlotManager.RegisterSlot_FilterByDeviceId`. They *dominate* the method
  count but represent irreducible protocol surface. The win is shrinking their bodies to
  one-line delegations, not relocating them. Sonar's method count will still include them; accept
  that the class lands near — not far below — 35 unless slot registration itself is
  data-driven (a table of `{MessageType → handler}`), which is a larger, riskier change best left
  out of this pass.
- **Spa-switch vs schedule writers look mergeable but aren't.** They share *scaffolding* (§4.1)
  but their phase graphs, page ids, command bytes, and verification predicates are distinct and
  capture-derived. Merge the runner, keep the policies separate.
- **IAQ page-id gating vs OneTouch Navigator crawl are different domains.** A unified
  "page navigation" interface across both devices would leak one model into the other. Keep the
  shared piece to the mechanical `PollGoalRunner`.
- **`ProcessMainStatus` body-inference heuristic** (auto-detect `DualBody_SharedEquipment` from
  `SpaMode`, create filter pump / heaters) is genuinely coupled to DataHub semantics. It belongs
  whole inside `IAQStatusPublisher`; do not shred it into micro-helpers for their own sake.

---

## 6. Phased migration plan

Every phase is independently shippable and behaviour-preserving, ordered **risk-ascending**. The
existing suite is the safety net: `test_devices_iaq.cpp`, `test_devices_iaq_spaswitch_config.cpp`,
`test_jandy_iaq_page_registry.cpp`, `test_jandy_iaq_schedule_parser.cpp`,
`test_jandy_iaq_schedule_read.cpp`, `test_jandy_iaq_schedule_write.cpp`. Run the full IAQ suite
green **and** the MockReplayHarness fixtures after each phase. Do **not** delete or weaken a test
to make a phase pass.

| Phase | Change | Risk | Blast radius | Wire/chlor/schedule-write? |
|---|---|---|---|---|
| 0 | Hoist pure helpers: `eq_ci → Utility::EqualsCaseInsensitive` (add if absent); move `DayCommandFor`/`ScheduleTimeValue` into the `IAQ` schedule module with unit tests | Very low | file-local | No |
| 1 | Extract **`IAQPageModel`** (cluster C). Slots write it; keep bodies otherwise identical. Mechanical field move | Low–med | slots + writers read sites | No (state relocation only) |
| 2 | Extract **`IAQStatusPublisher`** (cluster D) from `iaq_statusprocessors.cpp`; inject `DataHub&` + render seam | Low–med | read path only | No |
| 3 | Extract **`IAQPageSurvey`** (cluster J) | Low | 3 fields, 2 methods | No (emulated survey nav — mild) |
| 4 | Extract **`IAQScheduleReader`** (cluster G): `PublishSchedulePage` + 0x28 accumulation | Med | `PageEnd`/`TableMessage` | No (read path) |
| 5 | Introduce **`IAQCommandSink`** + extract **`IAQCommandChannel`** (cluster F). Wrapper first (delegate), then move fields | **High** | wire-write path | **Yes — wire** |
| 6 | Extract **`IAQActuator`** (cluster E) over the sink + page model | Med–high | capability entry points | Yes — chlorinator/setpoint/aux |
| 7 | Extract **`IAQSpaSwitchWriter`** (cluster I) as `ProcessStep(model, sink, hub)` | **High** | spa-switch write | **Yes — wire + spa-switch** |
| 8 | Extract **`IAQControllerScheduleWriter`** (cluster H) — create/delete/edit | **Highest** | schedule write path | **Yes — wire + schedule write** |
| 9 | *(optional, cross-device)* Factor `PollGoalRunner` + busy/passive precondition; adopt in IAQ **and** OneTouch | Med | both god-classes | Yes — behaviour-preserving refactor of both |

### Ordering rationale
- **Data before behaviour.** Phase 1 (`IAQPageModel`) and the `IAQCommandSink` seam (phase 5) are
  the enablers; the writers (phases 6–8) can only become isolated once both exist. Read-path
  extractions (2, 3, 4) are sequenced early because they touch no wire writes and give quick,
  safe metric relief.
- **Highest-risk last, alone.** Phases 5, 7, 8 each touch the RS-485 write path. Ship them as
  **separate PRs**, each validated against its capture fixtures
  (`iaq_spaswitch_edit{,2}.cap`, `iaq_schedule_{session,clean,picker}.cap`, `iaq_editdelete.cap`,
  `iaq_aux_setpoint.cap`) via MockReplayHarness — never batch two write-path extractions together.
- **Cross-device DRY (phase 9) is optional and last** so the shared abstraction is extracted from
  two *already-clean* call sites, not guessed up front.

### Per-phase gate (each PR)
1. Build green on `config-windows-msvc-debug` (and the Linux toolchains).
2. Full IAQ unit suite green; add new unit tests for each extracted collaborator (fake sink +
   hand-built `IAQPageModel`).
3. For phases 5–8: MockReplayHarness replay of the relevant `.cap` fixtures shows byte-identical
   emitted commands vs the pre-refactor baseline.
4. No behaviour change asserted in the PR description; Sonar method/field delta reported.

---

## 7. Expected outcome against the Sonar rules

- **`cpp:S1820` (fields):** `IAQDevice` drops from 37 to ~12 (4 op-state + ~8 collaborator
  members). Under the 20 limit. Each collaborator holds well under 20.
- **`cpp:S1448` (methods):** falls to cluster A (~9) + the 17 slots ≈ 26, under 35. If the
  reviewer wants deeper headroom, the optional data-driven slot-registration table (§5) removes
  the slots from the count — but that is out of scope for a behaviour-preserving pass and should
  be a separate, explicitly-scoped decision.

Both metrics are satisfied by phases 1–8 without the slot-table change; phase 9 is purely about
DRY/quality shared with `OneTouchDevice`.
