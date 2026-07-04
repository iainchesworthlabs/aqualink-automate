# IAQ / AqualinkTouch Schedule Protocol (reverse-engineered)

Status: **read path decoded** from a live RS-485 capture (2026-07-04, passive snoop of a
physical iAQ/AqualinkTouch 0x33 driven via its web UI); write path partially decoded.
Fixture/source capture: `captures/iaq_schedule_session.cap` (not committed; large).
Decoder: `captures/decode_iaq.py`.

This documents how the controller's **internal schedules** (its built-in program timers)
are read and set over the wire via the AqualinkTouch page protocol (device 0x33). It is
distinct from the app-side `SchedulerService` (which fires app commands on a clock). It
feeds `Scheduling::ControllerScheduleStore` (`/api/controller/schedules`).

## Validated premise

Schedules are **controller-resident**, not stored in the iAqualink gateway: the schedule
list is served by the master to page 0x28 over RS-485, and switching the active program
group (page 0x12) changes the served list — Group B was observed genuinely empty on the
wire while Group A returned five entries. So emulating 0x33 and reading/setting these pages
reads/writes the controller's real timers.

## Page id map (PageStart 0x23 payload byte 0)

| pid  | Page                     | Notes |
|------|--------------------------|-------|
| 0x0f | Settings / menu root     | |
| 0x12 | **Program Group**        | Group A / Group B toggle, Auto Switch, (start date on a deeper page) |
| 0x14 | System Setup menu        | VSP Setup, Label Aux, Color Lights, OneTouch Setup, Spa Remotes, … |
| 0x28 | **Schedule list**        | title `Schedule Group A`/`B`; the active group's entries |
| 0x29 | **Time picker**          | row1 `HH:MM`, row2 `AM`/`PM` |
| 0x38 | **Schedule editor / device picker** | `Devices` list: Filter Pump, Spa, Pool Heat, Spa Heat, Solar Heat, Spa Jets, Swim Jet |
| 0x49, 0x4a | deeper System Setup (Heat Pump, DST, …) | |

## Page message layouts

Frames are standard Jandy `DLE STX | dest cmd payload… chk | DLE ETX`, dest `0x33` for
page pushes (master→iAQ), dest `0x00` for the iAQ's control-data replies (master-addressed).

- **TitleMessage `0x2d`**: `[ascii title]` — e.g. `Schedule Group A`.
- **PageMessage `0x25`**: `[line_id][ascii text]` — used by the time picker (line 1 = `HH:MM`, line 2 = `AM`/`PM`).
- **TableMessage `0x26`**: `[idx][state][ascii text]` — **no u16 field**. Used for list rows.
  `idx` = 0 (group id), `state` = 1-based entry/row number, text = the row.
- **PageButton `0x24`**: `[idx][state][u16][ascii name]` — note the extra u16 vs TableMessage.
  `state` bit0 = selected/active (e.g. the current day-filter, the active program group).

### Schedule list row (page 0x28, TableMessage 0x26)

Text format: `<Target> <On h:mm> <AM|PM> <Off h:mm> <AM|PM> <Days>`

Observed initial Group A:
```
Filter Pump 11:00 AM 2:00 PM All
Filter Pump 11:00 AM 2:00 PM All
Pool Heat   11:00 AM 2:00 PM All
Solar Heat   2:00 PM 11:00 AM All   (off < on: overnight span)
Spillway    11:00 AM 2:00 PM All
```
`<Days>` ∈ { `All`, `Wkdays`, `Wkends`, `Su`,`M`,`Tu`,`W`,`Th`,`F`,`Sa` }. The controller
UI only allows all-days / weekdays / weekends / a **single** day (no arbitrary multi-day
bitmask), so the app-side `days_of_week` bitmask must round-trip through this constraint.
Target is the equipment label and may contain spaces — parse fields from the **right**
(Days, then AM/PM+time ×2) so a multi-word target is what remains on the left.

Times are **12-hour + AM/PM** on the wire (the UI is 12-hour). Convert to the store's
24-hour `on_hour`/`off_hour`. `off < on` denotes an overnight span (Solar Heat above).

### Program group (page 0x12, PageButton 0x24)

```
btn idx=0 'Group A'                 state bit0 = active
btn idx=2 'Auto Switch Disabled'
btn idx=3 'Group B'                 state bit0 = active
```
Only the **active** group's schedules are observable on page 0x28; reading the other group
requires switching active group (a write) — so a passive/emulated read exposes the active
group only. Auto Switch + a per-group start date (winter/summer style) exist under here; a
per-group custom label is also settable (see the user's controller PDF, pending).

## Write path (value-set handshake) — partially decoded

Setting a **time** uses the value-submit handshake (matches the known IAQ setpoint flow):
device highlights the time field → submit `0x80` → master sends **ControlReady `0x31`** →
iAQ replies with a **full `0x24` to master (0x00)** carrying ASCII `1` + `HH:MM`
(button-index char `'1'` then the 12-hour time). Observed writes and the value entered:

| write ascii | entered |
|-------------|---------|
| `109:00` | On 9:00 (create A) |
| `105:00` | Off 5:00 PM (create A) |
| `106:30` | On 6:30 (create B) |
| `108:15` | Off 8:15 (create B) |
| `106:00` | Off → 6:00 PM (edit A) |

AM/PM is a separate toggle (not in the time string). **Target selection, day selection,
create, and delete** are button presses carried on the poll-ACK command stream (nav
select/back/scroll + `0x11+index` button opcodes), not value-sets — not yet fully mapped;
needed only for the write phase.

## Implications for the store / model

`ControllerSchedule` currently models one span (target, days bitmask, on/off HH:MM). To
represent this controller faithfully add: **program group** (A/B) + which is active, and
respect the day constraint (all / weekdays / weekends / single day). Reading populates from
the active group's page 0x28; `PendingCapture` → `Available` once parsed.
