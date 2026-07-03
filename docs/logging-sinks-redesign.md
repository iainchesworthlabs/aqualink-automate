# Logging Sinks Redesign — OS-Native Log Delivery

**Status:** Slice 1 IMPLEMENTED (2026-07-03) on `claude/unruffled-nightingale-6f0d91`; slices 2–3 pending.

This document is a dated design snapshot. Verify claims against the code before
relying on any citation; symbols are used as anchors rather than line numbers.

## Implementation notes (2026-07-03, slice 1)

Slice 1 (phases 1A–1E) landed in `src/core/logging/sinks/` (`severity_mappings`,
`log_environment`, `sink_console`, `sink_registry`, `sink_filters`, `sink_native`)
plus the `Logging` options area and the `main` activation/shutdown wiring. Full
test suite green (2296 cases). Deviations from the design above, for accuracy:

- **Audit separation mechanism.** Audit records carry an `is_audit` **source
  attribute** (added to the audit logger via `add_attribute("IsAudit",
  constant<bool>(true))`), and operational sinks are built through the shared
  `MakeOperationalFilter()` which excludes it. This is because **Boost.Log
  evaluates sink filters at record-open time against source attributes only** — a
  streamed `add_value` reaches formatters but not filters, so it would have let
  audit leak. "Leak-proof by construction" (§10.2) therefore means *every
  operational sink is built through the shared excluding filter*, not that audit
  bypasses the core entirely.
- **Options namespace.** The options area is C++-namespaced `Options::LogSinks`
  (not `Options::Logging`) to avoid colliding with the `AqualinkAutomate::Logging`
  subsystem namespace in `main`; the user-facing area name stays "Logging".
- **Native sink frontend is synchronous for now.** §9 specifies async frontends
  for native/file; slice 1 ships the native (audit) sink synchronous, matching the
  prior audit behaviour (audit volume is low, not a hot path). The async frontend
  is deferred to the file-sink slice, where `SinkRegistry::FlushAll()` already
  provides the shutdown drain hook.
- **`docker-verify` log assertion deferred.** §14.3's "`docker logs` shows startup
  records" needs a replay fixture inside a test image (the runtime image ships
  none and a real run needs a serial source), so it is deferred. The Linux e2e
  job runs `e2e/logging.spec.ts` (picked up automatically by `npx playwright
  test`), which covers the real logging path incl. the journald-prefix case.
- **`configuration.md` channel list.** Removing `Audit` from the enum made the
  pre-existing "18 channels" list correct on this branch; the separately-tracked
  "add audit → 19" fix was for the old code state and does not apply here.

---

## 1. Motivation

The auth/authz work added an `Audit` logging channel whose events are forwarded
to the platform's native log (syslog on POSIX, the Windows Event Log). Building
that sink exposed the wider gap: the application has **no sink architecture** —
one hardcoded console sink for everything, and the audit OS sink hand-built as a
one-off inside the auth module. There is no way to configure where logs go, no
file logging, no structured output, and — the sharpest problem — **severity
information is lost at every OS boundary**, so `journalctl -p err` and Event
Viewer level filters see nothing useful.

The goal: a small, structured pattern for logging sinks that delivers records to
the *right* place per environment (interactive terminal, systemd service, Docker
container, Windows service, macOS), with correct priorities, without changing a
single `LogX()` call site.

## 2. Current state (verified 2026-07-03)

### 2.1 The general sink

`Logging::Initialise()` (`src/core/logging/logging_initialise.cpp`) registers
exactly one sink:

- `boost::log::sinks::synchronous_sink<text_ostream_backend>` writing to
  `std::clog` (stderr).
- Formatter: `Logging::Formatter` (`logging_formatter.cpp`) — human-readable
  text; Trace/Debug records additionally render `file:line`.
- Filter: `SeverityFiltering::PerChannelTest` — the per-channel runtime
  severity filter (adjustable live via `WebRoute_Diagnostics_Logging` and the
  `--loglevel-<channel>` developer options).
- `auto_flush(true)` — deliberate, so container log drivers and crash tails see
  every record immediately.

### 2.2 The audit special case

`Auth::RegisterAuditOsSink()` (`src/core/auth/audit_log.cpp`) registers a second
sink when auth is enabled, filtered to `Channel::Audit` only:

- POSIX: `sinks::syslog_backend` with `facility = syslog::user`,
  `use_impl = syslog::native` (journald ingests this on systemd distributions).
- Windows: `sinks::simple_event_log_backend` with
  `log_source = "Aqualink-Automate"`.
- Formatter: message text only. **No severity mapper is installed** (see §3.1).
- Registration failures are caught and logged; the JSONL file remains the
  durable trail.

Separately, `Auth::AuditLog::AppendJsonl` writes an owner-only JSONL file into
the auth state directory with hand-rolled size-based rotation
(`AuditLog::RotateIfNeeded`).

### 2.3 Deployment surfaces

| Surface | Log transport today | Storage / rotation |
|---|---|---|
| systemd service (`packaging/systemd/aqualink-automate.service`) | `StandardOutput/StandardError=journal`, `SyslogIdentifier=aqualink-automate` | journald owns both — **already native by transport** |
| Docker (`Dockerfile` runtime stages, tini + `docker-entrypoint.sh`) | stdout/stderr → container log driver | the driver owns both (json-file, journald, fluentd, …) — **already native by transport** |
| Interactive terminal (all platforms) | stderr | none needed |
| Windows | stderr only (no service wrapper exists today) | none |
| macOS (toolchain exists: `cmake/toolchains/macos.llvm.toolchain.cmake`) | stderr only | none |

The key observation: on the two primary production surfaces the *correct*
OS-native behaviour is **console output, done well** — the platform already owns
routing, storage, and rotation. What is missing is fidelity (priorities,
structure), not transport.

### 2.4 Options surface

`--debug`, `--trace` (`options_app_options.h`) and the data-driven
`--loglevel-<channel>` set (`options_developer_options.cpp`). No sink-related
options exist.

## 3. Gaps

### 3.1 Severity fidelity is lost at every boundary (verified against Boost 1.91)

- **journald**: every stderr line is recorded at the stream's default priority
  (info). `journalctl -u aqualink-automate -p err` returns nothing even when
  errors were logged; priority-based monitoring is blind. journald supports the
  sd-daemon protocol — a `<N>` prefix per line, N = syslog priority 0–7 — on
  streams connected via `StandardError=journal`. We do not emit it. Fixing this
  requires **no libsystemd dependency**; it is pure formatting.
