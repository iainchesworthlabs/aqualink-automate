# PC-host / bridge binaries — `Seradptr.exe`, `Pcdock.exe`, `PCInt.exe`

Reverse-engineered from the official Jandy *Alwin32* simulator suite
(`C:\Program Files (x86)\Alwin32`, debug MFC builds, VS2003 / `mfc71d.dll`).
Method: static x86 disassembly (`pefile` + `capstone` via `alw.py`).
All three images base at `0x400000`, so VA `0x4xxxxx` == RVA `0xxxxxx`; citations
below are `<exe>!0xRVA`. **Legend:** `[CONFIRMED]` = directly observed in code/data;
`[INFERRED]` = deduced from data flow.

These three were expected to be the "PC-host / bridge" side of the suite. Result:

* **`Seradptr.exe`** *is* a host-protocol ↔ RS-485 bridge — it implements the
  **Jandy "RS Serial Adapter" ASCII host protocol** (a keyword/value command set,
  fully recovered below) on the host side, and logs onto the RS-485 bus as a slave
  device on the other. **This is the valuable, previously-undocumented protocol.**
* **`Pcdock.exe`** is the **"RS System PC Docking Station"** — a config
  upload/download tool. Its "host protocol" is just a Windows GUI + `.RS` files; the
  real protocol it speaks is an RS-485 **bulk EEPROM/config block read-write** as a
  bus slave at address **`0x58`** (a new device class `0x0B`).
* **`PCInt.exe`** is **"AqualinkPC"** — the PC control-panel GUI. It has **no host
  serial protocol at all**; it is a **bus master / PowerCenter simulator** (imports
  `PwrCenterSim`, `RemoteGetNextAddress`) that drives the RS-485 bus directly and
  paints the AquaLink panel on screen. A thin GUI over the bus, nothing more.

None of the three open their own COM port (`CreateFileA`+`SetCommState`): the entire
serial layer lives in `NetIO.dll`. The `CreateFileA/ReadFile/WriteFile` imports are
MFC document/file support, not a UART. In real hardware the Serial Adapter has a host
UART; in this *simulator* both sides of `Seradptr` are rendered in its GUI window
(the "Commands" / "Responses" panes).

---

## 1. `Seradptr.exe` — RS Serial Adapter (host ASCII protocol bridge) `[CONFIRMED]`

### 1.1 Identity & imports
* Resource strings: **"RS Serial Adapter Simulation Version 1.0"**, *"RS Serial
  Adapter Windows Simulator"*, *"Jandy Products RS Serial Adapter, Rev %s"*, company
  "Squires Engineering, Inc." (`Seradptr.exe!0x29308`, `!0x265e0`, `!0x310e0`). The
  VS-version block is a left-over "8BUTTON" template (`!0x312d4`) — cosmetic only.
* Main window has three controls labelled **"Commands"**, **"Responses"**, **"CLEAR"**
  (`!0x3113a`, `!0x31166`, `!0x311ca`) — the host command line + response log.
* **NetIO imports (slave API only):** `RemoteLogOn, RemoteStatus, GetMasterMessage,
  RemoteLogOff, RemoteAddressUsed`. No `CommInit`/`CommPort`/`netIO`, no
  `SetCommState`/`BuildCommDCB` → the RS-485 side is a passive bus slave; there is no
  second physical serial port for the host side (it is the GUI). `[CONFIRMED]`

### 1.2 RS-485 (bus) role — passive slave
* Logs on via `RemoteLogOn(handle, addr=byte[obj+0xD9], initBuf=obj+0xCD)` at
  `Seradptr.exe!0x402655`. The log-on init buffer byte0 = `0x01`
  (`!0x40249a` writes `[obj+0xCD]=0x01`). The address byte `obj+0xD9` is **not** a
  code immediate — it is configured (read from the INI `Options`; `GetPrivateProfileIntA`
  is imported). So the adapter's bus address is **configurable**, not fixed.
  `[CONFIRMED — addr is config-driven, value not pinned]`
* The bus poll handler (`GetMasterMessage` at `!0x40274b`) is minimal: it pops the
  master message into `[obj+0x88]`, and **unless the master command byte == `0x13`**
  (a controller-status broadcast it ignores) it answers with a **fixed 3-byte status**
  `RemoteStatus(addr, [obj+0xCD], len=3, sendNow=1)` (`!0x4027a4`). It does *not* run a
  rich per-command dispatch on the bus — on the bus it is just a presence/keep-alive
  responder. `[CONFIRMED]`
