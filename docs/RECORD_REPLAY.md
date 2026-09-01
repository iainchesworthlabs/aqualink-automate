# Serial record / replay

*For contributors capturing real RS-485 traffic and replaying it (live or as a test fixture). Full developer-option semantics live in [docs/configuration.md](configuration.md); the runtime recording routes live in [docs/usage-and-api.md](usage-and-api.md).*

Aqualink-Automate can **record** the raw RS-485 serial traffic it exchanges with
real hardware to a capture file, and later **replay** that file instead of a real
port. The recorder and replayer share one on-disk format, so a capture records
cleanly and replays as-is — including as a reusable test fixture.

- Recorder: `src/core/developer/recording_serial_port_impl.cpp`
- Replayer: `src/core/developer/mock_serial_port_impl.cpp` (`HandleFileRead` / `DecodeReplayLine`)

CLI options are written with their leading dashes (e.g. `--replay-filename`).
Config-file keys are the same long name without the dashes (`replay-filename`).

---

## 1. Record against real hardware

Recording wraps the *real* serial port (physical or network), so you must point
the app at actual hardware as well as giving it a capture path:

```bash
# Physical RS-485 adapter
aqualink-automate --serial-port /dev/ttyUSB0 --record-serial session.cap

# Networked RS-485 (RFC2217 / raw socket)
aqualink-automate --remote-serial-port 192.168.1.50:6638 --rfc2217 --record-serial session.cap
```

Every byte read from the bus is appended to `session.cap` as it arrives (writes
the app sends are recorded too — see R/W below). The file is flushed per line, so
a capture is usable even if the process is killed.

`--record-serial` conflicts with `--replay-filename` (you cannot record and
replay at once).

## 2. Replay a capture

Replaying touches no hardware, but `--replay-filename` has hard option
dependencies that **every** replay command must satisfy or option validation
rejects the invocation:

- `--dev-mode` — replay is a developer-mode feature.
- `--profiler <tool>` — accepts `tracy`, `uprof`, or `vtune`. Tracy is not
  compiled into a normal build, so the profiler factory falls back to a no-op
  profiler; the value still has to be present to clear the dependency.
- **Every** per-channel `--loglevel-<channel> <severity>` option. These options
  carry no default, so the dependency only clears when each of the 18 channels is
  passed explicitly. The 18 channels are: `certificates`, `coroutines`,
  `developer`, `devices`, `equipment`, `exceptions`, `main`, `messages`, `mqtt`,
  `navigation`, `options`, `platform`, `profiling`, `protocol`, `scraping`,
  `serial`, `signals`, `web`. Each takes a `Severity`: one of `trace`, `debug`,
  `info`, `notify`, `warning`, `error`, `fatal`.

**Important:** a bare `aqualink-automate --dev-mode --replay-filename session.cap`
**fails** dependency validation. It is missing `--profiler` and the per-channel
log levels. Use a complete command like the one below.

A complete, validation-passing replay command (mirrors the working invocation in
`playwright.config.ts`):

```bash
aqualink-automate \
  --dev-mode \
  --replay-filename session.cap \
  --profiler tracy \
  --loglevel-certificates info \
  --loglevel-coroutines info \
  --loglevel-developer info \
  --loglevel-devices info \
  --loglevel-equipment info \
  --loglevel-exceptions info \
  --loglevel-main info \
  --loglevel-messages info \
  --loglevel-mqtt info \
  --loglevel-navigation info \
  --loglevel-options info \
  --loglevel-platform info \
  --loglevel-profiling info \
  --loglevel-protocol info \
  --loglevel-scraping info \
  --loglevel-serial info \
  --loglevel-signals info \
  --loglevel-web info
```

The app feeds the capture's **R-direction** bytes into the decode pipeline
exactly as if they had arrived from a real device.

**Tip:** scripting the log levels is less error-prone than typing all 18 by
hand. The Playwright config does exactly this — it generates one
`--loglevel-<channel> info` per channel and appends them to the command.

### Replay pacing

A real bus delivers bytes at a fixed rate; a capture file does not, so by
default the replayer paces itself to roughly the bus's natural inter-frame
period instead of consuming the whole file as fast as the parser will accept it.