- **syslog (audit sink)**: `syslog_backend::consume` in Boost.Log sends
  **every record at `syslog::info`** when no `set_severity_mapper` is installed
  (verified in Boost 1.91 `libs/log/src/syslog_backend.cpp`). Audit denials
  logged at `Notify`/`Warning` land in syslog as informational.
- **Windows Event Log**: identical default — `basic_simple_event_log_backend::consume`
  falls back to `event_log::info` without a mapper (verified in
  `libs/log/src/windows/event_log_backend.cpp`). Warnings and errors render as
  *Information* in Event Viewer.
- Boost's "direct" mappings cannot be used either: our `Logging::Severity` enum
  (7 levels including `Notify`) does not coincide numerically with syslog
  levels (where 0 = emergency). **Every OS backend needs an explicit
  `custom_severity_mapping`** (§7).

### 3.2 No pattern to extend

The audit sink lives in `src/core/auth`, not the logging layer. A second
OS-native consumer (e.g. "route `Error`+ to the Event Log") would be copy-paste.
Sink registration is add-and-forget: nothing owns the handles, so there is no
orderly flush/removal at shutdown and tests must thread handles around manually.

### 3.3 Windows Event Log registration

`simple_event_log_backend` performs **on-demand HKLM registration**: at
construction it checks the registry read-only, and if the event source is not
registered it attempts to create
`HKLM\SYSTEM\CurrentControlSet\Services\EventLog\Application\<source>` with
`EventMessageFile` pointing at the current module (verified in Boost 1.91
`windows/event_log_registry.hpp`). Consequences:

- An unprivileged first run cannot write HKLM → construction throws → the
  existing `try/catch` downgrades to "no OS audit sink". Audit events silently
  never reach the Event Log for non-admin users unless the source was
  pre-registered.
- Even when registration succeeds, Event Viewer renders the classic *"the
  description for Event ID … cannot be found"* wrapper unless the registered
  message file contains a matching message-table resource.

Registration is an **install-time job** that the packaging does not do (§11).

### 3.4 Synchronous sinks on the kernel thread

All sinks are `synchronous_sink`, and the application is single-threaded
cooperative (see `kernel-architecture`): a slow Event Log or file write stalls
the main poll loop, which is also servicing the RS-485 protocol. stderr with
`auto_flush` is acceptable; heavier backends are not.

### 3.5 Missing capabilities

- No file sink for platforms without a system-logger story (Windows run
  interactively, macOS under launchd, non-systemd Linux).
- No structured (JSON-lines) console format for container log pipelines
  (Loki/promtail, CloudWatch, fluentd parse JSON natively).
- No environment awareness — the app cannot distinguish "interactive TTY" from
  "systemd service" from "redirected/container", so it cannot choose defaults.
- Audit JSONL hand-rolls rotation that `sinks::text_file_backend` provides
  (acceptable for now — the audit file has ownership/permission requirements —
  but the general file sink must not repeat the pattern).

## 4. Design principles

1. **The `LogX(Channel, message)` facade does not change.** No call site is
   touched. This is sink *composition* at initialisation time only.
2. **Model on the profiler facade.** Multiple backends behind one registry,
   selected by options, no-op safe when unavailable — the same shape as
   `Factory::ProfilerFactory` / `RegisterAvailableProfilers`.
3. **Where the platform already owns log storage, feed it well — don't compete.**
   Under systemd and Docker the console *is* the native path; the job is
   priority fidelity and optional structure, not new transports.
4. **Defaults must never double-log or write into a void.** Auto-adding a
   syslog sink under systemd would record every line twice (journal via stderr
   + journal via syslog). Auto-adding syslog in a container without a syslogd
   writes to a `/dev/log` that does not exist and the records are silently
   dropped. Native sinks are therefore **explicit opt-in** on POSIX (§6).
5. **Explicit severity mappings everywhere.** Boost's defaults are wrong for
   our enum on every backend (§3.1); mapping tables are part of the sink layer,
   not left to backend defaults.
6. **Keep Boost.Log.** Everything required ships in the existing dependency; no
   new logging library, and no journald/os_log hard dependency (both are
   optional later increments behind CMake find-modules, mirroring
   `FindVTune`/`FindUProf`).

## 5. Proposed architecture

New module `src/core/logging/sinks/`:

```
src/core/logging/sinks/
    sink_registry.{h,cpp}        # owns sink handles; add/remove/flush; shutdown hook
    sink_console.{h,cpp}         # text | json; sd-daemon <N> prefixes when journal-connected
    sink_native.{h,cpp}          # POSIX syslog / Windows Event Log; explicit severity maps
    sink_file.{h,cpp}            # boost text_file_backend + rotation collector
    log_environment.{h,cpp}      # environment detection (TTY / journal / service)
    severity_mappings.{h,cpp}    # Severity -> syslog level / event type tables
```

### 5.1 `SinkRegistry`

A small owner (namespace-level singleton alongside the existing
`SeverityFiltering`, same lifetime pattern) that:

- constructs sinks from a resolved `SinkSet` (the outcome of options +
  environment detection),
- holds the `boost::shared_ptr<sinks::sink>` handles,
- exposes `FlushAll()` and `RemoveAll()` — `FlushAll()` is wired into the
  graceful-shutdown path (the same path that flushes coverage data on SIGTERM),
  and `RemoveAll()` gives tests bounded sink lifetimes (replacing the
  hand-returned handle from `RegisterAuditOsSink`, see the comment block in
  `src/core/auth/audit_log.h`).

`Logging::Initialise()` becomes a thin call into the registry:
`Initialise(SinkConfig)` where `SinkConfig` carries the resolved sink set,
format, and file settings. The current signature (no-arg) remains as an overload
resolving `auto` defaults, so early-startup logging (before options are parsed)
still works — see §8.3.

### 5.2 Console sink

The existing sink, upgraded:

- **Format `text` (default):** the current `Logging::Formatter` output,
  unchanged.
