# OneTouch Schedule Protocol (read path — reverse-engineered)

Status: **read path decoded AND implemented** (parser + detection fix + store population landed
2026-07-05); **write path DECODED** (add/change/delete keypress flow — see "Write path" below —
implementation is the next slice). Decoded from a live capture (`captures/onetouch_program.cap` in
the schedule-reveng worktree + the recorder log). Companion to the IAQ decode
(`docs/iaq_schedule_protocol.md`).
This is the "OneTouch delta" of the controller-schedule integration: the OneTouch (device 0x40)
is a **16×12 character text-menu** panel (not a touchscreen), so its Program pages are
reconstructed by the existing OneTouch Screen capability (`onetouch_messageprocessors.cpp` →
`ScreenDataPage`) and parsed by a `ScreenDataPage_Processor`.

Implementation:

- **Parser** `Devices::OneTouch::ParseProgramDetailPage` / `ParseProgramDetailLines`
  (`src/jandy/devices/onetouch/onetouch_schedule_parser.{h,cpp}`) — pure, mirrors the IAQ
  `ParseScheduleRow`. Extracts target (trimmed line 0), on/off (12h→24h from the ON/OFF rows),
  and the `days_of_week` bitmask (bit0=Mon..bit6=Sun) from the days row. Returns
  `std::optional<Scheduling::ControllerSchedule>` (nullopt for "No Programs" / malformed pages).
- **Detection fix** (see below): a second `ScreenDataPage_Processor` matcher `{ 2, "Pgm " }`
  routes the detail page to `PageProcessor_Program` (`onetouch_pageprocessors.cpp`).
- **Store population**: the OneTouch shows one program per detail page, so `OneTouchDevice`
  accumulates parsed programs keyed by `(target, program-index)` in `m_ControllerSchedules` and
  `Replace()`s the shared `Scheduling::ControllerScheduleStore` (status `Available`) with the
  whole snapshot on every detail-page visit. The store is resolved from the HubLocator in the
  constructor (`hub_locator.TryFind<Scheduling::ControllerScheduleStore>()`), exactly like the IAQ.
- **Tests**: `test/unit/devices/test_jandy_onetouch_schedule_parser.cpp` (the captured Filter Pump
  page, single-day + weekdays + midnight/noon variants, "No Programs" → nullopt, malformed →
  nullopt) and `test_jandy_onetouch_schedule_read.cpp` (drives the detail page through the real
  Screen/MessageLong/Status pipeline and asserts the store is populated + accumulates).

## Navigation

The OneTouch menu (`Menu/Help`) carries two relevant items (both open a submenu, marked `>`):

```
| Program        > |
| Program Group  > |
```

- **Program** → an equipment list; selecting an equipment opens **that equipment's Program detail
  page** (below). Equipment with no program shows `No Programs` + `Add Program` / `New Program`.
- **Program Group** → the A/B group selector (only the active group's programs are listed, same as
  the IAQ). Custom group labels + auto-switch live here too.

## Program detail page (per equipment) — the page to parse

```
+------------------+
|   Filter Pump    |   line 0: EQUIPMENT NAME (the schedule's target)
|                  |   line 1: blank
|    Pgm 1 of 1    |   line 2: "Pgm N of M"  (this program's index / total for the equipment)
| ON      11:00 AM |   line 3: "ON   <h:mm> <AM|PM>"
| OFF      2:00 PM |   line 4: "OFF  <h:mm> <AM|PM>"
| All Days         |   line 5: days — All Days / Weekdays / Weekends / Mon / Tue / ...
|                  |
| Add      Program |   action rows (the highlighted one carries the cursor `<--`)
| Delete   Program |
| Change   Program |
+------------------+
```

