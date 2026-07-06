# Performance Profiling

Aqualink-Automate ships a single profiling facade with three interchangeable
backends:

| Backend | Vendor | How it's found | What it gives you |
|---------|--------|----------------|-------------------|
| **Tracy** | open source | vcpkg port `tracy` (always available in a profiling build) | live frame/zone/plot timeline, memory tracking, connect-on-demand |
| **Intel VTune** (ITT) | Intel | `find_package(VTune)` → `ittnotify.h` + `libittnotify` | ITT tasks (zones), frames, counters (plots), markers; full hotspot sampling on Intel CPUs |
| **AMD uProf** | AMD | `find_package(UProf)` → `AMDProfileController.h` (+ optional `AMDTActivityLogger.h`) | resume/pause control; ActivityLogger markers for zones/events when the component is present |

All instrumentation goes through two factories (see the `backend-observability`
skill for the call-site conventions):

- **Call-site API** — `Factory::ProfilingUnitFactory::Instance().CreateZone/CreateFrame/CreateDomain`.
- **Process API** — `Factory::ProfilerFactory::Instance().Get()->{PlotValue,Message,EmitFrameMark,AppInfo,SetThreadName,Resume,Pause}`.

When profiling is compiled out, or no backend is selected, both factories return
no-op objects — every call site stays valid and costs effectively nothing.

## Building a profiling-enabled binary

Profiling is **off** in the default `release`/`debug` presets (the shipped
binary carries no profiler hooks). Use a dedicated profiling preset:

```sh
cmake --preset config-windows-msvc-profiling      # or -linux-gcc-/-linux-llvm-/-windows-llvm-/-macos-llvm-
cmake --build  build/config-windows-msvc-profiling --target aqualink-automate
```

These presets set `ENABLE_PROFILING=ON`, build `RelWithDebInfo` (optimised +
symbols — the right shape for a profiler) and force sanitizers off
(`ENABLE_PROFILING` and `ENABLE_SANITIZERS` are mutually exclusive — a
configure-time `FATAL_ERROR`). To flip profiling on for an existing build tree
instead, reconfigure any preset with `-DENABLE_PROFILING=ON`.

### Network-hardened "diagnostic" preset

A profiling build links the Tracy client, whose server thread **listens on a TCP
data port and answers UDP discovery broadcasts** — i.e. it opens a profiler
endpoint reachable from the LAN, independent of `--profiler`. For a shipped
beta / field-diagnostic build that should be permanently attachable but **not**
LAN-exposed, use the `*-diagnostic` presets instead
(`config-windows-msvc-diagnostic`, `-linux-gcc-`, …):

```sh
cmake --preset config-windows-msvc-diagnostic
cmake --build  build/config-windows-msvc-diagnostic --target aqualink-automate
```

These are identical to the `*-profiling` presets except they build via a
`*-diagnostic` vcpkg triplet (`cmake/vcpkg/triplets/*-diagnostic.cmake`) that
compiles the Tracy client with **`TRACY_ONLY_LOCALHOST`** (the TCP data port
binds to loopback only — `TracySocket.cpp` skips `AI_PASSIVE`) and
**`TRACY_NO_BROADCAST`** (no UDP discovery beacon at all). These are compile-time
Tracy macros, **not** runtime env vars, so they must be baked into the prebuilt
TracyClient via the triplet — they cannot be enabled by the application or a
`-D` on our own target. A profiler then connects only over loopback (e.g. an
SSH tunnel: `ssh -L 8086:127.0.0.1:8086 host`).

Trade-off: the distinct triplet changes the vcpkg ABI, so the first configure
with a `*-diagnostic` preset rebuilds the dependency set into its own installed
tree. The always-on memory-hook cost (below) still applies — this is a
deliberate diagnostic build, not a default release.

**Tracy** needs nothing extra. **VTune** and **uProf** are compiled in only when
their SDKs are found:

- Intel VTune: install the standalone VTune Profiler or the oneAPI Base Toolkit,
  or point `VTUNE_PROFILER_DIR` at it (the finder also checks the default
  `C:\Program Files (x86)\Intel\oneAPI\vtune\latest` and `/opt/intel/oneapi/vtune/latest`).
- AMD uProf: install AMD uProf, or point `AMD_UPROF_SDK_PATH` at it (default
  `C:\Program Files\AMD\AMDuProf` / `/opt/AMDuProf_*`). The ActivityLogger
  component (zones/markers) is optional; without it uProf still provides
  resume/pause control.

The configure output prints whether each backend was found; the chosen backend
is also logged at startup (see the warning below).