- **Format `json`:** one JSON object per line —
  `{"ts","severity","channel","message","file","line"}` (file/line only for
  Trace/Debug, mirroring the text formatter's rule in `Formatter`). Built with
  nlohmann (already a dependency) so escaping is correct. Opt-in via
  `--log-format json`; humans stay first.
- **journald priority prefixes:** when the process detects its stderr is
  connected to the journal (§6.1), each record is prefixed with `<N>` where N
  is the syslog priority from the mapping table (§7). This is the sd-daemon
  protocol; journald strips the prefix and records the real priority. Applied
  in the sink's formatter wrapper, not in `Formatter` itself, so the text/json
  body stays identical. Multi-line messages: only the first line carries the
  record's priority; continuation lines are recorded at the stream default
  (acceptable — multi-line records are rare; do not attempt per-line
  re-prefixing).
- Stays `synchronous_sink` + `auto_flush(true)` — the rationale in the existing
  comment block (container pipes, crash tails) is unchanged.

### 5.3 Native sink

One factory, three platform arms — generalised from `RegisterAuditOsSink`, which
moves here (the auth module keeps only the *policy*, §10):

- **Linux / other POSIX:** `sinks::syslog_backend`, `use_impl = native`,
  facility configurable with default `daemon` for the general sink (the current
  `user` is wrong for a service). The audit trail uses a **separate** native
  sink instance with the `security1` (`LOG_AUTHPRIV`) facility — see §10, which
  owns the audit path; it is not a filtered view of the general sink. Explicit
  `custom_severity_mapping` (§7).
- **Windows:** `sinks::simple_event_log_backend`,
  `log_source = "Aqualink-Automate"`, explicit `custom_event_type_mapping`
  (§7). Construction wrapped in the existing try/catch pattern (HKLM
  registration can fail unprivileged, §3.3); on failure log one `Warning` on
  `Channel::Main` and continue without it.
- **macOS:** the POSIX syslog arm. `syslog(3)` routes into the unified logging
  system via the ASL shim, visible in Console.app and `log show`. A true
  `os_log` backend is an optional later increment (§13, slice 3).
- **journald (optional, later):** a `sd_journal_send`-based backend passing
  structured fields (`PRIORITY`, `SYSLOG_IDENTIFIER`, `AA_CHANNEL`, `CODE_FILE`,
  `CODE_LINE`) — gated on a `Findsystemd` CMake module exactly like the
  VTune/uProf finders. Not required for correct behaviour: the `<N>`-prefix
  console path already delivers priorities under systemd.

Frontend: `asynchronous_sink` with a bounded queue (§9).

### 5.4 File sink

`sinks::text_file_backend` with:

- `rotation_size` (default 10 MiB) and a `file::collector` bounding total kept
  size (default 5 files) — Boost's built-in rotation; **no hand-rolled
  rotation**.
- Same formatter selection as the console (`text`/`json`).
- Default path: none — the sink only exists when `--log-file <path>` is given.
- Frontend: `asynchronous_sink` (§9).

## 6. Environment detection and the `auto` policy

`log_environment.{h,cpp}` provides three detections:

### 6.1 Detections

| Detection | Mechanism |
|---|---|
| **Interactive TTY** | `isatty(STDERR_FILENO)` / `_isatty(_fileno(stderr))` |
| **stderr is the journal** | `$JOURNAL_STREAM` is set by systemd to `<dev>:<inode>` of the stream it attached; compare against `fstat(STDERR_FILENO)`. Match ⇒ stderr lands in journald. |
| **Windows service context** | present only once a service wrapper exists (none today); until then always false. Detection at that point: the service entry sets a flag (preferred over heuristics). |

Container detection (`/.dockerenv`, `/run/.containerenv`, cgroup heuristics) is
**deliberately not needed**: under the policy below, a container resolves to
"console only" through the default arm, and the JSON format is an explicit
choice — so nothing depends on knowing "am I in Docker".

### 6.2 `auto` sink policy

`--log-sinks auto` (the default) resolves to:

| Environment | Sinks | Rationale |
|---|---|---|
| stderr is a TTY | `console` (text) | interactive use |
| stderr is the journal | `console` (text + `<N>` prefixes) | journald owns storage/rotation; adding syslog would **double-log** every record |
| Windows service context | `native` (Event Log) + `console` | the Event Log is the service-native trail; console keeps `sc`-attached debugging usable |
| anything else (redirect, pipe, container) | `console` (text) | the consumer (log driver, redirect target) owns storage; syslog here may write into a nonexistent `/dev/log` |

**Native and file sinks on POSIX are explicit opt-in** (`--log-sinks
console,native`, `--log-file …`) for non-systemd inits (SysV/OpenRC, launchd)
and users who want them. This keeps `auto` incapable of double-logging or
silently dropping records (§4, principle 4).

The audit trail is a separate subsystem, independent of `--log-sinks` and not
part of the sink set resolved here (§10).

## 7. Severity mappings

One table in `severity_mappings.{h,cpp}`, used by every backend and by the
journald `<N>` prefix:

| `Logging::Severity` | syslog / journald priority | Windows event type |
|---|---|---|
| `Trace` | 7 debug | Information |
| `Debug` | 7 debug | Information |
| `Info` | 6 info | Information |
| `Notify` | 5 notice | Information |
| `Warning` | 4 warning | Warning |
| `Error` | 3 err | Error |
| `Fatal` | 2 crit | Error |

Notes: `Notify` finally gets its natural syslog analogue (`notice`); nothing
maps to 0/1 (emerg/alert are system-wide conditions, not application ones);
`Fatal` maps to `crit`, not `emerg`. Expressed as `boost::log::sinks::syslog::custom_severity_mapping<Severity>`
and `event_log::custom_event_type_mapping<Severity>` keyed on the existing
`"Severity"` attribute.

## 8. Options surface

A new `logging` options area following the standard pattern
(`src/core/options/options_logging_options.{h,cpp}`, `tagLoggingSettings`,
registered in the `src/aqualink-automate.cpp` pipeline):

| Option (= config-file key) | Values | Default | Notes |
|---|---|---|---|
| `log-sinks` | `auto` or CSV of `console`, `native`, `file` | `auto` | `file` requires `log-file` (dependency check in `Validate()`) |
| `log-format` | `text`, `json` | `text` | applies to console and file sinks |
| `log-file` | path | *(unset)* | enables the file sink; implies `file` membership when `log-sinks=auto` |
| `log-file-max-size` | bytes | 10 MiB | rotation threshold |
| `log-file-max-files` | count | 5 | collector bound |
| `log-syslog-facility` | `daemon`, `user`, `local0`–`local7` | `daemon` | POSIX native sink only |

Validated types get `validate()` overloads under
`src/core/options/validators/` per the established pattern; conflicts (e.g.
`log-file-*` without `log-file`) are checked in `Validate()`.

### 8.1 Interaction with existing options

`--debug` / `--trace` / `--loglevel-<channel>` keep their meaning: they drive
the *filter* (`SeverityFiltering`), which remains sink-independent. Sinks decide
*where*, the filter decides *what*.

### 8.2 Per-sink thresholds — deliberately omitted

v1 has one global filter shared by console/native/file sinks; a per-sink
threshold (e.g. "Event Log gets Warning+ only") is a plausible v2, and the sink
filter lambda is where it would slot in. The audit sink is the exception and is
already channel-filtered.

### 8.3 Bootstrap ordering

Today `Logging::Initialise()` runs **before** options processing so option
errors are loggable. That stays: startup registers the `auto`-resolved console
sink immediately; after options are processed, a reconfiguration step applies
the resolved `SinkConfig` (adds native/file sinks, switches format). Console
records emitted between the two points use the default text format — acceptable,
and identical to how the profiler handles its post-options activation block.

## 9. Threading model

- **Console:** `synchronous_sink`, `auto_flush(true)` — unchanged.
- **Native + file:** `asynchronous_sink` with
  `bounded_fifo_queue<256, drop_on_overflow>` — a stalled Event Log RPC, DNS
  hiccup in a remote-syslog future, or slow disk must not block the kernel
  thread that is also servicing RS-485 (see `kernel-architecture`; the app is
  single-threaded cooperative). Drop-on-overflow is correct here: the console
  sink still records everything, and the audit trail's durability lives in the
  JSONL file, not the OS sink.
- **Shutdown:** `SinkRegistry::FlushAll()` runs `feed_records()`/`flush()` on
  the async frontends inside the ordered shutdown (before hub teardown), then
  `RemoveAll()`. Boost async sinks require an explicit stop before destruction —
  this is exactly the add-and-forget gap §3.2 calls out.

## 10. Audit trail policy

**Design principle: audit is a subsystem, not a log channel.** The
security audit trail (auth decisions, privileged actions on users/keys/sessions)
is a different *category* of data from operational logs — different retention,
access control, consumers (SIEM / incident response, not a developer tailing
`journalctl`), and integrity expectations. The industry standard is to keep the
two streams **separate** (NIST SP 800-92; PCI-DSS req. 10; CIS Controls), and on
POSIX the `LOG_AUTHPRIV` syslog facility exists precisely to route this class of
event to a restricted-permission file that can be forwarded off-box by facility.

The current code models audit incoherently — it is *both* a general
`Logging::Channel` riding the shared `LogX`/sink plumbing *and* a bespoke extra
sink (`RegisterAuditOsSink`). This redesign picks one lane: **audit owns its own
path and does not flow through the operational sinks at all.**

### 10.1 The three destinations, and which one is authoritative

`Auth::AuditLog::Record()` (the single existing entry point) drives all three;
it no longer calls the general `LogX` facade:

| Destination | Role | Integrity |
|---|---|---|
| **OS-native, forward-capable** — POSIX syslog `LOG_AUTHPRIV`, or a dedicated Windows Event Log source | **The authoritative copy.** Off-box forwarding (rsyslog/journald → SIEM) is the tamper-resistant path: the app cannot un-send a delivered record. | high (when forwarded) |
| **Local append-only JSONL** in the hardened state dir (`AuditLog::AppendJsonl`) | Convenience + availability: feeds the in-app audit viewer, survives a SIEM/network outage, and is the fallback when no OS sink registers. | **low** — the app's own uid can rewrite it; explicitly *not* the integrity anchor |
| **Dev echo to console** (opt-in, default **off**) | Local visibility during development / a container with no syslog + no SIEM. | n/a |

This corrects the earlier framing that called the JSONL "the durable trail." A
file the process can overwrite is not tamper-resistant against app compromise;
integrity comes from the forwarded copy, and the JSONL is demoted to
convenience/fallback. (Hash-chaining or WORM storage would raise local
integrity, but that is compliance machinery beyond this project's threat model —
see §12.)

### 10.2 Separation from operational logs

- Audit records **do not** go to the general console/file sinks. That kills the
  previous double-appearance under systemd (audit riding both console→journal
  *and* the syslog sink) and the information-hygiene leak of subject IDs / peer
  IPs into the general operational stream, which has looser access than
  `auth.log`.
- The audit OS sink is registered by the auth bootstrap whenever auth is
  enabled, **independent of `--log-sinks`** — audit routing is a security
  property, not a logging preference.
- Consequence for the `Channel` enum: `Audit` is **removed from the operational
  logging channels** (the sink layer never sees it), which makes the separation
  leak-proof by construction — a new operational sink cannot accidentally carry
  audit, because audit never enters the shared logging core as a normal channel.
  The optional dev echo, when enabled (its own toggle, e.g.
  `--audit-console-echo`, default off), is the only way audit text reaches the
  operational console.
- **Ripple of removing the enumerator** (mostly free because the per-channel
  loglevel options are data-driven off the enum via
  `magic_enum::enum_for_each<Logging::Channel>`): `--loglevel-audit` and its
  entry in `--replay-filename`'s dependency list **disappear automatically**;
  the hand-maintained lists need manual edits — `LOG_CHANNELS` in
  `playwright.config.ts` and the channel list in `docs/configuration.md` (which
  returns to 18 operational channels, with audit documented separately as a
  subsystem). Low-stakes: audit is unreleased (it arrives on `feat/auth-authz`),
  so `--loglevel-audit` never shipped. Note the ordering vs the do-now
  configuration.md fix (§16, cross-cutting): that fix makes the doc say **19**
  to match *today's* code; slice 1 then returns it to **18 + audit-subsystem**.
  Present-tense truth first, then the redesign updates it — sequential, not
  contradictory.

### 10.3 POSIX facility — `LOG_AUTHPRIV`, not `LOG_AUTH`

The audit sink uses the **private** auth facility: Boost's
`sinks::syslog::security1` (facility 10 on Linux = `LOG_AUTHPRIV`), **not**
`security0` (facility 4 = `LOG_AUTH`). `AUTHPRIV` is the modern choice for
records that carry identities: distros route it to a restricted-mode file
(`/var/log/auth.log` 0640 `root:adm` on Debian-family; `/var/log/secure` on
RHEL) rather than the world-adjacent `LOG_AUTH` destinations. (The Boost enum's
numeric value matches the Linux `LOG_AUTHPRIV` constant; verify per-platform
before relying on it on non-Linux POSIX.) With the explicit severity mapping
(§7), audit denials logged at `Notify`/`Warning` now carry `notice`/`warning`
priority instead of collapsing to `info`.

### 10.4 Windows — an honest asymmetry

The true platform analogue is the Windows **Security** event log, but writing to
it requires the process to be a registered LSA source — infeasible for an
ordinary service. So on Windows the audit trail uses a **dedicated Event Log
source in the Application log**, distinct from the operational source: separation
in name and queryability, but *not* the privileged Security log. This is a
known, documented limitation (see §11 for the install-time source registration),
not the equal of the POSIX `AUTHPRIV` story.

### 10.5 Which school this is

Two defensible models exist: the **classic syslog** approach (separate facility →
separate file → separate handling, above) and the **modern structured-events**
approach (one stream, tag audit with a field, route downstream in the pipeline).
This project chooses classic, because it delivers separation *by default* on any
systemd distro with **zero operator configuration** — the right default for a
self-hosted appliance where the operator may run neither a SIEM nor custom
rsyslog rules. An operator who prefers the structured-events model can still
forward-and-route by field; nothing here precludes it.

## 11. Packaging

- **Windows:** register the event source at install time — create
  `HKLM\SYSTEM\CurrentControlSet\Services\EventLog\Application\Aqualink-Automate`
  with `EventMessageFile` pointing at a shipped message resource and
  `TypesSupported = 0x07`. Without this, unprivileged runs get no Event Log at
  all (§3.3) and privileged runs render the "description not found" wrapper.
  The message resource is a minimal message-table DLL (or embedded in the exe)
  whose IDs match Boost's `simple_event_log_backend` insertion scheme. Owned by
  whatever Windows install artifact ships (currently none — this lands together
  with a future Windows service wrapper; until then the try/catch downgrade
  stands).
- **deb/rpm/systemd:** nothing to add — the unit file's journal routing is
  already correct, and the `<N>` prefixes need no packaging support.
- **Docker:** nothing to add — stdout/stderr semantics are unchanged;
  `--log-format json` is a user choice via CMD/args.

## 12. Non-goals

- **Replacing Boost.Log** (spdlog etc.) — everything needed is in the existing
  dependency.
- **A heavy abstraction over Boost.Log's sink API** — the registry is a thin
  owner; sinks are constructed with plain Boost types.
- **JSON console by default** — humans first; pipelines opt in.
- **Auto-detecting containers** — made unnecessary by the `auto` policy (§6.1).
- **Per-sink severity thresholds** (§8.2) and **remote syslog (RFC 3164 UDP)** —
  possible later; the backend supports it (`syslog::udp_socket_based`), but it
  needs the async frontend first and has no current demand.
- **Changing the audit JSONL *format*** — the on-disk record and its owner-only
  permissions are unchanged; §10 only reframes its *role* (convenience/fallback,
  not the integrity anchor) and stops audit flowing through the operational
  sinks.
- **Local audit integrity hardening** (hash-chaining, append-only/WORM media) —
  the forwarded copy is the integrity story (§10.1); local tamper-resistance is
  compliance machinery this threat model does not warrant.

## 13. Implementation slices

| Slice | Contents | Payoff |
|---|---|---|
| **1 — fidelity + structure** | `sinks/` module: `SinkRegistry`, severity-mapping tables, environment detection, journald `<N>` prefixes, native sink generalised out of `audit_log.cpp` (with explicit mappings + `daemon` general / `security1`=`LOG_AUTHPRIV` audit facilities), `Audit` removed from operational channels and given its own path (§10), async frontends, shutdown flush, `logging` options area (`log-sinks`, `log-syslog-facility`) | fixes every §3.1 fidelity bug; establishes the pattern; audit becomes a separate, forward-capable trail with correct priorities |
| **2 — file + json** | `log-file` (+ rotation options) via `text_file_backend`; `log-format json` for console and file | non-systemd installs and container pipelines |
| **3 — optional natives** | journald backend (`Findsystemd`, structured fields), macOS `os_log` backend, Windows event-source packaging + service wrapper defaulting | nice-to-have depth, each independently shippable |

Each slice ships with its tests (§14) and documentation updates (§15) in the
same change — a sink whose behaviour is untested or undocumented is not done.

## 14. Test strategy

Sinks are I/O boundaries, so the strategy is layered: pure logic gets exhaustive
unit tests, OS backends get intercepted at the nearest seam we control, and the
end-to-end layer exercises the *real process* writing to *real transports*
wherever the CI platform allows — with clean skips (not silent passes) where it
does not.

### 14.1 Unit tests (`test/unit/logging/` — new suite, needs a `CMakeLists.txt` entry; plus `test/unit/options/`)

Testability requirements this imposes on the design (build these in from the
start, do not retrofit):

- The console sink takes an injectable `std::ostream` (production passes
  `std::clog`; tests pass a `std::ostringstream`).
- `DetectLogEnvironment()` takes injectable environment/`fstat` providers;
  tests force TTY/journal/service states rather than detecting them.
- `SinkRegistry` supports full `RemoveAll()` teardown so suites are hermetic
  (generalising the handle-return pattern `RegisterAuditOsSink` uses today).

| Area | What is asserted |
|---|---|
| Severity mappings | Exhaustive over `Logging::Severity` via `magic_enum::enum_for_each` — a newly added severity fails the test instead of silently taking a backend default (the exact failure mode §3.1 describes). Both tables: syslog level and Windows event type. |
| Environment detection | `JOURNAL_STREAM` parsing (`dev:inode`, malformed values, unset), match/mismatch against injected `fstat` results, TTY and service flags; the full §6.2 `auto`-resolution truth table. |
| Console sink | Text format golden output; JSON format — every emitted line parses (nlohmann), field set matches §5.2, escaping of quotes/newlines/non-ASCII in messages; `<N>` prefix present exactly when journal-connected, correct N per severity, multi-line rule (first line only). |
| File sink | In a temp dir: rotation triggers at `log-file-max-size`; collector enforces `log-file-max-files`; text/json parity with console. |
| SinkRegistry | Add/flush/remove lifecycle; bootstrap-then-reconfigure (§8.3) does not drop or duplicate records; `FlushAll()` on an async frontend delivers every queued record before returning (emit N, flush, count N). |
| **Syslog on the wire** | The key trick: construct the syslog sink with `use_impl = udp_socket_based` targeting `127.0.0.1:<ephemeral>` where the test owns the UDP socket, and assert the RFC 3164 `PRI` field (`facility × 8 + severity`) for every severity and for both facilities (`daemon` general, `security1`=`LOG_AUTHPRIV` audit). This verifies the real Boost consume path and our mapping end-to-end, cross-platform (it is plain UDP — it runs on the Windows dev build too), without touching the system logger. |
| Audit policy | Audit trail registered whenever auth is on regardless of `log-sinks`; carries the `security1` (`LOG_AUTHPRIV`) facility (assert `PRI` via the UDP trick); and — the separation guarantee — an audit event does **not** appear on the general console/file sinks (emit an audit record with a `std::ostringstream` console sink installed, assert it is absent). Generalises the existing audit-sink test in `test/unit/auth/`. |
| Options | Parsing/validation of the `logging` area: CSV `log-sinks` values, `file`-requires-`log-file` dependency, `log-file-*`-without-`log-file` conflicts, facility enum validation. Config-file keys come free as option long names. |

Windows Event Log's `consume` path cannot run without a registered event source,
so at unit level only its mapping table is covered; the consume path is
integration-gated (§14.2).

### 14.2 Integration tests (environment-gated, skip cleanly when unavailable)

Gated with `boost::unit_test::precondition` (or a runtime skip that *reports*
skipped, never silently passes):

- **Windows Event Log round-trip** — requires elevation (HKLM registration,
  §3.3). Register the source, emit records at each severity through the real
  sink, read back via the Win32 `EvtQuery` API filtered on provider
  `Aqualink-Automate`, assert the `Level` of each. GitHub-hosted Windows
  runners execute elevated, so this runs in any future Windows CI leg; locally
  it runs in an admin shell and skips otherwise.
- **journald ingest (Linux)** — requires a running journald + permission to
  read it. Emit via the native-syslog audit path, then assert via
  `journalctl SYSLOG_IDENTIFIER=<test identifier> -o json` that `PRIORITY` and
  `SYSLOG_FACILITY` match. GitHub-hosted Ubuntu runners have journald;
  containers typically do not — hence the gate.

### 14.3 End-to-end (real process, real transports)

A new `e2e/logging.spec.ts` in the existing Playwright suite — no browser
needed; it uses the suite's app-launch conventions (replay fixture, the
`--replay-filename` dependency flag set from `playwright.config.ts`) but spawns
its own short-lived app processes and captures their stderr directly:

