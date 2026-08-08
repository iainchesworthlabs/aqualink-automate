#!/usr/bin/env bash
set -euo pipefail

RUNNER_VERSION="${GITHUB_RUNNER_VERSION:-2.321.0}"
RUNNER_HOME="/home/runner"
RUNNER_DIR="${RUNNER_HOME}/actions-runner"

echo "==> Installing GitHub Actions runner v${RUNNER_VERSION} (ephemeral mode)"

mkdir -p "${RUNNER_DIR}"

# Download runner
RUNNER_URL="https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
curl -fsSL "${RUNNER_URL}" | tar xz -C "${RUNNER_DIR}"

# Install runner dependencies
"${RUNNER_DIR}/bin/installdependencies.sh"

# Fix ownership
chown -R runner:runner "${RUNNER_DIR}"

# ---------------------------------------------------------------------------
# Ephemeral runner supervisor
# ---------------------------------------------------------------------------
# Each VM runs ONE job per registration on a pristine workspace, then recycles:
# wipe the work dir + transient cruft, re-register with a freshly-minted token,
# run the next job. This guarantees every build starts clean and that GitHub can
# never hand a runner a second job in a dirty state. The persistent vcpkg/ccache
# caches on /data/cache are preserved (only size-capped) so CI stays fast.
#
# Replaces the old persistent `svc.sh install` service + one-shot
# `github-runner-register.service`. The runner now mints its own registration
# tokens from an on-box credential (so it can re-register every cycle without an
# external orchestrator), so the per-VM guestinfo gains `runner_pat` in place of
# the old short-lived `runner_token`.
cat > "${RUNNER_DIR}/run-ephemeral.sh" <<'SUPERVISOR'
#!/usr/bin/env bash
# GitHub Actions ephemeral runner supervisor — see 08-github-runner.sh.
#
# Runs as the unprivileged `runner` user (systemd User=runner); the few
# privileged reset steps use passwordless sudo. Configuration is read from
# vSphere guestinfo, set per-VM at deploy time (see cicd/packer/README.md):
#   guestinfo.runner_repo    https://github.com/<owner>/<repo>
#   GitHub App (preferred) — all three required:
#     guestinfo.runner_app_id               the App's ID
#     guestinfo.runner_app_installation_id  the App's installation ID on the repo
#     guestinfo.runner_app_private_key      the App PEM, base64-encoded (base64 -w0)
#   guestinfo.runner_pat     PAT fallback (repo "Administration: read+write")
#   guestinfo.runner_name    (optional; defaults to the hostname)
#   guestinfo.runner_labels  (optional; defaults to self-hosted,linux,x64)
set -uo pipefail

RUNNER_DIR="/home/runner/actions-runner"
DATA="/data"
WORK="${DATA}/work"
CACHE="${DATA}/cache"

# Cache size caps (the data disk is only ~18 GB, shared between work/ and cache/
# and Docker). Tunable via the systemd unit's Environment= if you grow the disk.
CCACHE_MAX="${CCACHE_MAX:-3G}"
VCPKG_ARCHIVES_CAP_GB="${VCPKG_ARCHIVES_CAP_GB:-3}"
VCPKG_DOWNLOADS_CAP_GB="${VCPKG_DOWNLOADS_CAP_GB:-2}"
SONAR_CACHE_CAP_GB="${SONAR_CACHE_CAP_GB:-1}"

# Deterministic PATH for jobs: ccache shims first, then CMake (/usr/local/bin)
# and the apt toolchain (/usr/bin). Mirrors what `svc.sh install` used to capture.
export PATH="/usr/lib/ccache:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*"; }
# Read a guestinfo key. Falls back to sudo so a setup where the non-root runner
# user can't reach the vmtoolsd RPC still registers (rather than silently
# defaulting). An unset key returns empty either way -> the caller's default applies.
gi() {
    local v
    v="$(vmtoolsd --cmd "info-get guestinfo.$1" 2>/dev/null | tr -d '\r')"
    [ -z "$v" ] && v="$(sudo -n vmtoolsd --cmd "info-get guestinfo.$1" 2>/dev/null | tr -d '\r')"
    printf '%s' "$v"
}

