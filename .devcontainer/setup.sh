#!/usr/bin/env bash
set -euo pipefail

# deps/vcpkg is mounted as a Docker volume (shadows the bind mount) to avoid
# cross-platform file-lock issues between Windows hosts and Linux containers.
# We need to populate it ourselves since git submodule can't write into it.

VCPKG_DIR="/src/deps/vcpkg"
VCPKG_URL="https://github.com/iainchesworthlabs/vcpkg.git"

# Ensure the dev user owns the vcpkg volume (Docker creates volumes as root).
sudo chown dev:dev "$VCPKG_DIR"

# Read the expected commit from the repo's submodule pointer.
VCPKG_COMMIT=$(git -C /src ls-tree HEAD deps/vcpkg | awk '{print $3}')

if [ -z "$VCPKG_COMMIT" ]; then
    echo "ERROR: Could not determine vcpkg submodule commit" >&2
    exit 1
fi

# Initialize the volume if empty or at wrong commit.
if [ ! -d "$VCPKG_DIR/.git" ]; then
    echo "Initializing vcpkg volume (cloning $VCPKG_COMMIT)..."
    git clone "$VCPKG_URL" "$VCPKG_DIR"
    git -C "$VCPKG_DIR" checkout "$VCPKG_COMMIT"
else
    CURRENT=$(git -C "$VCPKG_DIR" rev-parse HEAD)
    if [ "$CURRENT" != "$VCPKG_COMMIT" ]; then
        echo "Updating vcpkg volume ($CURRENT -> $VCPKG_COMMIT)..."
        git -C "$VCPKG_DIR" fetch origin
        git -C "$VCPKG_DIR" checkout "$VCPKG_COMMIT"
    else
        echo "vcpkg volume already at $VCPKG_COMMIT"
    fi
fi

# Bootstrap vcpkg
"$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics
