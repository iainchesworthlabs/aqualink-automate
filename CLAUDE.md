# Aqualink Automate - Claude Code Instructions

## Swagger / OpenAPI Maintenance

The API spec lives at `assets/web/api/swagger.yaml` and is served by the built-in web server at `GET /api/swagger.yaml`.

Whenever you add, modify, or remove any of the following, update `assets/web/api/swagger.yaml` to match:

- HTTP API routes (paths, methods)
- Request or response JSON schemas (fields, types, enums)
- HTTP status codes or error responses
- Query parameters, path parameters, or request bodies

## Documentation Accuracy

The `docs/` tree and the root `README.md`/`CHANGELOG.md` are treated as part of the deliverable: **when you change behavior, update the doc that describes it in the same change.** A doc that contradicts the code is a defect, not just stale prose.

When you touch a subsystem, check (and update if affected) its companion doc:

| You change… | Update… |
|---|---|
| HTTP API routes, WebSocket event types, JSON request/response shapes | `assets/web/api/swagger.yaml` **and** `docs/usage-and-api.md` (route reference + WS event list) |
| CLI flags / config keys / defaults (`src/**/options/`) | `docs/configuration.md` (and `docs/mqtt-home-assistant.md`, `docs/hardware-rs485-connectivity.md`, `docs/raspberry-pi.md` for their areas) |
| Auth / TLS / bind / rate-limiting behavior | `docs/SECURITY.md` |
| GitHub Actions workflows (`.github/workflows/`), Packer/runner images (`cicd/`) | `docs/ci-cd.md`, `docs/design/cicd-redesign.md`, `docs/releasing.md` |
| CMake presets, build/install steps | `docs/INSTALL.md`, `docs/worktrees.md` |
| Profiling/logging facade | `docs/profiling.md` |
| Logging sinks / `--log-sinks` / `--log-syslog-facility` / audit routing (`src/core/logging/sinks/`, `src/core/auth/audit_log.*`) | `docs/logging-sinks-redesign.md`, `docs/configuration.md` (Logging section) **and** `docs/SECURITY.md` (audit trail) |
| Record/replay, mock harness | `docs/RECORD_REPLAY.md` |
| Matter bridge, device-ID maps | `docs/MATTER.md` |
| Jandy/Pentair wire protocol, opcodes, message types | the relevant protocol doc (`docs/to_master_decoding.md`, `docs/iaqualink2_init_handshake.md`, `docs/aqualink_rs_revisions.md`, `docs/alwin32_simulator_protocol.md`) |
| Web UI text, i18n catalogs, locales (`assets/web/i18n/`, `assets/web/scripts/i18n.js`) | `docs/i18n.md` |
| Web UI appearance — layout, theme, navigation, new/renamed views or controls, auth screens, the language list | regenerate the affected `docs/assets/*.png` (see "Documentation screenshots" below) |
| Home Assistant add-on wrapper (`homeassistant/`) — options/schema, `run.sh` flag mapping, base-image tag | `docs/homeassistant-addon.md` **and** `homeassistant/aqualink-automate/DOCS.md`; add/rename a `config.yaml` option → also update **every** `translations/<lang>.yaml` (configuration:/network: keys); **only edit the stable `aqualink-automate/` folder — `aqualink-automate-edge/` is GENERATED** by `scripts/gen-homeassistant-edge-addon.ps1` (re-run + commit after any change); keep each channel's `config.yaml` `version` == its `build.yaml` base-image tag == app release version (CI `Home Assistant Add-on` enforces lock-step + no-drift) |

Rules of thumb:

