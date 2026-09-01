# Changelog

*For end users tracking what has shipped. How releases and version numbers are cut lives in [docs/releasing.md](docs/releasing.md); the project overview is in [README.md](README.md).*

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [docs/releasing.md](docs/releasing.md) for how releases and version numbers are cut.

## [Unreleased]

### Fixed

- **Phantom auxiliaries no longer appear.** Auxiliary discovery had no check that a relay actually exists: anything that looked like an auxiliary — a status reply, or a labelling page the controller offers for every slot it *could* have — became a device, so a single-power-centre panel (an RS-8 Combo has seven relays on one centre) could grow a full set of "Aux B1" … "Aux D8" entries, which then reached the web UI, MQTT and Home Assistant discovery and persisted in the equipment cache. Discovery is now bounded by the model **the panel itself reports**, and relays outside it are removed on the first version-page scrape (so an existing cache is cleaned up too). A relay carrying a name you assigned is never removed, even if it falls outside the model's span, and the shared "Extra Aux" relay (which belongs to no power centre, and which a live capture confirms the panel does report) is left alone.
- **Setting the chlorinator output works again — and fast.** The iAQ (AqualinkTouch) accepted every chlorinator command unconditionally and fired a fixed sequence of page-button presses, assuming the AquaPure page always opened from the same index. The panel lays each page out from the installed equipment, and the panel's Menu/Back key is *one* button whose meaning depends on the screen it is pressed from, so those presses could land anywhere — while the iAQ still reported success, so the command dispatcher never fell back to a controller that could reach the setting, and the dashboard showed a target the panel never received. The iAQ now walks to the AquaPure page **verifying each hop**, resolves the Pool/Spa row **by its on-screen label**, and submits the value **absolutely** — where the OneTouch route can only step it 5% per key press. If the panel's menu has no AquaPure entry, it says so and defers to the OneTouch instead of guessing a button. Every write goal now also blocks the others, so two page walks can no longer interleave on the shared command channel.

### Changed

- **Pool and spa chlorinator outputs are now set independently.** The panel holds a **separate** output setpoint for each body — you can run the spa at 70% while the pool sits at 40% — but the app only ever wrote one of them, and which one depended on who serviced the command: the OneTouch always drove the pool row, while the iAQ path guessed from whichever body happened to be circulating. Setting "the" chlorinator percentage could therefore change the wrong body, or look like it did nothing because the value shown belonged to the other one. The body is now part of the command everywhere:
  - `POST /api/equipment/chlorinator` takes an optional `body` (`Pool` | `Spa`), defaulting to `Pool` — the body the single-value form always drove, so existing callers are unaffected.
  - MQTT gains `command/chlorinator/pool/percentage` and `command/chlorinator/spa/percentage`; the existing `command/chlorinator/percentage` keeps working and still means the pool.
  - Home Assistant gains a **Spa Output** number beside the existing setpoint control, now named **Pool Output**. That control also had its value template corrected — it read the *instantaneous* output, so it snapped back to 0 whenever the cell was idle instead of showing the configured target. It keeps its identity, so existing dashboards and automations survive.
  - The dashboard shows one target slider per body on a dual-body system, each with its own Set.

### Added

- **The SWG tile explains a 0% reading.** The reported output is the *instantaneous* one and is 0 whenever the cell is idle, which read as "off or broken" even when the chlorinator was configured, healthy, and simply waiting for the filter pump. The dashboard now captions the reading with the reason — "Idle — filter pump off", "Idle — no water flow", "Turned off", and so on. The reason is derived server-side and published as `generating_reason` on `GET /api/equipment` and on the MQTT device topic, with a matching **Output State** sensor (and a **Target %** sensor) in Home Assistant, so automations can act on it instead of guessing from the percentage. See [docs/usage-and-api.md](docs/usage-and-api.md) and [docs/mqtt-home-assistant.md](docs/mqtt-home-assistant.md).

## [0.13.0-beta.1] - 2026-08-18

### Added

- **Home Assistant companion package.** A ready-to-use bundle for the app's Home Assistant integration — nine automation blueprints, a script blueprint, a helpers package, and a pre-built dashboard — so you no longer have to hand-build entities and automations from the MQTT discovery payloads. Includes:
  - **Activity scenes** — a script blueprint (equipment/timer/boolean/dark-only-lights) plus an auto-off automation that fires when its timer finishes.
  - **Spa readiness** — an Off/Warming Up/Ready sensor (with a `percent_ready` attribute) plus a "spa ready" notification blueprint.
  - **A showcase dashboard** — an alternative, HACS-styled layout (button-card/card-mod/layout-card/mushroom/bar-card) for installs that already use those custom cards.

  Get it three ways:
  - **From your running instance** at `/homeassistant/` (through add-on ingress too) — matches the exact version you're running and needs no GitHub reachability, useful for air-gapped or LAN-only installs.
  - **From the repository** (tracks `main`) or as a version-pinned, signed release bundle.
  - **Installed automatically by the add-on** — enable the opt-in **Install companion package** option (off by default, since it reaches into Home Assistant's own `blueprints/` folder rather than just the add-on's own storage) and blueprints sync in on every start/update.

  See [docs/homeassistant-companion.md](docs/homeassistant-companion.md).

### Changed

- **BREAKING: the project moved from `iainchesworth/aqualink-automate` to the `iainchesworthlabs` GitHub organization.** Every published distribution channel moved with it and the old locations will **not** receive further updates:
  - **Docker / GHCR images**: `ghcr.io/iainchesworth/aqualink-automate` → `ghcr.io/iainchesworthlabs/aqualink-automate` (same for the Home Assistant add-on wrapper images, `.../homeassistant-{amd64,aarch64}`). Update your `docker run`/`docker compose` image reference.
  - **APT/DNF package repositories**: hosted at `iainchesworthlabs.github.io/aqualink-automate/apt` and `/rpm` (was `iainchesworth.github.io/...`). Re-run the install script from [docs/INSTALL.md](docs/INSTALL.md) (or `cicd/repo/install-apt.sh` / `install-dnf.sh`) to point your existing `sources.list`/`.repo` entry at the new host.
  - **Home Assistant add-on repository**: add `https://github.com/iainchesworthlabs/aqualink-automate` as the add-on repository in the Supervisor; remove the old `iainchesworth/aqualink-automate` entry.
  - **Documentation site**: now published at `iainchesworthlabs.github.io/aqualink-automate`.
  - Git remotes: GitHub transparently redirects `git clone`/`fetch`/`push` against the old `iainchesworth/aqualink-automate` slug, but update your remote URL when convenient.