## Selecting a backend at runtime

Pass `--profiler tracy|uprof|vtune` (case-insensitive). The selector is applied
*after* options are parsed, then capture is started and the trace is stamped
with the version string and the main thread name.

If you request a backend that was **not** compiled into this build, a warning is
logged on the `Profiling` channel and profiling stays disabled (it does not fall
back to a different backend):

```
[Profiling] Requested profiler 'VTune' is not available in this build; profiling is disabled. Compiled-in backends: [Tracy]
```

## Runtime control surface

`GET/POST /api/diagnostics/profiling` (and the **Profiling** card on the
Diagnostics page) report and control the profiler at runtime:

- `GET` → `{ "enabled": bool, "running": bool, "backend": "tracy", "available": ["tracy","vtune"] }`
- `POST {"action":"start"}` / `{"action":"stop"}` → resume / pause capture
- `POST {"action":"select","backend":"vtune"}` → switch the active backend (only backends compiled in)

`enabled` reflects whether any backend was compiled in; `running` reflects
resumed-vs-paused. Pause/resume map to each backend's native gating
(`__itt_pause`/`__itt_resume` for VTune, `amdProfilePause`/`amdProfileResume` for
uProf); Tracy is client-driven so they are no-ops for it.

## What is instrumented

- **Main loop** — one `MainLoop` frame mark per iteration, with the iteration
  decomposed into zones: `main -> io_poll`, `-> protocol_poll`, `-> watchdogs`,
  `-> http_poll`, `-> https_poll`, `-> mqtt_poll`, `-> io_poll_drain`. The
  per-frame Asio handler count is plotted as `Main Loop Handlers`.
- **Protocol / serial** — `ProtocolTask::Poll`, read/write primitives, the Jandy
  message generator phases, and many serial-port operations; plots for serial
  bytes, queue depth, parse/error counters, message-processing time.
- **Devices** — every Jandy/Pentair device message-slot handler.
- **Navigation** — `Navigator::OnPageUpdate`/`ComputePath`,
  `MenuModel::DetectPage`/`FindPath`, `SpiderEngine::ProcessStep`.
- **State hubs** — `DataHub::EmitTemperatureEvent`/`EmitChemistryEvent` (the
  signals2 listener fan-out), `EquipmentHub::AddEquipment`/`AddDevice`, plus
  temperature/chemistry/`Active Devices` plots.
- **Web / MQTT** — HTTP route handlers, JSON generation, MQTT hub publish paths,
  connection/queue plots.

## Tracy workflow

The Tracy **server tools** (the `tracy-profiler` GUI plus the `tracy-capture` /
`tracy-csvexport` / `tracy-import-*` CLIs) are a developer-side dependency and
are **not** part of the build — the application only links the Tracy *client*
(via the vcpkg `tracy` port). Download the matching prebuilt Windows tools from
the upstream release that matches the client version — for the pinned
**v0.13.1**, <https://github.com/wolfpld/tracy/releases/tag/v0.13.1> — or build
them from that tag, and put them anywhere on your `PATH`. They were previously
committed under `cmake/tools/tracy-0.13.1/`; that copy was removed (unauditable
committed binaries), so fetch your own and keep it in step with the port version
above.

1. Build & run a profiling binary with `--profiler tracy`.
2. Open the Tracy profiler GUI and connect (the client streams on demand — the
   vcpkg port uses the `on-demand` feature, so capture begins when the GUI
   attaches).
3. Frames appear under `MainLoop`; expand a frame to see the per-subsystem
   zones; plots show the queue depths / counters listed above. Memory allocation
   tracking is wired via global `operator new`/`delete` overrides.

## VTune workflow (incl. AMD hosts)

The ITT instrumentation API (tasks/frames/counters/markers) is **CPU-agnostic**
and is collected in VTune's software/user-mode collection, so it works on AMD
CPUs too — only PMU **hardware-event sampling** requires an Intel CPU.

```sh
# software-mode collection that captures the ITT API timeline
vtune -collect hotspots -knob sampling-mode=sw -- ./aqualink-automate --profiler vtune ...
```

Open the result in `vtune-gui`; the `AqualinkAutomate` ITT domain shows the
zones (tasks), the `MainLoop` frames, and the counters from `PlotValue`.

## uProf workflow

Build with the uProf SDK present and run with `--profiler uprof`. uProf controls
collection via resume/pause (`amdProfileResume`/`amdProfilePause`), driven at
startup and by the runtime control surface above. When the ActivityLogger
component is installed, zones and events also appear as markers; otherwise uProf
provides sampling with resume/pause gating only.