- **Prefer durable anchors over raw line numbers.** Cite symbols, function names, route URLs, option long-names, or section headers — not `file.cpp:NNN`. Bare line numbers drift the moment code is inserted above them and silently rot.
- **Design/analysis docs are dated snapshots.** The design/planning snapshots live under `docs/design/` (e.g. `docs/design/async_migration_*.md`, `docs/design/cicd-redesign.md`) and are point-in-time roadmaps. Do **not** trust their file:line citations as current truth; verify against the code before relying on them, and if you reconcile one, anchor it to symbols and date the reconciliation.
- **Verify before you write.** Confirm a claim against the code (read the source, don't assume) before documenting it. If unsure, mark it explicitly as a hypothesis / pending capture rather than asserting it as fact.

### Documentation screenshots

The PNGs in `docs/assets/` are embedded in `README.md`, `docs/index.md`, `docs/usage-and-api.md`, `docs/SECURITY.md`, and `docs/CONTRIBUTING.md`, and are held to the same standard as prose: **a screenshot showing a UI that no longer exists is a doc defect.**

- Every screenshot is generated (never hand-taken) by `scripts/capture-doc-screenshots.js`, which boots the real binary against recorded replay fixtures — the same harness as the Playwright e2e suite. Run it with `AQUALINK_EXE=<built exe> node scripts/capture-doc-screenshots.js` (subset: `--only hero,trends,…`; Node ≥ 20, `npm ci`, `npx playwright install chromium`).
- When a web-UI change alters what an embedded screenshot shows, regenerate the affected PNGs **in the same change** and eyeball them (Read the PNG) before committing.
- When adding a UI surface worth documenting, extend the script with a new capture instead of committing a one-off manual screenshot — a manual capture cannot be regenerated by the next session and rots silently.
- The script's waits key off English UI strings; capture English shots before switching locales (the RTL capture must stay last in its instance because the locale choice syncs to server preferences).

## Web UI Internationalization (i18n)

The web UI is fully internationalized (`docs/i18n.md`). **Never hardcode user-visible text in `assets/web` HTML/JS.** Every string a user can see goes through the translation catalog:

1. Add the key + English value to `assets/web/i18n/en.js` (flat `namespace.key`; placeholders are `{name}`; keys with markup end in `_html`; plurals use `.one`/`.other` via `tn()`).
2. Bind it: `x-text="$t('ns.key')"` / `:attr="$t('ns.key')"` in templates; `window.AquaI18n.t('ns.key', {...})` in stores/views/components.
3. Add the translated value to **every** locale catalog (`de.js`, `ar.js`, `ja.js`, …) — CI (`i18n-catalogs` job) fails on missing/extra keys or placeholder mismatches.
4. Run `pwsh scripts/check-i18n-keys.ps1` locally; `e2e/i18n.spec.ts` additionally fails on any missing-key console warning and on visible text that bypassed the catalog (pseudo-locale scan).

Do NOT translate: device-originated text (aux labels, panel screen lines — RS-485 data), wire enum tokens used in comparisons (map known ones for display via `status.*`/`swg_health.*`), log channels/severities, endpoint paths, brands. Server/device data rendered verbatim in a new element may need that element added to the pseudo-locale scan's exempt list (or a `data-i18n-exempt` attribute).

## Options / Configuration

CLI flags and the optional config file are merged by a monadic pipeline (Boost.program_options) defined in `src/core/options/options_registry.h` and assembled in `src/aqualink-automate.cpp`. Each subsystem contributes an `OptionsProcessor` + a settings struct (`src/core/options/options_<area>.{h,cpp}`; subsystem options under `src/<sub>/options/`). CLI args take precedence over the config file (first-write-wins into the `variables_map`); config-file keys are the option **long names** (flat INI, no sections).

To add a CLI option / config setting:

1. Add the field to the area's `tagXxxSettings` struct and initialise it in the constructor (this is the real default fallback, since `Process()` only writes when the option `IsPresent(vm)`).
2. Declare an `AppOptionPtr` via `make_appoption(...)` with a matching `->default_value(...)`, and add it to the area's options vector.
3. In `Process()`, set the field guarded by `if (OPTION->IsPresent(vm))`. Custom value types need a `validate()` overload in the *validated type's* namespace (`src/core/options/validators/`).
4. Add conflict/dependency checks in `Validate()` if needed (`Helper_CheckForConflictingOptions` / `Helper_ValidateOptionDependencies`; both ignore defaulted options).
5. Read it via `settings.Get<Options::Xxx::XxxSettings>()`.
6. Add a test under `test/unit/options/` (the config-file key needs no extra code — it is the option long name). Only a brand-new `.cpp` (new area/validator) needs a `CMakeLists.txt` entry.

## Platform Isolation — No OS Macros in Shared Code

**The operating system is a CMake decision, not a preprocessor decision.** OS-divergent code lives in `src/core/platform/<os>/` and `src/core/CMakeLists.txt` selects which `.cpp` compiles via `if(WIN32)`/`if(LINUX)`/`if(APPLE)`. Any shared, non-platform source file (outside `src/core/platform/**`) **must be free of OS macros** (`_WIN32`, `__APPLE__`, `__linux__`, `__unix__`, …). A line like `#elif !defined(__APPLE__)` should exist nowhere. Full reference + rationale: **`docs/platform-isolation.md`**.

Mechanical rules when code diverges by OS:

1. **Declare** the OS-agnostic interface in a shared header (`src/core/platform/<name>.h` or the subsystem's own header).
2. **Implement** one `.cpp` per OS: `platform/windows/<name>.cpp` and `platform/posix/<name>.cpp` (the **shared Unix** impl, listed in *both* the `if(LINUX)` and `if(APPLE)` blocks). Add a leaf `platform/linux/` or `platform/macos/` file **only** on genuine per-Unix divergence.
3. **Wire** each new `.cpp` into the right CMake `if()` block. Reusing an existing seam (e.g. adding a function to `safe_ctime.h`, already compiled everywhere) needs **no** CMake change.
4. **Never** add an OS `#if`/`#ifdef`/`#elif` to shared code, **never** use a negative-OS `#else` ("assume Linux"), and **never** use `$<PLATFORM_ID>` in `src/` CMake.

**Allowed exceptions (a separate concern, not flagged):** compiler macros (`_MSC_VER`, `__GNUC__`, `__clang__` — pragmas/intrinsic shims), architecture macros (`__x86_64__`, `__aarch64__` — ISA intrinsics), and build-feature macros (`TRACY_ENABLE`, … — CMake feature switches). Prefer a small shim header over spreading raw guards.

**Enforcement:** `scripts/check-os-macros.ps1` (CI job *Platform Macros*, a required check) fails the build on a new OS macro in shared code. Run it locally with `pwsh scripts/check-os-macros.ps1 -Root .`.
