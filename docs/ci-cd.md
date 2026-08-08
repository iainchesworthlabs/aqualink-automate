# CI/CD pipelines and self-hosted runners

*For maintainers operating the GitHub Actions pipelines. This is the source of truth for the workflow set, their triggers and jobs, the reusable build workflow, and self-hosted runner configuration. The release process these pipelines run is described in [docs/releasing.md](releasing.md); the branch and PR rules that drive CI triggers are in [CONTRIBUTING.md](CONTRIBUTING.md).*

## Workflow set

The table below is the complete inventory of everything under `.github/` — the core build/test/release pipeline, the scanners, and the self-contained supporting workflows, plus the repo's composite actions. Every workflow has a section below.

| File | Kind | Purpose |
|------|------|---------|
| `.github/workflows/ci.yml` | Workflow | Build, test, e2e, and Docker verification on every push and PR. |
| `.github/workflows/_build.yml` | Reusable workflow (`workflow_call`) | The shared configure/build/test/package matrix. Called by both `ci.yml` and `release.yml`. |
| `.github/workflows/release.yml` | Workflow | Build packages, publish the Docker + Home Assistant add-on images, create the GitHub release, and publish the APT/DNF repos for a `v*` tag. |
| `.github/workflows/publish-repos.yml` | Reusable workflow (`workflow_call`) + dispatch | Publish the GPG-signed APT/DNF package repos to `gh-pages`. Called by `release.yml` after the release is created; dispatchable to (re)publish a tag. |
| `.github/workflows/auto-tag.yml` | Workflow | Opt-in (repo variable `AUTO_TAG_ENABLED`): advances the prerelease counter tag on a code push to `main`, firing `release.yml`. Off by default — tags are pushed by hand. |
| `.github/workflows/suggest-version.yml` | Workflow | Advisory next-version suggestion on the `develop` → `main` release PR (sticky comment). Never creates a tag. |
| `.github/workflows/automated-codescanning.yml` | Workflow | CodeQL (C/C++ and JavaScript/TypeScript), SonarCloud, and MSVC static analysis on PRs and a weekly cron. |
| `.github/workflows/cleanup-branch-caches.yml` | Workflow | Delete a PR's branch caches when it closes. |
| `.github/workflows/trivy.yml` | Workflow | Trivy scan of the runtime container image (OS packages + Node deps) → Security tab. |
| `.github/workflows/osv-scanner.yml` | Workflow | OSV-Scanner CVE check of declared dependencies, incl. the vcpkg C++ manifest → Security tab. |
| `.github/workflows/scorecard.yml` | Workflow | OpenSSF Scorecard supply-chain posture grade → Security tab. |
| `.github/workflows/fuzzing.yml` | Workflow | Bounded libFuzzer run over the untrusted-input parsers (RS-485 decoders plus the JSON/config/web/auth inputs) on a weekly cron + parser-touching PRs. Feeds Scorecard's Fuzzing check. See [fuzzing.md](fuzzing.md). |
| `.github/workflows/dependabot-auto-merge.yml` | Workflow | Flags non-major Dependabot PRs for GitHub auto-merge; they land on `develop` once the required checks pass. |
| `.github/workflows/docs.yml` | Workflow | Build the MkDocs site (`mkdocs build --strict`) and publish it to the root of `gh-pages` on docs-affecting pushes to `main`. |
| `.github/workflows/homeassistant-addon.yml` | Workflow | Validate the Home Assistant add-on wrapper (YAML, shellcheck, translation coverage, generated-Edge-channel drift, version lock-step) on add-on-touching pushes and PRs. |
| `.github/workflows/ha-companion.yml` | Workflow | Validate the Home Assistant companion package (`homeassistant/companion/`): yamllint, entity references vs `entity-manifest.json`, and blueprint/package schema via `check_config` in the official Home Assistant container. |
| `.github/actions/setup-cpp-toolchain` | Composite action | Install the platform-appropriate compiler and build tools. |
| `.github/actions/setup-vcpkg-cache` | Composite action | Configure and restore the OS-keyed vcpkg binary cache. |
| `.github/actions/setup-msvc-env` | Composite action | Load the MSVC environment on self-hosted Windows runners (sources `vcvars64.bat`, exports the env delta to `$GITHUB_ENV`). |

`ci.yml` and `release.yml` share one build definition (`_build.yml`), so the suites that run on a PR and the suites that gate a release can never drift.

## ci.yml

`ci.yml` builds on push only for the long-lived branches (`main`, `develop`); every other branch is exercised through its pull request into `develop` or `main`.

### Triggers

```yaml
on:
  push:
    branches: ["main", "develop"]
  pull_request:
    branches: ["develop", "main"]
```

Only `main` and `develop` build on push. Feature branches build via their `pull_request` into `develop`/`main` (the `pull_request` trigger targets those two branches). A WIP push to a branch that has no open PR runs no CI at all — and a push to a branch that *does* have a PR is already covered by the PR run, so this also avoids the push+PR double-run.

Concurrency is keyed on the PR head branch (or the ref for branch pushes) with `cancel-in-progress: true`, so a new push supersedes an in-flight run for the same PR or branch.

### Jobs