prune_dir_to_cap() {
    local dir="$1" cap_gb="$2"
    [ -d "$dir" ] || return 0
    local cap=$(( cap_gb * 1024 * 1024 ))   # KiB
    local used
    used=$(du -sk "$dir" 2>/dev/null | cut -f1)
    [ -n "${used:-}" ] || return 0
    [ "$used" -le "$cap" ] && return 0
    log "pruning $dir (${used}KiB > ${cap}KiB cap), least-recently-used first"
    find "$dir" -type f -printf '%A@ %p\n' 2>/dev/null | sort -n | cut -d' ' -f2- | \
    while IFS= read -r f; do
        used=$(du -sk "$dir" 2>/dev/null | cut -f1)
        [ "${used:-0}" -le "$cap" ] && break
        rm -f "$f" 2>/dev/null || true
    done
}

prune_vcpkg_cache() {
    # Host (Ubuntu-26.04) binary cache, the release glibc-2.36 (bookworm) cache, and
    # the downloaded source tarballs — all persist on the data volume and would
    # otherwise grow unbounded.
    prune_dir_to_cap "${CACHE}/vcpkg/archives" "$VCPKG_ARCHIVES_CAP_GB"
    prune_dir_to_cap "${CACHE}/vcpkg-bookworm" "$VCPKG_ARCHIVES_CAP_GB"
    prune_dir_to_cap "${CACHE}/vcpkg/downloads" "$VCPKG_DOWNLOADS_CAP_GB"
    # sonar-scanner's JRE/plugin cache (~/.sonar -> /data/cache/sonar, see
    # 00-data-volume.sh) — old scanner/JRE versions accumulate forever otherwise.
    prune_dir_to_cap "${CACHE}/sonar" "$SONAR_CACHE_CAP_GB"
}

prune_old_runner_versions() {
    # The runner agent self-updates by laying down versioned bin.<ver>/
    # externals.<ver> dirs next to the live ones and repointing itself;
    # superseded versions (~680 MB each) are never removed and creep the OS
    # disk. Delete every versioned dir that isn't what the live bin/externals
    # resolve to. Safe here: a self-update only stages between job assignment
    # and job start, never while the supervisor is resetting between jobs.
    local keep="" link d
    for link in "${RUNNER_DIR}/bin" "${RUNNER_DIR}/externals"; do
        keep="${keep} $(readlink -f "$link" 2>/dev/null)"
    done
    for d in "${RUNNER_DIR}"/bin.* "${RUNNER_DIR}"/externals.*; do
        [ -d "$d" ] || continue
        case " $keep " in *" $(readlink -f "$d") "*) continue ;; esac
        log "removing superseded runner agent dir ${d##*/}"
        rm -rf "$d" 2>/dev/null || true
    done
}

