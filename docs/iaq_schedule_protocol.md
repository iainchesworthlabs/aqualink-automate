# IAQ / AqualinkTouch Schedule Protocol (reverse-engineered)

Status: **read path decoded** from a live RS-485 capture (2026-07-04, passive snoop of a
physical iAQ/AqualinkTouch 0x33 driven via its web UI); write path decoded and implemented
for CREATE, DELETE, and EDIT (the IAQDevice `ControllerScheduleWrite_ProcessStep` state machine
drives all three; routed via `/api/controller/schedules` POST/PUT/DELETE).
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

## Write path (decoded 2026-07-04)

Decoded from the same live capture (`captures/iaq_schedule_session.cap`) by correlating the
narrated user actions against the wire. Decoder mode: `python decode_iaq.py <cap> --commands`
prints, in time order, PageStart page-id transitions, every non-idle iAQ→master ACK command,
the `0x80` submit / `0x31` ControlReady handshake, and every `0x24` value write. Checksums:
26 911 / 26 926 frames pass; none of the write-relevant frames (submits, `0x24` writes,
delete-confirm dialogs) are among the 15 bad ones, so the decode below is trustworthy.

This section **supersedes** the earlier "value-set handshake — partially decoded" notes.

### The command channel: fixed touch-grid keys, not per-label buttons

The iAQ carries every keypress to the master on its poll-ACK frame: a 2-byte
`Ack (cmd 0x01)` to master(0x00) with payload `[AckType][Command]`.

- **`Command` (payload[1]) is the keypress.** `0x00` = no key. A genuine press is flagged by
  **`AckType (payload[0]) == 0x00`**; other AckType values (`0x1f`, `0x23`–`0x2d`, `0x40`,
  `0x41`, `0x70`, `0x72`, …) are page-render / poll echoes and carry `Command == 0x00`. The
  `AckType 0x1f` beacon with `Command 0x08`/`0x18` is the **chlorinator-active heartbeat**
  (from the 0xA3 device) leaking onto the bus — **not** a keypress; the decoder filters it.
- **Command values are a fixed screen-position grid**, `Command = 0x11 + position`
  (`0x11`=pos0 … `0x20`=pos15 …), **plus** the three nav opcodes `0x01`=SELECT/OK,
  `0x02`=BACK, and `0x80`=SUBMIT (value commit). The *meaning* of a position is whatever the
  master rendered there on the current page — so a press must be interpreted against the page
  layout at press time, **not** by matching a PageButton `idx`. Positions above the highest
  rendered PageButton `idx` (which tops out at 15) address **table rows / scroll hotspots**
  on list pages (positions 16, 17, 20, 22, 23, 24 were all seen — they move the row cursor or
  open the per-row time fields, see below).
- The highlighted list row is broadcast by the master as a **`0x40` frame `[00][row][sel]`**
  (`sel` bit = highlighted); this is the cursor that CREATE/EDIT/DELETE act on.

Firmly-established keys used by the schedule flows:

| Key (Command) | Page context | Meaning |
|---|---|---|
| `0x11` (pos0) | Schedule list 0x28 | **Add Program** (fixed hard-key; no on-screen button is rendered for it) → opens device picker 0x38 |
| `0x11` (pos0) | Time picker 0x29 | **Toggle AM ↔ PM** (master re-pushes PageMessage line 02) |
| `0x12` (pos1) | Schedule list 0x28, row highlighted | **Edit** (idx1 `'Edit'`) → enter row edit mode |
| `0x13` (pos2) | Schedule list 0x28, row highlighted | **Delete** → master shows confirm dialog (see DELETE) |
| `0x17`–`0x20` | Schedule list / editor 0x28 | **Day buttons** — `0x17`=All(idx6) `0x18`=M(7) `0x19`=Tu(8) `0x1a`=W(9) `0x1b`=Th(10) `0x1c`=F(11) `0x1d`=Sa(12) `0x1e`=Su(13) `0x1f`=Wkdays(14) `0x20`=Wkends(15). Pressing one sets that day on the highlighted row immediately. |
| `0x21` (pos16) | Schedule list / editor 0x28 | **Open ON-time field** → time picker 0x29 for the ON time |
| `0x22` (pos17) | Schedule list / editor 0x28 | **Open OFF-time field** → time picker 0x29 for the OFF time |
| `0x25`,`0x27`,`0x28`,`0x29` | Schedule list 0x28 | **Row-cursor scroll hotspots** — move the `0x40` highlight to another program row (exact per-position mapping not pinned; the `0x40 [00][row][01]` frame is the ground truth for which row is active) |
| `0x01` | any | **SELECT / OK** (advance a field, confirm a dialog) |
| `0x02` | any | **BACK** |
| `0x80` | Time picker 0x29 | **SUBMIT** value → `0x31` ControlReady → `0x24` write |