- **Default text format**: launch, capture stderr, assert startup records match
  the text format and severities/channels render.
- **JSON format**: launch with `--log-format json`, assert *every* stderr line
  parses as JSON with the §5.2 field set (a single non-JSON line is a failure —
  this is the guarantee container pipelines depend on).
- **journald prefixes** (Linux runners only): the spec `fstat`s the read end of
  the stderr pipe it created and sets `JOURNAL_STREAM=<dev>:<inode>` in the
  child's environment — both ends of a pipe share the inode, so the app's own
  `fstat(STDERR_FILENO)` matches and the real detection path (not a test flag)
  activates. Assert `<N>` prefixes with correct priorities.
- **File sink + shutdown flush**: launch with `--log-file` and a tiny
  `--log-file-max-size`, assert the file exists, rotates, and — after SIGTERM —
  contains the final records (this end-to-ends `SinkRegistry::FlushAll()` on
  the async frontend through the real ordered shutdown).
- Processes are stopped with **SIGTERM, never SIGKILL** — same rationale as the
  `gracefulShutdown` block in `playwright.config.ts`: the coverage e2e job
  (`automated-codescanning.yml`) only gets `.gcda` counters from a clean exit.

**CI wiring:**

- The `e2e` job in `ci.yml` (Linux install tree) picks the spec up
  automatically; the journald-prefix case runs for real there.
