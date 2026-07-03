# Platform Isolation — No OS Macros in Shared Code

> **Principle.** OS-specific preprocessor conditionals are a defect in shared code.
> In this codebase **the operating system is a CMake decision, not a preprocessor
> decision.** OS-divergent behaviour is expressed as *one shared interface header*
> plus *one implementation `.cpp` per OS* under `src/core/platform/<os>/`, and
> `src/core/CMakeLists.txt` selects which of those `.cpp` files compile via plain
> `if(WIN32)` / `if(LINUX)` / `if(APPLE)` `target_sources()` blocks. Any shared,
> non-platform source file — anything outside `src/core/platform/**` and the
> third-party trees — **must be free of OS macros** (`_WIN32`, `__APPLE__`,
> `__linux__`, `__unix__`, `__MACH__`, `_WIN64`, `__ANDROID__`, `__FreeBSD__`, …).
> In the ideal state a line such as `#elif !defined(__APPLE__)` appears **nowhere**
> in the tree.

This is a principle for **both AI agents and human contributors**. It is enforced
by CI (see [Enforcement](#enforcement)) — a newly introduced OS `#if` in shared
code turns the build red.

OS macros are the strongest instance of a broader rule; the [next section](#the-preprocessor-is-the-last-resort)
generalizes it. Compiler, CPU-architecture, and build-feature macros are a
*separate* concern from OS selection and are permitted under a narrow bar — see
[Allowed exceptions](#allowed-exceptions) — but even they should prefer a runtime
seam where one is feasible.

## The preprocessor is the last resort

The deeper goal is to **eliminate conditional-compilation `#if` blocks wherever
possible**, not just OS ones. Variation in behaviour should be resolved by the build
graph or at runtime — not by the preprocessor carving different code out of one
file. Prefer, in this order:

1. **Runtime polymorphism / factory / registry (most preferred).** One code path,
   compiled everywhere; behaviour is chosen at runtime by injecting an interface
   implementation, registering into a factory/registry, or probing a capability at
   runtime. Use this whenever the variation is a *backend* or *capability* that can
   coexist in one binary. Established examples in this tree:
   - `Logging::Sinks::SinkRegistry` + the `Make*Sink()` factories — sinks are chosen
     at runtime from the resolved policy, and **journald availability is probed at
     runtime via `dlopen`** (no build-time libsystemd dependency, no `#if` on
     whether systemd is present).
   - `ProfilingUnitFactory` with a **no-op fallback** — a zone/frame object is always
     returnable; when no profiler backend is active the factory hands back a no-op,
     so call sites need no `#ifdef` to *use* profiling.
   - `Protocol::MessageGeneratorRegistry` — Pentair registers at priority 0 ahead of
     Jandy; protocol selection is a runtime registry, not `#if PENTAIR`.
2. **CMake source selection.** For divergence that genuinely cannot share a
   translation unit — an OS API or a symbol/header absent on other platforms — put
   one `.cpp` per variant under `src/core/platform/<os>/` and let
   `if(WIN32)/if(LINUX)/if(APPLE)` pick it. This is the OS rule above. No `#ifdef`.
3. **Preprocessor `#if` (least preferred — last resort).** Justified only when
   neither of the above can work:
   - the code names a symbol/header that **does not exist** on the target, so the
     other arm literally cannot compile (this is the residual reason a whole
     platform `.cpp` may still open with a guard), **or**
   - the feature must expand to *nothing* for **zero measured overhead when disabled**
     (e.g. Tracy instrumentation macros) and no runtime seam can match that.

   Even then: confine the `#if` to the **smallest possible file** — ideally a single
   platform or feature translation unit — never threaded through shared logic, and
   never as a negative-OS `#else`.

The test to apply before writing any `#if`: *"Could a factory, a registry, an
injected interface, or a CMake source choice express this instead?"* If yes, do
that. A `#if` that survives this question is the exception, and it should be able to
say which of the two last-resort reasons above applies.

---

## Why

Four forces make CMake, not the preprocessor, the single source of OS truth.

1. **Correctness under real compilers.** An inline `#ifdef _WIN32` branch means the
   non-Windows arm is *never seen by MSVC* and the Windows arm is *never seen by
   GCC/Clang* — each compiler silently ignores half the code. That is exactly the
   class of MSVC-tolerated UB this project has repeatedly caught only on GCC/glibc
   (nlohmann brace-init decaying to an array, `.items()`-on-a-temporary
   use-after-free, the Boost.Log syslog teardown heap corruption). A per-OS `.cpp`
   is compiled **in full** by the toolchain that owns that OS, so both arms get real
   diagnostics.

2. **Single responsibility.** A file that calls `gmtime_r` / `localtime_r` /
   `ioctl(TIOCGWINSZ)` unbranched is about *one* platform and is trivially
   reviewable. A file laced with negative-OS `#elif`s (e.g. "not Apple ⇒ assume
   Linux") silently mislabels or miscompiles the next port (BSD, another Unix) with
   no failing test.

3. **Single source of OS truth.** When the OS→source mapping lives entirely in the
   CMake `if()` blocks, adding or auditing platform support is *one file to read*.
   Scattering it across `#ifdef`s in shared `.cpp` files makes the real platform
   matrix unknowable without grepping the whole tree.

4. **Testability & readability.** Platform seams behind a shared header (`SafeCtime`,
   `get_terminal_column_width`, the serial `*_timeout` methods, the logging native
   sinks) can be swapped, stubbed, and reasoned about. Buried `#ifdef`s cannot.

The mechanism already exists and is proven. The work is to **conform the last
stragglers to it**, not to invent anything.

---

## The mechanism

### OS-section directories

| Directory | Role |
|---|---|
| `src/core/platform/windows/` | Windows implementations. Also the home of Windows-only functions with no POSIX counterpart (e.g. `uac_elevation.cpp`). |
| `src/core/platform/posix/` | The **shared Unix** implementation, reused by **both** Linux and macOS. |
| `src/core/platform/linux/` | Leaf dir — **only** files where the single Unix impl genuinely diverges on Linux. |
| `src/core/platform/macos/` | Leaf dir — **only** files where the single Unix impl genuinely diverges on macOS. |

Reach for a leaf `linux/` or `macos/` file **only** when `posix/` cannot be shared.
Today the canonical divergence example is `physical_serial_port_timeout.cpp`
(termios vs IOKit) and the logging sinks (`native_log_sink.cpp`,
`journald_log_sink.cpp` differ per Unix).

### CMake source selection

`src/core/CMakeLists.txt` lists the OS-agnostic sources first in one
`target_sources(... PRIVATE ...)` block, then appends platform sources in three
sibling blocks. **No `$<PLATFORM_ID>` / `$<CXX_COMPILER_ID>` generator expressions**
— plain `if()` only:

```cmake
if(WIN32)
target_sources(libaqualink-automate PRIVATE
    platform/windows/safe_ctime.cpp
    platform/windows/get_terminal_column_width.cpp
    platform/windows/native_log_sink.cpp
    ...)
# Windows-only defines and link libs are scoped INSIDE this block, never in the
# shared unconditional target_compile_definitions() below:
target_compile_definitions(libaqualink-automate PUBLIC -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN ...)
target_link_libraries(libaqualink-automate PUBLIC comsuppw ole32 oleaut32)
endif(WIN32)

if(LINUX)
target_sources(libaqualink-automate PRIVATE
    platform/posix/safe_ctime.cpp            # <-- shared Unix impl
    platform/posix/get_terminal_column_width.cpp
    platform/linux/physical_serial_port_timeout.cpp   # <-- leaf: genuinely Linux-specific
    ...)
endif(LINUX)

if(APPLE)
target_sources(libaqualink-automate PRIVATE
    platform/posix/safe_ctime.cpp            # <-- SAME shared Unix impl as Linux
    platform/posix/get_terminal_column_width.cpp
    platform/macos/physical_serial_port_timeout.cpp   # <-- leaf: genuinely macOS-specific
    ...)
endif(APPLE)
```

Note the Linux and Apple blocks **both** list the same `platform/posix/<name>.cpp`,
and each *additionally* lists its leaf-dir file when one exists.

### Compiler / arch / stdlib concerns live in toolchains

Compiler discovery, linker flags, stdlib selection, per-arch flags, and platform
link libs belong in `cmake/toolchains/{platform}.{compiler}.toolchain.cmake`
(and vcpkg triplets in `cmake/vcpkg/triplets/`) — **never** in `src/` CMakeLists,
which must stay platform-independent. The sole in-`src` exception is genuinely
OS-API-tied defines and link libs scoped *inside* the `if(WIN32)` block.

---

## Rules

**Do**

- **Declare** the OS-agnostic interface in a shared header — either
  `src/core/platform/<name>.h` (like `safe_ctime.h`) or the owning subsystem's own
  header — in `namespace AqualinkAutomate::Platform` or the subsystem namespace.
- **Implement** one `.cpp` per OS: `platform/windows/<name>.cpp` for Windows and
  `platform/posix/<name>.cpp` as the shared Unix impl. Add a leaf
  `platform/linux/` or `platform/macos/` file **only** on genuine divergence.
- **Wire** every new `.cpp` into the correct `if(WIN32)`/`if(LINUX)`/`if(APPLE)`
  block. Remember the Linux and Apple blocks both list the same `posix/` file.
- **Reuse an existing seam** before inventing a file. Adding `SafeGmTime` to
  `safe_ctime.h` needs **zero** CMake change — `safe_ctime.cpp` is already compiled
  into all three blocks.

**Don't**

- **Don't** add an OS `#if`/`#ifdef`/`#elif` (`_WIN32`, `__APPLE__`, `__linux__`,
  `__unix__`, `__MACH__`, `_WIN64`, `__ANDROID__`, …) to any shared, non-platform
  `.cpp`/`.h`. If you catch yourself writing one in shared code, stop and extract
  the divergence behind a header + per-OS `.cpp`.
- **Don't** use a negative-OS test (`#else` / `#elif !defined(...)`) as a catch-all
  for "some other Unix". The `#else` that means "assume Linux" is the worst-offender
  pattern and is **banned** — name each OS explicitly via its own CMake block.
- **Don't** put `$<PLATFORM_ID>` / `$<CXX_COMPILER_ID>` in `src/` CMakeLists.
- **Don't** put compiler/arch/stdlib/linker concerns in shared `.cpp` files — those
  belong in the toolchain files.

---

## Worked examples

**Reusing a seam (zero CMake change).** To format a UTC timestamp, don't write
`#ifdef _WIN32 gmtime_s … #else gmtime_r`. Add
`bool SafeGmTime(std::tm& out, const std::time_t& t);` to `platform/safe_ctime.h`,
implement `gmtime_s` in `platform/windows/safe_ctime.cpp` and `gmtime_r` in
`platform/posix/safe_ctime.cpp`, and call `Platform::SafeGmTime(...)`. No
CMakeLists edit — those files already compile everywhere.

**Reference implementation — the logging native sinks.** The OS-native log sinks
(`MakeNativeSink`, `MakeJournaldSink`) are the model to copy. The header
`logging/sinks/sink_native.h` declares an OS-agnostic factory over an OS-neutral
`NativeSinkConfig` (carrying both a POSIX `SyslogFacility` and a
`WindowsEventSource`). The implementations live in
`platform/windows/native_log_sink.cpp` (Event Log),
`platform/{linux,macos}/native_log_sink.cpp` (syslog / os_log), each CMake-selected
— **no `#if` in any of them.** This replaced an earlier single
`sink_native.cpp` that switched backends with nested
`#if defined(_WIN32)` / `#elif !defined(__APPLE__)` / `#if defined(__APPLE__)` —
the precise anti-pattern this principle exists to prevent.

---

## Allowed exceptions

These macro classes are **not** OS selection and may remain, under a narrow bar.
The enforcement check deliberately does **not** flag them.

| Class | Examples | Bar for use |
|---|---|---|
| **Compiler** | `_MSC_VER`, `__GNUC__`, `__clang__` | Only for genuinely toolchain-specific concerns that cannot be a source-file choice — e.g. `#pragma warning(disable: 26449)` PREfast suppressions in `routing/node.h`. Must gate a diagnostic/pragma or an intrinsic-availability shim, never OS behaviour (clang-cl also defines `_MSC_VER`). Prefer a small compiler-shim header over spreading raw guards. |
| **Architecture** | `__x86_64__`, `__i386__`, `__aarch64__` | ISA-tied intrinsic selection — e.g. `<intrin.h>` vs `<immintrin.h>` and the `_mm_pause()` spin-hint in `tracy_memory.cpp`. Must be about the CPU/ISA, not the OS; ideally consolidated behind a cross-compiler shim (e.g. `CpuPause()`). |
| **Build-feature** | `TRACY_ENABLE`, `TRACY_ON_DEMAND`, `ENABLE_PROFILING`, sanitizer/coverage toggles | Selects an optional build configuration set by a CMake option, not a platform. Whole-TU feature gates like `#if defined(TRACY_ENABLE)` are fine. When a platform `.cpp` is extracted from such a TU, the new `.cpp` must carry the same feature guard. |

---

## Enforcement

A CI job (and a mirrored `scripts/check-os-macros.ps1` for local runs) fails the
build when an OS macro appears in a preprocessor directive in any non-exempt source
file. It scans `#if`/`#ifdef`/`#ifndef`/`#elif` lines for **OS tokens only** — it
does *not* flag the allowed compiler/arch/feature classes.

```bash
rg -n --pcre2 \
  -g '!src/core/platform/**' -g '!deps/**' -g '!third_party/**' \
  -g '!**/vcpkg_installed/**' -g '!build*/**' -g '!cmake/toolchains/**' \
  -g '*.h' -g '*.hpp' -g '*.cpp' -g '*.cc' \
  -e '^\s*#\s*(if|ifdef|ifndef|elif)\b.*\b(_WIN32|_WIN64|WIN32|__APPLE__|__MACH__|__linux__|__unix__|__ANDROID__|__FreeBSD__|__NetBSD__|__OpenBSD__)\b' \
  src/
```

Any match is a violation (exit non-zero, print `file:line`). Wire it as a required
check on PRs to `main`/`develop`.

**Exempt paths** (matches allowed, not scanned): `src/core/platform/**` (the
OS-section dirs — though even there, prefer none), `cmake/toolchains/**`, `deps/**`,
`third_party/**`, `**/vcpkg_installed/**`, `build*/**`.

---

## Remediation log

Every OS macro previously living in shared code has been migrated onto the
mechanism above; `scripts/check-os-macros.ps1` now passes (959 files scanned) and
CI keeps it that way. (The logging native-sink split was done separately — see the
worked example.)

- [x] **`src/core/http/webroute_version.cpp`** — `#ifdef _WIN32` (`gmtime_s` vs
  `gmtime_r`) → `Platform::SafeGmTime` on the existing `safe_ctime.h` seam (no CMake
  change).
- [x] **`src/core/scheduling/scheduler_service.cpp`** — `#ifdef _WIN32`
  (`localtime_s` vs `localtime_r`) → `Platform::SafeLocalTime` on `safe_ctime.h`
  (no CMake change).
- [x] **`src/core/platform/posix/get_terminal_column_width.cpp`** — banned
  `#if defined(__APPLE__)/#else` "macOS:"/"LINUX:" prefix → `Utility::PLATFORM_LOG_PREFIX`
  declared in the header, defined per-OS in leaf `platform/linux/` +
  `platform/macos/get_terminal_column_width_prefix.cpp`.
- [x] **`src/core/logging/sinks/log_environment.cpp`** — three `#if defined(_WIN32)`
  guards → `PlatformStderrIsTty`/`PlatformStatStderr` behind
  `log_environment_probes.h` with `platform/windows/` + `platform/posix/` impls.
- [x] **`src/core/profiling/memory/tracy_memory.cpp`** — `#ifdef _WIN32`
  aligned-alloc cluster → `Platform::AlignedMalloc`/`AlignedFree` behind
  `platform/aligned_alloc.h` with `platform/windows/` + `platform/posix/aligned_alloc.cpp`.
  The `_MSC_VER`/arch conditionals in the same file are allowed exceptions and stay.
- [x] **`src/core/options/options_app_options.cpp`** — `#if defined(_WIN32)` around
  the log-source-registration CLI action (caught by the new check, not the initial
  inventory) → `LogSourceRegistrationResult` tri-state; Windows registers, a
  `platform/posix/log_source_registration.cpp` stub returns `Unsupported`, and the
  shared handler branches on the result.
