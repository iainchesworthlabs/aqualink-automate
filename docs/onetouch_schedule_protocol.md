# OneTouch Schedule Protocol (read path — reverse-engineered)

Status: **read path decoded AND implemented** (parser + detection fix + store population landed
2026-07-05). Decoded from a live capture (`captures/onetouch_program.cap` in the schedule-reveng
worktree + the recorder log). Companion to the IAQ decode (`docs/iaq_schedule_protocol.md`).
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

## Write path — NOT yet decoded

Add/Change/Delete Program on the OneTouch is menu navigation (Navigator: select/back/scroll, arrow
value entry for times/days) rather than the IAQ touch grid. The capture recorded a full add → change
time → change day → delete cycle, but the per-keypress decode is deferred to a follow-up; the read
path lands first.