- The `docker-verify` job's *"Verify runtime starts (app)"* step gains two
  assertions: `docker logs` shows startup records (proves `auto_flush` through
  the container pipe end-to-end), and a second short run with
  `--log-format json` appended to CMD yields only parseable lines.
- Best-effort systemd leg (optional, Linux CI): `sudo systemd-run --wait -p
  StandardOutput=journal -p SyslogIdentifier=aqualink-e2e <exe> …` then assert
  priorities via `journalctl -o json`. Runner-fragile; implement with a
  hard-skip when `systemd-run` is unavailable and treat as advisory, not
  required.

### 14.4 Coverage-gate note

The SonarCloud new-code gate merges unit + Playwright-e2e coverage from the
Linux build. The sink module's POSIX arms are all reachable there: console/json/
file/registry via unit tests and `e2e/logging.spec.ts`, the syslog consume path
via the UDP unit test, journald prefixes via the pipe trick. The only
Linux-unreachable code is the Windows Event Log arm (`#if defined(_WIN32)`),
which is excluded from Linux coverage by preprocessing — so the gate sees no
uncovered new lines from it. Do not let the file/native sinks land ahead of
their tests: 8000-line coverage debts are how the PR #50 situation happened.

## 15. Documentation impact

Per the repo's doc-accuracy rules (a doc that contradicts the code is a
defect), each slice updates the docs it invalidates **in the same change**:

