# Protocol intel from the official Jandy "Alwin32" simulator suite

This documents what was recovered by **static reverse-engineering of the official Jandy
*Aqualink RS Rev T.0.1* training/simulator suite** (installer `aqualink_rs_simulator.exe`,
installed to `C:\Program Files (x86)\Alwin32`). The suite is a set of Windows MFC apps that
emulate either the controller (PowerCenter / OneTouch / PDA) or individual pieces of
equipment (chlorinator, pumps, heaters, …) and talk the **real RS-485 Aqualink protocol** to
each other (over a virtual/loopback link) or to real hardware on a COM port.

Because every binary ships as a **debug build** (`mfc71d.dll` / `msvcr71d.dll`, VS2003) the
code is straightforward to disassemble. Findings are cited `binary!RVA` so each claim is
independently checkable. This is **ground-truth from the vendor's own implementation**, not a
capture guess — where it overlaps existing project notes it **confirms** them; new details and
inferences are flagged.

**Per-device deep-dives (full byte-level detail + RVA citations) live in [`docs/alwin32/`](alwin32/):**
[chlorinator](#device-catalogue-one-line-summary-see-linked-files-for-byte-layouts) is worked inline below; pumps
([epump.md](alwin32/epump.md), [iflo.md](alwin32/iflo.md)), heaters
([heaters.md](alwin32/heaters.md), [aquatemp-wtrmatic.md](alwin32/aquatemp-wtrmatic.md)),
accessories ([lpc4-remaux.md](alwin32/lpc4-remaux.md), [panels.md](alwin32/panels.md)), and
the controllers ([pwrcntr-master.md](alwin32/pwrcntr-master.md),
[onetouch.md](alwin32/onetouch.md), [pda.md](alwin32/pda.md)) each have their own file.

**Controller behaviour & host interfaces:**
[pwrcntr-behavior.md](alwin32/pwrcntr-behavior.md) — the `ControllerType`→model/config table
(RS-4/6/8/12/16 Only/Combo/Dual, aux counts, pool/spa body model), config-gated behaviours
(freeze protection, config-dependent page opcodes), and the **IAQ page-message field layouts**
(incl. the `0x25` vs `0x26` resolution). Host/config interfaces:
[uapi-client.md](alwin32/uapi-client.md) (iAqualink2 Wi-Fi TLV status),
[pc-host-bridge.md](alwin32/pc-host-bridge.md) (RS Serial Adapter ASCII protocol + PC Docking
Station), [ctrlpnl-simio.md](alwin32/ctrlpnl-simio.md) (wired Control Panel + iodll option DIP
bits). **Testing:** [testing-ecosystem.md](alwin32/testing-ecosystem.md) — how to mint `.cap`
replay fixtures (generated + live sim-capture) to test behaviour; and
[sim-connection.md](alwin32/sim-connection.md) — how the sims interconnect (shared memory) and
a no-install raw-TCP feeder that drives the live app with sim-derived fixtures.

---

## 1. Suite layout — who emulates what

All sims link two shared libraries:

| Shared library | Role |
|----------------|------|
| **`NetIO.dll`** | The entire **RS-485 wire protocol + serial layer** (framing, checksum, addressing, the per-device 16-slot "network table", bus log-on/off, master polling). Three protocol families in one DLL: base Jandy, `*_iFloVS` (Pentair IntelliFlo VS), `*_ePumpAc` (Jandy ePump AC). |
| **`iodll.dll`** | Simulated **PowerCenter on-board hardware** — relays (`Get/SetRelay`), front-panel LEDs (`Get/SetLED`), temp sensors (`SetWater/Solar/FreezeDegF`), keypad/ADC (`SetKeyPressed`,`adc_10bit`), config **option DIP switches** (`Get/SetOptions`, `…_S2`), power/battery, RTC. |

---

## 2. The wire frame (`NetIO.dll!CommTx`, RVA `0x2ccb`)

`CommTx(dest, dataPtr, dataLen)` builds and transmits a frame. Decoded byte-for-byte:

```
 0x00 │ 0x10 0x02 │ dest │ data[0] data[1] … │ cksum │ 0x10 0x03 │ 0x00
 pad    DLE  STX     │      │                    │       DLE  ETX    pad
                     │      └ data[0] = COMMAND byte, data[1..] = payload
                     └ device replies are sent to dest = 0x00 (the master)
```

* **Leading `0x00`** idle pad before `DLE STX`, and a **trailing `0x00`** after `DLE ETX`.
  Both are emitted unconditionally and are **not** part of the checksum.
* **Checksum** = `(0x10 + 0x02 + dest + Σ data[]) & 0xFF` — a running 8-bit sum seeded with
  the `DLE` and `STX` bytes, accumulated through the last data byte.
  (seed `0x2d17`/`0x2d25`, accumulate `0x2db6`.)
* **DLE (`0x10`) byte-stuffing:** any `0x10` occurring in `dest`, `data[]`, **or the checksum
  byte itself** is followed by an inserted `0x00`. The checksum-stuffing case (`0x2e34`) is
  easy to miss and a common source of decode bugs.

This matches the project's DLE+8-bit-sum framing; the **pad bytes** and **checksum-stuffing**
are details worth double-checking in `src/jandy/…` serialize/deserialize. None of the base
Jandy devices use Pentair `0xA5` framing — see §6 for the IntelliFlo exception.

## 3. Addressing — 5-bit class + 3-bit instance (`NetIO.dll!RemoteGetNextAddress` `0x1d15`)

An address byte is `addr = (class << 3) | instance`:

* `class` = top 5 bits, selects the device **family**.
* `instance` = low 3 bits; the bus negotiates an unused instance, probing **0–3**.
* `addr = 0xFF` is reserved/broadcast (the master polls a whole class with it — see §6).

This is why Jandy ids cluster as `base, base+1, base+2, base+3`. The **complete recovered map
is §5.** Some sims negotiate via `RemoteGetNextAddress(class)`; others hard-code a small
consecutive-address table (e.g. ePump `00 78 79 7a 7b`) or a fixed configured address
(ChemLink `0x80`/`0x81`).

## 4. The device model — bidirectional "network table" (`NetIO.dll`)

`NetIO.dll` keeps a **16-slot table** of bus devices, base `0x10024000`, **`0x36C` (876) bytes
per slot**. Slot layout (from `RemoteLogOn 0x1dff`, `RemoteStatus 0x1fb5`,
`GetMasterMessage 0x2068`):

| Offset | Size | Meaning |
|-------:|-----:|---------|
| `+0x00` | 4 | owner handle/HWND |
| `+0x04` | 128 | **outbound status payload** (what this device TX's to the master) |
| `+0x84` | 1 | "status dirty / ready to send" flag |
| `+0xAD` | 1 | the device's **RS-485 address** |
| `+0xAE` | 2 | **inbound** master-message length |
| `+0xB0` | … | **inbound** master-message buffer (last command the master sent here) |

Equipment-sim API (each sim is a thin UI shell that calls these):

* **`RemoteLogOn(handle, addr, initBuf128)`** — claim a slot; copies a 128-byte initial status.
* **`RemoteStatus(addr, dataPtr, dataLen, sendNow)`** — copy `dataLen` bytes into the slot's
  `+0x04` payload, set the dirty flag, and if `sendNow` immediately `CommTx(0x00, data, len)`.
  **`data[0]` is the device's response command byte.**
* **`GetMasterMessage(addr, outBuf)`** — pop the last command the master addressed to this
  device; `outBuf[0]` is the master command byte. The sim dispatches on it via a
  `sub eax,<delta>; je` chain or a jump table.

So **all device-specific protocol knowledge = (a) which inbound command bytes the sim
recognises, and (b) the exact bytes it packs into `RemoteStatus`.**

### Master side (`Pwrcntr.exe`)

The master is the inverse: it builds `{u8 dest, u16 len, u8 cmd, payload}` and calls
`netIO(msg)` (`NetIO.dll!0x12b8`), which `CommTx`'s it and **writes the responder's address +
reply back into the same buffer**. Bus enumeration is implicit: broadcast `dest=0xFF`, retry
instances `0..4`, break on first reply, validate responder class. Full master vocabulary in
[pwrcntr-master.md](alwin32/pwrcntr-master.md) and summarised in §6.

### Comm error taxonomy / multi-protocol

Exported counters reveal the link state machine: `Comm_phase1/2_errors`, `Comm_t1/t2_errors`,
`Comm_nak_errors`, `Comm_len_errors`, `Comm_chk_errors` (two-phase TX, T1/T2 timeouts, NAK,
length+checksum validation). `CommRTSTimedCtrl`/`CommRTSisTimed` confirm **RTS-timed
half-duplex** RS-485 direction control. Every Net function has `_iFloVS` and `_ePumpAc` twins
for the Pentair IntelliFlo and Jandy ePump pump families.

---

## 5. Complete device address map (recovered)

Every device class recovered from the suite. **Bold** = newly pinned or cross-validated from
the vendor's own code. (`class` = `addr>>3`.)

| class | base addr | device | sim exe | frame | source |
|:-----:|:---------:|--------|---------|-------|--------|
| `0x01` | **`0x08`–0x0F** | **iAqualink wired Control Panel** (27-button) | `Ctrlpnl.exe` | Jandy DLE | [ctrlpnl-simio](alwin32/ctrlpnl-simio.md) |
| `0x02` | **`0x10`–0x17** | Dual-Spa / 2×4 remote keypad | `2x4rem.exe` | Jandy DLE | [panels](alwin32/panels.md) |
| `0x04` | **`0x20`–0x27** | "AllButton" 8-button keypad | `8button.exe` | Jandy DLE | [panels](alwin32/panels.md) |
| `0x05` | **`0x28`–0x2B** | Aux / Remote Power Center (8 relays) | `Remaux.exe` | Jandy DLE | [lpc4-remaux](alwin32/lpc4-remaux.md) |
| `0x06` | `0x30`–0x33 | AqualinkTouch / iAQ (rich status, pages) | (master side) | Jandy DLE | [pwrcntr](alwin32/pwrcntr-master.md) |
| `0x07` | `0x38`–0x3B | **LX gas heater** (carries setpoints) | `Aquatemp.exe` | Jandy DLE | [aquatemp-wtrmatic](alwin32/aquatemp-wtrmatic.md) |
| `0x08` | `0x40`–0x43 | OneTouch panel | `Onetouch.exe`/`Smallcp.exe` | Jandy DLE | [onetouch](alwin32/onetouch.md) |
| `0x0A` | `0x50`–0x53 | SWG chlorinator (AutoClear/AquaPure/AquaRite) | `Aquarite.exe` | Jandy DLE | §5 below |
| `0x0B` | **`0x58`** | **PC Docking Station** (paged config-EEPROM xfer) | `Pcdock.exe` | Jandy DLE | [pc-host-bridge](alwin32/pc-host-bridge.md) |
| `0x0C` | **`0x60`–0x63** | **PDA / AquaPalm remote** | `PDA.exe` | Jandy DLE | [pda](alwin32/pda.md) |
| `0x0D` | **`0x68`–0x6B** | **LXi gas heater** | `LXI Heater.exe` | Jandy DLE | [heaters](alwin32/heaters.md) |
| `0x0E` | **`0x70`–0x73** | **Heat pump / chiller** | `HeatPump.exe` | Jandy DLE | [heaters](alwin32/heaters.md) |
| `0x0F` | **`0x78`–0x7B** | **Jandy ePump (VSP)** | `ePump.exe` | Jandy DLE (`_ePumpAc`) | [epump](alwin32/epump.md) |
| (fixed) | `0x80`–0x81 | **ChemLink** (pH/ORP/salt feeder) | `wtrMatic.exe` | Jandy DLE | [aquatemp-wtrmatic](alwin32/aquatemp-wtrmatic.md) |
| `0x12` | **`0x90`** | Laminar Pulse Controller (LPC4) | `Lpc4.exe` | Jandy DLE | [lpc4-remaux](alwin32/lpc4-remaux.md) |
| (sep.) | `0x60`–0x63 | **Pentair IntelliFlo VS** | `iFlo.exe` | **Pentair `0xA5`** | [iflo](alwin32/iflo.md) |

> **Note on `0x60`:** the PDA (base-Jandy class `0x0C`) and the IntelliFlo (`0x60`) both nominally
> sit at `0x60`, but IntelliFlo lives in NetIO's **separate `_iFloVS` address space/table** (base
> `0x100276c0`), a parallel protocol family — they do not collide on the base-Jandy bus.

This cross-validates `src/jandy/devices/jandy_device_types.h` (LX_Heater `0x38`, OneTouch
`0x40`, AquaPure/chlorinator `0x50`, ChemLink `0x80` all match) and **adds** the wired Control
Panel (`0x08`), keypad classes (`0x10`/`0x20`), aux (`0x28`), PC Docking Station (`0x58`), PDA
(`0x60`), LXi/HeatPump heaters (`0x68`/`0x70`), ePump (`0x78`), and laminar (`0x90`).

### Device catalogue (one-line summary; see linked files for byte layouts)

* **Chlorinator** `0x50` — worked inline below.
* **ePump** `0x78` — ASCII commands `'A'`poll/`'B'`stop/`'C'`start/`'D'`set-speed(**RPM×4 LE**)/`'E'`query; 6-byte status. [epump.md](alwin32/epump.md)
* **IntelliFlo** `0x60` — Pentair `0xA5` payload (double-wrapped in a Jandy frame by the sim); cmds `0x01`set-speed/`0x04`run-stop/`0x05`mode/`0x06`set-RPM/`0x07`req-status; **RPM 16-bit big-endian**; 20-byte status. [iflo.md](alwin32/iflo.md)
* **LX heater** `0x38` — cmd `0x0C`=`[flags][PoolSP][SpaSP][MeasTemp?]`; reply `0x0D`=`[status][temp][switchBits]`; **Heating = status bit `0x08`** (matches `HeaterStates::Heating`). [aquatemp-wtrmatic.md](alwin32/aquatemp-wtrmatic.md)
* **LXi heater** `0x68` / **Heat pump** `0x70` — cmd `0x0C` enable (msg[1] bit3 = "Heat Cmd", HeatPump also bit5 mode); reply `0x0D` 4-byte `[0x0D][stateflags][0x00][faultbits]`. **No setpoint/temp on wire** (newer heaters; setpoint lives controller-side). Full fault-bit label tables in [heaters.md](alwin32/heaters.md).
* **ChemLink** `0x80` — register-file device; master writes config via cmds `0x01..0x20`, polls `0x00`; rich status is reply **`0x22`** with a sub-type byte (`0x20` main / `0x01` secondary). Fields: pH/ORP setpoints+alerts, acid/base feeder, salt, level switch, feed timers. [aquatemp-wtrmatic.md](alwin32/aquatemp-wtrmatic.md)
* **Aux / Remaux** `0x28` — cmd `0x00`poll/`0x02`bulk/`0x08`set-relay(16-bit)/`0x09`set-relay(8-bit); reply `[0x01][relay_bitmask][0x00]`. [lpc4-remaux.md](alwin32/lpc4-remaux.md)
* **Laminar (LPC4)** `0x90` — receive-only; cmd `0x11` = pulse effect (8 patterns: off/8s/4s/1s/random-group/random-single/slow/fast). [lpc4-remaux.md](alwin32/lpc4-remaux.md)
* **Keypads** (2x4rem `0x10`, 8button `0x20`) — master pushes LEDs via cmd `0x02` (**2 bits/indicator**: off/on/blink); panel replies `[0x01][0x00][buttonCode]` (button = small int index). [panels.md](alwin32/panels.md)
* **OneTouch `0x40` / PDA `0x60`** — classic **character-terminal** protocol (12 lines × 16 chars), NOT the IAQ page protocol. Inbound opcodes `0x00`poll/`0x02`status/`0x04`set-line/`0x05`toggle/`0x08`select/`0x09`clear/`0x0F`scroll/`0x10`set-cursor; reply fixed `[0x01][flag][flag]`. PDA adds a richer glyph map (arrows + `°`). [onetouch.md](alwin32/onetouch.md), [pda.md](alwin32/pda.md)

### Worked example — SWG chlorinator (`Aquarite.exe`)

Presents as **AutoClear / AquaPure / AquaRite** (resource strings `0x2aec0`); fields
"Salinity", "Add Salt", "Cmd Value", "Error 1–4".

* **Address:** class `0x0A` → **`0x50`–0x53** (`0x4013a9`). Log-on init `{0x01,0x00,0x00}`.
* **Inbound master commands** (dispatch `0x40160d`):

  | cmd | meaning | note |
  |----:|---------|------|
  | `0x00` | poll / keep-alive | |
  | `0x11` | **set output %** — `data[1]` = percent (0–100, 101=boost, 255=service) | confirms project |
  | `0x14` | **Get ID / model** — replies with identity ("AquaRite"/"AquaPure"/"BOOST") | confirms project |
  | `0x15` | output-% variant — same handler as `0x11` (`0x401683`), but the sim stores `data[1]÷10` into its value field (`0x40169a`: `idiv 10`) rather than `data[1]` verbatim | **not previously in project notes** |

* **Outbound response** (`0x401540`, via `RemoteStatus`): `0x12` idle (3 B) when offline;
  **`0x16`** (`AQUARITE_PPM`) 5-byte when active = `[0x16][saltPPM÷100][status-bits][16-bit]`.
  - `payload[1] = saltPPM/100` (raw at `+0x7c` scaled `×0x31CE /100`, `0x401570`).
  - `payload[2]` is a **bit-packed status byte** (`0x4015b6`) — the `AquariteStatuses` enum is a
    true bitfield, one flag per bit.
  - `payload[3..4]` = a 16-bit field (`+0x90`) — **new**; identify on a capture.
  - **Project status:** the above describes the *vendor simulator's* outbound frame. The
    aqualink-automate `AQUARITE_PPM` message currently models **only** `[saltPPM÷100][status-bits]`
    — `AquariteMessage_PPM` serializes/deserializes exactly the PPM byte (`Index_PPM`) and the status
    byte (`Index_Status`) and does **not** (de)serialize the trailing 16-bit field
    (`src/jandy/messages/aquarite/aquarite_message_ppm.{h,cpp}`). Treat the 16-bit field as an
    unimplemented/pending item in the project, not part of its current wire model.

* **No "get output level / setpoint" command — verified exhaustive (instruction-level).** The
  inbound dispatcher `0x4015dd` is the SWG's *only* master-message handler (`GetMasterMessage` has
  a single call site). It reads the command byte (`0x40160d`: `mov bl,[edi]`) and routes it through
  a subtract-chain that matches **exactly four** opcodes — `0x00`, `0x11`, `0x14`, `0x15` — and
  `jmp`s every other value straight to the epilogue with **no reply** (`0x401628`→`0x4016f2`, which
  `xor eax,eax; ret 8`). None of the four queries the chlorinator's current output %:
  - `0x00` → 3-byte idle keep-alive (`0x12`, buffer `+0xb1`, `0x4016d8`);
  - `0x11`/`0x15` → the salt-PPM + status frame (`0x16` built by `0x401540`) — it reports salt and
    status bits, **never the output %**;
  - `0x14` → the 18-byte model-identity string ("AquaRite"/"AquaPure"/"BOOST", `0x40162d`).

  So the output level is **master-PUSH-only**: the controller *sets* it (`0x11`/`0x15`) and the SWG
  never echoes, stores-for-readback, or reports it. There is **no SWG-side wire command to read the
  configured Pool/Spa setpoint** — confirmed against the official Rev T.0.1 vendor simulator.

  > **Implication for aqualink-automate.** Reading the *configured* chlorinator setpoint can only be
  > done by navigating the OneTouch/iAQ menu to the **Set AquaPure** page and screen-scraping it
  > (`OneTouchDevice::PageProcessor_SetAquapure`, Pool % = line 3 / Spa % = line 4), with proactive
  > re-acquisition driven by `Devices::ChlorinatorSetpointRefresh`
  > (`src/jandy/devices/chlorinator_setpoint_refresh.h`). A direct SWG query is **not** an available
  > alternative — this RE rules it out at the instruction level. The live `AQUARITE_Percent` (0x11)
  > the app already decodes is the master's *push*, i.e. the instantaneous output (0 while idle), not
  > a value the SWG can be asked for.