## [0.12.0-beta.9] - 2026-08-07

Home Assistant add-on maintenance release — no changes to the core application.

### Fixed

- **Add-on storage map renamed `addon_config` → `app_config`**, following the Supervisor's add-ons→apps rename. This silences the deprecation warning the Supervisor logged on every store refresh ("uses legacy map type 'addon_config'; use 'app_config' instead."). Same mount, same paths — but the add-on now needs Supervisor 2026.07.1 (2026-07-08) or newer; an older Supervisor will not list or update the add-on until it self-updates. See [docs/homeassistant-addon.md](docs/homeassistant-addon.md).

### Changed

- Dependency updates: Matter bridge (matter.js group) and Playwright test tooling (Dependabot).

## [0.12.0-beta.8] - 2026-07-12

More Home Assistant add-on fixes from real install testing — no changes to the core application.

### Fixed

- **Manual MQTT mode is configurable again.** The broker fields (`mqtt_host`, `mqtt_username`, `mqtt_password`) are now shown in the options form. Home Assistant hides optional fields that carry no default, so `mqtt_mode: manual` previously offered nowhere to enter the broker. The add-on now also fails fast with a clear message if the host is left blank in manual mode. See [docs/homeassistant-addon.md](docs/homeassistant-addon.md).

### Changed

- **Jandy emulation defaults to `auto`.** `jandy_device_type` now defaults to `auto`, standing up the app's full default device set (OneTouch + IAQ + Serial Adapter) instead of forcing a single OneTouch device — the previous default suppressed IAQ status and Serial Adapter commands. Choose a specific type to restrict emulation to one device; `jandy_device_id` becomes an optional per-type bus-address override (blank uses the type's default, e.g. OneTouch → 0x41).

## [0.12.0-beta.7] - 2026-07-09

Further Home Assistant add-on refinements from real install testing — no changes to the core application.

### Fixed

- **The serial field is visible again.** beta.6 made the serial field optional, which Home Assistant hides from the options form. Serial configuration is now a required **Serial protocol** selector (`usb` / `rfc2217` / `rawtcp`) plus the device or address (`serial_port`), both always shown. `plain` is dropped — it is the same transport as `rawtcp`. See [docs/homeassistant-addon.md](docs/homeassistant-addon.md).

### Added

- **Automatic Home Assistant device identity.** The add-on generates a stable, unique device identifier on first start and persists it under `/data`, so your equipment stays the **same** Home Assistant device across restarts and updates — no `ha-device-id` to configure.

## [0.12.0-beta.6] - 2026-07-09

Home Assistant add-on refinements from the first real install testing — no changes to the core application. The add-on manifests also moved to the repository root (Home Assistant reads a custom repository from the root).

### Changed

- **Home Assistant add-on usability + schema conformance.** The serial connection collapses to a single auto-detecting field (a device path or a `host:port`); MQTT TLS gains certificate options (via the `/ssl` share); the internal/direct port moves to `8099`; a read-write `addon_config` mount is added; and the manifest is brought in line with the current Home Assistant "apps" schema — modern `map` syntax, no defaults on optional fields, and `BUILD_VERSION` in the Dockerfile in place of the deprecated `build.yaml`. See [docs/homeassistant-addon.md](docs/homeassistant-addon.md).

## [0.12.0-beta.5] - 2026-07-08

The first release of the **Home Assistant add-on**. This is a pre-release intended for testing the add-on on real Home Assistant OS.

### Added

- **Home Assistant add-on.** Aqualink Automate can now be installed as a Home Assistant add-on on Home Assistant OS / Supervised — the Supervisor runs and manages the container, so there is no Docker to operate by hand. Highlights:
  - **Install & configure from a form.** USB-RS485 (device picker) or serial-over-ethernet, and **zero-config MQTT** that auto-discovers the Home Assistant broker (entity discovery on by default). Every option has a localised label and help text.
  - **Web UI via ingress** — in the sidebar, behind Home Assistant's login, not exposed on the LAN — plus an opt-in direct LAN port (with an optional built-in login) for cases like a wall tablet.
  - **Defers to Home Assistant by default** — the app's own auth, history, and scheduler are off, using Home Assistant's login, Recorder, and automations instead; each is an opt-in toggle.
  - **Two channels** — *Aqualink Automate* (stable) and *Aqualink Automate (Edge)* (tracks the newest pre-release) — installed from **prebuilt per-arch images** (`aarch64` + `amd64`), so installation is a quick download.
  - A **liveness watchdog** restarts the add-on if the app stops responding. A tailored AppArmor profile ships staged for validation.

  See [docs/homeassistant-addon.md](docs/homeassistant-addon.md).

### Changed

- **Web UI is now base-path aware.** The UI derives its base prefix from the document location (it uses hash routing) and rebases API, WebSocket, and asset URLs onto it, so it works both when served at the site root and under a path prefix such as Home Assistant ingress. No change to how it behaves when served directly.

## [0.12.0-beta.4] - 2026-07-07

A release-integrity update: release artifacts now carry verifiable build provenance and optional signatures, and the codebase gains a broad compile-time (`constexpr`/`consteval`) pass. No functional or configuration changes.

### Added

