#!/usr/bin/env bash
#
# Validate the arm64 (Raspberry Pi) package build locally, with NO arm hardware,
# by running the EXACT `config-linux-gcc-arm64` CI preset inside an emulated
# linux/arm64 container (QEMU via Docker/binfmt). This reproduces the native
# build the `ubuntu-24.04-arm` CI runner performs — same toolchain, same triplet,
# same vcpkg deps, same CPack packaging — so a green run here means CI should pass.
#
# Requirements: Docker (Docker Desktop on Windows/WSL2, or a Linux engine) with
# arm64 emulation registered. Verify with:
#     docker run --rm --platform linux/arm64 ubuntu:26.04 uname -m   # -> aarch64
# If that prints anything other than aarch64, register QEMU once:
#     docker run --privileged --rm tonistiigi/binfmt --install arm64
#
# Notes:
#   * The whole build (vcpkg deps + app) runs under emulation, so it is SLOW —
#     Boost alone can take an hour+ on the first run. A named Docker volume caches
#     the vcpkg binary archives, so re-runs only rebuild what actually changed.
#   * The working tree (INCLUDING uncommitted changes) is streamed into the
#     container over stdin — no bind mount — so it behaves identically from Git
#     Bash, WSL2 or Linux and never writes the multi-GB build onto the host
#     (keeps it off a tight Dev Drive). vcpkg is cloned fresh at the pinned SHA.
#
# Overrides (env): AQ_ARM64_IMAGE (base image), AQ_ARM64_CACHE_VOL (cache volume).
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VCPKG_SHA="$(git -C "$REPO" ls-tree HEAD deps/vcpkg | awk '{print $3}')"
IMAGE="${AQ_ARM64_IMAGE:-ubuntu:26.04}"
CACHE_VOL="${AQ_ARM64_CACHE_VOL:-aqlauto-vcpkg-arm64}"

if [ -z "$VCPKG_SHA" ]; then
    echo "error: could not resolve the pinned deps/vcpkg SHA from git." >&2
    exit 1
fi

echo "Repo root : $REPO"
echo "vcpkg pin : $VCPKG_SHA"
echo "Base image: $IMAGE"
echo "Cache vol : $CACHE_VOL"
echo

# Stream the working tree (minus heavy/irrelevant dirs) into the container and run
# the full configure -> build -> test -> package pipeline against the arm64 preset.
tar -C "$REPO" \
    --exclude=./.git --exclude=./.claude \
    --exclude=./build --exclude=./packages --exclude=./packages-arm64 \
    --exclude=./deps/vcpkg \
    --exclude=./node_modules --exclude=./matter-bridge/node_modules \
    --exclude=./test-results --exclude=./playwright-report \
    -cf - . \
| docker run -i --rm --platform linux/arm64 \
    -v "$CACHE_VOL":/vcpkgcache \
    -e VCPKG_SHA="$VCPKG_SHA" \
    "$IMAGE" bash -euo pipefail -c '
        export DEBIAN_FRONTEND=noninteractive
        echo "::: installing toolchain (matches CI: gcc-15) :::"
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends \
            build-essential gcc-15 g++-15 make cmake ninja-build git ca-certificates \
            autoconf autoconf-archive automake libtool pkg-config \
            curl zip unzip tar rpm fakeroot file
        # Point the default gcc/g++ at 15 so the toolchain (and every vcpkg port
        # build) uses it rather than the image default.
        update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-15 100
        update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-15 100

        echo "::: unpacking streamed source :::"
        mkdir -p /work && tar -C /work -xpf -
        cd /work

        echo "::: fetching pinned vcpkg ($VCPKG_SHA) :::"
        git clone -q https://github.com/iainchesworthlabs/vcpkg deps/vcpkg
        git -C deps/vcpkg checkout -q "$VCPKG_SHA"

        export VCPKG_DEFAULT_BINARY_CACHE=/vcpkgcache
        mkdir -p "$VCPKG_DEFAULT_BINARY_CACHE"
        ./deps/vcpkg/bootstrap-vcpkg.sh -disableMetrics

        echo "::: configure (vcpkg builds the deps here — the slow part) :::"
        cmake --preset config-linux-gcc-arm64 -DDERIVED_VERSION_OVERRIDE=0.0.0-dev

        echo "::: build :::"
        cmake --build --preset build-linux-gcc-arm64

        echo "::: test :::"
        ctest --preset test-linux-gcc-arm64 --output-on-failure

        echo "::: package :::"
        cpack --preset pack-linux-gcc-arm64

        echo "::: RESULT :::"
        ls -1 packages/
        echo "--- deb metadata ---"
        dpkg --info packages/*arm64.deb | grep -iE "package|version|architecture|depends"
    '

echo
echo "arm64 validation finished — see the deb metadata above (Architecture should be arm64)."