### 15.1 User-facing docs (published to the docs site by `docs.yml`)

| Doc | Update |
|---|---|
| `docs/configuration.md` | New "Logging" options section: the §8 table (`log-sinks`, `log-format`, `log-file`, `log-file-max-size`, `log-file-max-files`, `log-syslog-facility`), the `auto` policy table (§6.2), and interaction with `--debug`/`--trace`/`--loglevel-<channel>` (filter vs sink). **Pre-existing defect found while designing this** (fix independently — it does not wait for this feature): the channel list says "18 channels" and omits `audit` (there are 19), and the worked replay example omits `--loglevel-audit`, so copy-pasting it fails validation. |
| `docs/SECURITY.md` | Audit-trail section (§10): audit is a separate subsystem, not an operational log channel; the OS-native `LOG_AUTHPRIV` (`security1`) destination is the **authoritative, forward-capable** copy (events land in restricted `auth.log`/`secure` on POSIX), the local JSONL is convenience/fallback (owner-only, *not* the integrity anchor), audit does not flow to the operational sinks, and the Windows asymmetry (dedicated Application-log source, not the privileged Security log). |
| `docs/INSTALL.md` | "Viewing logs" notes: `journalctl -u aqualink-automate -p warning` now meaningful (slice 1); `docker logs` + `--log-format json` for container pipelines (slice 2). |
| `docs/raspberry-pi.md` | Check at implementation time — if it walks through log viewing, align the journalctl guidance. |
| `docs/profiling.md` | The doc-accuracy table maps the logging facade here; add a short "where records go" subsection linking to this design and the configuration reference. |
| `README.md` / `CHANGELOG.md` | CHANGELOG entry per slice; README only if it enumerates logging among features. |

Explicit non-changes, stated so they are checked rather than forgotten:
**`assets/web/api/swagger.yaml`** — no HTTP routes, schemas, or WS events change
in v1 (the diagnostics logging route is untouched); **i18n catalogs** — no
user-visible UI text changes in v1. Both become real work items only if a
"active sinks" card is ever added to the Diagnostics page (a possible v2, at
which point the swagger + `en.js`-plus-every-locale + pseudo-scan rules apply).

### 15.2 Repo / contributor docs