| Job | Runs on | What it does |
|-----|---------|--------------|
| `changes` | `ubuntu-latest` | Classifies the change via the GitHub API: `docs/**`, `mkdocs.yml`, `docs.yml`, `LICENSE`, and `*.md` never affect the binary. The heavy jobs below run only when it reports `code == 'true'`; anything not clearly docs counts as code. |
| `branch-name` | `ubuntu-latest` | PR only. Validates the PR head branch matches `<type>/<name>` with an allowed commit type, failing a non-conforming name. `develop`/`main` heads are accepted so the `develop` -> `main` release-promotion PR passes, and `dependabot/**` heads are accepted because Dependabot's branch names are not configurable (see [Dependabot auto-merge](#dependabot-auto-merge-dependabot-auto-mergeyml)). A **required** status check on `develop`/`main`, so a misnamed branch cannot merge. |
| `build-and-test` | Per-OS matrix (see [_build.yml](#_buildyml)) | Calls `_build.yml` with no packaging. Configures, builds, and runs the full test suite on Linux, Windows, and macOS. |
| `e2e-ui` | Linux | Downloads the install tree uploaded by `build-and-test` (no recompile), then runs the Playwright UI suite once per mode (see the mode table below). |
| `matter-bridge` | Linux | Node job in `matter-bridge/`: `npm ci`, typecheck (including the matter.js bridge), build, and unit tests. |
| `i18n-catalogs` | `ubuntu-latest` | Runs `scripts/check-i18n-keys.ps1`: every key referenced in web-UI code exists in `en.js`, and every shipped locale catalog has exact key + placeholder parity with English (see `docs/i18n.md`). Part of the `ci-status` aggregate. |
| `platform-macros` | `ubuntu-latest` | Runs `scripts/check-os-macros.ps1`: no OS preprocessor macro may gate shared, non-platform source (see [platform-isolation.md](platform-isolation.md)). Part of the `ci-status` aggregate. |
| `version-check` | `ubuntu-latest` | PR-into-`main` only. Compares what `git describe` resolves on the PR head versus the base. **Blocking at the job level** — when the resolved version is unchanged it emits `::error::` and `exit 1`, failing the job. It is, however, intentionally excluded from the `ci-status` aggregator's `needs` list, so a failed `version-check` does not by itself fail the aggregated `ci-status` required check. |
| `docker-verify` | Linux | Builds the `ci` and `runtime` Docker targets, smoke-tests the runtime image with `--version`, and asserts the Matter sidecar is bundled. |
| `ci-status` | `ubuntu-latest` | The aggregated **required** status check (alongside `branch-name`). Runs `if: always()` over every job above except `branch-name` and `version-check`, and fails only when one of them reported `failure`/`cancelled` — skipped jobs (docs-only changes, fork-gated jobs) count as OK, so a required check is never left pending. |

**Docs-only gating.** `build-and-test`, `e2e-ui`, `matter-bridge`, `version-check`, and `docker-verify` are all gated on `changes` reporting `code == 'true'`, so a docs-only change skips the whole build/test matrix while `ci-status` still reports success. `branch-name`, `i18n-catalogs`, and `platform-macros` run regardless (cheap, source-only checks).

**e2e-ui modes.** The Playwright runs mirror the modes encoded in `playwright.config.ts`, selected by environment variable. The three identity specs each run in their own step because they have incompatible auth-store preconditions (auth.spec needs an empty store for the first-run wizard; admin.spec self-seeds an admin; guest.spec configures the Guest scope), so each must boot against a fresh `--auth-state-dir`:

| Run | Env set | Specs exercised |
|-----|---------|-----------------|
| default (unauthenticated) | none | every spec except the identity ones |
| identity — first-run + sessions | `AQUALINK_AUTH_MODE=enabled`, `npx playwright test e2e/auth.spec.ts` | setup wizard, login/logout, session management |
| identity — admin management | `AQUALINK_AUTH_MODE=enabled`, `npx playwright test e2e/admin.spec.ts` | users/groups/entitlements/API-key admin UI |
| identity — guest mode + kiosk PIN | `AQUALINK_AUTH_MODE=enabled`, `npx playwright test e2e/guest.spec.ts` | anonymous guest browsing, affordance gating, kiosk PIN |
| history enabled | `AQUALINK_HISTORY_DB` | recorded series + chart |
| scheduler enabled | `AQUALINK_SCHEDULES_FILE` | schedule CRUD |

The remaining modes (TLS, bootstrap admin, open bind, MQTT, and the persistence modes) run in `automated-codescanning.yml`'s coverage sweep, which is a superset of this table — see [automated-codescanning.yml](#automated-codescanningyml).

**docker-verify checks.** It builds the `ci` target, then the `runtime` target (tagged `aqualink-automate:verify`), runs `docker run --rm -e MATTER_ENABLED=false aqualink-automate:verify --version`, and confirms the bundled sidecar is present (`node --version` plus a test for `/opt/matter-bridge/dist/index.js`). Both Docker builds are wrapped in a bounded retry so a transient Docker Hub base-image timeout does not fail CI. This cold, from-source build is kept deliberately as the gate that the multi-stage `Dockerfile` + vcpkg toolchain still build end to end — the release path's `docker-publish` assembles its image from prebuilt install trees and skips this, so `docker-verify` is its safety net.

## _build.yml

`_build.yml` is the single definition of how the C++ project is configured, built, tested, and optionally packaged on each platform. It is a `workflow_call` reusable workflow — it has no triggers of its own and only runs when another workflow calls it.

### Inputs

| Input | Type | Default | Used by |
|-------|------|---------|---------|
| `do_package` | boolean | `false` | Also run `cpack`, the per-OS install/extract smoke test, and the package artifact upload. The release path sets this `true`. |
| `version_tag` | string | `""` | The resolved `v*` tag, threaded into configure as `DERIVED_VERSION_OVERRIDE` so the build stamps it deterministically (see below). Empty in CI. |
| `fetch_depth` | number | `1` | Checkout depth. CI uses `1`; releases pass `0` so `git describe` and the cut-from-main check see full history and tags. |
| `upload_installtree` | boolean | `false` | On Linux, run `cmake --install` and upload the relocatable FHS install tree(s) that `e2e-ui` and the assembled release image consume instead of recompiling. Both CI and the release path set this `true`. |

### The matrix

The job runs a four-row matrix with `fail-fast: false`, so one platform failing does not cancel the others:

| Name | Compiler | Configure preset | Build preset | Test preset | Package preset |
|------|----------|------------------|--------------|-------------|----------------|
| Linux GCC | `gcc-15` | `config-linux-gcc` | `build-linux-gcc` | `test-linux-gcc` | `pack-linux-gcc` |
| Linux GCC (arm64) | `gcc-15` | `config-linux-gcc-arm64` | `build-linux-gcc-arm64` | `test-linux-gcc-arm64` | `pack-linux-gcc-arm64` |
| Windows MSVC | `msvc` | `config-windows-msvc` | `build-windows-msvc` | `test-windows-msvc` | `pack-windows-msvc` |
| macOS Clang | `llvm` | `config-macos-llvm` | `build-macos-llvm` | `test-macos-llvm` | `pack-macos-llvm` |

These are the same presets you use locally — see [INSTALL.md](INSTALL.md).

The arm64 row builds **natively** on an aarch64 runner (no cross-compile / QEMU) so the `.deb`/`.rpm`/`.tgz` install on a Raspberry Pi and other arm64 hosts; CPack derives the package architecture from the target, so the packages are correctly labelled `arm64`/`aarch64`. Both Linux rows can upload their install tree (`installtree-linux-gcc` / `installtree-linux-gcc-arm64`, gated on the `upload_installtree` input): CI uploads only the x64 tree (for `e2e-ui`), while the release path uploads both so `docker-publish` can assemble the multi-arch image without recompiling.

Runner selection per row is `vars.RUNNER_LINUX` / `vars.RUNNER_LINUX_ARM` / `vars.RUNNER_WINDOWS` with a GitHub-hosted fallback (the arm64 row falls back to `ubuntu-24.04-arm`); macOS always runs on `macos-latest`. See [Self-hosted runners](#self-hosted-runners).

### Test scope

The `Test` step runs the full test preset with **no `-L` label filter**, so unit and integration tests run together in one pass. There is no separate fast/slow split in CI.

### Packaging (release path only)

When `do_package` is `true`:

1. On Linux, the whole configure/build/test/`cpack` sequence for the release path runs **inside a `gcc:15-bookworm` container** (glibc 2.36 — the Debian Bookworm / Raspberry Pi OS baseline), natively per arch, so the produced packages run on a stock Pi and every newer distro; the bundled gcc-15 libstdc++/libgcc keep the older-glibc base safe for the C++23 code. Windows and macOS run `cpack --preset=<package_preset>` on the host after the normal build.
2. Smoke-tests the package on a clean target — installs the `.deb` in a fresh `debian:bookworm` container (Linux), extracts the ZIP and runs the exe (Windows), or extracts the TGZ and runs the binary (macOS). Each asserts `aqualink-automate --version` succeeds. The Linux test deliberately uses a clean `debian:bookworm` rather than the build image: it catches both a missing runtime dependency and a regression to a too-new glibc/libstdc++ baseline (which a newer-distro test would silently pass) before the package is published.
3. Uploads the packages as an artifact named `packages-<configure_preset>` (for example `packages-config-linux-gcc`), with `retention-days: 30`.

### Version stamping

The version is threaded explicitly into the configure step: `_build.yml` passes `-DDERIVED_VERSION_OVERRIDE=${{ inputs.version_tag || '0.0.0-dev' }}` (so a CI build with no tag stamps `0.0.0-dev` rather than emitting a noisy `git describe` warning on the shallow checkout), and `cmake/GitVersionDerivation.cmake` honors `DERIVED_VERSION_OVERRIDE` over `git describe` (it is also how the Docker image is versioned, since the build context omits `.git`). Release callers pass the resolved tag as `version_tag`, so a `workflow_dispatch` build stamps the right version even though its tag does not exist yet (`github-release` pushes it at the very end) — no local pre-tagging is needed.

## release.yml

`release.yml` produces a release from a version tag or a manual dispatch.

### Triggers

```yaml
on:
  push:
    tags: ["v*"]
  workflow_dispatch:
    inputs:
      version:     # e.g. v1.0.0, v1.0.0-beta.1
      prerelease:  # boolean
      dry_run:     # boolean — build packages but create no release
```

There is **no `push` to `main` trigger** — pushing to `main` does not release. A release is cut either by pushing a `v*` tag or by running the workflow manually. Concurrency is grouped on `release-<version>` with `cancel-in-progress: false`, so an in-flight release is never cancelled by a second run of the same version.

### Job graph

```
resolve-version ──> build-packages (_build.yml) ──> docker-publish ──┬─> homeassistant-addon-publish
                                                                     └─> github-release ──> publish-repos (publish-repos.yml)
```

Every job after `build-packages` is skipped on a dry run (`is_dry_run == 'true'`); `build-packages` still runs so the pipeline can be validated end to end without publishing. A `cleanup-failed-prerelease` job additionally watches the graph with `if: always()` (below).

| Job | What it does |
|-----|--------------|
| `resolve-version` | Resolves the version from the trigger, validates the tag, and enforces the release guards (below). Also re-runs the Home Assistant add-on version lock-step check (`scripts/sync-homeassistant-addon-version.ps1 -Check`), so a release cannot ship a drifted add-on version. |
| `build-packages` | Calls `_build.yml` with `do_package: true`, `upload_installtree: true`, `fetch_depth: 0`, and the resolved `version_tag`. |
| `docker-publish` | Assembles and pushes the multi-arch `runtime` image from the prebuilt install trees, then smoke-tests the published image on both architectures. |
| `homeassistant-addon-publish` | Builds and pushes the per-arch Home Assistant add-on wrapper images (`homeassistant-amd64` / `homeassistant-aarch64`) on top of the just-published app image, attests them, and smoke-tests the pulled wrapper (bashio + `run.sh` present). |
| `github-release` | Pushes the tag for dispatch builds, downloads the package artifacts, and creates the GitHub release. |
| `publish-repos` | Calls `publish-repos.yml` (with `contents: write` and `secrets: inherit`) to sign and publish the APT/DNF repos from the just-created release — see [Release helpers](#release-helpers-auto-tagyml-suggest-versionyml-publish-reposyml). |
| `cleanup-failed-prerelease` | If a non-dry-run **prerelease** fails after its tag exists, deletes the orphaned tag — never a stable tag, and never one that already has a published release — so the same version can be re-tagged cleanly. |

### resolve-version guards

`resolve-version` rejects a bad release before any package is built:

- **Tag format.** The tag must match `^v[0-9]+\.[0-9]+\.[0-9]+(-(alpha|beta|rc)\.[0-9]+)?$` (for example `v1.2.3` or `v1.2.3-rc.1`).
- **Cut from main.** Real releases must be contained in `origin/main` (`git merge-base --is-ancestor` against the fetched `origin/main`). `develop` is the integration branch; only `main` holds released code. Dry runs are exempt so the pipeline can be validated from any branch.
- **Existing tag.** On a `workflow_dispatch` (non-dry-run), the job fails fast if the tag already exists, so a collision is caught before building every package rather than at the final push.

### docker-publish

This job runs only on Linux when not a dry run, with `packages: write` (GHCR push) plus `id-token: write` + `attestations: write` (attestation signing).

1. Logs in to GHCR, and to Docker Hub only if `DOCKERHUB_USERNAME` is set (so fork PRs and credential-less runs still build anonymously).
2. Derives tags from `docker/metadata-action`: `type=sha`, the full semver, `major.minor`, a raw `latest` tag enabled **only when the release is not a prerelease**, and a raw `edge` tag enabled **only for prereleases** (the floating prerelease channel — see the tag table in [releasing.md](releasing.md)).
3. Downloads the per-arch install trees uploaded by `build-packages` and **assembles** the multi-arch (`linux/amd64` + `linux/arm64`, via QEMU) `runtime-assembled` target from them — no C++ recompile; only the Matter sidecar's TypeScript builds — pushing to GHCR (and Docker Hub when credentials are present), wrapped in a bounded retry (up to 3 attempts) so a transient Docker Hub CDN timeout does not fail the release. `--metadata-file` records the pushed image-index digest. (CI's from-source `docker-verify` remains the cold-build gate for the `runtime` target itself.)
4. **Attests the image by digest:** a keyless [build-provenance attestation](https://docs.github.com/actions/security-guides/using-artifact-attestations) (`actions/attest-build-provenance`, Sigstore/OIDC) and an SPDX **SBOM** attestation (`anchore/sbom-action` → `actions/attest-sbom`), both `push-to-registry: true` so they attach to the image in GHCR. Attesting by digest (not tag) binds the proof to the exact bytes pushed.
5. Runs a post-push smoke test: pulls the GHCR image and asserts `--version` contains the resolved version, for **both** architectures (arm64 under QEMU). Because `buildx build --push` pushes straight to the registry, this genuinely exercises the published artifact rather than a local cache.

### github-release

This job runs only when not a dry run, with `contents: write` (tag + release) plus `id-token: write` + `attestations: write` (attestation signing).

1. For a `workflow_dispatch` build, it pushes the resolved tag to `origin` at the end — the tag is created here, not at the start.
2. Downloads every `packages-*` artifact from `build-packages` (`merge-multiple: true`).
3. **GPG-signs the artifacts** (gated on `REPO_GPG_PRIVATE_KEY`, the repo-signing key): a detached `.asc` per binary, a signed `SHA512SUMS` manifest, and the exported public key, all added to the release. No-ops if the key is unset.
4. **Attests the packages:** a keyless build-provenance attestation and an SPDX SBOM attestation over the shipped binaries (the SBOM is also uploaded as a release asset). Verified by consumers with `gh attestation verify <file> --repo iainchesworth/aqualink-automate`.
5. Runs `gh release create <tag> --generate-notes`, adding `--prerelease` when the release is a prerelease, and attaches the downloaded package files, signatures, checksums, and SBOM.

Both attestation mechanisms are described end-to-end for consumers in [SECURITY.md > Verifying build authenticity](SECURITY.md#verifying-build-authenticity).

See [docs/releasing.md](releasing.md) for the operator-facing release walkthrough.

## Release helpers (auto-tag.yml, suggest-version.yml, publish-repos.yml)

Three small workflows surround `release.yml`. None is part of the PR gate.

### suggest-version.yml

Propose-only. On a `develop` → `main` release PR (`pull_request` into `main`, types opened/synchronize/reopened) it runs `scripts/next-version.sh --markdown` against the full history + tags and upserts the result as a sticky PR comment plus a job summary. It **never creates or pushes a tag** — the human still decides the version and pushes it (see the pre-release checklist in [releasing.md](releasing.md)). The same script is runnable locally: `scripts/next-version.sh`.

### auto-tag.yml

Opt-in tag advancement. It triggers on every push to `main`, but the single job runs **only when the repo variable `AUTO_TAG_ENABLED` is `"true"`** — by default it does nothing and versions are tagged by hand. When enabled, it advances **only the prerelease counter** (`vX.Y.Z-<channel>.N` → `.N+1`, channel from `AUTO_TAG_CHANNEL`, default `beta`) and pushes the tag, which fires `release.yml` exactly like a manual tag push. Everything else stays a human decision by design: it refuses to invent a first tag, bump the base version, or switch channels, and it skips when there are no new commits since the last tag or when the delta is docs-only (same classifier as `ci.yml`'s `changes` job — a docs merge must not trigger a redundant binary release).

### publish-repos.yml

Publishes the GPG-signed **APT** (reprepro) and **DNF** (createrepo_c) package repositories to the `gh-pages` branch, so users can `apt install` / `dnf install` and receive upgrades. It is a reusable workflow invoked by `release.yml`'s `publish-repos` job right after the GitHub release is created — a `release: published` trigger would never fire, because GitHub suppresses events for actions performed with the default `GITHUB_TOKEN` — and it is also dispatchable with a `tag` input to (re)publish an existing release. It downloads the release's `.deb`/`.rpm` assets, signs each `.rpm` in place (`rpm --addsign`; the DNF repo advertises `gpgcheck=1`, a per-package check), builds both repos, signs the repo metadata, and publishes with `keep_files: true` so previously published versions accumulate. **It no-ops until the `REPO_GPG_PRIVATE_KEY` secret is configured** — the one-time setup and operator walkthrough live in [releasing.md](releasing.md).

`docs.yml` shares the `gh-pages` branch with this workflow: the rendered docs site owns the root `index.html` and page tree, the package machinery owns `apt/`, `rpm/`, `key.gpg`, and the `install-*.sh` scripts. The filename sets are disjoint and both publish with `keep_files: true`, so neither clobbers the other (see [Docs site](#docs-site-docsyml)).

## automated-codescanning.yml

Code scanning is slow (each scanner does a full compile), so it runs only where code is genuinely new.

### Triggers

```yaml
on:
  push:
    branches: ["main"]
  pull_request:
    branches: ["develop", "main"]
  schedule:
    - cron: '42 21 * * 2'   # weekly, Tuesday 21:42 UTC
  workflow_dispatch:
```

Scanning happens on PRs into `develop` or `main`, once per merge to `main`, on the weekly cron, or on demand. The `push: [main]` trigger exists to keep the **default-branch security baseline** current: SARIF uploaded from a `pull_request` run is recorded against the throwaway `refs/pull/N/merge` ref and never updates `main`'s baseline in the Security tab, so without it the baseline would depend solely on the weekly cron. (The old `push: [main, feature/**, bug/**]` trigger that re-scanned every feature push remains gone — the push run fires once per promotion merge.)

Both triggers carry a docs `paths-ignore` (`docs/**`, `**/*.md`, `mkdocs.yml`, `docs.yml`, `LICENSE`): docs-only changes touch no compiled code, and since none of these jobs is a required status check, a path-filtered (non-running) workflow does not block the PR — unlike `ci.yml`, which must use a gate job (`changes`) for the same purpose.

### Jobs

| Job | Runs on | Scanner |
|-----|---------|---------|
| `CodeScanning_CodeQL` | Linux | CodeQL (`c-cpp`). Runs its own full build, filters the SARIF to `src/**`, and uploads to the Security tab. |
| `CodeScanning_CodeQL_JS` | Linux (GitHub-hosted) | CodeQL (`javascript-typescript`, `build-mode: none`) for the Alpine.js web UI (`assets/web/scripts`, `sw.js`) and the TypeScript Matter sidecar (`matter-bridge/src`). No compile, so it runs on `ubuntu-latest` (not the self-hosted C++ fleet); `.github/codeql/codeql-config-js.yml` scopes it to first-party source (vendored/minified libraries and `i18n/` data excluded). |
| `CodeScanning_E2ECoverage` | Linux | Builds a gcov-instrumented `aqualink-automate` (`config-linux-gcc-coverage`), drives the full Playwright mode sweep against it (see below), and `gcovr`s the `.gcda` into a SonarQube coverage report (`coverage-e2e.xml`) uploaded as the `e2e-coverage-xml` artifact. |
| `CodeScanning_SonarCloud` | Linux | SonarCloud via build-wrapper over a coverage build (`config-linux-gcc-coverage`, `-DUSE_SONARQUBE=ON`). Runs its own full compile, produces the unit/integration coverage report, downloads the e2e report, and scans with **both** (`sonar.coverageReportPaths` comma-separated; Sonar merges line coverage). |
| `CodeScanning_MSVCCodeAnalysis` | Windows | MSVC code analysis (`NativeRecommendedRules.ruleset`) over the `config-windows-msvc` build; uploads SARIF. |

Each job has the same skip condition: it does not run for a `develop` -> `main` promotion PR, because that code was already scanned when it entered `develop`. Re-scanning the promotion is pure duplication.

**Coverage = unit + e2e.** The SonarCloud new-code coverage gate reflects what *both* test layers exercise. The unit/integration binary's coverage alone misses every line only the running app reaches (HTTP routes, WebSocket, MQTT, bootstrap), which read as uncovered and depressed the gate. `CodeScanning_E2ECoverage` instruments the app, drives the Playwright suite against it (the app exits cleanly on Playwright's `SIGTERM` — see `gracefulShutdown` in `playwright.config.ts` — so gcov flushes `.gcda`), and emits a second report. `CodeScanning_SonarCloud` `needs:` it and merges the two; if the e2e job fails, the scan still runs with unit-only coverage (it never drops the whole gate).

**The coverage mode sweep.** The job runs the six `e2e-ui` modes from [ci.yml](#ciyml) plus seven more that exist to reach code the default modes cannot. Every mode step is `continue-on-error` — this job harvests coverage, it is not a functional gate (`e2e-ui` is), so a spec flake must not discard coverage already written:

| Run | Env set | Exercises |
|-----|---------|-----------|
| TLS (https) | `AQUALINK_TLS=enabled` | the default (non-identity) suite over an HTTPS origin (self-signed cert auto-provisioned) — the server's TLS/SSL session path and certificate self-provisioning |
| bootstrap admin | `AQUALINK_AUTH_MODE=enabled`, `AQUALINK_BOOTSTRAP_ADMIN=…`, `AQUALINK_BOOTSTRAP_ADMIN_PASSWORD=…`, `npx playwright test e2e/admin.spec.ts` | headless `--bootstrap-admin` start-up provisioning; admin.spec then logs in against the seeded admin |
| open bind — warning | `AQUALINK_OPEN_BIND=warn` | non-loopback bind (0.0.0.0) with auth off → the open-control-plane start-up warning |
| open bind — ack | `AQUALINK_OPEN_BIND=ack` | as above plus `--insecure-no-auth` → the acknowledged-posture branch |
| MQTT (broker + HA discovery) | `AQUALINK_MQTT=enabled`, `npx playwright test e2e/mqtt.spec.ts` | the only e2e mode that exercises the MQTT layer: Playwright starts a loopback broker and the app runs `--mqtt --home-assistant` against it (MqttClient, MqttIntegration, MqttHub, HA auto-discovery) |
| equipment cache persisted | `AQUALINK_EQUIPMENT_CACHE=<file>` | the equipment-cache load-or-init + write-scheduling paths, skipped by the in-memory default |
| preferences persisted | `AQUALINK_PREFERENCES_FILE=<file>` | the preferences load/persist path against a real file rather than in-memory only |

## Supply-chain scanning (trivy.yml, osv-scanner.yml, scorecard.yml)

Three lightweight workflows cover the supply-chain surfaces the compile-heavy `automated-codescanning.yml` does not. Unlike the code scanners, they run on GitHub-hosted `ubuntu-latest` (free for public repos, no self-hosted runner), need no C++ build, and publish to **Security > Code scanning**. None fails a PR — a finding surfaces in the Security tab rather than blocking the merge.

| Workflow | Scanner | Covers | Triggers |
|----------|---------|--------|----------|
| `trivy.yml` | [Trivy](https://github.com/aquasecurity/trivy-action) | The runtime **container image**: Ubuntu base packages + NodeSource Node + the Matter sidecar's `node_modules`. Builds only the `runtime-base` Docker stage (no C++ `ci` compile), which is exactly that OS/Node surface. Reports fixable HIGH/CRITICAL. | PR/push touching `Dockerfile`, `matter-bridge/**`, etc.; weekly cron; dispatch |
| `osv-scanner.yml` | [OSV-Scanner](https://github.com/google/osv-scanner-action) | Declared **dependencies** vs. the OSV database. The only scanner that reads the **vcpkg C++ manifest** (`vcpkg.json`) — Dependabot has no vcpkg ecosystem — plus the npm lockfiles. Uses the upstream reusable workflow, which uploads its own SARIF. | PR/push touching `vcpkg.json`, `**/package-lock.json`, etc.; weekly cron; dispatch |
| `scorecard.yml` | [OpenSSF Scorecard](https://github.com/ossf/scorecard-action) | **Supply-chain posture**: Action SHA-pinning, branch protection, token permissions, dangerous-workflow patterns, dependency tooling. `publish_results: true` feeds the public Scorecard badge/API. | `branch_protection_rule`; push to `main`; weekly cron; dispatch |

Division of labour with the existing tooling: **CodeQL / SonarCloud / MSVC** scan first-party source; **Dependabot** *updates* the Actions + npm manifests (and raises its own vulnerability alerts for them); **OSV-Scanner** closes the vcpkg gap Dependabot cannot see; **Trivy** covers the image layers no source or manifest scanner reaches. The vcpkg-built C++ libraries inside the image carry no package metadata for Trivy to match, so OSV-Scanner (reading the manifest) is what covers them — the two do not overlap.

The two PR-facing workflows (`trivy.yml`, `osv-scanner.yml`) share the code scanners' promotion-PR skip and fork-PR gating. `scorecard.yml` does not run on `pull_request` at all (its checks are repository-level and need a token a fork PR lacks). The weekly crons are staggered (22:17 / 22:27 / 22:37 UTC) so they do not all contend at once.

### Scorecard hardening applied

Two Scorecard checks are satisfied structurally rather than per-finding:

- **Token permissions** — every workflow declares `permissions: {}` (deny-all) at the top level and re-grants the minimum `write` scope on the single job that needs it (e.g. `contents: write` on the tag/publish jobs, `actions: write` on the cache-cleanup job, `security-events: write` on the SARIF-upload jobs). A top-level write is what Scorecard penalizes; a narrowly job-scoped write is the recommended pattern and the residual per-job grants are the least privilege each job genuinely requires.
- **Pinned dependencies** — the external base images in the root `Dockerfile` (`ubuntu:26.04`, `node:26-bookworm-slim`) are pinned by `@sha256:` digest. `.github/dependabot.yml` carries a `docker` ecosystem entry so Dependabot advances those digests weekly — a digest pin without an update path would otherwise freeze the image onto a stale, unpatched base. (Internal `FROM <stage>` references and the GitHub-hosted runner toolchains are not digest-pinnable and are not flagged as real findings.)

## Fuzzing (fuzzing.yml)

`fuzzing.yml` runs the libFuzzer harnesses over the app's **untrusted-input parsers** — the RS-485 protocol decoders (Jandy + Pentair message deserialisers) plus the schedule/WebSocket JSON, MQTT payload, config, query-string, JWT, duration, and replay-line parsers — the one supply-chain surface the posture scanners above cannot reach, because it is a *runtime* property of first-party parsing code. It builds the `config-linux-llvm-fuzzing` preset (Clang + `-fsanitize=fuzzer,address`), seeds each harness's corpus (the decoder corpora come from the recorded `test/fixtures/**/*.cap` captures), and fuzzes each harness for a bounded time; a crash fails the job and uploads the reproducer as an artifact. This gives the OpenSSF Scorecard **Fuzzing** check a genuine signal rather than a posture tick.

- **Triggers:** weekly cron (`47 22 * * 1`, staggered after the supply-chain crons); `pull_request` touching a fuzzed parser's source (`src/jandy/**`, `src/pentair/**`, and the fuzzed `src/core/` subsystems) or the fuzzing scaffolding (`fuzz/**`, `cmake/Fuzzing.cmake`); and `workflow_dispatch` (with a `max_total_time` input). It is **not** a required gate — a bounded run is best-effort reassurance, but any crash it does find is a real bug.
- **Matrix:** one job per harness — ten of them, `fuzz-jandy-message` through `fuzz-replay-line` (the full list lives in [fuzzing.md](fuzzing.md)) — on the GitHub-hosted `ubuntu-latest` (or `vars.RUNNER_LINUX`), with the same fork-PR gating as the build jobs.
- A crash is handled per the bug-fix discipline: fix the decoder + add a regression test, never weaken the parser. Full harness/corpus/run docs: [fuzzing.md](fuzzing.md).

## cleanup-branch-caches.yml

This workflow keeps the Actions cache from filling up with stale per-PR entries.

- **Trigger:** `pull_request` with `types: [closed]`.
- **Action:** On `ubuntu-latest` with `actions: write`, it installs the `gh-actions-cache` extension and deletes every cache key scoped to the closed PR's merge ref (`refs/pull/<number>/merge`). Deletion failures do not fail the workflow.

## Dependabot auto-merge (dependabot-auto-merge.yml)

Routine dependency bumps merge themselves. `.github/dependabot.yml` raises one grouped weekly PR per ecosystem (GitHub Actions, npm root, npm `matter-bridge/`, Docker digests) against `develop`; `dependabot-auto-merge.yml` runs on each Dependabot PR and — for non-major updates — enables GitHub **auto-merge** (squash) via `gh pr merge --auto`. GitHub then performs the merge only once every **required** status check passes ("Branch Name", which exempts `dependabot/**` heads, and the aggregated "CI Status"), so nothing lands without the full build/test matrix going green.

- **Major bumps stay manual.** `dependabot/fetch-metadata` reports the *highest* update type in a grouped PR, so any group containing a semver-major update is left open for review. Digest-only bumps (the Docker group) carry no semver level and auto-merge.
- **SonarCloud is expected to fail on Dependabot PRs** (Dependabot-triggered runs receive Dependabot secrets, not repo secrets such as `SONAR_TOKEN`). It is not a required check, so it does not hold up the merge.
- The squash commit takes the PR title, which Dependabot already emits in Conventional Commit form (`ci:`/`build:` prefixes configured in `.github/dependabot.yml`).
- Requires the repository setting **Allow auto-merge** (Settings > General > Pull Requests), which is enabled on this repo.

## Docs site (docs.yml)

The documentation site (MkDocs Material — the published subset is selected by the `nav` + `exclude_docs` in `mkdocs.yml`) is built from the in-repo Markdown and published to the **root** of the `gh-pages` branch, so the docs home is the GitHub Pages landing page.

- **Triggers:** push to `main` touching `docs/**`, `overrides/**`, `mkdocs.yml`, `README.md`, or the workflow itself; plus `workflow_dispatch`.
- **Build:** `pip install "mkdocs-material[imaging]>=9.7"` (a floor, no upper cap) plus the Cairo imaging libraries for the social-cards plugin, then `mkdocs build --strict` — broken internal links and other warnings **fail the build** instead of shipping a broken page.
- **Linking to files outside `docs/`.** `--strict` resolves every relative link against the *documentation* tree, not the repository. A link like `../docker-compose.yml` resolves on GitHub but 404s on the published site, so MkDocs rejects it — use an absolute `https://github.com/iainchesworth/aqualink-automate/blob/main/…` URL for repo files, and likewise for docs deliberately held back by `exclude_docs` (`design/`, `refactoring/`, `i18n-scoping.md`). Note the asymmetry: an unresolvable link to a *file* is a build-failing `WARNING`, but a link to a *directory* or to an excluded page is only logged at `INFO` — it ships broken and silent. Read the whole build log, not just the failures.
- **The MkDocs 2.0 banner is not an error.** Recent Material releases print a red "Warning from the Material for MkDocs team" block about the upstream MkDocs 2.0 rewrite. It is advocacy written to stderr, is not a MkDocs warning, and does **not** count toward `--strict`; the "Aborted with N warnings" tally counts only the `WARNING -` lines. MkDocs 2.0 also cannot arrive unannounced — `mkdocs-material` itself pins `mkdocs<2,>=1.6`.
- **Publish:** `peaceiris/actions-gh-pages` with `keep_files: true`, preserving the APT/DNF package tree that `publish-repos.yml` owns on the same branch (`apt/`, `rpm/`, `key.gpg`, `install-*.sh`); the filename sets are disjoint, so neither workflow deletes the other's files. Its `publish-docs` concurrency group is separate from `publish-repos`, so a docs build and a package publish can run at the same time.

## Home Assistant add-on validation (homeassistant-addon.yml)

Validates the Home Assistant add-on wrapper — the repo-root `repository.yaml` plus the stable `aqualink-automate/` and generated `aqualink-automate-edge/` channels (see [homeassistant-addon.md](homeassistant-addon.md)) — whenever it changes: `push` to `main`/`develop` and `pull_request` into `develop`/`main`, path-filtered to the add-on files and their generator/sync scripts.

One `validate` job (GitHub-hosted, no third-party actions to pin) checks, in order:

1. **YAML parses** — `repository.yaml`, both channels' `config.yaml`, and every translation catalog.
2. **The Edge channel is generated, not hand-edited** — re-runs `scripts/gen-homeassistant-edge-addon.ps1` and fails on any diff, so the two channels can never drift by hand.
3. **Options translations cover `config.yaml`** — every locale must document exactly the schema keys (`configuration:`) and ports (`network:`), no missing and no extra (mirrors the web UI's i18n key check).
4. **`run.sh` passes shellcheck.**
5. **Version lock-step** — each channel's `config.yaml` `version` equals every `build.yaml` base-image tag, via `scripts/sync-homeassistant-addon-version.ps1 -Check` (the same script the release process uses as the single writer in set mode, and which `release.yml` re-checks — so CI and release can never disagree).

The add-on wrapper **images** are published by `release.yml`'s `homeassistant-addon-publish` job, not by this workflow.

## Home Assistant companion package validation (ha-companion.yml)

Validates the Home Assistant companion package — the blueprints, helpers package, and dashboard under `homeassistant/companion/` (see [homeassistant-companion.md](homeassistant-companion.md)) — on `push` to `main`/`develop` and `pull_request` into `develop`/`main`, path-filtered to the companion tree, `scripts/check-ha-companion-entities.ps1`, and the workflow itself.

One `validate` job (GitHub-hosted) checks, in order:

1. **YAML lint** — `yamllint` with the relaxed profile and line-length off (Home Assistant YAML carries custom tags like `!input` and long Jinja lines).
2. **Entity references match the manifest** — `scripts/check-ha-companion-entities.ps1`: every active `*.aqualink_automate_*` reference in companion YAML must exist in `entity-manifest.json`; install-specific (panel-label) entities may only appear in commented examples or as blueprint inputs. The manifest itself is locked to `src/core/mqtt/ha_discovery.cpp` by `TestSuite_HaDiscovery_CompanionManifest` in the unit suite, closing the loop code → manifest → shipped YAML.
3. **Real schema validation** — assembles a scratch config (the helpers package, every blueprint instantiated by the `test-harness/` automations, and the documented `history_stats` recipes) and runs `hass --script check_config` in the official Home Assistant container. The step fails on a non-zero exit **or any logged ERROR line**: `check_config` exits 0 when a blueprint expands into an invalid automation, so the exit code alone is not a sufficient gate.

The companion **bundle** (`aqualink-automate-homeassistant-companion-<version>.zip`) is shipped by `release.yml`, not by this workflow — see [releasing.md](releasing.md).

## Self-hosted runners

Every Linux and Windows job picks its runner from a repository variable, falling back to GitHub-hosted runners when the variable is unset. macOS always runs on `macos-latest` (GitHub-hosted).

### Repository variables

Set these under **Settings > Variables > Actions**. Each value is a JSON array of runner labels.

| Variable | Type | Example | Applies to |
|----------|------|---------|------------|
| `RUNNER_LINUX` | JSON label array | `["self-hosted","linux","x64"]` | x64 Linux rows of `_build.yml`, `e2e-ui`, `matter-bridge`, `docker-verify`, `docker-publish`, `homeassistant-addon-publish`, the fuzzing matrix, CodeQL, E2ECoverage, SonarCloud |
| `RUNNER_LINUX_ARM` | JSON label array | `["self-hosted","linux","arm64"]` | The arm64 Linux row of `_build.yml`. Falls back to the GitHub-hosted `ubuntu-24.04-arm` (free for public repos). |
| `RUNNER_WINDOWS` | JSON label array | `["self-hosted","windows","x64"]` | Windows row of `_build.yml`, MSVC code analysis |

### Fallback

The pattern in the workflows is `${{ fromJSON(vars.RUNNER_LINUX || '["ubuntu-latest"]') }}` (and the Windows equivalent). If the variable is unset, or the self-hosted runners go offline, jobs fall back to the GitHub-hosted runners automatically. To force the fallback, remove the repository variables.

Self-hosted jobs also run extra steps the hosted jobs skip — they clean the workspace, point vcpkg at a persistent `~/.cache/vcpkg`, and (on Windows) load the MSVC environment with the local `./.github/actions/setup-msvc-env` composite action (pure PowerShell; sources `vcvars64.bat` and exports the env delta to `$GITHUB_ENV`).

### Provisioning

Runner VM images are built with Packer under `cicd/packer/`. See [cicd/packer/README.md](https://github.com/iainchesworth/aqualink-automate/blob/main/cicd/packer/README.md) for the full provisioning, deployment, and registration procedure.

The Linux runner base is **Ubuntu 26.04 LTS** (GCC 15, Clang/LLVM 21), provisioned by `cicd/packer/linux-runner.pkr.hcl` (boots `ISOs/ubuntu-26.04-autoinstall.iso`) and the `cicd/packer/scripts/linux/0{2,3}-*-toolchain.sh` scripts. Both the Architecture table in `cicd/packer/README.md` and the Packer template agree on this base. The Windows runner base is Windows Server 2022.

The Linux image is hardened against OS-disk creep: Docker's `data-root` **and** containerd's `root` live on the dedicated `/data` disk, `~/.cache` and `~/.sonar` are symlinked into the size-capped `/data/cache`, `unattended-upgrades` is removed (background dpkg runs also raced CI's own `apt-get` calls), and the ephemeral supervisor's per-job reset additionally deletes superseded self-updated runner-agent versions and runs apt hygiene (`apt-get clean`, wipe `/var/lib/apt/lists`). See `cicd/packer/README.md` ("Disk layout & the pristine ephemeral model").

## Caching

### vcpkg binary cache

The `setup-vcpkg-cache` composite action restores the vcpkg binary cache. It is keyed by runner OS, not by job:

```text
vcpkg-${{ runner.os }}-<vcpkg-commit-hash>-<hashFiles(vcpkg.json, vcpkg-configuration.json, triplets, overlays)>
```

Because OS maps 1:1 to the triplet in use, jobs that share a triplet — `build-and-test`, CodeQL, and SonarCloud all use `x64-linux-gcc` — share one cache instead of keeping four disjoint copies. The action still accepts a `cache-key-prefix` input for backward compatibility, but it is **deprecated and ignored**: it no longer affects the binary cache key.

Self-hosted runners use a persistent on-disk `~/.cache/vcpkg/archives` instead of the `actions/cache` restore, set up by the inline "Setup vcpkg cache (self-hosted)" steps.

### Compiler cache (ccache)

ccache is used inside the Docker builds (the in-Dockerfile cache mounts a `docker buildx` retry reuses) and is pre-installed on the self-hosted runner images (5 GB, compression enabled — see `cicd/packer/README.md`). The host-side CMake build steps in `_build.yml` rely on the vcpkg binary cache rather than a separate Actions-cached ccache.

### setup-cpp-toolchain behavior

The toolchain action installs only what each platform needs:

- **Linux:** installs the named compiler (`gcc-15` in CI) plus CMake and Ninja via `aminya/setup-cpp`.
- **Windows:** installs only CMake and Ninja; MSVC is assumed present (baked into the runner image).
- **macOS:** `brew install llvm cmake ninja` and prepends the Homebrew `llvm` to `PATH`, so the macOS row targets Clang 21 from Homebrew.

On self-hosted runners this action is skipped entirely — the toolchain is provisioned into the VM image.