### Time value handshake (firm)

Setting a time uses the known IAQ value-submit handshake:

```
(on time picker 0x29, HH:MM entered locally, optional pos0 AM/PM toggles)
iAQ  → master : SUBMIT   Ack [00 80]
master → iAQ  : ControlReady 0x31
iAQ  → master : 0x24 (PageButton to 0x00), 17-byte fixed field:
                31 <H H : M M> 00 00 …   ascii "1HH:MM"
```

The `0x24` payload is `'1'` (0x31, the **field index** within the picker — always `'1'` for
the single time field) followed by the 5-char `HH:MM`, NUL-padded to 17 bytes. Observed:

| write ascii | ts (ms) | field | note |
|---|---|---|---|
| `109:00` | 86147 / 99376 | create-A ON | 9:00 (entered twice during the fumble) |
| `117:00` | 109376 | create-A OFF | 17:00 — first (wrong) OFF attempt |
| `105:00` | 117189 | create-A OFF | 5:00 — corrected OFF |
| `106:30` | 176735 | create-B ON | 6:30 |
| `108:15` | 186779 | create-B OFF | 8:15 |
| `106:00` | 205126 | edit-A OFF | 5:00 PM → 6:00 PM |

**HH:MM digit entry is a local touch-panel widget and is *not* observable on the wire** —
between the picker render (default `01:00 PM`) and the submit, no per-digit ACK commands
appear, yet the value changes (e.g. `01:00`→`09:00`). Only the AM/PM toggle round-trips
(because the master must re-render line 02), and the final 12-hour value ships in the `0x24`.
The `117:00` vs `105:00` pair shows the panel will accept either a 24-h-looking or 12-h hour
during entry; the **committed** `0x24` is what the controller stores.

**AM/PM (firm):** on picker 0x29 the master pushes `PageMessage 01 = HH:MM`, `02 = AM|PM`.
Pressing **pos0 (`0x11`)** flips line 02 (`PM`→`AM`→`PM`, e.g. @83236/@84038). AM/PM is a
separate field from the HH:MM string.

### CREATE a program (firm skeleton; inner edit-nav partial)

Example — create-A = Filter Pump / ON 9:00 AM / OFF 5:00 PM / Monday (@45055–117220):

1. **Schedule list 0x28** → press **`0x11` (Add Program)** → **device picker 0x38** opens
   (`Devices` header + a scrollable device list as `TableMessage [00][row][name]`).
2. **Pick device on 0x38.** The list scrolls (a scroll key repaints the list window and moves
   the `0x40 [00][row][01]` highlight); a select press commits the **highlighted** row. The
   committed device becomes a **new list row with default `1:00 PM 1:00 PM All`**. (create-A
   selected Filter Pump with a single `0x13`; create-B *scrolled* — `0x12` repainted the list
   to reveal `Pool Light`, highlight moved to it via `0x40`, then a select committed it. The
   exact per-position key meaning *on this scrolling list* is **only partially pinned** — the
   `0x40` highlight frame is the reliable indicator of what gets selected, not the raw Command
   byte.) Note the device list here is the controller's **actuator** list (Filter Pump, Spa,
   Pool Heat/Spa Heat/Solar Heat, Spa Jets, Swim Jet, then Pool Light, Air Blower, Spillway,
   Spa Mode, Clean Mode …) — richer than the 7 devices visible without scrolling.
