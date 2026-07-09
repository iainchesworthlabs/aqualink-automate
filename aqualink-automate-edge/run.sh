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

# ── Serial transport (one field, auto-detected) ────────────────────────────────
# A value starting with "/" is a local device path (--serial-port); anything else is
# treated as a network address host:port (--remote-serial-port + the chosen protocol).
# uart:true maps host TTY devices into the container so /dev/... paths resolve.
if ! bashio::config.has_value 'serial_port'; then
    bashio::exit.nok "serial_port is required — set a device path (e.g. /dev/serial/by-id/...) or a network address (host:port)."
fi
serial_port="$(bashio::config 'serial_port')"
case "${serial_port}" in
    /*)
        args+=("--serial-port" "${serial_port}")
        ;;
    *)
        args+=("--remote-serial-port" "${serial_port}")
        case "$(bashio::config 'remote_protocol')" in
            rfc2217) args+=("--rfc2217") ;;
            rawtcp)  args+=("--rawtcp") ;;
            plain)   args+=("--no-rfc2217") ;;
        esac
        ;;
esac

# ── Equipment / Jandy (advanced) ───────────────────────────────────────────────
args+=("--pool-configuration" "$(bashio::config 'pool_configuration')")
args+=("--jandy-device-type" "$(bashio::config 'jandy_device_type')")
args+=("--jandy-device-id" "$(bashio::config 'jandy_device_id')")

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
    args+=("--home-assistant")
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
