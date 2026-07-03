---
name: cmake-build-system
description: CMake build system, toolchains, vcpkg, and presets for the Aqualink-Automate project. Use when editing any CMakeLists.txt, adding source files or new targets/libraries, adding or upgrading vcpkg dependencies, changing compiler/linker/sanitizer/coverage flags, working with toolchain or triplet files, generating version constants, or configuring/building/running/testing locally (including the Windows VS Dev Shell requirement and recovering from a stale toolset). Enforces the platform-isolation rule.
---

# CMake / Build System — Conventions

C++23, CMake 3.31+, Ninja, single-config builds driven **entirely by presets**. vcpkg is vendored as a git submodule at `deps/vcpkg` (manifest mode). All deps resolve through vcpkg (`deps/CMakeLists.txt` is an empty FetchContent placeholder).

## 0. THE rule: platform isolation

**`CMakeLists.txt` files under `src/` and `test/` are platform- and compiler-independent.** No `$<PLATFORM_ID:...>`, `$<C_COMPILER_ID:...>`, or `$<CXX_COMPILER_ID:...>` in them — verified: zero matches across `src/` and `test/`.

Where build logic must branch:
- **Compiler / linker / stdlib decisions → toolchain files** (`cmake/toolchains/`).
- **Dependency build settings (linkage, CRT, build-type, platform toolset) → vcpkg triplets** (`cmake/vcpkg/triplets/`).
- **Host-OS source differences → plain `if(WIN32)/if(LINUX)/if(APPLE)`** blocks with extra `target_sources(... platform/<os>/...)` (e.g. `src/core/CMakeLists.txt`). This is allowed; compiler-flag conditionals are not.
- **Project-wide defines/features → `target_compile_definitions(libaqualink-automate PUBLIC ...)`**, gated by `option()`/`*_FOUND`, never by compiler id.

If you're tempted to add a compiler/platform conditional to a `src/` CMakeLists, it belongs in a toolchain or triplet instead.

**The corollary — no OS macros in shared *source* code.** The OS is chosen by CMake source-selection, so OS divergence must NOT reappear as a preprocessor `#ifdef` in a shared `.cpp`/`.h`. OS-specific code lives in `src/core/platform/<os>/` behind a shared header; the `if(WIN32)/if(LINUX)/if(APPLE)` blocks pick the impl. A `#elif !defined(__APPLE__)` in shared code is a defect. Full reference: **`docs/platform-isolation.md`**.

- **`platform/posix/` is the shared Unix impl** — list it in *both* the `if(LINUX)` and `if(APPLE)` blocks. Leaf `platform/linux/` / `platform/macos/` files exist **only** on genuine per-Unix divergence (e.g. `physical_serial_port_timeout.cpp`, the native log sinks).
- **Windows-only defines/link-libs** (`-D_WIN32_WINNT=…`, `comsuppw`/`ole32`) stay scoped *inside* the `if(WIN32)` block, never the shared unconditional `target_compile_definitions()`.
- **Reusing a seam is free:** adding `SafeGmTime`/`SafeLocalTime` to `platform/safe_ctime.h` needs **zero** CMake change (`safe_ctime.cpp` already compiles in all three blocks). Adding a genuinely-divergent function needs a new leaf `.cpp` added to the matching block.
- **Allowed exceptions** (NOT OS selection, so permitted): compiler macros (`_MSC_VER` — pragmas/intrinsics), arch macros (`__x86_64__` — ISA intrinsics), build-feature macros (`TRACY_ENABLE` — CMake `option()` switches). The whole-file `#if defined(TRACY_ENABLE)` guard is fine; when extracting a platform `.cpp` from such a TU, carry the same feature guard (or add it to CMake only when the feature is on).
- **Enforcement:** `scripts/check-os-macros.ps1` (CI job **Platform Macros**, a required check) greps `#if`/`#ifdef`/`#ifndef`/`#elif` lines for OS tokens (`_WIN32`, `__APPLE__`, `__linux__`, …) across `src/`, exempting `src/core/platform/**`, `cmake/**`, `deps/**`, vendored trees. It does NOT flag compiler/arch/feature macros. Run locally: `pwsh scripts/check-os-macros.ps1 -Root .`.

