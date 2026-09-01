# Self-hosted CI runners

Several Linux and Windows jobs across `.github/workflows/` can run on a self-hosted runner
instead of a GitHub-hosted one — but only when one is actually available right now, never as
an all-or-nothing switch. ("Available" means *online and idle* for the shared fleet, and
merely *online* for the two `big` runners — see
[The `big` pair](#the-big-pair-heavy-legs-only) for why those differ.) The two macOS-only and one arm64-only leg of
[`_build.yml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/.github/workflows/_build.yml)
always stay on GitHub-hosted runners; there's no self-hosted equivalent for either
(`RUNNER_LINUX_ARM` remains a static optional override for the arm64 leg — see below).

This page describes what this repo's CI does with a self-hosted runner once one exists. It
does not describe how one comes to exist — the fleet itself (Packer images, provisioning
scripts, the org they register against) lives in
[iainchesworthlabs/ci-runners](https://github.com/iainchesworthlabs/ci-runners), a repo
shared across every project in the `iainchesworthlabs` organization rather than owned by
this one.

## How the decision gets made

[`_check-runners.yml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/.github/workflows/_check-runners.yml)
is a small reusable `workflow_call` that decides a runner-label set for Linux and one for
Windows. Every workflow that can build on the fleet — `_build.yml` (and transitively
`ci.yml`'s `build-and-test` / `release.yml`'s `build-packages`), `ci.yml` directly
(`e2e-ui`, `matter-bridge`, `docker-verify`), `release.yml` directly (`docker-publish`,
`homeassistant-addon-publish`), `fuzzing.yml`, and `automated-codescanning.yml` — calls it
once as its own `check-runners` job and reads `needs.check-runners.outputs.linux_runner` /
`windows_runner` (or their `_big` counterparts) at every `runs-on:` and
`contains(..., 'self-hosted')` step-gate that used to read a static repository variable. One
definition, no duplicated bash across six callers.

Per OS, in order:

1. **Fork PRs always get GitHub-hosted**, no exceptions and no live check. Untrusted code
   must never land on self-hosted infrastructure — the runners are ephemeral (wiped between
   every job) but that only bounds damage *between* jobs, not *during* one.
2. **An explicit override wins next.** Repository variables `RUNNER_LINUX_MODE` and
   `RUNNER_WINDOWS_MODE` accept `auto` (the default, used whenever the variable is unset),
   `self-hosted`, or `github-hosted`. A forced mode skips the live check entirely — if you
   force `self-hosted` and nothing is actually online, the job queues and waits, which is
   the expected cost of an explicit override.
3. **`auto` runs a live check**, in two parts, either one sufficient:
   - This repo's own registered runners (`GET /repos/iainchesworthlabs/aqualink-automate/actions/runners`,
     using the workflow's own `GITHUB_TOKEN` — no extra setup). Empty until a runner is
     actually registered directly against this repo, which may never happen under the
     org-level model `ci-runners` uses.
   - `iainchesworthlabs`'s org-level runners (`GET /orgs/iainchesworthlabs/actions/runners`),
     only attempted if the optional repository secret `ORG_RUNNERS_TOKEN` is set — an
     org-scoped PAT or GitHub App token with "Self-hosted runners: read". This is what
     actually matters under the org migration and runner group in `ci-runners`; until the
     secret is provisioned it's simply skipped, not an error.

   Either check finding at least one runner that is `online`, not `busy`, and labelled with
   both `self-hosted` and the right OS (`Linux` or `Windows`) selects the self-hosted label
   set (`self-hosted, Linux, X64, shared` / `self-hosted, Windows, X64, shared` — the exact
   labels the `ci-runners` fleet registers with). The `shared` label is what keeps an ordinary
   leg off the big pair, which carries `big` instead; see
   [The `big` pair](#the-big-pair-heavy-legs-only). The `_big` outputs run the same three
   steps against a stricter label set and a looser liveness test — see below. Finding nothing, or the API call itself failing for
   any reason, falls back to GitHub-hosted (`ubuntu-latest` / `windows-latest`) — this check
   being unavailable is never a reason to block CI.

Until the repo is added to the runner group's "selected repositories" allow-list (and
`ORG_RUNNERS_TOKEN` is provisioned), every leg simply keeps building on GitHub-hosted
runners — a deliberate no-op, not a bug: the mechanism is inert until the infrastructure
side is wired up.

## The `big` pair (heavy legs only)

Two runners in the fleet are deliberately larger, and live in their own runner group
(`Labs CI Big Runners`, scoped to this repo alone) with an extra `big` label:

| Runner | vCPU | RAM | Data disk | Labels |
|---|---|---|---|---|
| `tf-gh-big-linux-runner-01` | 8 | 32 GB | `/data` 48 GB | `self-hosted, Linux, X64, big` |
| `tf-gh-big-windows-runner-01` | 8 | 24 GB | `D:` 48 GB | `self-hosted, X64, Windows, big` |

They exist because the heavy legs OOM-killed and filled the disks on the shared 12 GB /
16 GB shape. `_check-runners.yml` exposes them as two extra outputs, `linux_runner_big` and
`windows_runner_big`, resolved by the same fork-PR guard, the same `RUNNER_*_MODE` overrides
and the same repo-then-org live check as the ordinary outputs — with two deliberate
differences:

- **The probe requires the `big` label.** Without it the gate would pass because some
  *shared* runner is idle, and the job would then queue against a label nothing idle
  carries. Getting only half of this change (the `runs-on` array but not the probe) is the
  easy mistake; the probe and the emitted array must agree.
- **The probe requires `online` but *not* `busy == false`.** For the shared fleet, "something
  is idle" is a fair proxy for "there is capacity" across 8 Linux / 5 Windows runners. There
  is only *one* big runner per OS, so demanding idle would bounce a heavy leg to
  GitHub-hosted the moment the big runner picked up anything else — and GitHub-hosted is
  precisely where those legs die. Queueing behind the current job is strictly better than
  routing to a runner that cannot finish. A registered, online runner drains its queue; only
  an *absent* label waits forever, which is what the probe actually guards against.

### Which legs go there, and why not all of them

Three jobs currently ask for `big`:

| Job | Output | Why |
|---|---|---|
| `CodeScanning_CodeQL (c-cpp)` | `linux_runner_big` | The only leg with direct evidence it *cannot* complete on a GitHub-hosted runner: killed twice mid-build, once at 108 min (`exit 143`, at object 721 of 762) and once at 2h33m. Both are resource kills deep into a near-complete build, not flakes — a retry will not clear them. |
| `CodeScanning_MSVCCodeAnalysis` | `windows_runner_big` | The longest job in CI at ~140 min, so it *is* the critical path; it also filled the shared 16 GB `D:`. |
| `Build & Test / Windows MSVC` | `windows_runner_big` | Cannot reliably fit the shared 16 GB `D:`. Measured on `tf-gh-windows-runner-01` mid-job on 2026-08-31: a 10.1 GB build tree against a volume that also carries a ~2 GB cache, leaving 3.6 GB free at peak — and on 2026-08-30 `tf-gh-windows-runner-05` tipped over it with `fatal error C1085 ... No space left on device`. Nearly all of it is LTCG: `INTERPROCEDURAL_OPTIMIZATION_RELEASE` puts `/GL` on `libaqualink-{automate,jandy,pentair}`, so their objects carry intermediate language and each `.lib` archives a second copy — 4.5 GB of `.obj` plus 4.7 GB of `.lib` across just those three targets, against 0.35 GB of PDBs and 0.65 GB of `vcpkg_installed`. |

> **Windows `big` is now oversubscribed, and that is the live risk to watch.** Both Windows
> entries above queue on the *same single machine*, and one of them is the ~140 min
> `CodeScanning_MSVCCodeAnalysis`. Because the `_big` probe accepts a *busy* runner (see
> above), a `Build & Test / Windows MSVC` leg can now sit behind that scan rather than
> falling back to `windows-latest` — so the Windows build gate's worst-case wait went up,
> on every PR, in exchange for it no longer dying on a full disk. If that queue starts
> hurting more than the disk exhaustion did, the cheaper lever is `ENABLE_IPO=OFF`
> (`CMakeLists.txt:38`, declared before `project()` precisely so a preset can override it)
> on the CI configure: that reclaims ~9 GB, puts the leg back on the shared fleet, and
> costs only that PR CI stops exercising the LTCG code path. The other lever is a second
> big Windows runner — see the count knobs below.

Everything else stays where it is. That is a measured decision, not an oversight — routing
*every* heavy leg to `big` is slower, because the big pair is one machine per OS and jobs
pointed at it serialise:

| | Linux | Windows |
|---|---|---|
| Sum of heavy legs (GitHub-hosted timings) | 212 min | 221 min |
| All of them serialised on one big runner, at an assumed 1.5–2× per-job speedup | 106–141 min | 111–126 min |
| Today's critical path (longest single job, run in parallel) | ~108 min | ~140 min |
| **Only the longest leg moved to `big`, rest parallel on hosted** | **~54–72 min** | **~80 min** |

Moving everything trades parallelism for per-job speed and roughly breaks even; moving only
the critical-path job keeps the parallelism *and* shortens the longest pole. (That arithmetic
covers the *scanning* legs only, and it still describes Linux exactly. Windows has since
gained a second `big` tenant — `Build & Test / Windows MSVC`, routed there for disk headroom
rather than speed — so the Windows column now understates contention; see the note above.)

If a second big runner per OS is ever provisioned (`big_linux_runner_count` / `big_windows_runner_count` in
`ci-runners`' `terraform.tfvars`), re-run this arithmetic before widening the routing —
check host capacity first, as `esxi-00` is CPU-saturated under load.

> The 1.5–2× speedup is an *assumption* from the hardware delta (8 vCPU / 32 GB against
> hosted's ~4 vCPU / 16 GB), not a measurement. It is the number to verify first; if it does
> not hold, the table above changes.

### Two known sharp edges

- **~~A `big` runner also satisfies the plain array.~~ Fixed in `ci-runners`.** It carries
  `self-hosted, Linux, X64` as well as `big`, so an ordinary leg asking for the bare array was
  eligible for it — the probe counted 9 Linux runners, not 8. Under saturation that is worse
  than a delay: the big runner gets taken by ordinary work *and* the heavy leg's probe then
  falls back to GitHub-hosted, losing the big runner exactly when it was needed. The shared
  fleet now carries a `shared` label the big pair does not, and the ordinary outputs name it.
- **The vcpkg cache is never pruned.** `_build.yml`'s self-hosted `Clean workspace` step does
  `rm -rf build install` and does not touch `deps/vcpkg`; `buildtrees` + `downloads` +
  `packages` grow monotonically (measured at 6.2 GB, with OpenSSL's buildtree alone ~1.0 GB
  and `boost-test` next at 425 MB). This is what filled the disks on the old shared fleet. A
  48 GB data disk gives the big pair a longer runway, but it is a runway, not a fix — the
  same wall arrives later unless something prunes.

## Why the check, not a static switch

The earlier design — a single repository variable (`RUNNER_LINUX` / `RUNNER_WINDOWS`)
holding the literal runner-label array, read directly at each `runs-on:` — only answered
"has someone configured self-hosted for this leg", not "is a self-hosted runner actually
able to pick this job up right now". With `ci-runners`' fleet shared across every repo in
the org, "configured" and "available" can genuinely differ moment to moment, so the live
check is what keeps a leg from silently queuing behind another repo's job instead of
falling back. Those two variables are retired by this change; only the `_MODE` variables
and `RUNNER_LINUX_ARM` (the static arm64 override, unaffected — there's no self-hosted
arm64 runner in the current fleet to check against) remain.

## Provisioning

Runner VM images (Packer templates, provisioning scripts) live in
[iainchesworthlabs/ci-runners](https://github.com/iainchesworthlabs/ci-runners), shared
across the org — not in this repo's `cicd/`.
