# Home Assistant Add-on: Aqualink Automate

Run [Aqualink Automate](https://github.com/iainchesworthlabs/aqualink-automate) as a
Home Assistant add-on — the Supervisor runs and manages the container for you, so
you get pool automation over RS-485 with native Home Assistant MQTT discovery
without ever touching Docker.

See [DOCS.md](DOCS.md) for installation and configuration.

## About this add-on

This is a thin wrapper around the multi-arch container image the project already
publishes to GHCR. The add-on image only layers Home Assistant's `bashio` helper and
a small `run.sh` that translates the add-on options form into the application's
command-line flags. It supports `aarch64` and `amd64` hosts.