3. Back on **0x28**, the new row is highlighted (`0x40 [00][row][01]`). Set fields on it:
   - **Day:** press a day key (`0x18`=M …). Row re-renders with the new day token immediately.
   - **ON time:** press **`0x21`** → picker 0x29 → enter HH:MM, toggle AM/PM (pos0), **SUBMIT**.
   - **OFF time:** press **`0x22`** → picker 0x29 → … → **SUBMIT**.
   These sub-steps are reached through an in-edit-mode cursor (`Edit`=`0x12` then a run of
   `SELECT 0x01` + field keys). The **landmarks are firm** (`0x21`/`0x22` open the ON/OFF
   picker every time — 7/7; day keys change the day token every time). The **precise ordering
   of the intervening `SELECT`s / field-advance keys inside edit mode is only partially
   determined** because the user fumbled and the field cursor is not separately rendered.
   A new program is **committed incrementally** — there is no distinct "save" opcode; each
   field write (day key, time submit) mutates the row on the controller as it happens.

### EDIT a program (firm)

- **Change a time** (edit-A OFF 5:00 PM → 6:00 PM, @200686→205156): highlight the row, press
  **`0x22`** (OFF field) → picker → SUBMIT `106:00` → row re-renders `Filter Pump 9:00 AM
  6:00 PM M`. Same handshake as CREATE; no separate "edit" verb for time.
- **Change the day** (Pool Light M→W→F, @225436–255320): press **`0x12` (Edit)** to enter row
  edit, then the target **day key** — `0x1a` (W) → row shows `…W` (@229746); later `0x12`
  then `0x1c` (F) → row shows `…F` (@255320). The row's day token is rewritten live.

### DELETE a program (firm)

Highlight the target row (row cursor via scroll keys; ground truth `0x40 [00][row][01]`), then:

```
iAQ → master : 0x13  (Delete key, pos2)
master → iAQ : PageSubMsg 0x2c  "Are you sure you want\nto delete this program?"  [Ok][Cancel]
iAQ → master : 0x01  (SELECT = Ok)
→ program removed; schedule list 0x28 re-renders with the row gone (rows shift up)
```

Observed twice, identically: delete Filter Pump @268521(`0x13`)→dialog@268568→`0x01`@270764
(row 3 gone @271200); delete Pool Light @374685(`0x13`)→dialog@374731→`0x01`@376659 (row 5
gone @377077). `Cancel` would be the other dialog button (not exercised in this capture).

### Switch the active Program Group (firm)

Page **0x12** renders three PageButtons: `idx0 'Group A'`, `idx2 'Auto Switch Disabled'`,
`idx3 'Group B'`; `state` bit0 marks the **active** group. To switch, press the target group's
key `Command = 0x11 + idx`:

- **`0x14`** (idx3) → **Group B** — re-renders with idx3 `state=01`; visiting 0x28 afterward
  shows title `Schedule Group B` (empty here) (@456277 → @461413).
- **`0x11`** (idx0) → **Group A** — re-renders idx0 `state=01`; 0x28 shows `Schedule Group A`
  (@471533 → @476511).

This is a genuine controller write: the served schedule list on 0x28 changes with the active
group, confirming schedules are controller-resident (see "Validated premise").

### Resolved by the clean capture (`captures/iaq_schedule_clean.cap`, 2026-07-04 #2)

A deliberate, un-fumbled run (create Pool Light → set ON → set OFF → set day → edit time →
edit day → delete) pinned the canonical flow. Genuine presses (`AckType==0x00`) only:

```
0x11 on Settings(0x0f)            -> Schedule list (navigate in)
0x11 on Schedule list(0x28)       -> ADD PROGRAM -> device picker (0x38)
[picker 0x38: scroll keys] then a select -> creates a row with defaults "1:00 PM 1:00 PM All"
0x21 on list(0x28)                -> open ON-time picker(0x29); 0x11 = AM/PM toggle; 0x80 submit -> 0x24 '1'+HH:MM
0x22 on list(0x28)                -> open OFF-time picker; (no 0x11 toggle) 0x80 submit
0x17..0x20 on list(0x28)          -> set day directly (0x1c=Fri here)
[row cursor] 0x12 = EDIT, re-uses 0x21/0x22 (time) and 0x17..0x20 (day)
0x13 = DELETE -> confirm dialog -> 0x01 (Ok)
```