The application already runs a fixed-period **frame loop** (it steps every
subsystem once per frame). Replay reuses it: the loop steps at the configured
processing period, and within each frame the protocol task reads just one serial
chunk. Pacing is therefore the frame loop's job — the serial read is never
blocked, so framing/sync is identical to a live port. The per-frame read bound
also keeps the read aligned with the fixed-size circular buffer; an unpaced
replay of a long capture slurps the whole file in one frame and overruns the
buffer, losing everything but the tail.

| Option | Short | Type | Default | Description |
|--------|-------|------|---------|-------------|
| `--replay-frame-period` | | `uint32` (ms) | `15` | Wall-clock period per replay frame. `0` = unpaced (free-running, as fast as possible — the old behaviour). |
| `--replay-speed` | | `double` | `1.0` | Scales the period: `>1` faster, `<1` slower (e.g. `--replay-speed 10` ≈ 1.5 ms/frame). |

Because the same dependency rules apply, the pacing examples below carry the full
`--profiler` + per-channel `--loglevel-*` set. The log-level block is collapsed
to `--loglevel-... info` for readability — substitute the 18 explicit options
from the complete command above.

```bash
# Default ≈15 ms/frame pacing
aqualink-automate --dev-mode --replay-filename session.cap \
  --profiler tracy --loglevel-... info   # ... = the 18 --loglevel-<channel> info options

# 10x faster (still paced; no buffer overflow)
aqualink-automate --dev-mode --replay-filename session.cap \
  --profiler tracy --loglevel-... info \
  --replay-speed 10

# Unpaced — consume the capture as fast as possible
aqualink-automate --dev-mode --replay-filename session.cap \
  --profiler tracy --loglevel-... info \
  --replay-frame-period 0
```

Pacing applies only to the `--replay-filename` path; real serial ports already
self-pace (the frame loop keeps its normal adaptive, low-latency step), and the
unit-test replay harness runs unbounded for speed.

For the full semantics of these developer options, see
[docs/configuration.md](configuration.md).

---

## 3. Capture file format

A capture is a UTF-8 text file:

```
# Serial recording started at: Sun Jun 08 12:00:00 2026
# Format: [timestamp_ms] direction byte|byte|byte|...
# Direction: R=read (from device), W=write (to device)
#
[       142] R 0x10|0x02|0x50|0x14|0x76|0x10|0x03
[       310] W 0x10|0x02|0x50|0x11|0x28|0x9b|0x10|0x03
```

Line kinds:

| Line                              | Meaning                                             | On replay |
|-----------------------------------|-----------------------------------------------------|-----------|
| `# ...`                           | comment / header / metadata                         | ignored   |
| *(blank / whitespace-only)*       | spacing                                             | ignored   |
| `[<ts>] R 0x##\|0x##\|...`         | bytes **read from** the device (the incoming stream) | **replayed** |
| `[<ts>] W 0x##\|0x##\|...`         | bytes the app **wrote to** the device (its own output) | ignored  |
| `0x##\|0x##\|...`                  | legacy bare format (no timestamp/direction)         | **replayed** (back-compat) |

- `<ts>` is milliseconds since recording start (informational; replay does not
  reproduce the recorded timestamps — it paces to a fixed `--replay-frame-period`
  instead; see *Replay pacing* above).
- Each byte token is exactly `0x##` (two hex digits). Pipe-separated.
- **Only `R` lines and legacy bare lines are replayed.** `W` lines are the
  application's *past output*; feeding them back as input would corrupt the
  stream, so they are skipped. Malformed tokens are skipped individually; the
  rest of the line still replays.

---

## 4. Use a capture as a test fixture

Drop a capture under `test/fixtures/` (CMake copies that directory next to the
test executables as a POST_BUILD step, so it resolves from the test working
directory). A reference capture lives at `test/fixtures/sample_session.cap`
(an AquaRite SWG identification + status session that populates a chlorinator in
the DataHub).

Load it with the helper in `test/unit/support/unit_test_loadfixture.h`:

