# Self-Hosted GitHub Actions Runners

Packer templates for building self-hosted GitHub Actions runner VMs on VMware vSphere (ESXi). These runners give fast CI (persistent vcpkg/ccache caches) and full control over the build environment, while staying **disk-bounded** and **pristine before every build**.

## Architecture

| Runner | Base OS | CPUs | RAM | OS disk | Data disk | Total | Pre-installed toolchain |
|--------|---------|------|-----|---------|-----------|-------|------------------------|
| Linux | Ubuntu 26.04 LTS | 32 | 48 GB | 12 GB | 18 GB | **30 GB** | GCC 15, Clang/LLVM 21, CMake, Ninja, Docker, ccache, SonarCloud build-wrapper |
| Windows | Windows Server 2022 | 32 | 48 GB | 30 GB | 16 GB | **46 GB** | VS 2026 Build Tools (MSVC 14.5x), CMake, Ninja, NSIS, ccache |

Disk sizes are deliberately small and **tunable** — they are single `disk_size` lines in
the `.pkr.hcl` templates, and the cache caps are `Environment=` overridable on the
supervisor. Linux lands at a tight 30 GB (the cache caps are set low to fit the work tree +
Docker images on the 18 GB data disk). Windows can't reach 30 GB: Windows Server + VS 2026
Build Tools alone is ~30 GB, so its OS disk floors there — bump the data disk if a build
runs short of space.

macOS CI stays on GitHub-hosted `macos-latest` runners.

## Disk layout & the pristine ephemeral model

The runners were previously single 150 GB disks that ran out of space and accumulated
cruft between jobs. Two design changes fix that:

