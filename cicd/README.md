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
| `packer/` | Packer templates + provisioning scripts that bake the self-hosted GitHub runner images (Linux & Windows). See [`packer/README.md`](packer/README.md). |
| `repo/` | End-user APT/DNF repository install scripts, published with the docs site. |

## Notes

- Packer variable files (`packer/*.pkrvars.hcl`) may contain secrets/infra
  details and are **git-ignored**; only the `*.pkrvars.hcl.example` templates
  are tracked.
- Self-hosted runner image build/publish details live in
  [`docs/ci-cd.md`](../docs/ci-cd.md) and [`docs/cicd-redesign.md`](https://github.com/iainchesworth/aqualink-automate/blob/main/docs/cicd-redesign.md).
