#!/usr/bin/env bashio
# shellcheck shell=bash
#
# Add-on entrypoint. Reads the Home Assistant options form (/data/options.json) and
# the injected MQTT service, assembles the app's CLI flags, and hands off to the
# base image's docker-entrypoint.sh (which owns privilege handling + process launch).
set -euo pipefail

# Run as root inside the add-on (the Home Assistant norm — simplest for serial-device
# and /data access), so skip the base image's PUID/PGID privilege drop.
export PUID=0 PGID=0

# The Matter bridge sidecar is not part of the Phase 1 add-on: Home Assistant is
# already a Matter controller, and mDNS commissioning needs host networking. Devices
# surface through MQTT discovery instead. (Matter is a later phase.)
export MATTER_ENABLED=false

# /data is the add-on's persistent volume — state written here survives restarts and
# add-on updates.
readonly DATA=/data

# Base flags: bind all interfaces on port 8099 (the ingress_port; also the opt-in
# direct port). Plain HTTP — TLS is terminated by Home Assistant's ingress proxy.
args=("--address" "0.0.0.0" "--http-port" "8099" "--disable-https")

# Auth stays OFF: ingress fronts the UI with Home Assistant's own authentication, so
# the app's identity system is left disabled (auth-mode defaults to disabled).

# ── Persistent app state (always on /data; not surfaced in the form) ────────────
# The app UI's own preferences and the instant-restart equipment cache are app state
# HA does not replace, so always persist them under /data.
args+=("--preferences-file" "${DATA}/preferences.json")
args+=("--equipment-cache-file" "${DATA}/equipment-cache.json")

# ── Serial captures (always under app_config; not surfaced in the form) ────────
# On-demand RS-485 captures (started from the web UI's Diagnostics page) default to
# <cwd>/captures INSIDE the container — persisted nowhere and reachable only with a
# Docker shell, which is exactly the workflow that most needs the file afterwards.
# Point them at the add-on's own app_config map instead: it is read-write, survives
# restarts and add-on updates, AND is user-browsable on the host at
# /app_configs/aqualink_automate/captures (/addon_configs/... before Supervisor
# 2026.07) via the Samba or File editor add-ons. Deliberately NOT /share: that is a
# cross-add-on area and would mean asking for a broader mapping than this needs.
# The Diagnostics page can also download a capture directly, so neither path needs
# a shell.
readonly CAPTURES=/config/captures
# Pre-create it purely so the folder is visible in the share before the first
# recording; the app creates it on demand anyway. Non-fatal on purpose -- `set -e`
# would otherwise turn an unwritable /config into a total start-up failure.
mkdir -p "${CAPTURES}" || bashio::log.warning "Could not pre-create ${CAPTURES}; it will be created when you start a recording."
args+=("--capture-directory" "${CAPTURES}")

# ── Serial protocol (one explicit choice: usb / rfc2217 / rawtcp) ───────────────
# serial_protocol picks local (--serial-port) vs a network serial-to-ethernet adapter
# (--remote-serial-port + the transport flag); serial_port is the device path or
# host:port. Both are required. uart:true maps host TTY devices into the container so
# /dev/... paths resolve.
if ! bashio::config.has_value 'serial_port'; then
    bashio::exit.nok "serial_port is required — for USB set the device path (e.g. /dev/serial/by-id/...); for a network adapter set the address (host:port)."
fi
serial_port="$(bashio::config 'serial_port')"
case "$(bashio::config 'serial_protocol')" in
    usb)
        args+=("--serial-port" "${serial_port}")
        ;;
    rfc2217)
        args+=("--remote-serial-port" "${serial_port}" "--rfc2217")
        ;;
    rawtcp)
        args+=("--remote-serial-port" "${serial_port}" "--rawtcp")
        ;;
esac

# ── Equipment / Jandy (advanced) ───────────────────────────────────────────────
args+=("--pool-configuration" "$(bashio::config 'pool_configuration')")
# jandy_device_type=auto (default): omit --jandy-device-type so the app stands up its
# full default emulation set (OneTouch + IAQ + SerialAdapter). A specific type restricts
# emulation to that one device; jandy_device_id optionally overrides its bus address
# (blank = the type's default, e.g. OneTouch -> 0x41).
jandy_device_type="$(bashio::config 'jandy_device_type')"
if [ "${jandy_device_type}" != "auto" ]; then
    args+=("--jandy-device-type" "${jandy_device_type}")
    if bashio::config.has_value 'jandy_device_id'; then
        args+=("--jandy-device-id" "$(bashio::config 'jandy_device_id')")
    fi