- **Verifiable release provenance.** Every release package and the Docker image now ships with a keyless [build-provenance attestation](https://docs.github.com/actions/security-guides/using-artifact-attestations) (SLSA provenance, signed via Sigstore using the release workflow's OIDC identity) and an SPDX SBOM attestation, so anyone can confirm a download was built by this repository from a specific commit — not swapped for a poisoned build. Verify with `gh attestation verify <file> --repo iainchesworthlabs/aqualink-automate` (or `oci://…` for the image). Release binaries are additionally GPG-signed (detached `.asc` per file plus a signed `SHA512SUMS`) when the project signing key is configured. See [SECURITY.md > Verifying build authenticity](docs/SECURITY.md#verifying-build-authenticity).

### Changed

- **Compile-time hardening (`constexpr`/`consteval`).** A codebase-wide sweep made pure in-header functions, constants, and small value types (device identifiers, restart timeouts) `constexpr`/`consteval` where sound, shifting more work to compile time. No behaviour change.

## [0.12.0-beta.3] - 2026-07-06

A robustness and supply-chain hardening release: the protocol and input parsers are now continuously fuzz-tested, a scheduling API crash on malformed input is fixed, and the CI/build posture is tightened. No new features, and no configuration changes.

### Fixed

- **Malformed schedule requests no longer crash the handler.** Posting a controller-schedule request whose JSON fields had the wrong type could throw out of the parser; the request now fails cleanly with a validation error instead.

### Changed

- **Fuzz testing of the wire protocol and input surfaces.** New libFuzzer harnesses continuously exercise the RS-485 Jandy/Pentair message decoders and the untrusted input parsers (schedule/web-API JSON, config file, WebSocket and MQTT payloads, query strings, JWTs, durations, and `.cap` replay files), guarding against crashes on hostile or corrupt input. No behaviour change for well-formed data.
- **Supply-chain and repository hygiene.** CI GitHub Actions are pinned by commit SHA with least-privilege `GITHUB_TOKEN` permissions (OpenSSF Scorecard posture), committed third-party binary tools were removed in favour of documented downloads, and per-developer IDE/editor configuration is no longer tracked.

## [0.12.0-beta.2] - 2026-07-06

A stabilization and code-health release: user-facing bug fixes alongside a large internal quality pass and two device-class refactorings. No new features, and no configuration changes.

### Fixed

- **Temperature values display the degree symbol correctly.** A build-toolchain quirk could render `°` as the literal text `u{B0}` (e.g. `22u{B0}C`) in temperature readouts and Home Assistant discovery payloads; temperatures now show `22°C` on every platform.
- **Diagnostics shows human-readable system-board and pool-configuration labels** instead of raw internal codes.
- **Spa-switch assignment no longer proceeds while the device is in a fault state**, avoiding a spurious assignment.
- **Controller schedule times parse reliably around midday/midnight.** The 12-hour AM/PM time parser used an uninitialised flag, so the 12 AM / 12 PM edge cases could be misread.
- **Body-of-water detection is consistent** across the controller's Version and StartUp pages.
- **Web UI polish.** The heater card's applied/rejected chip is vertically centred on desktop, and the equipment-stats WebSocket no longer logs a spurious "closed before established" message to the browser console.

### Changed

- **Internal code-health pass — no behaviour change.** Over 1,100 static-analysis findings were resolved across the codebase (modern C++ idioms, explicit lambda captures, `const`-correctness, narrowed exception handling, reduced function complexity), with the full test suite green throughout.
- **Device-handler decomposition.** The two largest device classes (`OneTouchDevice`, `IAQDevice`) were split into smaller, composable, unit-testable collaborators. No functional change.
- **CI builds the Windows leg on Visual Studio 2026** (MSVC 14.5x), matching the local development toolchain.

## [0.12.0-beta.1] - 2026-07-05

The scheduling and responsive-UI release. Aqualink Automate can now read, write, and manage the pool controller's own built-in RS-485 schedules alongside its richer app-level schedules in one unified view, and the entire web interface reflows for phones and tablets — a bottom tab bar, tap-tile equipment controls, and consolidated cards on small screens — while the desktop layout is unchanged.

### Added

- **Two-tier scheduling — the controller's own programs, first-class.** Aqualink Automate now decodes the schedules programmed into the Jandy controller itself (read from the IAQ Program page) and shows them alongside app-level schedules in one unified list and 24-hour timeline, each tagged with a per-source group badge. The controller tier is the resilient, always-works baseline; the app tier stays the rich, flexible one.
- **Write controller schedules over RS-485.** Create and delete the controller's built-in programs directly through an IAQ page-navigation write state machine (with the edit path for existing programs decoded), exposed as `POST` / `DELETE /api/controller/schedules`.
- **Promote an app schedule to the controller.** A feasibility constraint-checker validates whether an app schedule fits the controller's limited program model (A/B groups; all-days / weekdays / weekends / single-day), and a one-click **Promote to controller** action pairs an app on/off schedule into a controller span (`POST /api/schedules/{uuid}/promote`).
- **Responsive mobile + tablet web UI.** The single web app now reflows across three viewport bands with an adaptive navigation pattern: a fixed **bottom tab bar** on phones (secondary destinations behind a grouped **More** sheet with icons and toggle switches), a **hamburger drawer** on tablet portrait, and the full **inline navigation** on desktop. On phones the dashboard reorders around what you act on most — a condensed Pool/Spa/Air header, equipment as **tap-tiles**, a consolidated water-chemistry card, and heater rows with the setpoint steppers folded in — and the dense **Diagnostics** page collapses to its essentials behind a "Show advanced diagnostics" disclosure. iPad portrait and landscape get dedicated layouts. The desktop layout is unchanged, and dark mode and right-to-left are carried through every breakpoint.

### Fixed

- **Phone views no longer hide their last card behind the tab bar.** A CSS specificity bug (`.app-container` overriding a bare `main` selector) silently zeroed the bottom-clearance padding, so the final card on Settings, About, and other views scrolled under the fixed tab bar.

## [0.11.0-beta.2] - 2026-07-04

A translation-polish release for the internationalized web UI: device operating-state labels and every user-visible number now localize correctly in all nine languages. Bug fixes only — no behavior or configuration changes.

### Fixed

- **Device operating-state labels are now translated.** The diagnostics device cards displayed raw catalog keys (`devcard.op_state_normal`, `…_fault`, `…_not_present`, and four more) instead of readable text; all seven states now render in every language.
- **Numbers localize consistently across the interface.** Temperatures, setpoints, chemistry readings, the chlorinator output, diagnostics counters, and schedule times now all format through the locale-aware formatter. Right-to-left locales such as Arabic render Eastern Arabic-Indic digits everywhere — previously the dashboard chlorinator output and several counters showed Western digits regardless of the selected language.

## [0.11.0-beta.1] - 2026-07-03

The authentication and internationalization release. Aqualink Automate gains a full optional identity system (first-run admin setup, username/password sign-in, roles-free attribute-based access control, guest mode, and a users/groups/API-keys admin UI), and the entire web interface is now translatable, shipping in nine languages with full right-to-left support. Under the hood, a redesigned logging subsystem adds file and JSON logs plus platform-native sinks, the app can run as a managed Windows service, and temperature reporting is now honest about stale or missing readings. Authentication is off by default, so existing installs are unaffected until you turn it on.

### Added

- **Optional identity system.** `--auth-mode enabled` requires sign-in: a first-run wizard creates the initial administrator, then users log in with a username and password (argon2id-hashed). Off by default — the app behaves exactly as before.
- **Attribute-based access control (ABAC).** Fine-grained entitlements resolved through person → group → entitlement, with a default-deny policy engine enforcing every request server-side. No fixed roles.
- **Administration UI.** Manage users, groups, entitlements, API keys, and active sessions, with a full password lifecycle (change/reset and session revocation).
- **Guest mode + kiosk PIN.** An administrator can grant anonymous visitors a read-only Guest scope for wall-panel/kiosk use, with locked controls that a kiosk PIN can elevate.
- **Sessions and sign-in methods.** Local accounts, API keys, and a stateless JWT session model (login/refresh/logout); session revocation propagates immediately to live WebSocket connections. An in-app OIDC framework is scaffolded.
- **Per-user preferences.** Temperature units, theme, accent, and chemistry bands are stored per user when signed in.
- **Internationalization.** The web UI is fully translatable and ships with English, German, Spanish, French, Arabic, Hebrew, Japanese, Chinese, and Yiddish catalogs. Arabic, Hebrew, and Yiddish render with a fully mirrored right-to-left layout and vendored per-script fonts. Numbers, temperatures, digits, and durations format per locale; API errors and alert details are translated from structured payloads.
- **File and JSON logs.** A rotating file sink (`--log-file`) and a structured JSON log format (`--log-format json`) alongside the console.
- **Platform-native log sinks.** journald on systemd Linux, the Apple unified log on macOS, and the Windows Event Log, selected automatically or via `--log-sinks`; the security-audit trail is now a separate durable subsystem.
- **Run as a managed Windows service.**
- **Honest stale/missing temperature display**, backed by a new `temperature_stale` alert condition.

### Changed

- **Temperature display units flow everywhere** — the display-units preference is honored by the dashboard, the Trends chart, and MQTT / Home Assistant setpoint entities.

### Fixed

- **WebSocket authentication under the identity system** — the token now rides the WebSocket subprotocol, since browsers cannot set an `Authorization` header on the upgrade.
- **Pool temperature no longer reports the heat setpoint** as the water temperature.
- **Several GCC/Clang-only memory bugs** surfaced by full multi-platform CI (a preferences use-after-free, a JSON container that initialized as an array, and two static-teardown heap corruptions); Windows was unaffected.
- **Idle WebSockets stay alive** through reverse-proxy timeouts.
- Numerous i18n and web-UI polish fixes (viewport-clamped alerts dropdown, synchronous locale load at boot, Matter commissioning-QR scaling, settings-view auth timing).

## [0.10.0-beta.1] - 2026-07-02

A major release headlined by a complete redesign of the web interface on a new "calm-premium" design system, plus a unified Schedules view, a net-new Detailed system view, and user-selectable theming. Under the hood the latency tracker gains a rolling 900-second window. All additions are backward-compatible.

### Added

- **Completely redesigned web UI.** Every view — the app shell (navigation, status, connection chip, alerts dropdown), the dashboard hero and controls (gauges, equipment, heater, SWG), the shared card system and primitives, Trends, Settings, Schedules, Diagnostics, and About — has been rebuilt on a new calm-premium design-token foundation for a consistent, modern look, including redrawn 270° chemistry gauge rings.
- **Appearance settings.** A new Settings panel lets you switch between light and dark themes and choose an accent colour; the choice is remembered locally.
- **Unified Schedules view.** The Schedules page now presents the app's schedules alongside the controller's internal schedules in a single merged list and 24-hour timeline, flagging overlapping entries as conflicts.
- **Net-new Detailed view.** A new live system-state view surfaces per-subsystem cards — heating, SWG output/target, pump rows, system status, started/uptime, and heat source — at a glance.
- **System Health panel in Diagnostics.** Diagnostics gains a dedicated System Health panel.
- **Offline fonts.** The Bricolage and Hanken typefaces are now vendored locally as woff2, so the interface renders correctly with no external network access.
- **Honest-control feedback.** Controls flash their command state so you can see that a command was sent and is being applied.
- **Rolling latency window.** The latency percentile tracker now keeps a 900-second sliding window alongside its cumulative aggregate, giving both recent and lifetime views.

### Changed

- **Diagnostics rebuilt as flat, always-visible cards** (no more accordions), with three detail modals and the bandwidth chart's left legend doubling as its series toggle.
- **Matter and Profiling moved from Diagnostics to Settings** to match the new information architecture.

### Fixed

- **About refetches the version on navigation**, fixing a startup race that could leave the version stale or empty.

## [0.9.0-beta.5] - 2026-07-01

A security-hardening release on top of 0.9.0-beta.4 that fully closes the auto-generated-key exposure only partially mitigated in beta.4, plus a version-stamping fix. No application behaviour change on a normally-configured install.

### Security

- **Auto-generated TLS key material no longer falls back to the world-writable system temp directory.** When no certificate is configured and the install tree is read-only (the common packaged case), beta.4 wrote the generated private key under the system temp directory and merely `chmod`-ed it afterward — a pattern still open to symlink/pre-creation attacks and to the reuse path trusting attacker-planted files, and still flagged by static analysis (SonarCloud S5443). The fallback now targets a **per-user private** directory instead — `$STATE_DIRECTORY` (systemd `StateDirectory`), then `$XDG_RUNTIME_DIR`, then `$HOME/.local/state` on Linux; `%LOCALAPPDATA%` on Windows — and each candidate is created owner-only (`0700`) and verified to be a self-owned, non-symlink directory before the key is written or an existing pair is reused. Another local user can therefore neither read the key nor pre-seed material the server would trust.

### Fixed

- **CI-built binaries no longer report "uncommitted changes" on the About page.** The build-time git-dirty probe (`GitWatcher`) ran a bare `git status --porcelain`, which the release build leg tripped by `chmod +x`-ing the packaging maintainer scripts (an exec-bit-only diff) and bootstrapping the `deps/vcpkg` submodule — so a clean, CI-released binary stamped itself dirty. The probe now ignores exec-bit-only changes, untracked build artifacts, and the bootstrapped submodule (so "dirty" means a tracked source file actually differs from the commit), and the packaging scripts are committed executable so the workflow `chmod` is a no-op.

## [0.9.0-beta.4] - 2026-07-01

A security-hardening and static-analysis cleanup release on top of 0.9.0-beta.3. One hardening fix to the auto-generated HTTPS key material; the remainder clears outstanding code-scanning findings with no application behaviour change.

### Security

- **The auto-generated HTTPS private key is now stored in an owner-only directory.** When no certificate is configured and the install tree is read-only, the self-signed key and certificate fall back to a world-writable system temp directory. The key file was already written `0600`, but its containing directory was unrestricted — leaving it readable by, or pre-seedable by, other local users on a shared host (the reuse-on-restart path would then trust that material). The directory holding the key is now restricted to owner-only (`0700`) before the key is written or an existing pair is trusted.

### Changed

- **Cleared outstanding static-analysis (code-scanning) findings.** A `std::optional` unwrap in the auxiliary reconciliation path — already guarded behind a helper the analyser could not see through — now carries an explicit `has_value()` check, and a `[[fallthrough]]` annotation in the Jandy startup coordinator was moved directly before its `case` label so it is recognised. No behaviour change.

## [0.9.0-beta.3] - 2026-06-30

A bug-fix and hardening release on top of 0.9.0-beta.2. Seven user-facing fixes — the Trends view and Schedules page failing to load, duplicate auxiliary devices in MQTT/Home Assistant, panel display-line rendering, reduced MQTT/WebSocket churn, cleaner numeric API output, and a Matter bridge startup crash-loop — plus build-toolchain and test-coverage hardening with no other application behaviour change.

### Fixed

- **The Trends view now loads its history graphs.** Every API request that carried a query string — most visibly `GET /api/history/series?key=…&from=…&to=…`, which every Trends chart issues — was answered `400 Bad Request` before reaching its handler: the router parsed the whole request target (path **and** query) as a URL path, and the `?` that begins a query is not a valid path character, so the parse failed. The router now parses the target as an origin-form URL and matches on the path only, so query parameters route correctly.
- **The Schedules page no longer breaks when two devices share a label.** The "Device" target dropdown keyed its options on the device label; two equipment items with the same label produced a duplicate key, which crashed Alpine's list reconciliation and took the whole schedules form down. The dropdown — which targets a device by label — now de-duplicates labels.
- **Duplicate auxiliary devices no longer appear in MQTT and Home Assistant.** A pre-stable-id cache placeholder (random UUID, generic `AuxN` label) and the live-discovered stable-id device for the same physical aux could both be published as separate entities (e.g. both `aux5` and `pool light`). The aux is now collapsed by identity at the first live touch — before its custom label is known — the identity-less placeholder is no longer persisted across restarts, and a removed or relabelled device has its now-stale retained device/state topics cleared and its Home Assistant discovery entity tombstoned. On startup the broker is reconciled against the live device set, so ghost retained topics left by a prior (buggy or differently-labelled) run are healed rather than served indefinitely.
- **OneTouch and iAQ panel screens now render as the panel intends.** The LCD rows are NUL-padded fixed-width cells; the decoder previously ran that padding through a generic sanitiser, surfacing it as literal `?` (`More OneTouch??`, `Pool Heat?OFF?`, `Home???`), and the web UI collapsed the panel's leading-space centring. The padding is now stripped, interior NUL column-separators render as spaces, and centred/right-aligned rows keep their spacing. This applies to both the OneTouch (`JandyMessage_Message`/`_MessageLong`) and iAQ (PageMessage/PageButton/TitleMessage/TableMessage) display paths.
- **Home Assistant and WebSocket consumers no longer churn on unchanged values.** Temperature/chemistry setters, per-button state, and the base device-status publisher re-fired on every (~1/sec) poll even when nothing had changed, flooding MQTT/WebSocket subscribers with identical payloads. Emits are now guarded to fan out only on a real change (values still re-stamp their timestamp for liveness).
- **Numeric API/WebSocket output is rounded to a sensible precision.** Unit conversions and float32→double promotion leaked floating-point noise into the JSON (e.g. pH `7.0999999`); temperature, pH, and bandwidth-utilisation values now snap to their real resolution.
- **The Matter bridge sidecar no longer crash-loops on startup.** The node's `serialNumber` and `uniqueId` were derived from the 52-character per-install id, overflowing the Matter `BasicInformation` 32-character limit (and the spec's "must differ" rule); node initialisation rolled back with `AggregateError: Behaviors have errors` and the entrypoint restarted the sidecar every 15s. Both identifiers are now derived as short, distinct, deterministic values, so the bridge starts and stays up.

