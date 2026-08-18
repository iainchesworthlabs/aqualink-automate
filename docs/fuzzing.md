# Fuzzing the RS-485 protocol decoders

The application parses **untrusted wire data** off the RS-485 bus in the Jandy and
Pentair message deserialisers. That is exactly the class of code — a malformed
length field or truncated frame turning into an out-of-bounds read — that
coverage-guided fuzzing catches and example-based unit tests miss. This page
describes the libFuzzer harnesses that exercise those decoders, how to build and
run them, where the corpus lives, and the discipline for acting on a crash.

This also satisfies the OpenSSF Scorecard **Fuzzing** check with a real signal
rather than a posture tick: [`fuzzing.yml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/.github/workflows/fuzzing.yml) runs
the harnesses on a schedule (see [ci-cd.md](ci-cd.md)).

## What is fuzzed

The harnesses target the highest-level "raw bytes → message" entry points, so one
harness covers many message types:

| Harness (`fuzz/`) | Entry points exercised |
|---|---|
| `fuzz-jandy-message` | `Generators::GenerateMessageFromRawData` (framing, DLE-null de-escaping, checksum, packet-boundary scanning, buffer cleanup) + `Factory::JandyMessageFactoryT::CreateFromSerialData` → every registered `JandyMessage` subtype's `DeserializeContents` |
| `fuzz-pentair-message` | `Pentair::Generators::GenerateMessageFromRawData` (0xA5 preamble scan, 16-bit BE checksum) + `Pentair::Factory::PentairMessageFactory::CreateFromSerialData` → every `PentairMessage` subtype's `DeserializeContents` |
| `fuzz-schedule-json` | the web-API schedule request-body validators `Scheduling::FromJson` + `ControllerScheduleFromJson` (POST/PUT `/api/schedules` and `/api/controller/schedules`), fed arbitrary bytes parsed as JSON exactly as the handlers do |
| `fuzz-websocket-json` | the inbound WebSocket message-envelope parser `HTTP::WebSocket_Event::ConvertFromStringView` (`{type, payload}` JSON + case-insensitive event-type enum-cast) that every browser frame flows through |
| `fuzz-mqtt-payload` | the untrusted MQTT command-payload parsers `Mqtt::PayloadParsing::ParsePayloadNumber<T>` / `ParsePayloadString` / `SanitiseForLog` (range-checked numeric extraction, string extraction, log sanitising) |
| `fuzz-config-parse` | config-file reading + option-value validation: arbitrary INI text through `boost::program_options::parse_config_file` + the project's custom validators (`Severity` / `ProfilerTypes` / `SyslogFacility` / MQTT `ProtocolVersion`) over a representative options grammar |
| `fuzz-query-string` | HTTP request-target / query-string parsing — `HTTP::ParseQueryString` (`boost::urls::parse_origin_form` + param lookup) over a fuzzed Boost.Beast request target |
| `fuzz-jwt` | JWT bearer-token verify robustness — `Auth::JwtCodec::Verify` (backed by an empty key store) must return `std::nullopt` on any malformed token, never throw/crash |
| `fuzz-duration` | the `HH:MM:SS` timeout-duration option parser `Utility::TimeoutDurationStringConverter` (fixed-width, `std::from_chars`; `noexcept`, so this catches OOB indexing) |
| `fuzz-replay-line` | the record/replay `.cap` capture-file parser, driven through the public `MockSerialPortImpl` file path (line parse + multi-read buffering); a dev/local input |

Each harness does two things per input (see `fuzz/fuzz_jandy_message.cpp` /
`fuzz/fuzz_pentair_message.cpp`):

1. **Raw** — the fuzzer bytes are pushed straight into the generator. Most random
   inputs are rejected at framing/checksum, but the scan/cleanup code itself is
   fuzzed on every input.
2. **Wrapped** — a *valid, checksum-correct, wire-escaped* frame is built around a
   mutated `(address bytes, payload)` triple (`fuzz/fuzz_frame_builders.h`, which
   reuses the production serialisers) and pushed through the factory **and** the
   generator, so a mutated payload actually reaches the per-type deserialiser past
   the checksum gate.

## Build & run (Linux/clang — the primary path)

libFuzzer is a Clang feature, so coverage-guided fuzzing uses the LLVM toolchain —
the same environment OSS-Fuzz builds in. The `config-linux-llvm-fuzzing` preset
builds the decode libraries with `-fsanitize=fuzzer-no-link,address` and the
harnesses with `-fsanitize=fuzzer,address` (wired in `cmake/Fuzzing.cmake`, kept
out of `src/` CMake per [platform-isolation.md](platform-isolation.md)).

```bash
# Configure + build the harnesses (target: fuzzers).
cmake --preset config-linux-llvm-fuzzing
cmake --build --preset build-linux-llvm-fuzzing

# Seed the corpus from the recorded capture fixtures (idempotent).
python3 fuzz/seed_corpus.py

# Fuzz a harness (Ctrl-C to stop; -max_total_time bounds it).
./build/config-linux-llvm-fuzzing/fuzz/fuzz-jandy-message  fuzz/corpus/jandy   -max_total_time=120
./build/config-linux-llvm-fuzzing/fuzz/fuzz-pentair-message fuzz/corpus/pentair -max_total_time=120
```

libFuzzer grows `fuzz/corpus/<protocol>/` in place. A crash writes a reproducer to
`crash-<sha1>`; re-run the harness with that file as the sole argument to reproduce.

macOS/clang works identically via `config-macos-llvm-fuzzing`.

## Build & run (Windows / non-Clang — standalone replay)

MSVC has no libFuzzer engine, so on Windows (and any non-Clang compiler) the
harnesses build as **standalone corpus-replay drivers** (`fuzz/standalone_main.cpp`
supplies `main()`): they replay every input in a directory / every file named
through `LLVMFuzzerTestOneInput` under a normal build — no coverage-guided mutation,
but exactly the right tool for **reproducing a crash** or a **CI smoke test**.

```powershell
cmake --preset config-windows-msvc-fuzzing
cmake --build --preset build-windows-msvc-fuzzing
python fuzz/seed_corpus.py
.\build\config-windows-msvc-fuzzing\fuzz\fuzz-jandy-message.exe fuzz\corpus\jandy
# Reproduce a specific crash found on Linux:
.\build\config-windows-msvc-fuzzing\fuzz\fuzz-jandy-message.exe crash-abc123
```

## Corpus

- **Seed source:** `fuzz/seed_corpus.py` parses every `test/fixtures/**/*.cap`
  recording, extracts the on-the-wire byte frames, and sorts them into
  `fuzz/corpus/jandy/` and `fuzz/corpus/pentair/` (Pentair identified by its
  `FF 00 FF A5` preamble; everything else Jandy). It also writes a handful of
  representative schedule/controller-schedule request bodies to
  `fuzz/corpus/schedule-json/` (these are not from `.cap` — they seed the
  schedule-json harness with valid structure). Files are named by SHA-1, so
  re-running de-duplicates.
- **Not committed:** `fuzz/corpus/` is git-ignored (`fuzz/.gitignore`) — it is
  generated from the fixtures and grown by the fuzzer. Regenerate it any time with
  the seed script.

## How fuzzing is tied to the test suite (RS-485 changes)

Fuzzing the decoders is guaranteed on any change to RS-485 processing by **two
layers**, so you never have to remember to run it:

### 1. Always-on in-suite regression guard (every build, every platform)

The unit test
[`test/unit/fuzz/test_protocol_fuzz_smoke.cpp`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/test/unit/fuzz/test_protocol_fuzz_smoke.cpp)
runs in the normal test suite (`testaqualink-automate`) — so it executes on **every**
`ctest`/CI run on every platform, with **no Clang or fuzzing preset required**. It
drives the same decode paths with:

- the shared frame builders, asserting a built frame round-trips through the
  factories (guards the harnesses themselves against rot),
- a large **deterministic** pseudo-random corpus of malformed frames,
- a sweep of **every** message-type / command byte at edge-length payloads, and
- the real recorded `.cap` wire frames.

Because that sweep drives the message **factories** over the whole `0x00–0xFF`
command space, a newly added message type is fuzzed **automatically** — you do not
have to extend the test when you add a decoder. Under a debug STL
(`_ITERATOR_DEBUG_LEVEL`, `_GLIBCXX_ASSERTIONS`) an out-of-bounds access aborts, so
this test crashes in CI if a decoder is ever made unsafe. This is the gate that
covers the `ci.yml` build-and-test on every PR.

### 2. Coverage-guided libFuzzer on decoder-touching changes

[`fuzzing.yml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/.github/workflows/fuzzing.yml) runs the real libFuzzer harnesses
on a weekly cron **and** on every PR whose diff touches `src/jandy/**`,
`src/pentair/**`, `src/core/protocol/**`, `fuzz/**`, or the fuzzing scaffolding — so
a change to how RS-485 is processed triggers an actual mutation-based fuzz run, not
just the deterministic in-suite pass.