fi

# ── MQTT ───────────────────────────────────────────────────────────────────────
mqtt_mode="$(bashio::config 'mqtt_mode')"
if [ "${mqtt_mode}" = "auto" ]; then
    if bashio::services.available 'mqtt'; then
        args+=("--mqtt")
        args+=("--mqtt-host" "$(bashio::services 'mqtt' 'host')")
        args+=("--mqtt-port" "$(bashio::services 'mqtt' 'port')")
        if bashio::var.has_value "$(bashio::services 'mqtt' 'username')"; then
            args+=("--mqtt-username" "$(bashio::services 'mqtt' 'username')")
            args+=("--mqtt-password" "$(bashio::services 'mqtt' 'password')")
        fi
        if [ "$(bashio::services 'mqtt' 'ssl')" = "true" ]; then
            args+=("--mqtt-tls")
        fi
        bashio::log.info "MQTT auto-configured from the Home Assistant broker."
    else
        bashio::log.warning "mqtt_mode=auto but no MQTT service is available; running without MQTT."
    fi
elif [ "${mqtt_mode}" = "manual" ]; then
    if ! bashio::config.has_value 'mqtt_host'; then
        bashio::exit.nok "mqtt_mode is 'manual' but mqtt_host is empty — enter the broker hostname or IP (or switch mqtt_mode to 'auto' to use the Home Assistant broker)."
    fi
    args+=("--mqtt" "--mqtt-host" "$(bashio::config 'mqtt_host')" "--mqtt-port" "$(bashio::config 'mqtt_port')")
    if bashio::config.has_value 'mqtt_username'; then
        args+=("--mqtt-username" "$(bashio::config 'mqtt_username')")
        args+=("--mqtt-password" "$(bashio::config 'mqtt_password')")
    fi
    if bashio::config.true 'mqtt_tls'; then
        args+=("--mqtt-tls")
    fi
fi

# MQTT TLS trust material — applies whenever the broker connection is TLS (auto broker
# advertising ssl, or manual + mqtt_tls). Certificate options are FILENAMES you place in
# Home Assistant's /ssl share (mounted read-only). --mqtt-client-cert/-key are mutually
# required by the app (set both or neither).
if [ "${mqtt_mode}" != "disabled" ]; then
    if bashio::config.true 'mqtt_tls_skip_verify'; then
        args+=("--mqtt-tls-skip-verify")
    fi
    if bashio::config.has_value 'mqtt_ca_cert'; then
        args+=("--mqtt-ca-cert" "/ssl/$(bashio::config 'mqtt_ca_cert')")
    fi
    if bashio::config.has_value 'mqtt_client_cert'; then
        args+=("--mqtt-client-cert" "/ssl/$(bashio::config 'mqtt_client_cert')")
    fi
    if bashio::config.has_value 'mqtt_client_key'; then
        args+=("--mqtt-client-key" "/ssl/$(bashio::config 'mqtt_client_key')")
    fi
fi

# Home Assistant MQTT discovery rides on top of MQTT (never valid when disabled).
if [ "${mqtt_mode}" != "disabled" ] && bashio::config.true 'home_assistant_discovery'; then
    # Stable, unique device identity. HA groups all discovered entities under one
    # device keyed by --ha-device-id. Persist a random id to /data on first boot so it
    # stays constant across restarts AND add-on updates (unlike anything derived from
    # the serial endpoint or container state, which would orphan the device on change),
    # and is unique per install so two add-on instances on one broker never collide.
    # Regenerates only if /data is wiped (a full uninstall).
    ha_device_id_file="${DATA}/ha-device-id"
    if [ ! -s "${ha_device_id_file}" ]; then
        printf 'aqualink_%s\n' "$(tr -d '-' < /proc/sys/kernel/random/uuid | cut -c1-12)" > "${ha_device_id_file}"
        bashio::log.warning "Generating a new Home Assistant device identity (no ${ha_device_id_file} found). If this is a fresh install, ignore this. If you're re-adding this add-on after a reinstall, host migration, or switching between the stable and edge channels, your previous Aqualink entities are now orphaned in Home Assistant — remove the old 'aqualink-automate' device(s) under Settings -> Devices & Services -> Devices to stop stale 'offline' entities."
    fi
    args+=("--home-assistant" "--ha-device-id" "$(cat "${ha_device_id_file}")")
fi

# ── Home Assistant companion package (opt-in) ────────────────────────────────
# Installs the blueprints already bundled in this image (built from
# homeassistant/companion/ — see docs/homeassistant-companion.md) directly into
# Home Assistant's own blueprints/ folder, so they appear under Settings ->
# Automations & Scenes -> Blueprints without a manual import. Off by default:
# turning it on requires the homeassistant_config map (see config.yaml), a
# read-write view of Home Assistant's own /config. Idempotent — a restart or
# add-on update just re-syncs the current files — and additive only: it never
# touches configuration.yaml, and never removes a blueprint you've deleted.
if bashio::config.true 'install_companion_package'; then
    companion_blueprints="/opt/aqualink-automate/share/aqualink-automate/web/homeassistant/blueprints"
    # The Supervisor mounts an unqualified map `type:` at /<type-name> by default
    # (https://developers.home-assistant.io/docs/apps/configuration/#map)  —
    # confirmed against the same rule this add-on already relies on for `ssl`
    # (mounted at /ssl, see the mqtt_ca_cert/mqtt_client_cert/mqtt_client_key
    # handling above) — so `homeassistant_config` mounts at
    # /homeassistant_config, NOT /homeassistant.
    ha_config="/homeassistant_config"
    if [ ! -d "${ha_config}" ]; then
        bashio::log.warning "install_companion_package is on but ${ha_config} is not mounted (is the homeassistant_config map missing?) — skipping."
    elif [ ! -d "${companion_blueprints}" ]; then
        bashio::log.warning "No companion blueprints found in this image at ${companion_blueprints} — skipping."
    else
        mkdir -p "${ha_config}/blueprints"
        cp -r "${companion_blueprints}/." "${ha_config}/blueprints/"
        bashio::log.info "Installed the Home Assistant companion blueprints into ${ha_config}/blueprints/."
    fi
fi

# ── History (opt-in; default OFF → use HA's Recorder) ──────────────────────────
if bashio::config.true 'enable_history'; then
    args+=("--history-db" "${DATA}/history.db")
    bashio::log.info "App history enabled (${DATA}/history.db)."
fi

# ── Scheduler (opt-in; default OFF → use HA automations) ───────────────────────
if bashio::config.true 'enable_scheduler'; then
    args+=("--schedules-file" "${DATA}/schedules.json")
    bashio::log.info "App scheduler enabled (${DATA}/schedules.json)."
fi

# ── Web UI authentication (opt-in; default OFF → ingress provides login) ────────
# Enable this to protect the UI when you publish the direct LAN port (the direct
# port is otherwise unauthenticated), or if you want app-level users via ingress.
# The admin is bootstrapped on first start (a no-op once /data/auth has users); the
# password goes via env, not the command line, so it never shows in the process list.
# /api/health stays unauthenticated, so the watchdog keeps working.
if bashio::config.true 'enable_auth'; then
    auth_password="$(bashio::config 'auth_password')"
    # The app rejects a bootstrap-admin password shorter than 12 characters, which
    # would silently leave auth enabled with NO admin — a login wall nobody can pass.
    # Fail fast here instead so the reason is obvious.
    if [ "${#auth_password}" -lt 12 ]; then
        bashio::exit.nok "enable_auth is on but auth_password is missing or too short — use at least 12 characters."
    fi
    mkdir -p "${DATA}/auth"
    args+=("--auth-mode" "enabled" "--auth-state-dir" "${DATA}/auth")
    args+=("--bootstrap-admin" "$(bashio::config 'auth_username')")
    export AQUALINK_BOOTSTRAP_ADMIN_PASSWORD="${auth_password}"
    bashio::log.info "Web UI authentication enabled (admin: $(bashio::config 'auth_username'))."
fi

# ── Log level ──────────────────────────────────────────────────────────────────
# Sinks stay at the default console sink so the add-on Log tab captures stdout.
case "$(bashio::config 'log_level')" in
    debug) args+=("--debug") ;;
    trace) args+=("--trace") ;;
esac

bashio::log.info "Starting aqualink-automate..."
exec /usr/local/bin/docker-entrypoint.sh "${args[@]}"
