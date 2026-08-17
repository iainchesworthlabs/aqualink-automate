# `cicd/` — build & infrastructure helpers

CI/CD for this project lives in **two** places:

- **`.github/workflows/`** — the GitHub Actions pipelines that actually run
  (build/test, release, docs, fuzzing, packaging, security scans). Start there
  for "what runs on a push/PR/tag". See [`docs/ci-cd.md`](../docs/ci-cd.md).
- **`cicd/`** (this directory) — the supporting scripts and machine images the
  workflows (and developers) invoke.

## Contents

| Path | What it is |
|------|------------|
| `build.sh` | Cross-platform (Linux/macOS) build helper — presets, compiler/type selection, optional packaging. |
| `build.ps1` | Windows build helper (same role, PowerShell). |
| `validate-arm64-local.sh` | Local ARM64 cross-build/package smoke check. |
| `repo/` | End-user APT/DNF repository install scripts, published with the docs site. |

## Notes

- **Self-hosted runner provisioning has moved.** The Packer templates and
  provisioning scripts that used to live here as `packer/` now live in
  [iainchesworthlabs/ci-runners](https://github.com/iainchesworthlabs/ci-runners),
  a repo shared across every project in the `iainchesworthlabs` organization
  rather than owned by this one (runners register at the org level, not
  per-repo). This repo only *consumes* the fleet — see the CI-side mechanics
  in [`docs/ci-self-hosted-runners.md`](../docs/ci-self-hosted-runners.md) and
  [`docs/ci-cd.md`](../docs/ci-cd.md).