| Doc | Update |
|---|---|
| `.claude/skills/backend-observability/SKILL.md` | The checked-in skill documents the logging half of observability. Add: the sink layer (registry, sink kinds, `auto` policy), an "add a sink kind" checklist mirroring the existing "add a Channel" one, and new gotchas (every OS backend needs the explicit severity mapping; async sinks must be flushed via the registry before shutdown; console stream is injectable for tests). |
| `CLAUDE.md` doc-accuracy table | Extend the "Profiling/logging facade" row so sink/option changes also point at `docs/configuration.md` + `docs/SECURITY.md`. |
| `docs/logging-sinks-redesign.md` (this doc) | Reconcile + date per slice as it lands; it is a dated snapshot, not living truth. |
| `playwright.config.ts` | Comment block documents the launch flags — extend if the logging spec adds conventions others must follow (SIGTERM rule, JOURNAL_STREAM trick). |

Guard rail: the new `logging` options must **not** be added to
`--replay-filename`'s dependency list (`options_developer_options.cpp`) — that
list is per-channel log *levels*, not sinks; accidentally extending it breaks
every documented replay invocation and the e2e harness.

## 16. Work plan (phases, tasks, dependencies)

The three slices of §13 decompose into phases. Tests (§14) and docs (§15) are
**not** separate phases — each phase lands with its own tests and doc updates in
the same change, or it is not done. Ordering within a slice is by dependency,
not calendar.

### Slice 1 — fidelity + structure

The load-bearing slice: it fixes every §3.1 priority bug, establishes the
pattern, and separates the audit trail onto its own path (§10). Phases 1A and 1B
are pure and independent (build them first, in parallel); everything downstream
depends on them.

| Phase | Tasks | Depends on | Lands with |
|---|---|---|---|
| **1A — Severity mappings** | `severity_mappings.{h,cpp}`: `Severity → syslog::level`, `Severity → event_log::event_type`, and `Severity → <N>` priority (§7). Pure functions, no I/O. | — | Exhaustive `magic_enum` mapping unit test (both tables + `<N>`). |
| **1B — Environment detection** | `log_environment.{h,cpp}`: `isatty`, `JOURNAL_STREAM`+`fstat` match, service flag — all behind **injectable providers**. `ResolveAutoSinks()` implements the §6.2 truth table. | — | Unit tests: `JOURNAL_STREAM` parsing edge cases, match/mismatch, full auto-resolution table. |
| **1C — Console sink + registry** | `sink_console.{h,cpp}` (injectable `std::ostream`, extract existing `Formatter` as the text format, `<N>`-prefix wrapper); `sink_registry.{h,cpp}` (owns handles, `Add`/`FlushAll`/`RemoveAll`). Refactor `logging_initialise.cpp` to build the console sink via the registry; keep a no-arg overload for pre-options startup (§8.3). | 1A, 1B | Unit: console text golden, `<N>` present iff journal-connected + multi-line rule, registry add/flush/remove lifecycle, bootstrap-then-reconfigure keeps records. |
| **1D — Native sink + audit separation** | `sink_native.{h,cpp}`: POSIX `syslog_backend` + Windows `simple_event_log_backend` arms, each with the explicit mapping from 1A and a configurable facility (`daemon` general / `security1`=`LOG_AUTHPRIV` audit). Move `RegisterAuditOsSink`’s body here. **Remove `Audit` from the operational `Channel` enum** and reroute `AuditLog::Record()` off the `LogX` facade onto its own dedicated native sink + JSONL + optional off-by-default dev echo (§10). Async frontend (§9). | 1A, 1C | Unit: syslog **UDP-wire** PRI test (all severities × both facilities incl. `LOG_AUTHPRIV`), audit-separation test (audit absent from the general console sink). Integration (gated): Event Log `EvtQuery` round-trip, journald ingest. |
| **1E — Options + activation** | `options_logging_options.{h,cpp}` + `tagLoggingSettings` with `log-sinks` and `log-syslog-facility` (slice-1 subset). Wire into `aqualink-automate.cpp`: early `Initialise()` (console), then post-options reconfigure applying the resolved `SinkConfig`; register `FlushAll()`/`RemoveAll()` into the ordered shutdown. | 1C (registry), 1B (auto policy) | Unit: options parse/validate (CSV `log-sinks`, facility enum, config-file keys as long names). |
| **1F — E2E + slice-1 docs** | `e2e/logging.spec.ts`: text-format startup capture; journald-prefix case via the `JOURNAL_STREAM` pipe-inode trick (Linux). `docker-verify`: assert `docker logs` shows startup records. Docs: `configuration.md` logging section, `SECURITY.md` audit routing, `backend-observability` skill, `CLAUDE.md` doc-accuracy row. | 1C–1E | (is the test/doc phase) |

### Slice 2 — file + json

| Phase | Tasks | Depends on | Lands with |
|---|---|---|---|
| **2A — File sink** | `sink_file.{h,cpp}`: `text_file_backend` + `file::collector` (size rotation + kept-file bound), async frontend. Options `log-file`, `log-file-max-size`, `log-file-max-files`. | 1C, 1E | Unit (temp dir): rotation at max-size, collector enforces max-files. |
| **2B — JSON format** | JSON-lines formatter (nlohmann) shared by console + file; `log-format` option; `auto` treats `log-file` presence as implying the `file` sink. | 1C, 2A | Unit: every line parses, §5.2 field set, escaping (quote/newline/non-ASCII). |
| **2C — E2E + slice-2 docs** | `e2e/logging.spec.ts`: `--log-format json` every-line-parses; `--log-file` + tiny max-size rotates and survives SIGTERM flush. `docker-verify`: a second run with `--log-format json` yields only parseable lines. Docs: `INSTALL.md` container-pipeline + log-viewing notes, `configuration.md` format/file options. | 2A, 2B | (is the test/doc phase) |

### Slice 3 — optional natives (independent, lower priority)

Each is independently shippable and none blocks slices 1–2.

| Phase | Tasks | Depends on | Lands with |
|---|---|---|---|
| **3A — journald backend** | `sd_journal_send` backend with structured fields (`PRIORITY`, `SYSLOG_IDENTIFIER`, `AA_CHANNEL`, `CODE_FILE`, `CODE_LINE`), behind a `Findsystemd` CMake module (mirrors `FindVTune`/`FindUProf`). Optional upgrade over the `<N>`-prefix path. | 1D | journald-ingest integration test asserts the structured fields. |
| **3B — macOS `os_log`** | Native `os_log` backend replacing the syslog-shim arm on macOS. | 1D | Gated integration test (`log show`). |
| **3C — Windows event source + service** | Message-table resource + install-time HKLM registration (§11), sequenced with a Windows service wrapper that sets the service-context flag (§6.1). | 1D | Elevated Event Log round-trip becomes unconditional on Windows CI. |

### Cross-cutting, do-now (independent of the slices)