---

## 6. Master command vocabulary (`Pwrcntr.exe`, the bus master)

Recovered by decoding all 62 `netIO` call sites (full table in
[pwrcntr-master.md](alwin32/pwrcntr-master.md)). Highlights:

* **Bus enumeration / probe**: no separate scan — every command broadcasts to `dest=0xFF`,
  retries instances `0..4`, breaks on first reply, then validates the responder's class. `0x00`
  = bare keep-alive poll; `0x14` = GetId/login (copies the model-id string back).
* **Generic**: `0x00` poll, `0x14` GetId, `0x05` version.
* **Heater**: `0x0C` enable (+ setpoint for the LX class), responder class `0x0d`. Confirms project.
* **Aux / panels**: `0x13` 24-byte controller-status broadcast, `0x03` 18-byte LED/display push,
  `0x08`/`0x09` relay set.
* **Pentair IntelliFlo**: a separate `iFloNetIO` primitive with a smaller `{cmd,val,dest}` struct
  — cmds `0x01/0x04/0x05/0x06/0x02`. Confirms project's IntelliFlo bytes.
* **iAQ / AqualinkTouch page protocol** (class 6, `0x30`–0x33; the master gets a 450 ms guard
  time for it): `0x23` PageStart (payload `0x2a` = **EquipmentStatus** page type), `0x24`
  PageButton, `0x26` PageMessage, `0x27` page sub-message, `0x28` PageEnd, `0x31` ControlReady,
  `0x1c`/`0x1e` iAQ control/command. **Independently validates `iaq_protocol.md` from the master
  side.** ⚠ Opcode-naming note (likely reconciled): the master builds what this RE labels
  **PageMessage as `0x26`**, whereas the project records **`IAQ_PageMessage = 0x25`** and
  **`IAQ_TableMessage = 0x26`** (`src/jandy/messages/jandy_message_ids.h`). The most plausible
  reconciliation is that the master's "`0x26`" is the project's **TableMessage**, not its
  PageMessage — i.e. a naming difference, not a wire-value conflict. The project's `0x25`/`0x26`
  assignment stands unless a live capture shows otherwise.
* **iAqualink2 WiFi-module command name table** (recovered from `netIO`'s debug logger,
  `NetIO.dll!0x16ba`) — the application command set of the iAqualink2 cloud module:

  | byte | name | byte | name | byte | name |
  |-----:|------|-----:|------|-----:|------|
  | `0x00` | ENQ | `0x57` | GET RSSI | `0x5B` | EVENT LOGGING |
  | `0x53` | SSID | `0x58` | REV | `0x5C` | REGION |
  | `0x54` | NETKEY | `0x59` | CALC RSSI | `0x70` | MAINSCR_INFO |
  | `0x55` | ENCRYPT | `0x5A` | PERIODIC LOGGING | `0x71` | ONETOUCH_INFO |
  | `0x56` | NETLINK STATUS | | | `0x72` | AUXILIARY_INF |
  | | | | | `0x73` | IPHONE_INFO |

* **Config model**: INI `Options` → `CommPort`, `ControllerType` (default `0x7225`) → aux-relay
  count (3/5/6/10/14) = the RS-4/6/8 capacity model.

---

## 7. Cross-validation vs the project

**Confirms** (independently, from the vendor's code): the DLE+8-bit-sum framing; the
`(class<<3)|instance` addressing; device addresses LX_Heater `0x38`, OneTouch `0x40`,
chlorinator `0x50`, ChemLink `0x80`, AqualinkTouch `0x30`–0x33; chlorinator cmds `0x11`/`0x14`
and the `0x16` PPM reply with PPM÷100; heater `Heating = 0x08`; IntelliFlo cmd bytes; the iAQ
page protocol (`0x23`/`0x24`/`0x28`/`0x31`, page type `0x2a`).

**Extends / new**: the frame pad bytes + checksum DLE-stuffing (§2); the chlorinator `0x15`
command and the 5-byte `0x16` reply's trailing 16-bit field; the **keypad/aux/PDA/heater/ePump/
laminar address classes** (§5); the heater fault-bit label tables and LX-heater setpoint command;
the ChemLink `0x22` status command + sub-type model; the classic OneTouch/PDA character-terminal
opcode set; and the **iAqualink2 WiFi command-name table** (§6).

**Discrepancies to reconcile against a capture**: PageMessage `0x26` (master) vs `0x25` (project
notes); and which heater frame variant (`0x0D` 4-byte device frame vs MainStatus `0x70`
controller encoding) the project's `HeaterMessage_Status` index-4/6 decode corresponds to.

**Scope caveat**: the classic `Onetouch.exe`/`PDA.exe` sims implement the legacy
character-terminal protocol, **not** the IAQ/AqualinkTouch page protocol the project models.
The IAQ page-message *field layouts* are best decoded from `Pwrcntr.exe` (the master that emits
them), not from these slave panels.

---

## 8. How to apply this to aqualink-automate

* **Validate framing**: confirm serialize/deserialize handle the leading/trailing `0x00` pads
  and **checksum-byte DLE-stuffing** (§2).
* **Address map**: extend `jandy_device_types.h` with the newly-pinned classes (§5).
* **Chlorinator**: add the `0x15` command and the 5-byte `0x16` reply shape (trailing 16-bit
  field); the status byte is a per-bit bitfield.
* **iAqualink2**: the WiFi command-name table (§6) is a ready-made vocabulary for the project's
  iAQ work; the master-side iAQ page builders confirm `iaq_protocol.md`.
* **Capture real fixtures without hardware** (highest-leverage): these sims emit *genuine*
  protocol bytes. Point a controller sim (`Pwrcntr.exe`) and an equipment sim at a virtual COM
  pair (e.g. com0com) or the app's RFC2217 bridge, and record with `--record-serial` → a `.cap`
  fixture for the replay harness (`docs/RECORD_REPLAY.md`). This turns the simulator suite into a
  fixture generator for **every device type**.

### Follow-up RE targets (not yet decoded)
* The IAQ page-message *field layouts* from `Pwrcntr.exe` (PageStart/Button/Message/End byte
  offsets) — the master emits them.
* Pentair `_iFloVS` and `_ePumpAc` framing internals in `NetIO.dll` (`iFloNetIO`/`ePumpAcNetIO`).
* `iodll.dll` option DIP-switch bits (`GetOptions`/`GetOptions_S2`) → controller config model.
* Capture-gated per-byte field maps flagged in the linked files (ChemLink `0x22` block, IntelliFlo
  status offsets, keypad LED byte maps beyond byte 1).

---

## 9. Reproducing — the RE toolkit

A helper drives `pefile` + `capstone` (kept outside the repo at `C:\Users\i_che\alwin32-re\alw.py`,
since it only works against a local Alwin32 install):

```
python alw.py info     "<exe>"                       # PE header, imports, NetIO exports
python alw.py strings  "<exe>" --dedup --grep ...    # ASCII + UTF-16 (resource) strings
python alw.py disasm   "<dll>" --name CommTx         # disasm an export (or --rva 0x1540)
python alw.py callsites "<exe>" RemoteStatus         # find every call into a NetIO import,
                                                     #   following the debug-build double-thunk
                                                     #   chain (E8 → ILT E9 jmp → IAT FF25 jmp),
                                                     #   and print the pushed args
```

**Per-device recipe**: (1) `callsites <exe> RemoteGetNextAddress` → class arg → address; (2)
`callsites <exe> GetMasterMessage` → inbound dispatch chain → master→device commands; (3)
`callsites <exe> RemoteStatus` → payload builder → device→master response layout (watch for
`shl reg,1; add reg,flag` bit-packing and `imul`/`idiv` scaling); (4) `strings --dedup` for
field labels/units.