```cpp
#include "support/unit_test_loadfixture.h"
#include "support/unit_test_mockreplayharness.h"

// (a) Just the bytes (R-direction only), parsed by the SAME production reader
//     used by --replay-filename, so it is byte-for-byte what a live replay sees:
std::vector<uint8_t> bytes = Test::LoadFixture("fixtures/sample_session.cap");

// (b) Or replay straight through the full Jandy decode stack and assert state:
Test::MockReplayHarness harness;
auto device_id = std::make_shared<Devices::JandyDeviceType>(Devices::JandyDeviceId(0x50));
auto& swg = harness.AddDevice<Devices::AquariteDevice>(device_id);

Test::ReplayFixture(harness, "fixtures/sample_session.cap");   // = LoadFixture + harness.Replay

BOOST_REQUIRE_EQUAL(harness.DataHub()->Chlorinators().size(), 1u);
BOOST_CHECK_EQUAL(harness.DataHub()->SaltLevel().value(), 3200.0);
```

Helper API (`AqualinkAutomate::Test`):

- `std::vector<uint8_t> LoadFixture(const std::string& path)` — returns the
  replayable bytes; throws `std::runtime_error` if the file is missing.
- `std::vector<uint8_t> ReplayFixture(MockReplayHarness&, const std::string& path)`
  — `LoadFixture` then `harness.Replay(...)`; returns the bytes replayed.

Relative paths resolve against the test working directory; absolute paths are
used as-is.

### Recording a fixture from a session you captured

A file produced by `--record-serial` (section 1) is already in this exact format
— copy it straight into `test/fixtures/` and load it with `LoadFixture`. No
conversion step is needed; that round-trip (record → replay-as-fixture) is
covered by `test/unit/protocol/test_record_replay_roundtrip.cpp`.

You can also start and stop recording on a **running** system through the
diagnostics web UI / REST route rather than from boot — see section 5 below and
the recording routes in [docs/usage-and-api.md](usage-and-api.md).

---

## 5. Record at runtime, and get the file back

`--record-serial` records from boot to a path you choose. The **Diagnostics page**
of the web UI records on demand against a system that is already running, which is
usually what you want when chasing a behaviour you can reproduce live.

### Where those captures go

On-demand captures are confined to one directory — `--capture-directory`, default
`captures` under the working directory. Everything about that directory is
deliberate:

- The filename you type in the UI (or send as `POST /api/diagnostics/recording`
  `{"action":"start","filename":"..."}`) is treated as a **bare `*.cap` basename**
  and jailed into the directory. Path separators, drive letters, leading
  separators, `..`, control characters, and quotes are rejected with `400`, so the
  endpoint can never be steered at another file on the host.
- The directory is created on the first recording, so a fresh install leaves
  nothing behind until you actually record.
- `--record-serial` is **not** affected: it is an operator-supplied path on the
  command line and is used exactly as given.

Point `--capture-directory` somewhere you can reach when the app runs where you
cannot get a shell. That is precisely what the **Home Assistant add-on** does: it
passes `--capture-directory /config/captures`, which is the add-on's `app_config`
map — reachable on the host at `/app_configs/aqualink_automate/captures`
(`/addon_configs/...` before Supervisor 2026.07) through the Samba or File editor
add-ons. See [docs/homeassistant-addon.md](homeassistant-addon.md).

### Getting the capture off the machine

The Diagnostics page's **Serial Recording** card lists the captures already on the
server (name, size, timestamp) with a **Download** button on each, so a finished
capture reaches your machine without a shell on the host. The same surface is
available directly:

```bash
# List what is there
curl -s http://<host>/api/diagnostics/recording/captures

# Pull one down
curl -sOJ http://<host>/api/diagnostics/recording/captures/session.cap
```

The download route applies the **same** jail as the recording route, so it only
ever serves a capture from the capture directory (and never a symlink pointing out
of it). Because the response is buffered rather than streamed, a capture larger
than **64 MiB** is refused with `413` — at the bus's ~5 KB/s of capture text that
is a multi-hour recording; copy those straight out of the capture directory
instead.

A downloaded capture is byte-for-byte the format in section 3, so it replays with
`--replay-filename` and drops straight into `test/fixtures/` as a fixture.
