# Installing and building Aqualink Automate

*For anyone installing a release build or building Aqualink Automate from source. After installing, see [Configuration reference](configuration.md) and [Usage and API](usage-and-api.md) for how to run and use it.*

This is the single authoritative guide for getting Aqualink Automate onto a machine: install a pre-built package, build from source with the helper scripts, or run it in Docker.

## Contents

- [Pre-built binaries](#pre-built-binaries)
- [Building from source — prerequisites](#building-from-source--prerequisites)
- [Clone](#clone)
- [Build on Linux or macOS](#build-on-linux-or-macos)
- [Build on Windows](#build-on-windows)
- [CMake presets](#cmake-presets)
- [Manual CMake workflow](#manual-cmake-workflow)
- [Running tests](#running-tests)
- [Development container](#development-container)
- [Docker](#docker)

## Linux: APT / DNF repository (recommended)

On Debian / Ubuntu / **Raspberry Pi OS** (`amd64` or `arm64`), add the signed APT
repository — you then get `apt upgrade` updates and a managed `systemd` service:

```bash
curl -fsSL https://iainchesworth.github.io/aqualink-automate/install-apt.sh | sh
```

On Fedora / RHEL / openSUSE:

```bash
curl -fsSL https://iainchesworth.github.io/aqualink-automate/install-dnf.sh | sh
```

After installing, set your serial port in `/etc/aqualink-automate/aqualink-automate.conf`
and `sudo systemctl start aqualink-automate`. See the [Raspberry Pi guide](raspberry-pi.md).
There is also a [Docker image](#docker) (multi-arch: `linux/amd64` + `linux/arm64`).

### Viewing logs

By default logs go to the console, which each platform captures natively — no
configuration needed:

- **systemd**: `journalctl -u aqualink-automate`. Priorities are preserved, so
  `journalctl -u aqualink-automate -p warning` shows only warnings and above.
- **Docker**: `docker logs <container>` (whatever your log driver captures from
  stdout/stderr).

For a structured pipeline (Loki, CloudWatch, Elastic, …) add `--log-format json`
so every line is a single JSON object. To also keep a local rotating file, add
`--log-file /var/log/aqualink/app.log` (see the
[Logging options](configuration.md#logging-sinks)). The security **audit trail**
is separate — see the [Security guide](SECURITY.md).

## Pre-built binaries

Prefer the repository above on Linux; otherwise download the package for your platform
from the [GitHub Releases](https://github.com/iainchesworth/aqualink-automate/releases) page.

| Platform | Package types | Notes |
|----------|---------------|-------|
| Windows  | `.exe` (NSIS installer), `.zip` | Run the installer, or unzip the portable build anywhere. |
| Linux    | `.deb`, `.rpm`, `.tgz` | Built for **`amd64`** and **`arm64`** (Raspberry Pi 3/4/5 and other aarch64 hosts on a 64-bit OS). Pick the package matching your architecture (`dpkg --print-architecture` / `uname -m`), then install the `.deb`/`.rpm` with your package manager or unpack the `.tgz`. |
| macOS    | `.dmg`, `.tgz` | Open the disk image and drag the app across, or unpack the `.tgz`. |

Every release artifact ships with a matching `.sha512` checksum file. Verify the download before installing.

After installing, head to [Configuration reference](configuration.md) for every CLI flag and config-file key, and [Usage and API](usage-and-api.md) for what to do next.

## Building from source — prerequisites

Install these tools before building. Versions in the **Tested** column are what CI and the Docker images build and run against; the **Minimum** column is an unenforced floor — older versions are not checked by CMake and have not been verified.

| Tool | Minimum | Tested | Notes |
|------|---------|--------|-------|
| Git | any | — | Submodule support is required (vcpkg is a submodule). |
| CMake | 3.31 | 3.31.12 | Enforced — the root `CMakeLists.txt` and every preset pin 3.31 as a `FATAL_ERROR` floor. |
| Ninja | any | — | Required generator. The build scripts only check for its presence, not its version. |
| C++ compiler | C++23 support | GCC 15 / Clang 21 / MSVC (VS 2022+) | See below. |

**Important:** there is **no compiler-version check** anywhere in the CMake configuration or the toolchain files. The "minimum" is a documented expectation, not something the build enforces. What is actually built and tested is:

- **GCC 15** — the CI Linux matrix uses `gcc-15` and the Docker image builds with `GCC_VERSION=15`.
- **Clang 21** — the Docker image builds with `LLVM_VERSION=21`, and the Linux/macOS LLVM toolchains prefer `clang-21`.
- **MSVC from Visual Studio 2022 or later** — CI builds on `windows-latest`. The MSVC toolchain adds only `/utf-8` and `/bigobj` and otherwise lets vcpkg detect MSVC.

Older GCC/Clang releases may compile the C++23 code, but that is unverified — prefer the tested versions. The project is built as **C++23, required, no compiler extensions, with warnings treated as errors globally**, so a too-old or too-lax compiler will fail loudly rather than silently degrade.

vcpkg is vendored as a git submodule at `deps/vcpkg` and is bootstrapped automatically. The root `CMakeLists.txt` even initializes the submodule for you (before `project()`) if `deps/vcpkg/.vcpkg-root` is missing, so a fresh clone configures without a manual submodule step. The build scripts bootstrap it with `-disableMetrics`.

## Clone

```bash
git clone --recurse-submodules https://github.com/iainchesworth/aqualink-automate.git
cd aqualink-automate
```

If you already cloned without `--recurse-submodules`, fetch the submodule:

```bash
git submodule update --init --recursive deps/vcpkg
```

## Build on Linux or macOS

`cicd/build.sh` validates dependencies, initializes and bootstraps vcpkg, then runs configure, build, and test.

```bash
./cicd/build.sh                                   # default: debug; GCC on Linux, Clang on macOS
./cicd/build.sh --compiler clang --type release   # Clang release build
./cicd/build.sh --preset config-linux-gcc         # use an explicit configure preset
./cicd/build.sh --package                         # build, then create installer packages
```

Flags:

| Flag | Values | Default | Description |
|------|--------|---------|-------------|
| `--preset` | any configure preset | derived from `--compiler`/`--type` | Use a named preset directly; overrides `--compiler`/`--type`. |
| `--compiler` | `gcc`, `clang` | `gcc` on Linux, `llvm` on macOS | `clang` maps to the LLVM toolchain. macOS rejects anything but Clang/LLVM. |
| `--type` | `debug`, `release` | `debug` | Selects the `-debug` preset or the release preset. |
| `--package` | — | off | After building, run the `pack-aqualink-automate` target to produce packages. |
| `-h`, `--help` | — | — | Print usage and exit. |

If a required tool is missing, the script tells you what to install:

```bash
# Ubuntu/Debian
sudo apt install build-essential cmake ninja-build git

# macOS
brew install cmake ninja llvm
```

## Build on Windows

Run `cicd\build.ps1` from PowerShell. It locates Visual Studio with `vswhere`, activates the x64 developer environment with `vcvarsall amd64`, bootstraps `vcpkg.exe` if needed, then configures, builds, tests, and optionally packages.

```powershell
.\cicd\build.ps1                                      # default: MSVC debug
.\cicd\build.ps1 -Compiler clang -Type release        # Clang (clang-cl) release build
.\cicd\build.ps1 -Preset config-windows-msvc-debug    # use an explicit configure preset
.\cicd\build.ps1 -Package                             # build, then create installer packages
```

Parameters:

| Parameter | Values | Default | Description |
|-----------|--------|---------|-------------|
| `-Preset` | any configure preset | derived from `-Compiler`/`-Type` | Use a named preset directly. |
| `-Compiler` | `msvc`, `clang`, `llvm` | `msvc` | `clang` and `llvm` both select the LLVM (clang-cl) toolchain. |
| `-Type` | `debug`, `release` | `debug` | Selects the `-debug` preset or the release preset. |
| `-Package` | — | off | After building, run the `pack-aqualink-automate` target. |

The script reports any missing tools. Install them with [Chocolatey](https://chocolatey.org):

```powershell
choco install git cmake ninja visualstudio2022buildtools
```

## CMake presets

Presets are named `config-<platform>-<compiler>[-debug|-coverage]`. There are 12 named configure presets (plus four hidden base presets — `core`, `debug`, `release`, `profiling` — that the named presets inherit from). Every configure preset has a matching `build-*` and `test-*` preset; only the **Release** presets have a matching `pack-*` preset.

| Configure preset | Platform | Compiler | Build type | Notable flags | `build-*` | `test-*` | `pack-*` |
|------------------|----------|----------|-----------|---------------|:---------:|:--------:|:--------:|
| `config-windows-msvc` | Windows | MSVC | Release | — | yes | yes | yes |
| `config-windows-msvc-debug` | Windows | MSVC | Debug | sanitizers forced **off**, clang-tidy **on** | yes | yes | — |
| `config-windows-msvc-coverage` | Windows | MSVC | Debug | coverage on, benchmarks off | yes | yes | — |
| `config-windows-llvm` | Windows | Clang/LLVM | Release | — | yes | yes | yes |
| `config-windows-llvm-debug` | Windows | Clang/LLVM | Debug | ASan + UBSan | yes | yes | — |
| `config-linux-gcc` | Linux | GCC | Release | — | yes | yes | yes |
| `config-linux-gcc-debug` | Linux | GCC | Debug | ASan + UBSan | yes | yes | — |
| `config-linux-gcc-coverage` | Linux | GCC | Debug | coverage on, benchmarks off | yes | yes | — |
| `config-linux-llvm` | Linux | Clang/LLVM | Release | — | yes | yes | yes |
| `config-linux-llvm-debug` | Linux | Clang/LLVM | Debug | ASan + UBSan | yes | yes | — |
| `config-macos-llvm` | macOS | Clang/LLVM | Release | — | yes | yes | yes |
| `config-macos-llvm-debug` | macOS | Clang/LLVM | Debug | ASan + UBSan | yes | yes | — |

Derive the `build-*`/`test-*`/`pack-*` name by swapping the `config-` prefix (for example, `config-linux-gcc` → `build-linux-gcc`, `test-linux-gcc`, `pack-linux-gcc`).

A few details worth knowing:

- **`config-windows-msvc-debug` is not a sanitizer build.** Under MSVC (`cl.exe`), the project's sanitizer mechanism cannot apply AddressSanitizer in a `-MDd` debug build, so this preset forces `ENABLE_SANITIZERS`, `SANITIZE_ADDRESS`, and `SANITIZE_UNDEFINED` **off** and keeps clang-tidy **on** as the static-analysis gate. For a sanitized Windows build, use `config-windows-llvm-debug` (clang-cl), which enables ASan and UBSan.
- **Coverage presets exist only for Linux GCC and Windows MSVC** (`config-linux-gcc-coverage`, `config-windows-msvc-coverage`). There is no LLVM or macOS coverage preset.
- **Sanitizers and profiling are mutually exclusive.** Setting both `ENABLE_SANITIZERS` and `ENABLE_PROFILING` to `ON` is a configure-time `FATAL_ERROR` — Tracy's profiling instrumentation is incompatible with the sanitizer runtimes.
- **`ENABLE_NATIVE_ARCH` is opt-in.** Each toolchain exposes this flag to raise the target instruction-set baseline (for example, AVX2 codegen). It is off by default; turn it on only for builds you will run on the same hardware that compiled them.

Package presets emit these generators, all writing to `${sourceDir}/packages` with SHA512 checksums:

| Package preset | Generators |
|----------------|------------|
| `pack-windows-msvc`, `pack-windows-llvm` | `ZIP`, `NSIS` |
| `pack-linux-gcc`, `pack-linux-llvm` | `TGZ`, `DEB`, `RPM` |
| `pack-macos-llvm` | `TGZ`, `DragNDrop` |

The `--package`/`-Package` helper-script flags drive the `pack-aqualink-automate` CMake target; you can equivalently run a `pack-*` package preset directly (`cmake --build --preset build-<p>` then `cpack --preset pack-<p>`). Both routes emit to `${sourceDir}/packages` with SHA512 checksums.

Maintainers cutting a release should follow [Releasing](releasing.md) rather than running `pack-*` by hand.

## Manual CMake workflow

If you would rather not use the helper scripts, drive CMake directly. The root `CMakeLists.txt` auto-initializes the vcpkg submodule, but you can bootstrap it explicitly:

```bash
# Bootstrap vcpkg (one-time; auto-run by the build scripts and on first configure)
./deps/vcpkg/bootstrap-vcpkg.sh -disableMetrics    # Linux/macOS
```

```powershell
.\deps\vcpkg\bootstrap-vcpkg.bat -disableMetrics    # Windows
```

Then configure, build, and test with matching presets:

```bash
cmake --preset config-linux-gcc          # configure (substitute your platform's preset)
cmake --build --preset build-linux-gcc   # build
ctest --preset test-linux-gcc            # run unit + integration tests
```

## Running tests

The test presets run with CTest. By default a `test-*` preset runs the **unit and integration** suites together — the presets apply no label filter.

```bash
ctest --preset test-linux-gcc            # all default tests (unit + integration)
```

Tests are labelled, so you can filter them. After a build you can run `ctest` directly from the build directory with a label selector:

| Label | CTest test name | Build target | Contents |
|-------|-----------------|--------------|----------|
| `unit` | `AqualinkAutomateUnitTests` | `testaqualink-automate` | Fast, isolated unit tests. |
| `integration` | `AqualinkAutomateIntegrationTests` | `intaqualink-automate` | Slower fixture-replay / end-to-end flow tests. |
| `perf` | `AqualinkAutomatePerfTests` | `perfaqualink-automate` | Google Benchmark performance tests (built, not run by default). |

```bash
ctest -L unit          # unit tests only
ctest -L integration   # integration tests only
```

**Performance tests are built but not run automatically.** Invoke them explicitly when you want them:

```bash
ctest -L perf          # run the benchmark suite through CTest
```

You can also run the benchmark executable directly to pass Google Benchmark flags.

## Development container

A Dev Container is provided for VS Code. It builds the Dockerfile's `dev` target (GCC 15, Clang 21, clang-tidy, CMake, Ninja, and ccache pre-installed), runs as the `dev` user, and runs `.devcontainer/setup.sh` as its `postCreateCommand`.

To use it:

1. Install [Docker](https://www.docker.com/products/docker-desktop/) and the [Dev Containers](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers) VS Code extension.
2. Clone the repository and open the folder in VS Code:
   ```bash
   git clone --recurse-submodules https://github.com/iainchesworth/aqualink-automate.git
   cd aqualink-automate
   code .
   ```
3. When prompted, choose **Reopen in Container**. The `postCreateCommand` initializes and bootstraps vcpkg.

Three named volumes persist across container rebuilds:

| Volume | Mount point | Purpose |
|--------|-------------|---------|
| `aqualink-ccache` | `/ccache` | Compiler cache (ccache). |
| `aqualink-vcpkg-cache` | `/vcpkg-cache` | vcpkg binary cache. |
| `aqualink-vcpkg` | `/src/deps/vcpkg` | vcpkg checkout (a named volume here avoids cross-platform submodule lock issues). |

Build inside the container exactly as on the host:

```bash
cmake --preset config-linux-gcc
cmake --build --preset build-linux-gcc
ctest --preset test-linux-gcc
```

Both the GCC and Clang/LLVM toolchains are available in the container, and the C++ IntelliSense, CMake Tools, and clangd extensions are installed automatically.

## Docker

The Dockerfile is multi-stage (`base`, `dev`, `ci`, `matter-builder`, `runtime`); the default build target is `runtime`. The runtime image is built on `ubuntu:26.04` with `tini` as PID 1, Node.js 24 for the Matter bridge sidecar, and `curl` for the container health check. It runs as an unprivileged `aqualink` user (uid/gid 10000), exposes port 80, declares a `HEALTHCHECK` against `/api/health`, and its default command is `--address 0.0.0.0 --disable-https`.

**Security:** the default command binds to `0.0.0.0` (all interfaces) and disables HTTPS, which is appropriate for a container behind your own network boundary but exposes an **unauthenticated** HTTP API that can actuate pool equipment. Authentication is off by default, so the app logs a prominent startup warning whenever it binds a non-loopback address with no `--api-auth-token`. Before exposing the container beyond a trusted segment you should: set a strong `--api-auth-token` (32+ random chars), terminate TLS in front of it (reverse proxy) or enable HTTPS, and optionally set `--api-allowed-origin` / `--api-require-csrf-header`. If the open posture is deliberate (e.g. behind a trusted reverse proxy), pass `--insecure-no-auth` to acknowledge it and quiet the warning. Restrict access at the network layer regardless. See [Configuration reference](configuration.md) for the authentication and TLS options.

### Build and run the runtime image

```bash
docker build --target runtime -t aqualink-automate .
docker run -p 80:80 aqualink-automate
```

To talk to a physical RS-485 adapter, pass the device through and point the app at it. CLI flags after the image name are forwarded to the app:

```bash
docker run -p 80:80 --device /dev/ttyUSB0 aqualink-automate --serial-port /dev/ttyUSB0
```

The `--serial-port`/`-s` default is platform-resolved, so set it explicitly inside a container. Every flag is documented in the [Configuration reference](configuration.md); config-file keys are the same names without the leading dashes (for example, the flag `--serial-port` is the key `serial-port`).

### Health check

The runtime image ships a Docker `HEALTHCHECK` that polls the unauthenticated liveness endpoint `GET /api/health` with `curl` every 30s (after a 20s start period). It stays reachable even with `--api-auth-token` set, so the container reports `healthy`/`unhealthy` automatically under both `docker run` and `docker compose` — no extra flags required:

```bash
docker inspect --format '{{.State.Health.Status}}' <container>
```

Combined with `restart: unless-stopped` (set in Compose), an `unhealthy` container is restarted. `docker-compose.yml` also declares the check explicitly so it is easy to tune. If you change the web port (or serve HTTPS only), update the probe URL to match — via the `healthcheck:` block in Compose, or `--health-cmd 'curl -f http://127.0.0.1:<port>/api/health'` for `docker run`. A richer, authenticated readiness view is available at `GET /api/health/detailed` (`200` when ready, `503` while starting) — see the [API guide](usage-and-api.md).

### Docker Compose

```bash
docker compose up
```

The `app` service builds the `runtime` target, restarts `unless-stopped`, sets `MATTER_ENABLED=true`, and persists Matter state in the `matter-data` volume (mounted at `/data/matter`). It runs with `network_mode: host` and therefore declares **no** `ports:` mapping — see the Matter note below for why.

### Matter bridge and host networking

The runtime image bundles a **Matter bridge sidecar** (a Node.js process) that exposes the pool equipment to Apple Home, Google Home, Alexa, and SmartThings. The container entrypoint supervises it alongside the app. Runtime defaults: `MATTER_ENABLED=true`, `MATTER_STORAGE_PATH=/data/matter`, and `AQUALINK_API_URL=http://127.0.0.1:80`.

Matter commissioning uses IPv6 mDNS (UDP 5540 + 5353), which Docker's bridge network driver does not forward. Choose the run mode that matches your platform:

- **Linux, Matter enabled (recommended):** run with host networking so the discovery traffic reaches the LAN. This exposes both the HTTP API and Matter discovery on the host directly, so no `-p` mapping is used.
  ```bash
  docker run --network host -v aqualink-matter:/data/matter aqualink-automate
  ```
  `docker compose up` already sets `network_mode: host` and the `matter-data` volume, so commissioned fabrics survive restarts. Open the web UI's **Diagnostics → Matter** panel to scan the pairing QR.
- **HTTP only, or Docker Desktop (macOS/Windows):** host networking is Linux-only, so Matter cannot be commissioned from the LAN there. Disable the bridge and use normal port mapping:
  ```bash
  docker run -p 80:80 -e MATTER_ENABLED=false aqualink-automate
  ```
  Setting `MATTER_ENABLED=false` mirrors the app flag `--matter false`. The app runs normally; only the Matter sidecar is skipped.

For a deeper look at the bridge — pairing, the sidecar architecture, and the storage layout — see [Matter bridge](MATTER.md) and the [`matter-bridge/`](https://github.com/iainchesworth/aqualink-automate/blob/main/matter-bridge/README.md) README.

### API documentation (Swagger UI)

The Compose file ships an optional `swagger-ui` service behind the `docs` profile. It serves the project's OpenAPI spec on port 8080:

```bash
docker compose --profile docs up
```

Open `http://localhost:8080` to browse the API. The service mounts `assets/web/api/swagger.yaml` read-only. The raw spec is also served by the running app at `GET /api/swagger.yaml`.

---

Next steps: [Configuration reference](configuration.md) for every flag and config key, [Usage and API](usage-and-api.md) for operating the app, and [CONTRIBUTING.md](CONTRIBUTING.md) if you are building against the `develop` branch to send a patch.
