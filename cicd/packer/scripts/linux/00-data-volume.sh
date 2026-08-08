#!/usr/bin/env bash
set -euo pipefail

# Set up the dedicated data volume (disk1) that keeps build junk off the OS disk
# — the historic cause of the runners filling the shared datastore. The data
# volume holds two areas:
#   /data/work   — the runner work dir (checkout + build tree); wiped every job
#                  by the ephemeral supervisor (see 08-github-runner.sh).
#   /data/cache  — persistent vcpkg binary cache + ccache; survives the wipe,
#                  size-capped by the supervisor so it can't grow without bound.
# $HOME/.cache is symlinked here so vcpkg (~/.cache/vcpkg) and ccache
# (~/.cache/ccache) land on the data volume automatically, with no workflow change.
#
# The OS is installed only to the smaller disk0 (http/linux/user-data pins the
# install to the smallest disk), so at build time disk1 is a raw, unpartitioned
# whole disk — that is how we identify it below.

echo "==> Setting up the data volume (runner work + persistent caches)"

DATA_LABEL="RUNNERDATA"
DATA_MNT="/data"
RUNNER_USER="runner"
RUNNER_HOME="/home/${RUNNER_USER}"

# parted may not be in the base live image yet (this runs before 01-base-packages).
if ! command -v parted >/dev/null 2>&1; then
    apt-get update -qq
    apt-get install -y -qq parted
fi

# Format the data disk only if our labelled filesystem doesn't already exist
# (keeps the script idempotent across re-provisions).
if ! blkid -L "$DATA_LABEL" >/dev/null 2>&1; then
    # The data disk is the whole disk (TYPE=disk) with no child partitions and
    # nothing mounted: the OS disk already carries efi/boot/LVM partitions, the
    # data disk is raw at build time.
    DATA_DISK=""
    for d in $(lsblk -dno NAME -e7); do
        case "$d" in sr*|loop*|fd*) continue ;; esac
        if [ -z "$(lsblk -no NAME "/dev/$d" | tail -n +2)" ] \
           && [ -z "$(lsblk -no MOUNTPOINT "/dev/$d" | tr -d '[:space:]')" ]; then
            DATA_DISK="/dev/$d"
            break
        fi
    done

    if [ -z "$DATA_DISK" ]; then
        echo "ERROR: could not identify an empty data disk" >&2
        lsblk >&2
        exit 1
    fi

    echo "==> Partitioning and formatting data disk ${DATA_DISK}"
    parted -s "$DATA_DISK" mklabel gpt
    parted -s "$DATA_DISK" mkpart primary ext4 0% 100%
    udevadm settle || sleep 2

    # Partition node is sdb1 (SCSI) or nvme0n1p1 (NVMe).
    PART="${DATA_DISK}1"
    [ -b "${DATA_DISK}p1" ] && PART="${DATA_DISK}p1"
    mkfs.ext4 -F -L "$DATA_LABEL" "$PART"
fi

# Mount by LABEL (stable across clones, where /dev/sdX can reorder) with
# `discard` so freed blocks are reclaimed in real time as work/ is wiped each
# job — keeps the thin vmdk lean. `nofail` so a missing data disk can't wedge
# boot; the supervisor refuses to run unless /data is actually a mountpoint.
mkdir -p "$DATA_MNT"
if ! grep -q "LABEL=${DATA_LABEL}" /etc/fstab; then
    echo "LABEL=${DATA_LABEL}  ${DATA_MNT}  ext4  defaults,discard,nofail  0  2" >> /etc/fstab
fi
mount "$DATA_MNT" 2>/dev/null || mount -a

# work/ (wiped each job) and cache/ (persistent). vcpkg lives under cache/vcpkg,
# ccache under cache/ccache (see 07-ccache.sh). docker/ is Docker's data-root and
# containerd/ is containerd's root, so images/layers pulled by release builds
# land here, not on the OS disk (see 05-docker.sh).
mkdir -p "${DATA_MNT}/work" "${DATA_MNT}/cache" "${DATA_MNT}/docker" "${DATA_MNT}/containerd"
chown -R "${RUNNER_USER}:${RUNNER_USER}" "${DATA_MNT}/work" "${DATA_MNT}/cache"

# Redirect ~/.cache onto the persistent cache area. Replace a real dir if one
# exists (unlikely this early), preserving anything already inside it.
if [ -e "${RUNNER_HOME}/.cache" ] && [ ! -L "${RUNNER_HOME}/.cache" ]; then
    cp -a "${RUNNER_HOME}/.cache/." "${DATA_MNT}/cache/" 2>/dev/null || true
    rm -rf "${RUNNER_HOME}/.cache"
fi
ln -sfn "${DATA_MNT}/cache" "${RUNNER_HOME}/.cache"
chown -h "${RUNNER_USER}:${RUNNER_USER}" "${RUNNER_HOME}/.cache"

# sonar-scanner caches its JRE/plugins under ~/.sonar (NOT ~/.cache, so the
# symlink above doesn't cover it) and it grows past 1 GB on the OS disk.
# Redirect it into the size-capped cache area (the supervisor prunes
# /data/cache/sonar — see 08-github-runner.sh).
install -d -o "${RUNNER_USER}" -g "${RUNNER_USER}" "${DATA_MNT}/cache/sonar"
if [ -e "${RUNNER_HOME}/.sonar" ] && [ ! -L "${RUNNER_HOME}/.sonar" ]; then
    cp -a "${RUNNER_HOME}/.sonar/." "${DATA_MNT}/cache/sonar/" 2>/dev/null || true
    rm -rf "${RUNNER_HOME}/.sonar"
fi
ln -sfn "${DATA_MNT}/cache/sonar" "${RUNNER_HOME}/.sonar"
chown -h "${RUNNER_USER}:${RUNNER_USER}" "${RUNNER_HOME}/.sonar"

echo "==> Data volume ready: ${DATA_MNT}/work (ephemeral) + ${DATA_MNT}/cache (persistent); ~/.cache -> ${DATA_MNT}/cache; ~/.sonar -> ${DATA_MNT}/cache/sonar"
