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

# Base flags: bind all interfaces on port 80 (mapped to the host by config.yaml
# `ports:`), plain HTTP (TLS is terminated/handled by Home Assistant in Phase 2).
args=("--address" "0.0.0.0" "--http-port" "80" "--disable-https")

# ── Serial transport ───────────────────────────────────────────────────────────
serial_mode="$(bashio::config 'serial_mode')"
if [ "${serial_mode}" = "usb" ]; then
    if bashio::config.has_value 'serial_port'; then
        args+=("--serial-port" "$(bashio::config 'serial_port')")
    else
        bashio::exit.nok "serial_mode is 'usb' but no serial_port was selected."
    fi
else
    if bashio::config.has_value 'remote_serial_port'; then
        args+=("--remote-serial-port" "$(bashio::config 'remote_serial_port')")
        case "$(bashio::config 'remote_protocol')" in
            rfc2217) args+=("--rfc2217") ;;
            rawtcp)  args+=("--rawtcp") ;;
            plain)   args+=("--no-rfc2217") ;;
        esac
    else
        bashio::exit.nok "serial_mode is 'network' but no remote_serial_port was set."
    fi
fi

# ── Jandy device emulation (advanced) ──────────────────────────────────────────
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

# Home Assistant MQTT discovery rides on top of MQTT (never valid when disabled).
if [ "${mqtt_mode}" != "disabled" ] && bashio::config.true 'home_assistant_discovery'; then
    args+=("--home-assistant")
fi

# ── HTTP API auth ──────────────────────────────────────────────────────────────
if bashio::config.has_value 'api_auth_token'; then
    args+=("--api-auth-token" "$(bashio::config 'api_auth_token')")
fi

# ── Log level ──────────────────────────────────────────────────────────────────
case "$(bashio::config 'log_level')" in
    debug) args+=("--debug") ;;
    trace) args+=("--trace") ;;
esac

bashio::log.info "Starting aqualink-automate..."
exec /usr/local/bin/docker-entrypoint.sh "${args[@]}"