- **Two thin disks per VM — OS is protected from build junk.** `disk0` holds only the OS
  + toolchains; `disk1` is a data volume (Linux `/data`, Windows `D:`) split into:
  - `work/` — the runner work dir (checkout + build tree), **wiped every job**;
  - `cache/` — the persistent **vcpkg binary cache + ccache + sonar-scanner cache**, kept
    across jobs but **size-capped** (ccache 3 GB; each vcpkg archive/download cache pruned
    to 3 GB / 2 GB; sonar 1 GB) so it can't grow without bound. `~/.cache` is redirected
    onto `cache/` (Linux symlink / Windows junction) so vcpkg and ccache land there with
    no workflow change; on Linux `~/.sonar` (which is *not* under `~/.cache`) is likewise
    symlinked to `cache/sonar`.
  - `docker/` + `containerd/` (Linux only) — Docker's `data-root` **and** containerd's
    `root`, so multi-GB base images/layers pulled by release builds never touch the OS
    disk (moving only Docker's data-root is not enough — buildx/BuildKit park content in
    containerd's own store); the supervisor prunes them each job.

  Build artifacts can therefore never fill the OS disk, and the thin vmdks stay near
  actual usage (`discard`/`fstrim` on Linux, `Optimize-Volume -ReTrim` on Windows).

- **Ephemeral runners that recycle themselves.** A supervisor (Linux `github-runner.service`,
  Windows `GitHubRunnerEphemeral` scheduled task) runs a loop: reset the workspace + Docker
  + temp + the runner's `_diag` logs, delete superseded self-updated agent versions
  (`bin.<ver>`/`externals.<ver>`, ~680 MB each on the OS disk), run apt hygiene
  (`apt-get clean` + wipe `/var/lib/apt/lists`), cap the caches, `fstrim`, mint a fresh
  registration token from an on-box
  credential, then register a one-shot `--ephemeral` runner and run **exactly one job**.
  After the job the runner deregisters and the loop recycles. Every build starts from a
  clean slate, and GitHub can never hand a runner a second job in a dirty state. Throughput
  is one job per VM at a time — clone more runners from the template to parallelise.

## Prerequisites

- [Packer](https://www.packer.io/) 1.9+
- VMware vSphere / ESXi with vCenter access
- `xorriso`, `curl`, and `sha256sum` (available in WSL/Linux) for the Ubuntu repack step
- Internet access — the OS ISOs are **downloaded on demand** at build time rather than
  stored in the repo:
  - Ubuntu 26.04 LTS Server source → fetched + repacked with autoinstall by `repack-iso.sh`
  - Windows Server 2022 evaluation → fetched directly by Packer from its default `iso_url`

## Configuration

### 1. Copy the example files

```bash
cd cicd/packer
cp variables.pkrvars.hcl.example variables.pkrvars.hcl
cp secrets.auto.pkrvars.hcl.example secrets.auto.pkrvars.hcl
```

### 2. Edit `variables.pkrvars.hcl`

Fill in your vSphere connection details and runner version. ISO sources are **not** set
here — each template defaults its own `iso_url` (Ubuntu → the local repack output,
Windows → the Microsoft eval download), so this shared file can't accidentally feed one
OS's ISO to the other build:

```hcl
vcenter_server     = "vcenter.example.com"
vcenter_username   = "administrator@vsphere.local"
vcenter_datacenter = "Datacenter"
vcenter_cluster    = "Cluster"
vcenter_datastore  = "datastore1"
vcenter_network    = "VM Network"
vcenter_folder     = "Templates"

github_runner_version = "2.321.0"
```

### 3. Edit `secrets.auto.pkrvars.hcl`

```hcl
vcenter_password = "your-vcenter-password"
ssh_password     = "packer"
winrm_password   = "Packer@2024!"
```

Both `*.pkrvars.hcl` files are gitignored. Only the `.example` files are committed.

## Building Templates

Initialize the Packer plugins (first time only):

```bash
cd cicd/packer
packer init linux-runner.pkr.hcl
packer init windows-runner.pkr.hcl
```

### Linux

Download the upstream Ubuntu source and repack it with autoinstall (run in WSL/Linux —
needs `xorriso`/`curl`/`sha256sum`). With no arguments it fetches the default Ubuntu
26.04 LTS source from releases.ubuntu.com, verifies its SHA256, caches it under `ISOs/`,
and writes `ISOs/ubuntu-26.04-autoinstall.iso`:

```bash
./repack-iso.sh
```

Then build — no `-var iso_url=` needed, the template defaults to the repack output:

```bash
packer build -force \
  -var-file=variables.pkrvars.hcl \
  -var-file=secrets.auto.pkrvars.hcl \
  linux-runner.pkr.hcl
```

### Windows

Packer downloads the eval ISO itself (into `packer_cache/`) from the default `iso_url`,
so there is nothing to pre-stage:

```bash
packer build -force \
  -var-file=variables.pkrvars.hcl \
  -var-file=secrets.auto.pkrvars.hcl \
  windows-runner.pkr.hcl
```

> If Microsoft refreshes the eval build, the bundled `iso_checksum` will mismatch and the
> download fails fast — update `iso_url`/`iso_checksum` in `windows-runner.pkr.hcl` (or
> override with `-var`) to match the current build.

This creates two VM templates in vCenter:
- `tpl-github-runner-linux`
- `tpl-github-runner-windows`

## Deploying Runners

### 1. Clone VMs from templates

In vCenter, clone each template to a new VM. Name them as you like (e.g. `github-runner-linux`, `github-runner-windows`).

### 2. Create the on-box credential (once)

Ephemeral runners re-register on **every** job, so each VM mints its own registration
tokens from a credential stored on the box — instead of a one-shot registration token that
expires in an hour. The supervisor accepts **either** of two credentials (it prefers the
App if both are present):

**Recommended — a GitHub App** (short-lived, scoped, revocable tokens; no long-lived secret
on the box):

1. Create a GitHub App (Settings → Developer settings → GitHub Apps → New). Give it the
   repository permission **Administration: Read and write** (what the registration-token
   endpoint requires); no webhook needed.
2. **Install** it on the `aqualink-automate` repo and note the **Installation ID** (in the
   install URL: `…/installations/<id>`).
3. Generate a **private key** (downloads a `.pem`). The supervisor signs an App JWT with it
   (via `openssl`), exchanges that for a short-lived installation token, and uses that to
   mint each registration token.
4. Note the **App ID** (on the App's settings page).

Base64-encode the key for guestinfo (single line, survives the RPC):
`base64 -w0 your-app.private-key.pem`.

**Fallback — a fine-grained PAT** scoped to just this repository with **Administration:
Read and write**, passed as `guestinfo.runner_pat`. Simpler, but a long-lived secret lives
on each runner — scope it to the single repo, least privilege, and rotate it.

### 3. Set guestinfo variables

Before booting, set these guestinfo properties on each VM (via vCenter > VM > Configure > vApp Options, or using `govc`):

| Variable | Value |
|----------|-------|
| `guestinfo.runner_repo` | `https://github.com/{owner}/aqualink-automate` |
| `guestinfo.runner_app_id` | **(App)** the App's ID |
| `guestinfo.runner_app_installation_id` | **(App)** the App's installation ID on the repo |
| `guestinfo.runner_app_private_key` | **(App)** the App `.pem`, base64-encoded (`base64 -w0`) |
| `guestinfo.runner_pat` | **(PAT fallback)** fine-grained PAT, repo Administration: read+write |
| `guestinfo.runner_name` | e.g. `linux-runner` or `windows-runner` |
| `guestinfo.runner_labels` | e.g. `self-hosted,linux,x64` or `self-hosted,windows,x64` |

Provide the App trio **or** the PAT (the App wins if both are set). The other keys are
required (name/labels default if unset).

> These key names must match exactly what the supervisor scripts read
> (`scripts/linux/08-github-runner.sh`, `scripts/windows/05-github-runner.ps1`).
> If `runner_repo` plus a credential is missing the supervisor
> logs a FATAL line and sleeps (it does **not** silently skip) — check
> `journalctl -u github-runner` (Linux) or the `GitHubRunnerEphemeral` task history (Windows).

Using `govc` (GitHub App credential):

```bash
govc vm.change -vm github-runner-linux \
  -e guestinfo.runner_repo="https://github.com/{owner}/aqualink-automate" \
  -e guestinfo.runner_app_id="123456" \
  -e guestinfo.runner_app_installation_id="78901234" \
  -e guestinfo.runner_app_private_key="$(base64 -w0 your-app.private-key.pem)" \
  -e guestinfo.runner_name="linux-runner" \
  -e guestinfo.runner_labels="self-hosted,linux,x64"
```

Or with the PAT fallback, replace the three `runner_app_*` lines with a single
`-e guestinfo.runner_pat="github_pat_XXXXXXXXXXXX"`.

### 4. Boot the VMs

On boot, the ephemeral supervisor reads the guestinfo variables, mints a registration
token, and brings up a one-shot `--ephemeral` runner. After each job it recycles (resets
the workspace, re-mints a token, re-registers) automatically — there is no `.registered`
marker and no manual re-registration.

- **Linux**: systemd service (`github-runner.service`) runs the supervisor loop
- **Windows**: scheduled task (`GitHubRunnerEphemeral`) runs the supervisor loop at startup

### 5. Enable in workflows

Set these **repository variables** in GitHub (Settings > Variables > Actions):

| Variable | Value |
|----------|-------|
| `RUNNER_LINUX` | `["self-hosted","linux","x64"]` |
| `RUNNER_WINDOWS` | `["self-hosted","windows","x64"]` |

Workflows automatically use self-hosted runners when these variables are set. Remove the variables to fall back to GitHub-hosted runners.

## What's Installed

### Linux Runner

| Script | Packages |
|--------|----------|
| `00-data-volume.sh` | Partitions/formats the data disk, mounts it at `/data` (`discard`), creates `work/` + `cache/` + `docker/` + `containerd/`, symlinks `~/.cache` → `/data/cache` and `~/.sonar` → `/data/cache/sonar` |
| `01-base-packages.sh` | build-essential, ca-certificates, curl, git, gpg, jq, pkg-config, tar, unzip, wget, zip; purges `unattended-upgrades` + masks the apt-daily timers (background dpkg runs race CI's apt calls and creep the OS disk); sets a default `DPkg::Lock::Timeout`; enables `discard` (continuous TRIM) on the root fs **and** sets `fstrim.timer` to hourly so the thin vmdk stays lean under churny CI |
| `02-gcc-toolchain.sh` | gcc-15, g++-15, gcov-15 (from 26.04 LTS main repos; PPA fallback only on older bases), update-alternatives symlinks |
| `03-llvm-toolchain.sh` | clang-21, clang-tidy-21, lld-21, libc++-21-dev, libc++abi-21-dev (from 26.04 LTS `universe`; apt.llvm.org fallback only on older bases), update-alternatives symlinks |
| `04-cmake-ninja.sh` | CMake 3.31.12 (Kitware binary), ninja-build |
| `05-docker.sh` | Docker Engine CE + buildx plugin; Docker `data-root` → `/data/docker` **and** containerd `root` → `/data/containerd` (both off the OS disk, both ordered after `data.mount`) |
| `06-dev-tools.sh` | python3, gcovr, autoconf, automake, libtool |
| `07-ccache.sh` | ccache (4 GB max on `/data/cache/ccache`, compression enabled) |
| `08-github-runner.sh` | GitHub Actions runner agent + **ephemeral supervisor** (`github-runner.service`) |
| `09-sonarcloud-tools.sh` | SonarCloud build-wrapper for Linux |
| `10-cleanup.sh` | apt clean, log truncation, `fstrim` (UNMAP) free-space reclaim to keep the thin vmdk small |

### Windows Runner

| Script | Packages |
|--------|----------|
| `00-data-volume.ps1` | Initializes/formats the data disk as `D:`, creates `D:\work` + `D:\cache`, junctions `C:\Users\runner\.cache` → `D:\cache` |
| `01-base-config.ps1` | TLS 1.2, Chocolatey, Git |
| `02-vs-buildtools.ps1` | VS 2026 Build Tools (MSVC 14.5x, Windows SDK 22621, MSBuild, ASan) — installs from the `aka.ms/vs/18/stable` channel to match the C++23 toolset developers build with locally |
| `03-cmake-ninja.ps1` | CMake + Ninja via Chocolatey |
| `04-choco-packages.ps1` | NSIS, 7zip, ccache (4 GB max on `D:\cache\ccache`, compression enabled) |
| `05-github-runner.ps1` | GitHub Actions runner agent + **ephemeral supervisor** (`GitHubRunnerEphemeral` task) |
| `06-cleanup.ps1` | DISM cleanup, temp removal |

## Maintenance

### Runner agent updates

The GitHub Actions runner agent auto-updates itself. No manual intervention needed.

### Rebuilding templates

When toolchain versions change, update the provisioning scripts and rebuild (re-run
`./repack-iso.sh` first for Linux — it reuses the cached source if present):

```bash
packer build -force -var-file=variables.pkrvars.hcl -var-file=secrets.auto.pkrvars.hcl linux-runner.pkr.hcl
```

Then deploy new VMs from the updated template and re-register.

### Reclaiming disk space

The ISOs are transient — fetched on demand and regenerated on the next build. Free the
disk between builds by emptying `ISOs/` and `packer_cache/` (both gitignored):

```powershell
./clean-isos.ps1 -WhatIf   # preview what would be removed
./clean-isos.ps1           # reclaim the space
```

WSL/bash equivalent:

```bash
rm -rf ISOs/* packer_cache/*
```

### Fallback to GitHub-hosted

Remove the `RUNNER_LINUX` and `RUNNER_WINDOWS` repository variables. Workflows automatically fall back to `ubuntu-latest` and `windows-latest`.

## File Structure

```
cicd/packer/
  linux-runner.pkr.hcl              # Packer template — Ubuntu 26.04 LTS
  windows-runner.pkr.hcl            # Packer template — Windows Server 2022
  variables.pkrvars.hcl.example     # vSphere variables (copy and fill in)
  secrets.auto.pkrvars.hcl.example  # Passwords (copy and fill in)
  repack-iso.sh                     # Download + repack the Ubuntu source ISO (autoinstall)
  clean-isos.ps1                    # Delete ISOs/ + packer_cache/ to reclaim disk
  http/
    linux/
      meta-data                     # cloud-init metadata
      user-data                     # cloud-init autoinstall config
    windows/
      autounattend.xml              # Windows unattended install
      install-vmtools.ps1           # VMware Tools silent installer
  scripts/
    linux/                          # Linux provisioning scripts (00-10; 00 sets up the data volume)
    windows/                        # Windows provisioning scripts (00-06; 00 sets up the data volume)
  ISOs/                             # Downloaded/repacked ISOs (gitignored; cleared by clean-isos.ps1)
  packer_cache/                     # Packer download cache (gitignored; cleared by clean-isos.ps1)
```
