#!/usr/bin/env bash
set -euo pipefail

echo "==> Installing Docker Engine CE"

# Add Docker GPG key and repository
install -m 0755 -d /etc/apt/keyrings
curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 --max-time 60 https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
chmod a+r /etc/apt/keyrings/docker.gpg

CODENAME=$(lsb_release -cs)
DOCKER_REPO="https://download.docker.com/linux/ubuntu/dists/${CODENAME}"

# Verify Docker repo exists for this codename; fall back to nearest LTS if not
if ! curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 --max-time 30 --head "${DOCKER_REPO}/Release" >/dev/null 2>&1; then
    echo "WARNING: Docker repo not available for '${CODENAME}', falling back to 'noble'"
    CODENAME="noble"
fi

echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu ${CODENAME} stable" \
    > /etc/apt/sources.list.d/docker.list

apt-get update
apt-get install -y --no-install-recommends \
    docker-ce \
    docker-ce-cli \
    containerd.io \
    docker-buildx-plugin \
    docker-compose-plugin

# Add runner user to docker group
usermod -aG docker runner

# Keep Docker's images/layers off the small OS disk: point the data-root at the
# data volume (/data/docker, created by 00-data-volume.sh). Release builds pull
# multi-GB base images (gcc:15-bookworm, debian:bookworm); on the OS disk those
# could exhaust it. The supervisor's `docker system prune` clears them each job.
mkdir -p /etc/docker
cat > /etc/docker/daemon.json <<'JSON'
{
  "data-root": "/data/docker"
}
JSON

# Docker must not start until /data is mounted, otherwise it would initialise its
# data-root under the (pre-mount) /data directory on the OS disk and then be
# shadowed by the mount. Order it after the generated data.mount unit.
mkdir -p /etc/systemd/system/docker.service.d
cat > /etc/systemd/system/docker.service.d/data-root.conf <<'DROPIN'
[Unit]
After=data.mount
Requires=data.mount
DROPIN

# containerd keeps its own content/snapshotter store, and it defaults to
# /var/lib/containerd on the OS disk — moving Docker's data-root does NOT move
# it, and buildx/BuildKit park gigabytes of Docker-managed content there (it
# exhausted a 9.5 GB OS disk in production). Point containerd's root at the
# data volume too (/data/containerd, created by 00-data-volume.sh).
mkdir -p /etc/containerd /data/containerd
cat > /etc/containerd/config.toml <<'TOML'
# Managed by packer (05-docker.sh). Matches the containerd.io package default
# (CRI disabled; Docker drives containerd over its own socket) with the content
# root moved onto the data volume so image/snapshotter content stays off the
# small OS disk.
disabled_plugins = ["cri"]
root = "/data/containerd"
TOML

# Same mount-ordering constraint as docker.service: containerd must not start
# until /data is mounted, or it would initialise its root on the OS disk and
# then be shadowed by the mount.
mkdir -p /etc/systemd/system/containerd.service.d
cat > /etc/systemd/system/containerd.service.d/data-root.conf <<'DROPIN'
[Unit]
After=data.mount
Requires=data.mount
DROPIN

systemctl daemon-reload

# Enable and start Docker (data-root takes effect on the next start).
systemctl enable docker

echo "==> Docker $(docker --version) installed (data-root on /data/docker, containerd root on /data/containerd)"