- **Device picker (0x38) is a SCROLLING list**, confirmed: the created device was **Pool Light**,
  which is *not* among the 7 rows the picker first renders (Filter Pump/Spa/Pool Heat/Spa Heat/
  Solar Heat/Spa Jets/Swim Jet) — so it was reached by scrolling. Implement device selection the
  same way as the spa-switch writer: scroll, read the `0x40 [00][row][sel]` highlight + the `0x26`
  row text, repeat until the target label is highlighted, then select. Do **not** hard-map a key→device.
- **AM/PM = the `0x11` toggle on the time picker**, applied to whichever field's picker is open,
  starting from the field's current value. In the capture ON was toggled (→ `9:00 AM`) and OFF was
  not (→ `10:00 PM`), matching the resulting list rows. The `0x24` write still carries only `'1'`+`HH:MM`;
  AM/PM never rides that frame.
- **A new program defaults to `1:00 PM / 1:00 PM / All`**; fields are then set incrementally (there
  is no batch "save" — each submit/day-press mutates the highlighted row live).

### Device picker (0x38) — fully decoded (`captures/iaq_picker.cap`, 2026-07-04 #3)

The picker is a **scrolling touchscreen list**; a mouse click on a row is a single touch-position
command, not a navigate+select. Four controlled picks (device at row 1, 2, 4, and a scrolled row)
pinned the mapping, consistent with the earlier Pool-Light pick (row 3 → `0x16`):

- **Click visible row R (1-based) → `0x13 + R`** — row1 `0x14`, row2 `0x15`, row3 `0x16`, row4 `0x17`, …
- **`0x12` = scroll the picker down** one page (rows re-render as Attribute 1..7 of the new page).
- **`0x13` = OK / confirm** the highlighted device → returns to the list with the new program
  (defaulted to `1:00 PM / 1:00 PM / All`).

Select flow: scroll (`0x12`) until the target label appears in the group-0 rows, click its row
(`0x13 + row`), then `0x13` to confirm. Implemented in `IAQDevice::ControllerScheduleWrite_ProcessStep`.

The full create is now implemented in `IAQDevice::ControllerScheduleWrite_ProcessStep`
(navigate → Add → pick device → set ON/OFF times → set day → verify), including the time
submit: open the field (`0x21`/`0x22`) → on the time picker read line 2, toggle AM/PM (`0x11`)
to match, then `0x80` submit → the value rides the control-data response as `"1"+HH:MM`
(12-hour), via the same `IAQ_ControlReady` handshake as the setpoint writer.

### Edit / delete an existing program (`captures/iaq_editdelete.cap`, 2026-07-05) — decoded

Existing programs are rows in the list; **click program row R → `0x22 + R`** (row1 `0x23`,
row2 `0x24`, …), confirmed against the `0x40 [00][row][sel]` highlight (pressing `0x23` highlights
row 1, `0x24` highlights row 2). With a row highlighted:

- **Edit = `0x12`** → enter edit mode, then change fields with the same keys as create
  (ON `0x21` / OFF `0x22` + submit, days `0x17`-`0x20`).
- **Delete = `0x13`** → the master shows the confirm dialog (PageSubMsg `0x2c`) → **Ok = `0x01`**
  (deletes) or **Cancel = `0x02`/Back** (leaves it). Both paths observed: a cancelled delete of
  row 1, then a confirmed delete of row 2 (= Pool Heat).

So DELETE(program) = navigate to the list → find the program's row ordinal in the parsed rows →
click it (`0x22 + ordinal`) → `0x13` → `0x01` → verify it is gone. EDIT(existing, desired) =
navigate to the list → find `existing`'s row ordinal → click it (`0x22 + ordinal`) → `0x12`
(Edit) → re-run the create field phases against `desired` (ON `0x21` / OFF `0x22` + submit,
day `0x17`-`0x20`) → verify the list now shows `desired`. Both are implemented by the shared
`ControllerScheduleWrite_ProcessStep` state machine (op = Delete / Edit; the goal carries a
`match` = existing program to locate and, for edit, a `program` = desired). UI rules can add
steps (e.g. a day change needing a `0x01` confirm); the writer tolerates extra renders via its
settle/backstop.

## Implications for the store / model

`ControllerSchedule` currently models one span (target, days bitmask, on/off HH:MM). To
represent this controller faithfully add: **program group** (A/B) + which is active, and
respect the day constraint (all / weekdays / weekends / single day). Reading populates from
the active group's page 0x28; `PendingCapture` → `Available` once parsed.