- **Fix the stale channel list in `configuration.md`** — says "18 channels",
  omits `audit` (there are 19), and the worked replay example omits
  `--loglevel-audit` so it fails copy-paste. Pre-existing defect surfaced during
  design; already tracked separately. Not gated on any slice.

### Decision gates

Two §18 open decisions gate specific phases and should be settled before they
start (neither blocks 1A–1C):

- **1D / 1E**: the general-sink syslog facility (`daemon` vs `user`) and the
  audit dev-echo option name (§18.1, §18.3; the systemd double-record question is
  now resolved by the audit separation, §18.2).
- **2B**: the JSON field schema, if a specific pipeline (Loki/CloudWatch) is the
  target.
- **3C**: predicated on a Windows service wrapper existing at all.

## 17. End-state matrix

Two mental models to correct first:

1. **Operational channels do not route to different sinks.** Each of the 18
   operational `Logging::Channel` values flows to *every active operational
   sink*, gated only by the per-channel severity filter (`SeverityFiltering`,
   unchanged). So the operational "sink × channel matrix" is a wall of *yes*;
   the real structure is in the environment and severity dimensions.
2. **Audit is not on that grid at all.** The audit trail (§10) is a separate
   subsystem with its own destinations (`LOG_AUTHPRIV` native + JSONL + opt-in
   dev echo); it does not appear on the operational sinks and is not a
   `Logging::Channel`. It is shown below as its own row precisely to make that
   separation visible.

### 17.1 Which sinks are live, and how each renders (environment × sink)

`--log-sinks auto` (default). Explicit `--log-sinks …` / `--log-file …` override
the operational console/native/file columns; the audit row is a separate
subsystem, independent of `--log-sinks` (§10).

| Environment (auto-detected) | console | native (syslog / Event Log) | file |
|---|---|---|---|
| Interactive TTY | ✅ text | — | opt-in (`--log-file`) |
| systemd service (`JOURNAL_STREAM` matches stderr) | ✅ text + `<N>` priority prefix | — *(syslog would double-log via the journal)* | opt-in |
| Windows service context | ✅ text | ✅ Event Log | opt-in |
| Container / pipe / redirect (default arm) | ✅ text *(or json via `--log-format`)* | — *(no syslogd; would drop silently)* | opt-in |
| **Audit trail** (separate subsystem, when auth on) | — *(opt-in dev echo only)* | ✅ dedicated native sink, `security1`/`LOG_AUTHPRIV` (POSIX) or dedicated Application-log source (Windows); the **authoritative, forward-capable** copy | JSONL always (owner-only, state dir; convenience/fallback, *not* the integrity anchor) |

The audit row is deliberately *not* additive to the operational sinks: audit no
longer flows to the general console/file (§10.2). Threading: console is
`synchronous_sink` + `auto_flush`; native and file are `asynchronous_sink`
(bounded, drop-on-overflow), flushed by `SinkRegistry::FlushAll()` on shutdown
(§9).

### 17.2 Severity → transport priority (fixed mapping, all backends)

| `Logging::Severity` | syslog / journald `<N>` | Windows event type |
|---|---|---|
| `Trace` | 7 debug | Information |
| `Debug` | 7 debug | Information |
| `Info` | 6 info | Information |
| `Notify` | 5 notice | Information |
| `Warning` | 4 warning | Warning |
| `Error` | 3 err | Error |
| `Fatal` | 2 crit | Error |

This is the whole point of slice 1: today every row collapses to *info* /
*Information* on the OS backends (§3.1).

### 17.3 Channel → sink (flow, not routing)

| Source | console | native (general) | native (audit) | file | JSONL |
|---|---|---|---|---|---|
| 18 operational channels — `Main`, `Web`, `Mqtt`, `Serial`, `Protocol`, `Messages`, `Devices`, `Equipment`, `Navigation`, `Scraping`, `Signals`, `Options`, `Platform`, `Certificates`, `Coroutines`, `Developer`, `Exceptions`, `Profiling` | ✅ | ✅ *(only if native opted in / Windows service)* | — | ✅ *(if `--log-file`)* | — |
| Audit subsystem (`AuditLog::Record`, not a `Channel`) | — *(opt-in dev echo)* | — | ✅ *(always, when auth on)* | — | ✅ *(always, when auth on)* |

The operational `native (general)` "only if …" reflects that this sink is opt-in
on POSIX and automatic only in the Windows-service arm. The two rows share no
destination: operational logs never reach the audit sink, and audit never
reaches the operational console/general-native/file sinks — the leak-proof
separation of §10.2. Operational rows remain subject to the severity filter
(these columns mean "eligible to reach this sink", not "bypasses filtering");
audit has no per-channel filter — it is always recorded when auth is on.

### 17.4 What a single record carries, end to end

`LogWarning(Channel::Web, "…")` in a systemd deployment, after slice 1:

1. Filter check: `SeverityFiltering::ShouldLog(Web, Warning)` → pass.
2. Console sink (text + journal-connected): emits `<4>00001234: <Warning> (Web) …`.
3. journald strips `<4>`, records `PRIORITY=4`, `SYSLOG_IDENTIFIER=aqualink-automate`.
4. `journalctl -u aqualink-automate -p warning` now returns it — the capability
   that does not exist today.

## 18. Open decisions

1. **General-sink syslog facility** — `daemon` proposed (current audit code
   uses `user`); confirm.
2. **Audit double-record under systemd** — **resolved** (§10.2): audit no longer
   flows to the operational console, so it is not double-recorded in the general
   journal stream. The audit trail reaches the journal *once*, via its own
   `LOG_AUTHPRIV` syslog sink, which journald tags with the auth facility so
   `journalctl SYSLOG_FACILITY=10` (and rsyslog `auth,authpriv.*`) isolate it —
   the separation the earlier "accepted double-record" note was paying for.
   Nothing left to confirm here beyond the facility choice below.
3. **Audit dev echo toggle** — the opt-in `--audit-console-echo` (§10.2, default
   off) is the only way audit reaches the operational console. Confirm the
   option name and that off-by-default is right (it is, for the separation
   guarantee; on is a dev convenience).
4. **`LOG_AUTHPRIV` on non-Linux POSIX** — Boost's `security1` numerically
   matches Linux `LOG_AUTHPRIV` (facility 10); on macOS/BSD verify the platform
   constant before relying on `auth.log`-style routing (§10.3).
5. **JSON schema** — the proposed field set (§5.2) is minimal; if a specific
   pipeline (Loki, CloudWatch) is the target, align field names before slice 2.
6. **Windows service wrapper** — slice 3 assumes one will exist; the Event Log
   default (§6.2) and the dedicated audit Application-log source (§10.4) are moot
   until then. Sequence it with, or after, that work.