* **All the protocol intelligence is on the host side** (§1.3–1.5): when the host asks
  for / sets a value, the adapter schedules a bus **GET/SET transaction** (request
  codes written to `[0x43231d]`, e.g. `0x13`, `0x14`; see §1.6) and reports
  `485 ... TIME-OUT` if the bus does not answer.

### 1.3 Host framing — three configurable prefix characters `[CONFIRMED]`
The host protocol is **ASCII, line-oriented**, framed by three single-character
prefixes held in globals, defaulted at `Seradptr.exe!0x402849`:

| global | default | char | role |
|--------|--------:|:----:|------|
| `CMDCHR` `0x432274` | `0x21` | `!` | **command** start char (host→adapter request) |
| `NRMCHR` `0x43226d` | `0x3f` | `?` | **normal** response prefix (adapter→host OK) |
| `ERRCHR` `0x432299` | `0x23` | `#` | **error** response prefix (adapter→host error) |

These are themselves settable at runtime via the `CMDCHR`/`NRMCHR`/`ERRCHR` keywords
(§1.4). The setter (`!0x4020f6`) validates the new char is printable
(`0x21..0x7e`) else replies `BAD COMMAND ARG`. `[CONFIRMED]`

The response builder (`!0x4021a7`) selects the prefix char by a per-command "response
class" field (`cmd_record[+8]`): **3 → ERRCHR, 4 → CMDCHR, 5 → NRMCHR** (decoded from
the `sub ecx,3 / dec` chain at `!0x4021b5`). `[CONFIRMED]`

Response line templates use `sprintf` (`!0x403590`) into the form
`"<prefix><body>"`. Observed body templates (the `%c` is the prefix char):
* `"%c%02d %u"` (`!0x429334`), `"%c%02d %s"` (`!0x429340`) — **`<prefix><2-digit-index> <value>`**;
* `"%c00 %s"` (`!0x42967c`, `!0x42b47c`) — index `00` + text (e.g. used for `VERS`
  echo at `!0x401f09`).

So a normal value response looks like e.g. `?00 75 F` for `POOLTMP = 75 F`, and an
error like `#NOT WHEN SPA IS ON`. The `%02d` index is the command's table index.
`[CONFIRMED — templates; exact index assignment per-keyword INFERRED]`

There is also an **`ECHO`** toggle and an **`RSPFMT`** (response-format) setting
(keywords below) that select whether/how the echo and the numeric response format are
emitted (`ECHO = %u` `!0x429c78`, `RSPFMT = %u` `!0x429c5c`). `[CONFIRMED keywords]`