The harnesses are also registered as **CTest** tests under a fuzzing preset, so a
local fuzzing-preset test run exercises them too (bounded per `AQ_FUZZ_CTEST_SECONDS`,
default 30s each; the corpus is auto-seeded first via a CTest fixture):

```bash
cmake --preset config-linux-llvm-fuzzing
cmake --build --preset build-linux-llvm-fuzzing
ctest --preset test-linux-llvm-fuzzing        # runs fuzz-seed-corpus + each *-fuzz test
# or just the fuzz tests:  ctest --test-dir build/config-linux-llvm-fuzzing -L fuzz
```

On a non-Clang toolchain (Windows/MSVC) the same `ctest` runs the standalone
**corpus-replay** variant instead of a coverage-guided run.

## Future targets — web / HTTP / API inputs

The web/HTTP/API/WebSocket surface **should** be fuzzed too — it also parses
untrusted input — but the split matters: the HTTP framing (Boost.Beast), the raw
JSON parse (`nlohmann::json`), URL parsing (Boost.URL) and JWT decode (jwt-cpp) are
**third-party libraries that are already fuzzed upstream**. The first-party value is
in the code that turns a *parsed* request into a domain object — the validators and
dispatchers. The same `ENABLE_FUZZING` scaffolding extends to these with a new
harness per target; they are ranked by value × isolatability:

