# Self-hosted CI runners

Several Linux and Windows jobs across `.github/workflows/` can run on a self-hosted runner
instead of a GitHub-hosted one — but only when one is actually online and idle right now,
never as an all-or-nothing switch. The two macOS-only and one arm64-only leg of
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
`windows_runner` at every `runs-on:` and `contains(..., 'self-hosted')` step-gate that used
to read a static repository variable. One definition, no duplicated bash across six callers.

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
   set (`self-hosted, Linux, X64` / `self-hosted, Windows, X64` — the exact labels the
   `ci-runners` fleet registers with). Finding nothing, or the API call itself failing for
   any reason, falls back to GitHub-hosted (`ubuntu-latest` / `windows-latest`) — this check
   being unavailable is never a reason to block CI.

Until the repo is added to the runner group's "selected repositories" allow-list (and
`ORG_RUNNERS_TOKEN` is provisioned), every leg simply keeps building on GitHub-hosted
runners — a deliberate no-op, not a bug: the mechanism is inert until the infrastructure
side is wired up.

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