- Times are **12-hour + AM/PM** (convert to the store's 24-hour on/off, like the IAQ).
- Days line: `All Days`, `Weekdays`, `Weekends`, or a single day `Mon`/`Tue`/`Wed`/`Thu`/`Fri`/`Sat`/`Sun`
  (the controller's usual all/weekdays/weekends/single-day constraint).
- `Pgm N of M` — an equipment can hold several programs; Page Up/Down cycles them. Each (N) is one
  `ControllerSchedule` for that target.

## The detection bug this capture exposed (FIXED)

The `Page_Program` matcher was `{ line 0, "Program" }` and `PageProcessor_Program` was a stub.
But the **detail page's line 0 is the equipment name** ("Filter Pump"), so the detail page did
NOT match that matcher — it fell through to **`PageProcessor_EquipmentOnOff`** (whose
`{ 0, "Filter Pump" }` matcher DID fire on the detail page), which then logged `Failed to convert
equipment status; malformed input -> Pgm 1 of 1` etc. (attempting to parse a program as an
equipment on/off list).

**Fix (landed):** a second matcher `{ 2, "Pgm " }` was added for `Page_Program` — the `Pgm N of M`
row is present on every detail page and absent from the Program *list* page — so the detail page
is now routed to `PageProcessor_Program`. The `{ 0, "Filter Pump" }` EquipmentOnOff matcher still
fires on the detail page, but every detail-page row is rejected by the aux state converter (none
end in `ON`/`OFF`/`ENA`/`***`), so that processor is a harmless no-op. The processor loop runs
**all** matching processors, so both fire; only `PageProcessor_Program` does real work.

## Read-path implementation notes

- Add a detector + `ScreenDataPage_Processor` for the Program **detail** page keyed off a stable row
  (e.g. line contains `Change   Program`, or a `Pgm ` row), separate from the `Program` menu/list.
- The processor parses: target = trimmed line 0; on/off from the `ON`/`OFF` rows (12h→24h); days
  from the days row; and emits one `ControllerSchedule` per visible `Pgm N of M`. Populate the
  shared `Scheduling::ControllerScheduleStore` (status `Available`, group from Program Group), the
  same store the IAQ read path and `/api/controller/schedules` use.
- Spidering must route through Program → each equipment's detail page (multi-instance) so a
  OneTouch-only system's controller programs are read at start-up.

## Spidering completeness — GAP (deferred)

The parser + detection + store population are wired, so a Program detail page is read correctly
**whenever it is rendered** (a live/replay navigation into it, or the write-path spider once that
lands). What is **not yet** implemented is the autonomous SpiderEngine route that visits each
equipment's detail page at start-up. The menu model's `PageId::Program` page currently has only
`Back` + `LineUp`/`LineDown` self-loops and no `Select` edge into a per-equipment
`ProgramDetail` page, so the crawler does not descend into the detail pages on its own.

Adding that requires a **multi-instance** `ProgramDetail` page (one incoming Select edge per
equipment row, like `LabelAux`) plus knowledge of which list rows are equipment vs actions and the
list's scroll behaviour — capture-gated work best done alongside the write path (which must drive
the same navigation). Until then, a OneTouch-only system populates the controller-schedule store
only once a Program detail page is actually visited. Tracked here rather than half-wiring fragile
menu-model edges.

## Write path — DECODED (2026-07-05)

Decoded from the same capture (`captures/onetouch_program.cap` + recorder log), which recorded a full
**add → change-time → change-day → delete** cycle on the Pool Light equipment. Unlike the IAQ
touchscreen (fixed touch-grid `0x11+position` + a `0x31/0x24` value-submit handshake), the OneTouch
is driven entirely by **discrete navigation keys** — the same keys the existing `Navigator` /
`OneTouchDevice::m_KeyCommand_ToSend` path already emits. There is **no value payload on the wire**:
values are dialled in on the panel with arrow keys and the controller commits internally.

### Keypress wire encoding

A OneTouch keypress rides the device's **poll-ACK** to the master: `JandyMessage_Ack` with
`AckType = 0x80` (`V2_Normal`) and the `Command` byte = a `KeyCommands` value
(`onetouch_device.h`). Only four keys appear in the whole write session:

| Command | `KeyCommands` | Role in the write flow |
|--------:|---------------|------------------------|
| `0x02`  | `Back_Or_Select2` | Exit a page / editor without committing the remaining fields |
| `0x04`  | `Select`          | Enter a menu item; **commit the current editor field and advance to the next** |
| `0x05`  | `LineDown`        | Move cursor **down** one menu row; **−1** on the active editor field |
| `0x06`  | `LineUp`          | Move cursor **up** one menu row; **+1** on the active editor field |

(`PageDown_Or_Select1 = 0x01` / `PageUp_Or_Select3 = 0x03` were not used in this session.)
Acks with `AckType = 0x1f`/`Command = 0x00` are the chlorinator/idle heartbeat, **not** keys.

### Navigating to an equipment's Program detail page

```
Home ──Select(Menu/Help, row 11)──▶ Menu
Menu ──Select(Program, row 2)─────▶ Equipment list (Group A/B)
Equipment list ──scroll──▶ target equipment ──Select──▶ Program detail page
```

The **equipment list** is a scrolling list: the cursor parks on a fixed selectable row and the list
text scrolls beneath it (`LineDown` scrolls the list down, `LineUp` up — mirrors the IAQ device
picker). `Select` on the row holding the target equipment opens its detail page. This is the *same*
navigation the read-path spider must eventually drive (the deferred spidering gap above).

### Detail-page action rows → which editor

- Equipment **with** a program → detail shows three action rows: **`Add Program` (row 9)**,
  **`Delete Program` (row 10)**, **`Change Program` (row 11)**. `Select` the highlighted one.
- Equipment **with no** program → detail shows a single **`Add Program` (row 9)**; `Select` it.

### The editor (Add and Change share one wizard)

`Select`ing **Add Program** opens the editor titled **`New Program`** (line 1) with defaults
`ON 1:00 PM / OFF 1:00 PM / All Days`. `Select`ing **Change Program** opens the identical editor
titled **`Change Program`**, pre-filled with the program's current values. The editor also shows
`Use Arrow Keys / to set value. / Then SELECT.` (lines 7–9). The panel reports **no line highlight**
in the editor (`PDA_Highlight line_id = 255 / 0xFF`) — the field cursor is internal, so the driver
must track the active field itself.

**Field order (fixed):** `ON hour → ON minute → OFF hour → OFF minute → days`. For each field:

- **`LineUp` = +1, `LineDown` = −1** on that field's value (the screen echoes the new value on the
  next frame, so the driver can **closed-loop**: read the echoed value, step one key toward target,
  repeat).