reset_workspace() {
    log "resetting workspace + transient cruft"
    # Recreate the work dir clean AND runner-owned. The wipe must run as root
    # because a job can leave root-owned files inside (the release-packaging
    # Docker container writes into the checkout as root). But /data is root:root
    # — 00-data-volume.sh only chowns work/ and cache/, not the mount point
    # itself — so a plain non-sudo `mkdir` cannot recreate the `work` entry under
    # it: it fails with EACCES, and because this heredoc runs `set -uo pipefail`
    # (no -e) the failure is silent. The work dir is then missing/unwritable and
    # the next job dies at worker init with "Access to the path '/data/work' is
    # denied". So recreate it with sudo and hand ownership back to the runner
    # user that runs config.sh and the job worker. `install -d` is idempotent: it
    # also re-applies owner/group/mode if a failed wipe left the dir behind.
    sudo rm -rf "${WORK:?}" 2>/dev/null || true
    sudo install -d -o runner -g runner -m 755 "$WORK"
    # Drop the last job's Docker images/containers/volumes/build cache.
    docker system prune -af --volumes >/dev/null 2>&1 \
        || sudo docker system prune -af --volumes >/dev/null 2>&1 || true
    # Transient temp dirs.
    sudo rm -rf /tmp/* /var/tmp/* >/dev/null 2>&1 || true
    # The runner's _diag logs live on the OS disk and accumulate per job; the
    # previous job's are already uploaded, so clear them to bound OS-disk growth.
    rm -rf "${RUNNER_DIR}/_diag"/* >/dev/null 2>&1 || true
    # Superseded self-updated agent versions also live on the OS disk.
    prune_old_runner_versions
    # apt hygiene: downloaded .debs and list metadata sit on the OS disk and
    # creep between recycles (lists alone reached ~144 MB). Safe to wipe the
    # lists — every workflow that installs packages runs `apt-get update` first.
    sudo apt-get clean >/dev/null 2>&1 || true
    sudo rm -rf /var/lib/apt/lists/* >/dev/null 2>&1 || true
    # Preserve caches but bound their size.
    command -v ccache >/dev/null 2>&1 && ccache --max-size="$CCACHE_MAX" >/dev/null 2>&1 || true
    prune_vcpkg_cache
    # Reclaim freed blocks so the thin vmdk stays near actual usage.
    sudo fstrim -av >/dev/null 2>&1 || true
}

# --- token minting ---------------------------------------------------------
b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }

# Build a short-lived (<=10 min) RS256 App JWT signed with the App private key.
app_jwt() {
    local key_file="$1" now header payload unsigned sig
    now="$(date +%s)"
    header='{"alg":"RS256","typ":"JWT"}'
    payload="{\"iat\":$((now - 60)),\"exp\":$((now + 540)),\"iss\":\"${APP_ID}\"}"
    unsigned="$(printf '%s' "$header" | b64url).$(printf '%s' "$payload" | b64url)"
    sig="$(printf '%s' "$unsigned" | openssl dgst -sha256 -sign "$key_file" 2>/dev/null | b64url)"
    printf '%s.%s' "$unsigned" "$sig"
}

# Obtain a runner registration token. Prefers the GitHub App (JWT -> installation
# token -> registration token); falls back to the PAT.
get_reg_token() {
    local auth jwt key_file
    if [ "$have_app" -eq 1 ]; then
        key_file="$(mktemp)"; chmod 600 "$key_file"
        printf '%s' "$APP_KEY_B64" | base64 -d > "$key_file" 2>/dev/null
        jwt="$(app_jwt "$key_file")"
        auth="$(curl -fsSL -X POST \
            -H "Authorization: Bearer ${jwt}" \
            -H "Accept: application/vnd.github+json" \
            -H "X-GitHub-Api-Version: 2022-11-28" \
            "https://api.github.com/app/installations/${APP_INSTALL_ID}/access_tokens" \
            | jq -r '.token // empty')"
        rm -f "$key_file"
        [ -z "${auth:-}" ] && { log "App installation-token request failed"; return 0; }
    else
        auth="$RUNNER_PAT"
    fi
    curl -fsSL -X POST \
        -H "Authorization: Bearer ${auth}" \
        -H "Accept: application/vnd.github+json" \
        -H "X-GitHub-Api-Version: 2022-11-28" \
        "https://api.github.com/repos/${OWNER_REPO}/actions/runners/registration-token" \
        | jq -r '.token // empty'
}

REPO_URL="$(gi runner_repo)"
NAME="$(gi runner_name)";     NAME="${NAME:-$(hostname)}"
LABELS="$(gi runner_labels)"; LABELS="${LABELS:-self-hosted,linux,x64}"

# Credentials for minting registration tokens. Preferred: a GitHub App (short-
# lived, scoped, revocable) — set runner_app_id + runner_app_installation_id +
# runner_app_private_key (the App's PEM, base64-encoded so it survives guestinfo).
# Fallback: a fine-grained PAT in runner_pat.
APP_ID="$(gi runner_app_id)"
APP_INSTALL_ID="$(gi runner_app_installation_id)"
APP_KEY_B64="$(gi runner_app_private_key)"
RUNNER_PAT="$(gi runner_pat)"

have_app=0
[ -n "${APP_ID:-}" ] && [ -n "${APP_INSTALL_ID:-}" ] && [ -n "${APP_KEY_B64:-}" ] && have_app=1

if [ -z "${REPO_URL:-}" ] || { [ "$have_app" -ne 1 ] && [ -z "${RUNNER_PAT:-}" ]; }; then
    log "FATAL: need guestinfo.runner_repo and either the runner_app_* trio or runner_pat; sleeping 5m"
    sleep 300
    exit 1
fi

# owner/repo slug for the REST API.
OWNER_REPO="$(printf '%s' "$REPO_URL" | sed -E 's#^https?://github.com/##; s#/+$##; s#\.git$##')"

# Unique hostname (keeps cloned VMs from colliding on a DHCP client-id).
sudo hostnamectl set-hostname "$NAME" 2>/dev/null || true

# Refuse to run if the data volume isn't mounted — otherwise work/ and cache/
# would silently land on the OS disk and reintroduce the exhaustion bug.
if ! mountpoint -q "$DATA"; then
    log "FATAL: ${DATA} is not mounted; refusing to run"
    sleep 60
    exit 1
fi

cd "$RUNNER_DIR" || { log "FATAL: ${RUNNER_DIR} missing"; exit 1; }
log "ephemeral supervisor started for ${OWNER_REPO} as '${NAME}' [${LABELS}]"

while true; do
    reset_workspace

    log "minting registration token"
    TOKEN="$(get_reg_token)"

    if [ -z "${TOKEN:-}" ]; then
        log "could not obtain a registration token; retrying in 30s"
        sleep 30
        continue
    fi

    # Clear any stale local config from a previous unclean exit so config.sh
    # starts fresh; --replace re-claims the same runner name on the server.
    rm -f "${RUNNER_DIR}/.runner" "${RUNNER_DIR}/.credentials" \
          "${RUNNER_DIR}/.credentials_rsaparams" 2>/dev/null || true

    log "configuring ephemeral runner"
    if ! ./config.sh \
            --url "$REPO_URL" \
            --token "$TOKEN" \
            --name "$NAME" \
            --labels "$LABELS" \
            --work "$WORK" \
            --ephemeral \
            --replace \
            --unattended; then
        log "config.sh failed; retrying in 30s"
        sleep 30
        continue
    fi

    log "running one job"
    ./run.sh || true   # with --ephemeral, run.sh exits after a single job

    log "job complete; recycling"
done
SUPERVISOR
chmod +x "${RUNNER_DIR}/run-ephemeral.sh"
chown runner:runner "${RUNNER_DIR}/run-ephemeral.sh"

# systemd unit: run the supervisor as the runner user on boot, restart forever.
cat > /etc/systemd/system/github-runner.service <<'UNIT'
[Unit]
Description=GitHub Actions ephemeral runner supervisor
After=network-online.target vmtoolsd.service docker.service data.mount
Wants=network-online.target
Requires=data.mount

[Service]
Type=simple
User=runner
Group=runner
WorkingDirectory=/home/runner/actions-runner
ExecStart=/home/runner/actions-runner/run-ephemeral.sh
Restart=always
RestartSec=10
# Jobs can run long; never time the supervisor out.
TimeoutStartSec=0

[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable github-runner.service

echo "==> GitHub Actions runner v${RUNNER_VERSION} installed to ${RUNNER_DIR}"
echo "==> Ephemeral supervisor enabled (github-runner.service); reads guestinfo.runner_repo + guestinfo.runner_pat"