## 1. Root `CMakeLists.txt`

- `cmake_minimum_required(VERSION 3.31)`. Global: `CMAKE_CXX_STANDARD 23`, `CXX_EXTENSIONS OFF`, `CMAKE_COMPILE_WARNING_AS_ERROR ON`, `CMAKE_POSITION_INDEPENDENT_CODE ON`, `CMAKE_EXPORT_COMPILE_COMMANDS ON`, `CMAKE_CXX_SCAN_FOR_MODULES OFF` (headers, not modules; keeps clang-tidy working).
- `include(GitVersionDerivation)` runs **before** `project()`; the derived semver feeds `project(VERSION ...)`.
- `ENABLE_*` `option()`s are declared **before** `project()` (so preset cache values win): `ENABLE_{BENCHMARKS,COVERAGE,TESTS,PROFILING,CLANG_TIDY,PVS_STUDIO,ASIO_TRACKING,SANITIZERS}`.
- `ENABLE_SANITIZERS` + `ENABLE_PROFILING` together → `FATAL_ERROR` (Tracy is incompatible with sanitizer runtimes).
- Order: `add_subdirectory(deps)` → `find_package`s → coverage/profiling/sanitizer setup → version templating → `add_subdirectory(src)` → static analysis → `add_subdirectory(test)` → `target_enable_sanitizers(...)` → `include(CPackConfig)`.

## 2. Toolchains (`cmake/toolchains/*.toolchain.cmake`)