Implemented so far (beyond the RS-485 decoders):

1. **Schedule request bodies** — ✅ `fuzz-schedule-json` (`Scheduling::FromJson` /
   `ControllerScheduleFromJson`). This harness immediately found a real bug:
   `FromJson` read optional fields via nlohmann `json.value(key, default)`, which
   **throws** `type_error.302` on a present-but-wrong-typed field (e.g.
   `{"name": 5}`) instead of returning `nullopt` per its contract — an uncaught
   exception on the untrusted `/api/schedules` body. Fixed (type-safe reads,
   matching `ControllerScheduleFromJson`) + regression tests in
   `test/unit/scheduling/test_schedule.cpp`.
2. **WebSocket message envelope** — ✅ `fuzz-websocket-json`
   (`WebSocket_Event::ConvertFromStringView`, `src/core/http/websocket_event.cpp`).
3. **MQTT command payloads** — ✅ `fuzz-mqtt-payload` (the pure helpers in
   `src/core/mqtt/mqtt_payload_parsing.h`).
4. **Config file + option values** — ✅ `fuzz-config-parse`. Note the config file is
   operator-controlled (a weaker threat model than the wire/HTTP surfaces), so the
   goal here is robustness: a malformed value must not crash startup. The custom
   validators only throw `boost::program_options::error` (the one exception type the
   production config path catches); this harness guards that invariant — any *other*
   escaping exception is a real startup-crash bug.

5. **Query string / request target** — ✅ `fuzz-query-string`.
6. **JWT verify** — ✅ `fuzz-jwt`. Note: with an empty key store every signature
   check fails, so this fuzzes the jwt-cpp decode + the first-party `kid` extraction
   and guards the "`Verify` never throws" contract; the claim-extraction path past
   signature verification is only reachable with a validly-signed token, which a
   mutation fuzzer cannot forge.
7. **`HH:MM:SS` duration** — ✅ `fuzz-duration`.
8. **`.cap` replay parser** — ✅ `fuzz-replay-line` (via the public `MockSerialPortImpl`
   file path, since the line parser is private).

That covers the input surfaces worth fuzzing. What remains is genuinely third-party
and already fuzzed upstream — the Boost.Beast HTTP framing, the nlohmann JSON parser
itself, Boost.URL, Boost.program_options' own INI tokeniser, and jwt-cpp's base64/
decode — so a first-party harness would mostly re-test someone else's code. Add a new
harness when a new first-party parser of untrusted input appears (a new wire message
type is already covered automatically; a new request-body validator or inbound
message format is not).

## If the fuzzer finds a crash

A crash is a **real bug** in an untrusted-input parser. Per the project's bug-fix
discipline:

1. Reproduce it (`<harness> crash-<sha1>`), then **fix the production
   deserialiser** — bound the read, guard the length, reject the frame.
2. **Never weaken the parser to silence the fuzzer** (do not delete a field, widen
   a buffer past the wire contract, or swallow the input).
3. Add a **regression test** — add the crashing frame to
   `test_protocol_fuzz_smoke.cpp` (or a targeted message test) so it fails before
   the fix and passes after.
4. Commit the minimised reproducer into the corpus so the fuzzer never re-explores
   it from scratch.
