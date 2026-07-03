# ==============================================================================
# Stage: base (shared tooling for dev, ci, and runtime builds)
# ==============================================================================

FROM ubuntu:26.04 AS base

ARG GCC_VERSION=15
ARG LLVM_VERSION=21
ARG CMAKE_VERSION=3.31.12

ENV DEBIAN_FRONTEND=noninteractive

# Install build toolchain (all from the Ubuntu 26.04 repos):
#   - GCC 15 (default in Ubuntu 26.04)
#   - LLVM/Clang 21 from universe (compiler, linker, libc++, clang-tidy) -- the
#     stock Ubuntu packages, so no third-party apt.llvm.org repo is needed.
#   - CMake 3.31+ via official binary (project requires 3.31)
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        autoconf \
        autoconf-archive \
        automake \
        ca-certificates \
        ccache \
        curl \
        git \
        libtool \
        linux-libc-dev \
        gcovr \
        ninja-build \
        pkg-config \
        tar \
        unzip \
        wget \
        zip \
    # GCC (default toolchain in Ubuntu 26.04)
        build-essential \
        gcc-${GCC_VERSION} \
        g++-${GCC_VERSION} \
    # LLVM/Clang (Ubuntu 26.04 universe)
        clang-${LLVM_VERSION} \
        clang-tidy-${LLVM_VERSION} \
        lld-${LLVM_VERSION} \
        libc++-${LLVM_VERSION}-dev \
        libc++abi-${LLVM_VERSION}-dev \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-${GCC_VERSION} ${GCC_VERSION} \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-${GCC_VERSION} ${GCC_VERSION} \
    && update-alternatives --install /usr/bin/gcov gcov /usr/bin/gcov-${GCC_VERSION} ${GCC_VERSION} \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-${LLVM_VERSION} ${LLVM_VERSION} \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-${LLVM_VERSION} ${LLVM_VERSION} \
    && update-alternatives --install /usr/bin/clang-tidy clang-tidy /usr/bin/clang-tidy-${LLVM_VERSION} ${LLVM_VERSION} \
    && update-alternatives --install /usr/bin/lld lld /usr/bin/lld-${LLVM_VERSION} ${LLVM_VERSION} \
    && update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-${LLVM_VERSION} ${LLVM_VERSION} \
    # CMake via official binary release
    && wget -qO- "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.tar.gz" \
        | tar xz -C /usr/local --strip-components=1 \
    # Cleanup
    && rm -rf /var/lib/apt/lists/*

ENV CCACHE_DIR=/ccache
ENV CMAKE_C_COMPILER_LAUNCHER=ccache
ENV CMAKE_CXX_COMPILER_LAUNCHER=ccache

# ==============================================================================
# Stage: dev (development container — source bind-mounted at runtime)
# ==============================================================================

FROM base AS dev

RUN apt-get update \
    && apt-get install -y --no-install-recommends sudo \
    && rm -rf /var/lib/apt/lists/* \
    && groupmod --new-name dev ubuntu \
    && usermod --login dev --home /home/dev --move-home ubuntu \
    && echo "dev ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/dev \
    && mkdir -p /ccache /vcpkg-cache /src \
    && chown dev:dev /ccache /vcpkg-cache /src \
    && git config --system --add safe.directory /src

RUN ccache --set-config=max_size=2G \
    && ccache --set-config=compression=true

ENV VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache

WORKDIR /src

USER dev

CMD ["bash"]

# ==============================================================================
# Stage: ci (build + test + clang-tidy verification)
# ==============================================================================

FROM base AS ci

# Version to stamp into the binary. The build context intentionally omits .git
# (see .dockerignore), so `git describe` cannot derive the version inside the
# container; the release workflow passes the resolved tag via --build-arg
# VERSION=v<M>.<M>.<P>[-prerelease]. Empty (local builds) => 0.0.0-dev fallback.
ARG VERSION=""

WORKDIR /src
COPY . .

# Bootstrap vcpkg
RUN ./deps/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Configure (with ccache and vcpkg binary + asset caching)
#
# Three persistent caches, each invalidated by something different:
#   - /vcpkg-cache     : prebuilt binary packages, keyed by full ABI hash (churns
#                        on any toolchain / triplet / baseline bump).
#   - /vcpkg-assets    : source tarballs + vcpkg's own tool fetches (cmake, ninja),
#                        keyed by SHA512 -- only invalidated when the UPSTREAM
#                        source changes. This is the resilience layer: when the
#                        binary cache misses and vcpkg must rebuild from source, it
#                        serves the sources from here instead of re-fetching from
#                        GitHub, so a transient GitHub HTTP 400 / rate-limit (which
#                        vcpkg treats as non-retryable) no longer fails the build.
#   - /vcpkg-downloads : vcpkg's own download dir (the cold-start landing spot).
#
# X_VCPKG_ASSET_SOURCES drives the asset cache: `clear` drops any inherited
# default, then a read-write file-backed store under the cache mount.
RUN --mount=type=cache,target=/ccache \
    --mount=type=cache,target=/vcpkg-cache \
    --mount=type=cache,target=/vcpkg-downloads \
    --mount=type=cache,target=/vcpkg-assets \
    VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache \
    VCPKG_DOWNLOADS=/vcpkg-downloads \
    X_VCPKG_ASSET_SOURCES="clear;x-azurl,file:///vcpkg-assets,,readwrite" \
    cmake --preset config-linux-gcc \
        -DDERIVED_VERSION_OVERRIDE="${VERSION}" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

# Build
RUN --mount=type=cache,target=/ccache \
    cmake --build --preset build-linux-gcc

# Test
RUN ctest --preset test-linux-gcc

# Install (creates the install tree the runtime stage copies from). Stage through
# DESTDIR (not the configured prefix) so the absolute /etc/aqualink-automate config
# (see cmake/CPackConfig.cmake) lands inside the tree instead of leaking to this
# build stage's /etc -- keeping it identical to the assembled runtime image's tree.
# --prefix / keeps the layout flat (bin/lib/share at the root).
RUN DESTDIR=/src/install/config-linux-gcc cmake --install build/config-linux-gcc --prefix /

# ==============================================================================
# Stage: matter-builder (build the matter.js sidecar)
# ==============================================================================
#
# Built on a glibc Node image so the produced node_modules are ABI-compatible with
# the glibc Node installed in the runtime stage. @matter/main and ws are pure JS, so
# this is belt-and-braces, but it keeps the door open for any future native dep.

FROM node:24-bookworm-slim AS matter-builder

WORKDIR /opt/matter-bridge

# Install deps against the lockfile first (cached unless the lockfile changes).
COPY matter-bridge/package.json matter-bridge/package-lock.json ./
RUN npm ci

# Build the TypeScript, then drop dev dependencies so only the runtime tree ships.
COPY matter-bridge/ ./
RUN npm run build \
    && npm prune --omit=dev

# ==============================================================================
# Stage: runtime-base (OS + deps + Matter sidecar + entrypoint, minus the app)
# ==============================================================================
#
# Everything the production image needs EXCEPT the compiled app install tree. The
# two final stages layer the app on top and differ ONLY in where the install tree
# comes from, so the runtime contract is defined once here:
#   - runtime           : app from the `ci` stage (built from source — the cold gate)
#   - runtime-assembled : app from a prebuilt install tree staged into the context
#                         (no recompile; used by release.yml docker-publish)

FROM ubuntu:26.04 AS runtime-base

ENV DEBIAN_FRONTEND=noninteractive

# ca-certificates for TLS; curl for the container HEALTHCHECK (also used here to
# import the NodeSource signing key); tini as a tiny init that reaps zombies and
# forwards signals to docker-entrypoint.sh (which supervises the app + Matter
# sidecar); Node.js 24 (NodeSource) to run the Matter bridge sidecar; gosu so the
# entrypoint can drop from root to the configurable PUID/PGID. gnupg is only
# needed to import the key, so it is purged afterwards; curl is intentionally kept
# (a few hundred KB) so the HEALTHCHECK and compose examples can use the standard
# `curl --fail` probe.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl gnupg tini gosu \
    && mkdir -p /etc/apt/keyrings \
    && curl -fsSL https://deb.nodesource.com/gpgkey/nodesource-repo.gpg.key | gpg --dearmor -o /etc/apt/keyrings/nodesource.gpg \
    && echo "deb [signed-by=/etc/apt/keyrings/nodesource.gpg] https://deb.nodesource.com/node_24.x nodistro main" > /etc/apt/sources.list.d/nodesource.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends nodejs \
    && apt-get purge -y --auto-remove gnupg \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --gid 10000 aqualink \
    && useradd --uid 10000 --gid 10000 --create-home --shell /bin/false aqualink

WORKDIR /opt/aqualink-automate

# The Matter bridge sidecar (built dist/ + production node_modules) and the
# supervising entrypoint that launches it alongside the app.
COPY --from=matter-builder /opt/matter-bridge /opt/matter-bridge
COPY docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Fabric/commissioning persistence for the Matter bridge (mount a volume here so
# pairing survives container restarts). Owned by the app user.
RUN mkdir -p /data/matter && chown -R aqualink:aqualink /data/matter

# 80 = HTTP API. Matter commissioning additionally needs UDP 5540 + mDNS 5353,
# which require host networking (see docker-compose.yml) -- they cannot be mapped
# through Docker's bridge driver, so EXPOSE alone is insufficient for Matter.
EXPOSE 80

# PUID/PGID: the uid:gid the app is run as. The entrypoint (started as root) remaps
# the bundled `aqualink` user to these, chowns the writable paths, then drops to them
# via gosu. Override per-deployment; PUID=0 keeps the container running as root.
ENV MATTER_ENABLED=true \
    MATTER_STORAGE_PATH=/data/matter \
    AQUALINK_API_URL=http://127.0.0.1:80 \
    PUID=10000 \
    PGID=10000

# Container liveness probe: GET /api/health (unauthenticated by design, so it works
# even when --api-auth-token is set). `curl --fail` exits non-zero on any status >= 400
# or a connection error, and --max-time bounds a stalled request; a hung poll loop
# never answers -> the probe trips and the orchestrator can restart. Reuses
# AQUALINK_API_URL so it tracks the same in-container API endpoint the Matter sidecar
# talks to -- override that env (or this HEALTHCHECK) if you change the app's
# --http-port or move it behind HTTPS-only.
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
    CMD curl --fail --silent --show-error --max-time 4 "${AQUALINK_API_URL}/api/health" || exit 1

# tini (PID 1) reaps zombies + forwards signals to the entrypoint, which supervises
# the app and (unless MATTER_ENABLED=false) the Matter sidecar. CMD is forwarded to
# the app, so the historical args/behaviour are preserved.
#
# doc-root and the SSL material default to paths resolved relative to the
# executable (share/aqualink-automate/...), so they do not need to be passed.
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/docker-entrypoint.sh"]
CMD ["--address", "0.0.0.0", "--disable-https"]

# ==============================================================================
# Stage: runtime (from-source — the cold-build gate)
# ==============================================================================
#
# The compiled install tree comes from the `ci` stage (built from source in this
# very image, so its glibc/libstdc++ is the base image's by construction). CI's
# docker-verify builds this target, so a vcpkg/Dockerfile/toolchain regression is
# always caught end to end. The install tree carries the vcpkg runtime libraries in
# lib/aqualink-automate/ with an $ORIGIN-relative RPATH (see cmake/CPackConfig.cmake),
# so no separate shared-library copy or LD_LIBRARY_PATH is required.

FROM runtime-base AS runtime

COPY --from=ci /src/install/config-linux-gcc/ .
# No USER directive: the image starts as root so the entrypoint can apply PUID/PGID
# and drop privileges via gosu (set PUID=0 to stay root). See docker-entrypoint.sh.

# ==============================================================================
# Stage: runtime-assembled (prebuilt install tree — no recompile)
# ==============================================================================
#
# The compiled install tree is staged into the build context at docker/context/
# (release.yml docker-publish downloads installtree-linux-gcc there) and copied
# straight in — NO vcpkg/source recompile. The install tree's glibc/libstdc++
# baseline must be EQUAL-OR-OLDER than this base image; release.yml builds it in the
# glibc-2.36 container (see _build.yml) so the tree runs on both this base and a
# stock Pi. The from-source `runtime` stage (CI docker-verify) remains the
# cold-build gate that catches any ABI/vcpkg/Dockerfile regression this path cannot.

FROM runtime-base AS runtime-assembled

# Multi-arch: buildx builds this stage once per --platform, setting TARGETARCH
# (amd64 / arm64). Each install tree is built in the glibc-2.36 container for its
# arch (release.yml docker-publish stages both under docker/context/ as
# installtree-linux-gcc-<arch>.tar.gz); pick the matching one. The runtime base
# (ubuntu:26.04, glibc 2.43) is newer than the package floor, so the tree runs.
ARG TARGETARCH
# The install tree arrives as a tarball (lossless symlinks + exec bits through the
# CI artifact round-trip) and is unpacked here INSIDE the image, so it is correct
# regardless of the host/runner filesystem (e.g. a Windows box cannot represent the
# versioned-.so symlinks the binary loads by SONAME).
COPY docker/context/installtree-linux-gcc-${TARGETARCH}.tar.gz /tmp/installtree.tar.gz
RUN tar xzf /tmp/installtree.tar.gz -C /opt/aqualink-automate && rm /tmp/installtree.tar.gz
# No USER directive: starts as root; the entrypoint applies PUID/PGID then drops
# privileges via gosu (set PUID=0 to stay root). See docker-entrypoint.sh.