Selected via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` in each configure preset; mirrored by a triplet.

| Toolchain | Compiler discovery | Notable flags / specifics |
|---|---|---|
| `linux.gcc` | `find_program` gcc/g++ `gcc-15…10`, REQUIRED | `-m64`, coloured diagnostics, `add_link_options(-pthread)` |
| `linux.llvm` | `find_program` clang `clang-21…15`; finds `ld.lld-*` | `-fuse-ld=lld` if found |
| `macos.llvm` | `find_program` clang (Homebrew/MacPorts) | `-arch arm64/x86_64`; **uses LLVM's own libc++** (`-nostdinc++ -isystem`), `CMAKE_OSX_DEPLOYMENT_TARGET 13.3` (for `std::to_chars`); LLD intentionally avoided |
| `windows.msvc` | **none** — lets vcpkg do standard MSVC detection | only `/utf-8` and `/bigobj`. "Minimal customization" by design — relies on an active VS environment |
| `windows.llvm` | `find_program clang-cl.exe`, REQUIRED | uses MSVC **`link.exe`** (not lld-link) found via `$ENV{VCToolsInstallDir}`; forces `/MD` (release CRT) for **all** configs incl. Debug, because ASan needs `/MD` not `/MDd` |

To change a compiler/linker/stdlib flag for a platform, edit the matching toolchain — **not** a `src/` CMakeLists.

## 3. vcpkg

- **Manifest**: `vcpkg.json` (root) — flat `dependencies` array; features use the object form `{ "name": "tracy", "features": ["on-demand"] }`. Pinning is via the `deps/vcpkg` submodule commit (no `builtin-baseline`/`overrides`).
- **Triplets** `cmake/vcpkg/triplets/*.cmake` — wired by `VCPKG_OVERLAY_TRIPLETS` (in the `core` base preset). Each sets arch/system/linkage/env-passthrough and chainloads its toolchain. `x64-windows-msvc` uses `VCPKG_PLATFORM_TOOLSET v143` and does **not** chainload (stock MSVC). `x64-windows-llvm` sets `VCPKG_BUILD_TYPE release` (release-only deps — a deliberate `/MD` vs `/MDd` mismatch avoidance; don't "fix" it).
- **Declare → consume**: add to `vcpkg.json` → `find_package(<Pkg> REQUIRED)` in root → in the consuming lib add `${<pkg>_INCLUDE_DIRS}` under `target_include_directories(... SYSTEM PUBLIC ...)` (SYSTEM so warnings-as-errors don't fire on third-party headers) and the imported target to `target_link_libraries`. Example: `nlohmann-json` → `find_package(nlohmann_json REQUIRED)` → `nlohmann_json::nlohmann_json`.
- `cmake/vcpkg/overlays/` is reserved (empty placeholder), not yet wired.

## 4. Presets (`CMakePresets.json`, v9)

- `binaryDir` is always `build/${presetName}`; install `install/${presetName}`.
- Hidden bases: **`core`** (Ninja, vcpkg toolchain, overlay triplets, default ENABLE_* flags, `VCPKG_KEEP_ENV_VARS`), **`debug`** (Debug + sanitizers ON + clang-tidy ON + profiling OFF), **`release`** (Release).
- Per-platform configure presets: `config-<os>-<compiler>[-debug|-coverage]` — e.g. `config-windows-msvc`, `config-windows-msvc-debug`, `config-linux-gcc`, `config-macos-llvm`, `config-windows-msvc-coverage`. Each sets `VCPKG_TARGET_TRIPLET` + `VCPKG_CHAINLOAD_TOOLCHAIN_FILE` and a host `condition`. (Debug variants inline their cache vars rather than inheriting `debug`.)
- `build-<...>` and `test-<...>` presets are 1:1 with the configure presets. `workflowPresets` (`ci-<os>-<compiler>`) chain configure→build→test→package; `packagePresets` (`pack-<...>`) set per-OS generators.

## 5. Targets & sources

| Target | Kind | CMakeLists |
|---|---|---|
| `libaqualink-automate` | lib | `src/core/CMakeLists.txt` |
| `libaqualink-jandy` | lib (PRIVATE-links core) | `src/jandy/CMakeLists.txt` |
| `libaqualink-pentair` | lib | `src/pentair/CMakeLists.txt` |
| `aqualink-automate` | app | `src/CMakeLists.txt` |
| `testaqualink-automate` / `intaqualink-automate` / `perfaqualink-automate` | test exes | `test/CMakeLists.txt` |

Conventions: target declared with `add_library()/add_executable()` taking **no** sources, then `target_sources(<t> PRIVATE ...)` with one file per line, paths relative to that CMakeLists, grouped by subdirectory. Each lib repeats `target_compile_features(<t> PUBLIC cxx_std_23)` and two include blocks (project `PUBLIC`, third-party `SYSTEM PUBLIC`). Conditional sources use generator expressions (`$<$<BOOL:${ENABLE_PROFILING}>:profiling/tracy_profiler.cpp>`); generated sources are listed by build-dir path + `set_source_files_properties(... GENERATED 1)`.

## 6. Tooling

- **Sanitizers** (`cmake/Sanitizers.cmake`, `target_enable_sanitizers`): incompatible pairs → FATAL. **MSVC ASan is auto-disabled in Debug** (needs `/MD` not `/MDd`) — use the **Windows-LLVM (clang-cl)** preset for Debug+ASan. TSan/MSan unsupported on MSVC. Mutually exclusive with profiling.
- **clang-tidy** (`cmake/tools/FindClangTidy.cmake`): builds `CMAKE_CXX_CLANG_TIDY` under `ENABLE_CLANG_TIDY` (on in debug presets), applied to `libaqualink-automate` + the app; checks in root `.clang-tidy` / `test/.clang-tidy`. This is why tidy warnings appear during the MSVC build.
- **Coverage**: `cmake/tools/coverage/` — OpenCppCoverage (MSVC/Windows, PDB-based) or gcovr/lcov (others); target `coverage-testaqualink-automate`. Presets `*-coverage` (Windows MSVC + Linux GCC only).
- **Version generation**: `GitVersionDerivation.cmake` (semver from `git describe`, before `project()`), plus build-time `ALL` targets `check_git`/`check_cmake_version` (`GitWatcher.cmake`/`CMakeVersionWatcher.cmake`) that `configure_file` `version_*.constants.cpp.in` → build dir whenever git/version state changes.
- **CPack** (`cmake/CPackConfig.cmake`): per-OS generators (Win ZIP;NSIS, Linux TGZ;DEB;RPM, mac TGZ;DragNDrop), installs `assets/ssl`, `assets/web`, example configs; `pack-aqualink-automate` convenience target.

## 7. Checklists

**Add a source file to an existing lib**
1. Create `src/<lib>/<subdir>/<name>.{cpp,h}`.
2. Add `<subdir>/<name>.cpp` to that lib's `target_sources(... PRIVATE ...)` in the right group. Platform-specific → the `if(WIN32/LINUX/APPLE)` block; feature-gated → `$<$<BOOL:${ENABLE_X}>:...>`.
3. Don't add include dirs/link libs already covered by the lib's PUBLIC deps.
4. Reconfigure if the build doesn't auto-pick the new file.
5. Add a test in `test/CMakeLists.txt`; update `assets/web/api/swagger.yaml` if the HTTP/JSON surface changed.

**Add a vcpkg dependency**
1. Add to `dependencies` in `vcpkg.json`.
2. `find_package(<Pkg> REQUIRED)` (or `CONFIG REQUIRED`) in root.
3. `${<pkg>_INCLUDE_DIRS}` under `SYSTEM PUBLIC` + imported target in `target_link_libraries` of the consumer.
4. Reconfigure (vcpkg installs on configure). Variant-specific build settings go in the **triplet**, never `src/`.

**Change a platform/compiler flag** — pick the right home, never `src/`:
- Compiler/linker/stdlib flag → `cmake/toolchains/<platform>.<compiler>.toolchain.cmake`.
- Dep linkage/CRT/build-type/toolset → `cmake/vcpkg/triplets/x64-<os>-<compiler>.cmake`.
- Project-wide define/feature → `target_compile_definitions(libaqualink-automate PUBLIC ...)` (option/`*_FOUND`-gated).
- Sanitizer → `cmake/Sanitizers.cmake`; tidy → `FindClangTidy.cmake` / `.clang-tidy`.

**Add a new library** — copy `src/pentair/CMakeLists.txt`: `add_library` → `target_sources` → `target_compile_features(... cxx_std_23)` → project + `SYSTEM` include blocks → `target_link_libraries(... PUBLIC Boost::boost magic_enum::magic_enum PRIVATE libaqualink-automate)` → `add_subdirectory` in `src/CMakeLists.txt` → add to app + test link lists.

**Local build / test** (pattern `config|build|test-<os>-<compiler>[-debug]`):
```
cmake --preset config-windows-msvc-debug
cmake --build --preset build-windows-msvc-debug          # or: --target testaqualink-automate
ctest --preset test-windows-msvc-debug
```
Linux: `config-linux-{gcc,llvm}[-debug]`; macOS: `config-macos-llvm[-debug]`; release = drop `-debug`. One-shot CI: `cmake --workflow --preset ci-windows-msvc`. App runs from `build/config-windows-msvc-debug/src/aqualink-automate.exe`.

## 8. Gotchas

- **Windows: `cmake`/`ctest`/`ninja` ship with VS and are NOT on PATH.** First launch the VS Dev Shell in the *same* shell, then build:
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation`
  (or use `cicd/build.ps1`, which runs `vcvarsall.bat` via vswhere). Running the test exe also needs the dev shell (debug-CRT DLLs).
- **Stale toolset after a VS update** → cached `CMAKE_CXX_COMPILER` path vanishes ("not a full path to an existing compiler tool"). Reconfigure with `cmake --preset config-windows-msvc-debug --fresh` (vcpkg restores from binary cache, so it's fast). Editing any CMakeLists triggers an auto-reconfigure on next build, which surfaces this.
- Use **MSVC debug** as the canonical local Windows build; the **LLVM** preset is the one for Debug+ASan.
- `ENABLE_SANITIZERS` and `ENABLE_PROFILING` are mutually exclusive (FATAL).
- vcpkg is a submodule — a fresh clone needs `git submodule update --init --recursive` (bootstrap is automatic on first configure / via `cicd/build.ps1`).
- Warnings are errors globally; keep third-party includes `SYSTEM`.
- HTTP-routing unit/integration tests need the `assets/web` templates, which a POST_BUILD step copies next to the test exe.

## Key files

`CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `cmake/toolchains/*.toolchain.cmake`, `cmake/vcpkg/triplets/*.cmake`, `cmake/{Sanitizers,GitVersionDerivation,GitWatcher,CMakeVersionWatcher,CPackConfig}.cmake`, `cmake/tools/{FindClangTidy.cmake,coverage/*}`, `src/{,core/,jandy/,pentair/}CMakeLists.txt`, `test/CMakeLists.txt`, `cicd/build.ps1`.