### Changed

- **Build toolchain refreshed.** The dev/CI/runtime Docker images are rebased on Ubuntu 26.04 LTS, and the pinned CMake is bumped to 3.31.12 and Node.js to 24.
- **Expanded automated test coverage** across the state hubs, navigator, MQTT client/integration, the OneTouch device, and the history/equipment-cache services, and the documentation tree was reconciled against the codebase.

## [0.9.0-beta.2] - 2026-06-30

A bug-fix release on top of 0.9.0-beta.1. One user-facing Home Assistant fix; the remainder is build, CI, and test-coverage hardening with no application behaviour change.

### Fixed

- **Home Assistant spa-mode, clean-mode, and second-pool-heater sensors now settle to a real on/off state.** The `Spa Mode`, `Clean Mode`, and `Pool Heater 2 (TEMP2)` binary sensors rendered their JSON-boolean source through a raw `value_template`, which Home Assistant's Jinja renders capitalised (`True`/`False`) — so it never matched the lowercase `payload_on`/`payload_off` and Home Assistant logged "No matching payload found" on every circulation/temperature publish, leaving the entities stuck. The templates now coerce to lowercase `true`/`false` (and map the not-reported case to `false`).

## [0.9.0-beta.1] - 2026-06-29

A spa-control and dual-body feature release.