- **`Select`** commits the current field and advances to the next.
- After the **last** field (days) is committed with `Select`, the program is **saved** and the panel
  returns to the detail page showing the new `Pgm N of M`. **There is no separate save opcode.**
- **`Back`** at any point abandons the edit.

Field value semantics:

- **Hour** is a single 12-hour-with-meridiem wheel: `… 11 AM, 12 PM, 1 PM, 2 PM …`; crossing 12
  flips AM/PM. There is **no separate AM/PM field** (simpler than the IAQ). Convert to/from the
  store's 24-hour value.
- **Minute** is a 0–59 wheel (wraps).
- **Days** is a wheel over the controller-allowed set only — observed `LineUp` order:
  `All Days → Weekends → Weekdays → Sun → Mon → Tue → Wed → Thu → Fri → Sat → …`. This is exactly the
  all/weekdays/weekends/single-day constraint the promotion checker (`ClassifyDaySelection`) already
  enforces — **no arbitrary multi-day** (so a promoted app schedule that isn't representable is
  rejected before we ever reach the panel).

Worked example from the capture (Add Pool Light `11:00 AM → 2:01 PM All Days`):

```
Select(Add Program)                     → New Program, ON 1:00 PM
LineDown ×2  (1PM→12PM→11AM)  Select    → ON hour = 11 AM, advance
Select                                   → ON minute = 00 (unchanged), advance
LineUp  (OFF 1PM→2PM)         Select    → OFF hour = 2 PM, advance
LineUp  (OFF 2:00→2:01)       Select    → OFF minute = 01, advance
Select                                   → days = All Days (unchanged) → SAVED
```

### Delete

`Select` **Delete Program** (row 10) on the detail page removes the program **immediately — there is
NO confirmation dialog** (contrast the IAQ, which pops a confirm dialog needing `0x01` Ok). The panel
returns to the detail page showing `No Programs`.

### Implementation plan (tee-up for the write slice)

Mirror the IAQ write state machine (`ControllerScheduleWrite_ProcessStep`) but drive the OneTouch
**Navigator** instead of touch commands — the `OneTouchDevice` already owns a Navigator, menu model,
and the `m_KeyCommand_ToSend` send path, and already implements the shared
`Capabilities::ControllerScheduleWriter` for the read path's device. So the HTTP routes + dispatcher
(`Create/Delete/EditControllerProgram` → `DispatchToCapable<ControllerScheduleWriter>`) and the
promotion/constraint checks are **already wired** — this slice only adds the OneTouch device-side
actuation.

- **Phases (Create/Edit):** `NavigateToProgramMenu → SelectEquipment(target)` (scroll-to-target,
  closed-loop on the parked row) `→ EnterEditor` (Add row for Create, Change row for Edit)
  `→ SetOnHour → SetOnMinute → SetOffHour → SetOffMinute → SetDays → Verify` (re-parse the returned
  detail page via `ParseProgramDetailPage` and confirm target+times+days).
- **Phases (Delete):** `NavigateToProgramMenu → SelectEquipment → SelectDeleteRow → Verify` (detail
  now `No Programs`).
- **Each field phase is closed-loop, not press-counting:** read the field's current value from the
  reconstructed editor screen (ON = line 3, OFF = line 4, days = line 5), emit one `LineUp`/`LineDown`
  toward the target, wait a poll for the echo, repeat until it matches, then `Select` to advance. This
  is robust to wheel direction, meridiem crossing, minute/day wrap, and pre-filled Edit values without
  hardcoding step counts. Track the active field by counting `Select`s since editor entry
  (`0`=ON-hour … `4`=days) since the panel gives no field-cursor highlight.
- **Backstop:** reuse the IAQ writer's per-poll page-gating + settle/abandon backstop so a UI that
  adds/rejects steps (e.g. max-programs, day rules) fails cleanly rather than mis-keying.

Capture coverage is complete for a single-program add/change/delete on one equipment; **still
unexercised** (defer or capture if needed): multiple programs per equipment (`Pgm N of M` > 1, i.e.
Page Up/Down to select which program to Change/Delete), and the equipment-list scroll key→row map for
an equipment far down the list (the read-path spidering gap covers the same navigation).