### 1.4 Host command set — keyword/value vocabulary `[CONFIRMED strings]`
The host sends `<CMDCHR><KEYWORD>` to **query**, or `<CMDCHR><KEYWORD> <value>` to
**set**. The adapter replies `<NRMCHR><KEYWORD> = <value>` on success. Keyword tokens
and their response templates are both in `.rdata`; the parser matches the inbound
token against the keyword list, then runs the matching command record's handler
(records at `!0x428d28…0x42a1f8`, each with a `+0x24` handler code pointer; the
response format string is the record's bound `[rec+0x0c]`). Full recovered list:

**Equipment state — pool/spa values** (query/set; reply `KEYWORD = value`):

| keyword | response template | cite |
|---------|-------------------|------|
| `MODEL` | `MODEL = %d` | `!0x4296c0` |
| `OPTIONS` | `OPTIONS = %d` (a bit-mask — see [OPTIONS bit map](#141-options-bit-map)) | `!0x4296d0` |
| `OPMODE` | `OPMODE = %s` (AUTO/SERVICE/TIMEOUT/UNKNOWN) | `!0x4296f0`, enums `!0x42960c…0x429624` |
| `UNITS` | `UNITS = %c` | `!0x429700` |
| `POOLSP` / `POOLSP2` | `POOLSP = %u %c` / `POOLSP2 = %u %c` | `!0x429710`, `!0x429720` |
| `SPASP` | `SPASP = %u %c` | `!0x429730` |
| `POOLTMP` | `POOLTMP = %u %c` | `!0x429740` |
| `AIRTMP` | `AIRTMP = %u %c` | `!0x429750` |
| `SPATMP` | `SPATMP = %u %c` | `!0x429760` |
| `SOLTMP` | `SOLTMP = %u %c` | `!0x429770` |
| `PUMP` | `PUMP = %d` | `!0x429780` |
| `PUMPLO` | `PUMPLO = %d` | `!0x429800` |
| `CLEANR` | `CLEANR = %d` | `!0x429790` |
| `WFALL` | `WFALL = %d` | `!0x4297a0` |
| `SPA` | `SPA = %d` | `!0x4297b0` |
| `SPAHT` | `SPAHT = %d` | `!0x4297c0` |
| `POOLHT` / `POOLHT2` | `POOLHT = %d` / `POOLHT2 = %d` | `!0x4297e0`, `!0x4297d0` |
| `SOLHT` | `SOLHT = %d` | `!0x4297f0` |
| `SALT` | `SALT = %d PPM` | `!0x429820` |
| `HICHLOR` | `HICHLOR = %d` | `!0x429830` |
| `AUX%d` / `AUXX` | `AUX%d = %d`, `AUXX = %d`, dimmer: `AUX%d = 1 %d%%`, `AUXX = 1 %d%%` | `!0x42952c…0x429554` |

#### 1.4.1 `OPTIONS` bit map `[MANUAL-CONFIRMED for bits 0-4 and 7; bit 6 CAPTURE-GATED]`

`OPTIONS` reports a single **bit-mask byte**, not an ordinal: it is the Power Center's
**S1 option DIP-switch bank**, one bit per switch, **LSB first** (bit 0 == S1 DIP #1).
The switch functions are from the Jandy AquaLink RS installation manual **P/N 6840**,
§4.1 "DIP Switch Functions" plus the S1 tables in §4.2 / §4.3:

| bit | mask | S1 DIP | function when ON | `SerialAdapter_SCS_Options` field |
|:---:|:----:|:------:|------------------|-----------------------------------|
| 0 | `0x01` | #1 | AUX 1 controls the pool cleaner | `HasCleaner` |
| 1 | `0x02` | #2 | AUX 2 controls filter-pump low speed (2-speed pump) | `TwoSpeedPump` |
| 2 | `0x04` | #3 | AUX 3 controls spa spillover | `HasSpillover` |
| 3 | `0x08` | #4 | heater cool-down disabled | `HeaterCoolDownDisabled` |
| 4 | `0x10` | #5 | factory calibration ("factory use only") | `ServiceCalibrationMode` |
| 5 | `0x20` | #6 | spare AUX activates with filter pump in spa mode / (dual equipment) pool and spa share one heater | `SpareAuxOnWithFilterPumpAndSpa` |
| 6 | `0x40` | #7 | manual lists "not used" / (dual equipment) air sensor becomes solar sensor | `CommonHeaterForSpaAndPool` |
| 7 | `0x80` | #8 | heat pump instead of gas heater (5-minute post-setpoint delay, not 3) | `ExtraDelayForHeatPump` |

Only bits 0 and 2 are consumed today: `SerialAdapterDevice::Slot_SerialAdapter_DevStatus`
rewrites the AUX 1 poll to the `CLEANR` pump query and the AUX 3 poll to `SPILLOVER`,
because a panel with those options set **errors** when asked about that relay by raw
device id.

> **Caveats.**
> * **Bit 6.** Manual 6840 documents S1 **#6** as carrying *both* the spare-AUX and the
>   shared-pool/spa-heater meanings (which one applies depends on system type), and lists
>   S1 **#7** as "not used" / air-sensor-becomes-solar-sensor. The app's
>   `CommonHeaterForSpaAndPool` field therefore sits at bit 6 without manual backing; the
>   decode keeps a straight positional bit image rather than guessing.
> * **Alternative hypothesis.** [`ctrlpnl-simio.md`](ctrlpnl-simio.md) recovers the Alwin32
>   simulator's *internal* S1 byte with a **different** assignment (`0x01`=No Cool,
>   `0x02`=Calibrate, `0x04`=CL JVA Asn, `0x08`=Heat Delay, `0x10`=Cleaner, `0x20`=Heat Pump,
>   `0x80`=Spillover). That byte is PowerCenter-internal shared memory which the same doc
>   states does **not** appear on the RS-485 wire, and its own notes warn the simulator's UI
>   layout does not mirror the physical switch order — so the manual's DIP numbering is used
>   for the wire value. **Confirm against a live capture from a panel with a known S1
>   configuration.**
> * AqualinkD does **not** decode these bits at all (`source/serialadapter.c` logs `OPTIONS`
>   only as a raw value), so it is not a cross-check here.

**Adapter status / diagnostics:**

| keyword | response | cite |
|---------|----------|------|
| `VERS` | `VERS = %s` (also `RS SERIAL ADAPTER REV …`) | `!0x429bcc`, `!0x429d38` |
| `LEDS` | `LEDS = %u %u %u %u %u` (5 LED states) | `!0x429be0` |
| `VBAT` | `VBAT = %u` / `VBAT = %u LOW` | `!0x42962c`, `!0x429638` |
| `COSMSGS` | `COSMSGS = %u` (change-of-state message count) | `!0x4296e0` |
| `DIAG` | diagnostic dump | `!0x429bc4` |
| `S1` | `S1 = %u` (option/DIP switch S1) | `!0x429c90` |

**Adapter configuration (settable framing/behaviour):**

| keyword | response | cite |
|---------|----------|------|
| `CMDCHR` | `CMDCHR = %u` | `!0x429c48` |
| `NRMCHR` | `NRMCHR = %u` | `!0x429c34` |
| `ERRCHR` | `ERRCHR = %u` | `!0x429c20` |
| `RSPFMT` | `RSPFMT = %u` | `!0x429c5c` |
| `ECHO` | `ECHO = %u` | `!0x429c78` |

(The keyword tokens themselves are the back-to-back null-terminated strings at
`!0x429b24…0x429c9c`: `SOLHT, AUXX, SOLTMP, POOLHT2, POOLSP2, SPATMP, SPASP, SPAHT,
WFALL, HICHLOR, SALT, CLEANR, PUMPLO, PUMP, AIRTMP, POOLTMP, POOLSP, POOLHT, UNITS,
DIAG, VERS, LEDS, VBAT, OPMODE, OPTIONS, MODEL, ERRCHR, NRMCHR, CMDCHR, RSPFMT,
COSMSGS, ECHO, S1, AUX%d`.) `[CONFIRMED]`

### 1.5 Host error / status responses `[CONFIRMED strings]`
Emitted with the `ERRCHR` prefix on failure (cites are the format-string VAs):

* Parse errors: `BAD COMMAND ARG` (`!0x42934c`), `BAD COMMAND FORM` (`!0x429368`),
  `INVALID COMMAND` (`!0x429410`), `BAD CHAR PAST COMMAND` (`!0x429cc0`),
  `BAD START COMMAND CHAR` (`!0x429cd8`), `BAD EXTRA FIELD FOR VALUE=%d` (`!0x429580`),
  `UNEXPECTED CMD STATUS = %d` (`!0x429564`), `INTERNAL ERROR` (`!0x429684`).
* Bus-side failures: `485 PROTOCOL ERROR` (`!0x429388`), `485 POLL TIME-OUT`
  (`!0x429cf0`), `485 GETVALUE TIME-OUT` (`!0x429d04`), `485 SETVALUE TIME-OUT`
  (`!0x429d1c`), `SERIAL ADAPTER IS OFFLINE` (`!0x429660`),
  `CTRL OPERATION FAILED` (`!0x42939c`), `SETPT OPERATION FAILED` (`!0x429648`).
* Equipment lock-out / state errors (the controller's own rule set, surfaced to the
  host): `NOT WHEN SUPERCHLORINATE IS ON`, `NOT WHEN CLEANER IS ON`,
  `EXTRA AUX UNAVAIL: SOLAR PRESENT`, `TIMEOUT MODE IS ACTIVE`,
  `SERVICE MODE IS ACTIVE`, `NOT WHEN SPA IS ON`, `NOT WHEN SPILLOVER ACTIVE`,
  `PUMP HIGH NOT ON`, `NOT WHEN FREEZE IS ON`, `FUNCTION IS LOCKED OUT`,
  `OPTION SWITCH IS SET`, `OPTION SWITCH NOT SET`, `AUX OFF: DIMMER CTL IGNORED`,
  `AUX NOT ASSIGNED TO DIMMER`, `VALUE IS UNAVAILABLE`, `SENSOR IS OPEN`,
  `SENSOR IS SHORTED` (`!0x4293b4…0x4295c8`). `[CONFIRMED]`

### 1.6 Host ↔ bus mapping `[CONFIRMED mechanism]`
When the host issues a value keyword, the handler does **not** answer from a local
cache for live equipment values; it schedules an RS-485 transaction by writing a
request-code byte to `[0x43231d]` and a flag to `[0x43231c]`, then the worker drives
the bus and the reply unblocks the host response (or yields a `485 … TIME-OUT`):

* `[0x43231d] = 0x13` (`!0x401f57`), `= 0x14` (`!0x401f68`), `= 0x11/0x12` variants
  (`!0x401f30`/`!0x401f45`) — these are the **485 GET/SET request opcodes** mapped to
  master commands of the same numbering family used by `Pwrcntr.exe`
  (`0x13` controller-status, `0x14` GetId/value). `[CONFIRMED codes; exact
  keyword→opcode assignment INFERRED]`
* Several handlers gate on an **online flag** `mode_dword [0x432294]` and emit
  `SERIAL ADAPTER IS OFFLINE` when the bus master is not present
  (`!0x401e48`/`!0x401eab`). `[CONFIRMED]`
* The `ControllerType`/aux-relay-count model is decoded the same way as in
  `Pwrcntr.exe` (jump table at `!0x4028ce`, values 3/5/6/7/0xA/0xB/0x20 for the
  RS-4/6/8/… capacity), feeding how many `AUXn` keywords are valid (`!0x402849`
  onward). `[CONFIRMED]`

**Net:** `Seradptr.exe` is the simulator of the real Jandy **RS Serial Adapter**,
exposing the AquaLink system to a host computer through a compact **ASCII
keyword=value command/response protocol** with three configurable framing characters
(`!`/`?`/`#`), bridging each host query/set to an RS-485 GET/SET on the bus.

---

## 2. `Pcdock.exe` — RS System PC Docking Station (config block transfer) `[CONFIRMED]`

### 2.1 Identity & imports
* **"RS System PC Docking Station Application Version 5.0"**, copyright 2003-2010,
  internal name `PCDOCK` (`Pcdock.exe!0x2dd66`, `!0x2e4f0`). Menus: **Transfer →
  From RS System / To RS System**, **Comm Setup** (`!0x2dbae…0x2dbfc`,`!0x2dc0e`);
  document type **RS Files (*.RS)** (`!0x30a72`). UI fields: *Firmware:*, *RS Model:*,
  *Status:* (`!0x2df52`,`!0x2df9a`,`!0x2dfe2`); progress *"Reading Data FROM RS
  System"* / *"Sending Data TO RS System"* / *"Transfer Complete"*
  (`!0x26c18`,`!0x26c34`,`!0x26cf0`). It searches for the controller on boot
  ("Searching for communication from Power Center", up to 30 s, `!0x2e186`).
* **NetIO imports:** `CommInit, CommEnd, CommInUse, CommID, CommRxCount,
  CommRTSisTimed, RemoteLogOn, RemoteStatus, GetMasterMessage, RemoteLogOff` — it
  drives the comm port (`CommInit`) and logs on as a **slave** that the controller
  (master) reads/writes. `[CONFIRMED]`

### 2.2 Bus role — slave at address `0x58` (new class `0x0B`)
* Logs on with **address byte = `0x58`** (`Pcdock.exe!0x401569` writes
  `[obj+0x195]=0x58`), log-on init byte0 `[obj+0x196]=0x01` (`!0x401593`).
  `0x58 >> 3 = 0x0B` → **device class `0x0B`, instance 0** — a class not in the
  existing suite map. `[CONFIRMED]`
* This is the "docking station" address the PowerCenter recognises for **bulk setup
  transfer**.

### 2.3 The transfer protocol — block read/write of the config image `[CONFIRMED]`
`GetMasterMessage` (`!0x401b33`) dispatches on the master command byte
(`byte[obj+0x150]`) via a `sub`/`je` chain (`!0x401b64`):

| master cmd | handler | meaning |
|----------:|---------|---------|
| `0x00` | `!0x401ef8` | poll / keep-alive |
| `0x03` | `!0x401e2c` | status/handshake |
| `0x08` | `!0x401d52` | (sets `[obj+0x1ed]=1`, captures a 16-bit field `word[edi+2]`) — transfer setup/params |
| `0x19` | `!0x401ca1` | **read block** (controller reads a page out of the dock's image → "To RS System" direction) |
| `0x1a` | `!0x401b9e` | **write block** (controller writes a page into the dock's image → "From RS System" direction) |

The block engine is parameterised by a **device-size flag** `word[obj+0x1ef]`:
* if `word[obj+0x1ef] == 0x1000` → **small/legacy device**: page index is **8-bit**
  (`byte[edi+1]`), page size **0x10 (16) bytes** (`!0x401ba7`/`!0x401caa`).
* else → **large device**: page index is **16-bit** (`word[edi+1]`), page size **0x40
  (64) bytes** (`!0x401bd4`/`!0x401cc0`).

The page is copied to/from the dock's resident config image at
`obj+0x1F7 + index*pagesize` (`memcpy` `!0x403710` at `!0x401c19`, `!0x401d0a`). The
running page index is tracked in `word[obj+0x197]` and auto-incremented; on the last
page (`word[obj+0x1f3]` reached) the transfer completes (`!0x401c21`). Every block is
acknowledged via `RemoteStatus(addr=byte[obj+0x195], buf=obj+0x196, len=1,
sendNow=1)` with the response command byte at `byte[obj+0x1d9]` (the inbound cmd is
echoed: `0x1a→0x1a` etc., `!0x401aed`/`!0x401b09`). `[CONFIRMED]`

So `Pcdock`'s "protocol" is not a host command language — it is a **paged
read/modify/write of the controller's entire configuration EEPROM over RS-485**,
saved to / loaded from `.RS` files on disk. The host side is purely the GUI + file.

---

## 3. `PCInt.exe` — "AqualinkPC" (PC control panel; bus master, no host protocol) `[CONFIRMED]`

### 3.1 Identity & imports
* **"AqualinkPC Version 5.0"**, company **"Jandy Products"**, internal name `PCint`,
  `OriginalFilename PCint.EXE`, `FileDescription AqualinkPC Application`
  (`PCInt.exe!0x33056`,`!0x34d98`,`!0x34e60`,`!0x34ef4`,`!0x34de0`). The window title
  is `AqualinkPC` / "Ctrlpnl Windows Application" (`!0x373a4`,`!0x3743e`).
* It is the AquaLink RS **PC control-panel GUI** migrated from a Visual Basic app:
  leftover VBX form descriptors `THREED.VBX;SSPanel;AquaLink RS`,
  `THREED.VBX;SSPanel;POOL / SPA  CONTROL`, `BTNVBX10.VBX;vaBtn;vaBtn1..5`
  (`!0x33136…0x3354e`), and it imports an empty-named control DLL **`BTN32D20.dll`**
  (the 32-bit successor of the `vaBtn` button VBX). `[CONFIRMED]`
* Setup menu models the full controller line: **Comm Port**, **RS System Model →
  RS8/RS12/RS16, RS4/6/8/12/16 Pool+Spa, RS 2/6, RS 2/10, RS 2/14**
  (`!0x32c24…0x32d90`); colour customisation for the on-screen Aqualink panel;
  **Delay Override**, **Program Operation** (`!0x32ecc`,`!0x32efe`).
* **NetIO imports (master + slave):** `PwrCenterSim, RemoteGetNextAddress, CommInit,
  CommEnd, CommPort, CommID, CommInUse, CommRxCount, CommRTSisTimed,
  CommRTSTimedCtrl, RemoteLogOn, RemoteStatus, GetMasterMessage, RemoteLogOff`.
  The presence of **`PwrCenterSim`** and **`RemoteGetNextAddress`** (and the
  RTS-timed direction control `CommRTSTimedCtrl`) is the master-side signature —
  same as `Pwrcntr.exe`. `[CONFIRMED]`

### 3.2 Role — bus master, GUI front-end
* `RemoteGetNextAddress` is called at `PCInt.exe!0x401454` (inside a thread/timer
  set-up with `push 0x65` timer id) — i.e. it **negotiates bus addresses** for the
  panel/keypad roles it represents, the way a controller does. `[CONFIRMED]`
* It establishes the link with `CommInit` and reports purely **UI status**, no host
  command grammar: *"Could NOT establish communication with Power Center."*,
  *"Communication with Power Center has been established."*, *"Waiting for Comm"*,
  *"COMM port is in use by another Application."*, *"Initialization failed on COM%d
  (%d)"* (`!0x2afd8`,`!0x2b010`,`!0x2b148`,`!0x2aed4`,`!0x2b15c`). `[CONFIRMED]`
* **There is no ASCII command/response keyword table, no printf command templates, no
  `#`/`?`/`!` framing** — searched and absent. Its only "protocol" is the RS-485 bus
  (handled by NetIO) plus the on-screen panel. `[CONFIRMED — by absence]`

**Net:** `PCInt.exe`/AqualinkPC is a thin GUI that *is* the interface — the user
clicks the on-screen AquaLink panel and the app drives the RS-485 bus as a
master/PowerCenter sim. No separate host-facing serial protocol of its own.

---

## 4. Summary table

| binary | what it is | host-facing protocol | RS-485 (NetIO) role |
|--------|-----------|----------------------|---------------------|
| **`Seradptr.exe`** | RS **Serial Adapter** sim (Squires Eng.) | **ASCII keyword=value** cmd/response, framing chars `!`/`?`/`#` (configurable), `ECHO`/`RSPFMT` options — full set in §1.4 | passive **slave**, configurable addr; answers polls with fixed 3-byte status; bridges host GET/SET → bus opcodes `0x13`/`0x14` |
| **`Pcdock.exe`** | RS System **PC Docking Station** v5.0 (config transfer) | none (Win32 GUI + `.RS` files) | **slave @ `0x58`** (class `0x0B`); paged config-EEPROM read (cmd `0x19`)/write (cmd `0x1a`), 16- or 64-byte pages |
| **`PCInt.exe`** | **AqualinkPC** v5.0 control-panel GUI (Jandy) | none | **bus master** (imports `PwrCenterSim`, `RemoteGetNextAddress`); drives the bus + paints the panel |

## 5. For the aqualink-automate project
* **New, directly useful:** the **RS Serial Adapter host protocol** (§1) is a clean,
  text-based facade over the same AquaLink data the project already models
  (POOLTMP/SPATMP/AIRTMP/SOLTMP, POOLSP/SPASP, PUMP/CLEANR/WFALL/SPA/heaters,
  SALT/HICHLOR, AUXn incl. dimmer `%`, OPMODE AUTO/SERVICE/TIMEOUT). The error/lockout
  strings (§1.5) are a ready-made enumeration of the controller's command-rejection
  reasons.
* **New address class:** **`0x0B` @ `0x58`** = the **PC Docking Station** (config
  transfer endpoint) — extends `jandy_device_types.h`. Its cmd `0x19`/`0x1a` paged
  block read/write (§2.3) is how a host bulk-reads/writes the controller config, with
  the `0x1000` flag selecting 16-byte/8-bit-index vs 64-byte/16-bit-index paging.
* **`PCInt`/`Pcdock` confirm the bus split:** AqualinkPC and the docking station both
  use the same `NetIO` master/slave model as the rest of the suite — no third
  "AqualinkPC wire protocol" exists; the only wire protocol is the documented RS-485
  bus.

### Confidence
CONFIRMED: every keyword/format-string/error VA in §1; the three framing-char globals
+ defaults + setter/selector logic; Seradptr's slave-only NetIO usage; the
host↔bus GET/SET scheduling mechanism (opcodes `0x13`/`0x14` cited, exact
keyword→opcode pairing INFERRED); Pcdock's address `0x58`, its cmd dispatch
(`0x00/0x03/0x08/0x19/0x1a`) and the paged block read/write engine; PCInt's
master-side imports and absence of any host protocol. INFERRED (flagged inline):
Seradptr's configured bus address value (config-driven, not a literal); the per-keyword
response-index assignment; the precise keyword→485-opcode map.