### Added

- **Spa control from the web UI, MQTT, and Home Assistant.** Spa controls are now **writable** — a web-UI spa-mode toggle, and MQTT/Home Assistant entities that actuate heater modes via `SetHeaterMode`. MQTT entities now respect the installed pool configuration (so they match the equipment you actually have) and surface temperature freshness/staleness. This closes the gaps for migrating Home Assistant dashboards off the Ondilo ICO / iAqualink integrations onto aqualink-automate.
- **Second pool setpoint and maintenance heating.** The panel's second pool setpoint (`POOLSP2`, shown as "TEMP2") and its maintenance-heating mode (`POOLHT2`) are now surfaced (read-only).
- **Live circulation state.** A new `CirculationUpdate` WebSocket event publishes circulation state in real time.
- **Online documentation site.** The full documentation is now rendered to GitHub Pages — served at the site root (<https://iainchesworthlabs.github.io/aqualink-automate/>) and single-sourced from the in-repo Markdown.

### Changed

- **Emulated devices are gated by firmware revision** and validated by device-id placement before being allowed "online", and spa modes are gated to dual-body systems (reserving the AquaLink-PC control-panel-4 slot).

### Fixed

- **Pool/spa setpoint writes over RSSA** now use the correct two-step ready/set (`readySP`/`setSP`) sequence, emitted in reply to the device-ready poll.

## [0.8.0-beta.2] - 2026-06-28

Build, CI, and packaging reliability fixes only — no application behaviour changes from 0.8.0-beta.1.

### Fixed

- **Relocatable Linux install tree no longer needs root.** The default config installs to an absolute `/etc/aqualink-automate/aqualink-automate.conf` (correct for the `.deb`/`.rpm`), but the relocatable `cmake --install` tree staged it with `--prefix`, which does not relocate absolute destinations — so producing the tree aborted on a non-root host (`cannot create directory /etc/aqualink-automate`). It is now staged via `DESTDIR`. The `.deb`/`.rpm` packaging was unaffected.
- **Docker image builds reliably from a cold cache.** The from-source Docker build now installs `autoconf-archive` (required to build the `libbacktrace` dependency from source) and uses a persistent vcpkg **asset cache**, so a binary-cache miss no longer re-fetches sources from GitHub — which intermittently returns a non-retryable HTTP 400 on the busy build host. The same asset cache was added to the host build.
- **End-to-end UI tests run on the current CI runner.** Bumped Playwright to 1.61.1 for Ubuntu 26.04 support.

## [0.8.0-beta.1] - 2026-06-27

### Added

- **Built-in performance profiling.** A profiling build can now be captured with Tracy, Intel VTune, or AMD uProf — selected at runtime with `--profiler` — and the app is instrumented end-to-end: the main loop is broken into per-phase zones (io / protocol / watchdogs / http / https / mqtt), with navigation, the DataHub event fan-out, and equipment updates all marked on the profiler timeline. A new **Profiling** card on the Diagnostics page (and `GET`/`POST /api/diagnostics/profiling`) shows the active backend and lets you pause and resume capture and switch backends without restarting. Previously the VTune and uProf backends never actually resumed capture; both are now fixed and verified.
- **Network-hardened "diagnostic" profiling builds.** New `*-diagnostic` build presets produce an always-attachable field/beta build whose Tracy client is compiled to listen on loopback only and to not answer UDP discovery broadcasts (`TRACY_ONLY_LOCALHOST` + `TRACY_NO_BROADCAST`), so it is never exposed on the LAN. A profiler connects over loopback or an SSH tunnel (e.g. `ssh -L 8086:127.0.0.1:8086`). See [docs/profiling.md](docs/profiling.md).
- **Origin allow-list and CSRF-header enforcement.** The previously-dormant Origin allow-list and CSRF-header checks are now wired to options — `--api-allowed-origin` (repeatable) and `--api-require-csrf-header`. Both are off by default. See [SECURITY.md](docs/SECURITY.md) and [docs/configuration.md](docs/configuration.md).

### Fixed

- **Raw-TCP remote serial no longer corrupts data.** `--rawtcp` / `--no-rfc2217` were parsed but never reached the network transport, so a "raw TCP" remote serial port always ran the RFC2217 telnet handler — mangling legal `0xFF` data bytes through IAC escaping and sending telnet negotiation the peer never asked for. Raw mode is now a transparent byte pipe with no protocol handler.
- **Colliding device names are flagged instead of silently dropped.** When two equipment labels reduce to the same MQTT/Home Assistant command-topic slug, the second device was silently left uncontrollable. The collision is now logged with a warning naming both devices and which one owns the topic, so it can be resolved by renaming one.
- **Static asset paths with redundant segments resolve correctly.** A web-UI request whose URL contained `//`, `/./`, or `/../` segments that normalize away to a legitimate in-root asset could spuriously 404. Paths are now resolved from the same normalized segments used to match them. (The path jail always held — this was a latent 404, not a traversal.)

### Security

This release folds in an end-to-end security review. All hardened behaviour preserves the historical defaults unless a network address is exposed.

- **Unique TLS certificate per install.** A shared self-signed certificate and its **private key** were previously committed to the repository and shipped in every package. That key is removed; each install now generates its own 2048-bit key and self-signed certificate on first boot (key stored `0600`, fingerprint logged).
- **Per-install Matter commissioning credentials.** The Matter bridge shipped the publicly-known matter.js example passcode (`20202021`) and discriminator, so any device on the LAN could commission the bridge and actuate equipment. Each install now generates a cryptographically-random passcode, discriminator, and unique id on first boot, persists them `0600`, and reuses them across restarts; the bridge refuses to start with the example passcode.
- **Brute-force and connection-exhaustion limits.** Failed bearer-token attempts are now rate-limited per source IP (answered `429` with `Retry-After` after repeated failures, cleared on success), and a per-IP cap limits how many simultaneous connections a single peer can hold, so one client can no longer consume the whole connection budget.
- **Serial-bus denial-of-service fixes.** A malformed spa-side-remote assignment line could throw on the serial dispatch path and kill the daemon; numeric parsing on the wire path is now non-throwing, and a process-wide exception barrier around per-message handler dispatch prevents any single throwing handler from terminating the app (surfaced as a new error counter).
- **Outbound alert webhook now verifies TLS.** The alert webhook client accepted any certificate (`verify_none`); it now performs peer and hostname verification, matching the MQTT client.
- **Smaller hardening fixes.** Reflected request targets in `405` error pages are HTML-escaped (reflected-XSS fix); malformed preferences input returns `400` instead of `500`; serial capture files are created `0600` and auto-stop at a 256 MiB cap (disk-fill DoS); broker-controlled MQTT topic strings are sanitized before logging (log-forging); and `--disable-content` is now honoured (it was parsed but never enforced, leaving the doc-root served).
- **Open-bind and weak-token warnings.** Binding a non-loopback address with no auth token now logs a loud startup warning and requires an `--insecure-no-auth` acknowledgement; weak (`<16` character) tokens are also warned. Supply tokens via a file or environment rather than the command line.

## [0.7.0-beta.1] - 2026-06-27

### Added

- **Runs on stock Raspberry Pi OS / Debian Bookworm.** The Linux `.deb`/`.rpm`/`.tgz` are now built on a glibc-2.36 base and bundle the gcc-15 C++ runtime, so they install and run on Raspberry Pi OS Bookworm (and every newer distro) on both `amd64` and `arm64`. Previously they required a too-new glibc/libstdc++ and would not even load on a stock Pi.
- **Installs as a `systemd` service.** The `.deb`/`.rpm` create an `aqualink` service account (in `dialout` for serial access), install a default config at `/etc/aqualink-automate/aqualink-automate.conf`, and a hardened `systemd` unit (enabled on boot; start it once your serial port is set). The `.tgz` ships an `install.sh` that does the same.
- **APT and DNF package repositories.** Install and stay updated with your package manager from a signed repository — `curl -fsSL https://iainchesworthlabs.github.io/aqualink-automate/install-apt.sh | sh` then `sudo apt install aqualink-automate` (and a `dnf` equivalent). See [INSTALL.md](docs/INSTALL.md).
- **Multi-arch Docker image.** The GHCR image is now `linux/amd64` + `linux/arm64`, so a Raspberry Pi pulls the arm64 variant from the same tag.
- **Raspberry Pi guide** ([docs/raspberry-pi.md](docs/raspberry-pi.md)): install, RS-485 hardware (USB adapter vs GPIO UART), udev stable device naming, the service, and troubleshooting.

## [0.6.0-beta.1] - 2026-06-19

### Added

- **Native arm64 (Raspberry Pi) packages.** `.deb`, `.rpm`, and `.tar.gz` are now built for 64-bit ARM (aarch64) alongside the existing x86-64 builds, so Aqualink-Automate installs on a Raspberry Pi (3 / 4 / 5 and Zero 2 running a 64-bit OS) and other arm64 Linux hosts. The arm64 binaries are built natively on an `ubuntu-24.04-arm` runner — not cross-compiled or emulated — run the full test suite before packaging, and the `.deb` carries the correct `Architecture: arm64`. Pick the package matching your machine (`dpkg --print-architecture` / `uname -m`). The Docker image remains `linux/amd64` only.

### Fixed

- **Matter commissioning QR code now renders.** The diagnostics page's Matter pairing QR (added in 0.4.0-beta.1) did not draw; it now displays correctly, so the bridge can be paired by scanning rather than entering the manual pairing code.

## [0.5.0-beta.1] - 2026-06-19

### Added

- **Selectable MQTT protocol version (3.1.1 or 5.0).** A new `--mqtt-protocol-version` option (config-file key `mqtt-protocol-version`; default `3.1.1`) selects whether the MQTT client speaks MQTT 3.1.1 or 5.0, so a deployment can match the dialect its broker and Home Assistant install expect. The active version is reported on `GET /api/diagnostics/mqtt`.

### Changed

- **MQTT client rebuilt on the async-mqtt library.** The hand-rolled MQTT engine is replaced by the maintained async-mqtt (Boost.Asio) library, which now owns wire framing, keep-alive, and packet encoding/decoding. MQTT 5.0 is fully supported as a result (previously 3.1.1 only). Reconnection with exponential backoff, the bounded drop-oldest publish queue, Last-Will, TLS with hostname verification, and Home Assistant auto-discovery are unchanged. Connecting, subscribing, and publishing were verified against a live broker on both protocol versions.

## [0.4.0-beta.1] - 2026-06-18

### Added

- **Spa-side remote button programming, redesigned.** Each spa-side remote (Dual Spa Switch / Spa Link) now appears as a visual keypad on its card in the *Emulated*/*Actual Devices* sections of Diagnostics, showing what every button is mapped to and its live indicator state. On an emulated remote you can press a key to act as that remote on the bus; on any remote whose button-to-switch mapping is known, you can reprogram a key's function inline — choosing only from the functions the pool controller can actually assign (the controller's own picker list). This replaces the separate spa-side section and its free-text assignment form.
- **Matter commissioning QR code.** The diagnostics page renders the Matter commissioning QR code, so the bridge can be paired by scanning rather than typing the manual pairing code.

### Fixed

- **OneTouch recovers from controller fault states.** When controller communications resume after a fault, an emulated OneTouch now returns to normal operation instead of remaining stuck in the fault state.

## [0.3.0-beta.3] - 2026-06-17

### Added

- **Chlorinator output setpoint in the web UI.** The dashboard now shows the chlorinator's *configured* output setpoint — distinct from the instantaneous generating percentage — with per-body pool and spa values where the controller reports them. The app keeps the figure current by proactively re-reading it (periodically and after a communications recovery) from the controller's Set AquaPure menu, and it is exposed on `/api/equipment` and in the OpenAPI spec.

### Fixed

- **Web-UI equipment toggles now reliably reach the controller.** Clicking an equipment button while emulating a OneTouch and/or RS Serial Adapter could appear to succeed while nothing actually changed. The RS Serial Adapter now sends the aux on/off command in the byte order the controller expects; an adapter the master is not polling — or a controller still starting up or in a fault state — no longer silently swallows the command, but instead routes to a controller that can act (or reports an honest failure). The web UI no longer shows a premature "Applied": a command stays pending until the controller confirms the new state, or it times out.

## [0.3.0-beta.2] - 2026-06-17

### Changed

- **Prerelease Docker images are tagged `edge`.** Stable releases continue to move the `latest` tag on GHCR; prereleases now move a rolling `edge` tag (in addition to the exact `<version>` tag), so you can track the newest prerelease with `ghcr.io/<owner>/aqualink-automate:edge`.

### Fixed

- **Trends chart renders when the page opens.** The Trends graph could appear blank when the app opened directly on that view, because the chart tried to draw before its panel had been laid out. It now draws as soon as the panel has a size, and redraws on window resize.
- **Trends selections survive a reload.** The chosen metrics, time range, and the show-inactive toggle are remembered across page reloads.

## [0.3.0-beta.1] - 2026-06-17

### Added

- **Show the aux id alongside the friendly name.** A new `show_aux_id_in_label` preference (off by default) renders device names as `Pool Light (Aux5)` in the web UI. The canonical label used for command dispatch, MQTT, and Home Assistant is unchanged.

### Changed

- **Trends page redesign.** The Trends view is rebuilt around faceted, vertically-stacked panels that share one time axis and a synchronized hover crosshair — a temperature axis, an auto-scaled water-chemistry overlay, and an equipment-runtime timeline (on/off bars reconstructed from transitions) — with per-window stats (now / min / max / avg) and grouped metric pickers. Device history is keyed by the equipment's stable UUID and carries its friendly label, so a device renamed after discovery (e.g. `Aux5` → `Pool Light`) is a single series rather than two; legacy duplicate series are folded in automatically.
- **Stable auxiliary identities.** Auxiliaries are assigned a deterministic identity derived from their hardware aux id instead of a random one regenerated each run, so a device keeps the same identity across restarts and reinstalls. Aux-id labels are also parsed consistently — `Aux5`, `Aux 5`, and a friendly name like `Pool Light` all resolve to the same device for control.

### Fixed

- **Auxiliaries no longer duplicate after a restart.** Equipment restored from the cache at start-up is reconciled with live discovery by stable identity, so devices such as multiple `Swim Jet` / `Spa Jet` outputs are no longer doubled-up on the dashboard after an upgrade or restart.

## [0.2.0-beta.4] - 2026-06-17

### Added

- **Jandy/Zodiac RS-485 integration.** Decode and control AquaLink RS panels over the RS-485 serial protocol.
- **Pentair RS-485 support.** Pentair equipment is decoded alongside Jandy/Zodiac: VSP/IntelliFlo pumps, IntelliChlor salt-water generators, and IntelliCenter/EasyTouch controllers. The protocol is auto-detected from the frame preamble, so a mixed or Pentair-only bus needs no extra configuration.
- **Spa-side remote support.** Dual Spa Switch and Spa Link spa-side remotes are decoded, emulated, and surfaced in the web UI (button presses and LED state).
- **Chlorinator / AquaRite support.** AquaPure and AquaRite salt-water generators report salt PPM, output percentage, and status, including boost mode.
- **Serial connectivity options.** Connect over a physical USB-to-RS485 adapter, or over the network to a remote serial server using RFC2217 or a raw TCP stream.
- **Record and replay.** Capture live serial traffic to a `.cap` file with `--record-serial`, then replay it offline. Recording can also be started and stopped at runtime from the diagnostics view.
- **Matter (smart-home) bridge.** Expose the pool equipment to Apple Home, Google Home, Amazon Alexa, and Samsung SmartThings over Matter. The bridge runs as a Node.js sidecar and is **on by default**; opt out with `--matter false` (config-file key `matter = false`). See [docs/MATTER.md](docs/MATTER.md).
- **MQTT integration with Home Assistant auto-discovery.** Publish system, pool, and per-body state to an MQTT broker, and have entities appear in Home Assistant automatically via the MQTT discovery payload. Command topics route back to the equipment.
- **Web UI.** An Alpine.js single-page app for monitoring and controlling equipment, served by the built-in HTTP server.
- **HTTP REST API and WebSocket protocol.** Read and control equipment over HTTP routes (under `/api/...`) and stream live updates over WebSocket. The OpenAPI spec is served at `GET /api/swagger.yaml`.
- **Health check endpoints.** `GET /api/health` is an unauthenticated **liveness** probe returning `{"status":"ok","uptime_seconds":N}`. The Docker image ships a `HEALTHCHECK` that polls it, so orchestrators (Docker, Kubernetes) can detect and restart a wedged container. It stays reachable without credentials even when `--api-auth-token` is set, and carries no sensitive data. A richer `GET /api/health/detailed` adds a **readiness** view — overall readiness (`200` when configured, `503` while starting), uptime, version, and per-subsystem checks (configuration + equipment validation, equipment count, MQTT connectivity); it is gated by the bearer-token policy since it exposes internal state.
- **Prometheus metrics.** Scrape runtime metrics from `/metrics` in the Prometheus text exposition format.
- **History database.** Enable a time-series store with `--history-db` and query it via `GET /api/history/series` (list series, or fetch bucket-averaged points for one series). The Trends page charts this history.
- **Scheduler.** Run time-based automation from a schedules file with `--schedules-file`, managed over a CRUD API: `GET`/`POST /api/schedules` (POST creates a schedule, returning `201` plus the new entity) and `GET/PUT/DELETE /api/schedules/{uuid}`.
- **Preferences and equipment cache.** Persist user preferences with `--preferences-file` (exposed at `/api/preferences`) and cache discovered equipment with `--equipment-cache-file`. When the respective file option is empty, the feature stays in memory only or disabled.
- **Alerting.** Raise alerts on low salt and serial communications loss, and POST every alert transition to a webhook with `--alert-webhook-url`.
- **Configurable container user (`PUID`/`PGID`).** The Docker image runs the app as a configurable uid:gid instead of a hard-coded `10000`. Set `PUID`/`PGID` (defaults `10000`); the entrypoint starts as root, remaps its user, chowns `/data`, then drops privileges to that user via `gosu`. `PUID=0` keeps it running as root. Bind-mounted read-only config still needs to be readable by the chosen uid on the host.

### Changed

- **API access control is opt-in.** The HTTP/WebSocket control plane can require a bearer token, restrict origins to an allow-list, and demand a CSRF header on state-changing requests. All of these are **off by default**, preserving the historical no-auth behavior unless you enable them (for example, by passing `--api-auth-token`).

### Security

- **The server binds to localhost by default and authentication is off.** With auth disabled, anyone who can reach the bound address can read and control your pool equipment. Enable `--api-auth-token` and bind only to trusted networks before exposing the server beyond localhost.
- **MQTT credentials are sent in cleartext without TLS.** Use TLS for any broker that is not on a fully trusted local network, and never use `--mqtt-tls-skip-verify` in production.
