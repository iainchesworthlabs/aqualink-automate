# Server and hosting requirements

*For pool owners choosing where to run Aqualink Automate. This covers the host machine and operating system; the physical RS-485 wiring to the panel lives in [Hardware requirements for connectivity](hardware-rs485-connectivity.md), and step-by-step install lives in [INSTALL.md](INSTALL.md).*

Aqualink Automate runs continuously, polling the RS-485 bus and serving the web UI. This page helps you pick a machine to host it on, from a cheap single-board computer to an existing always-on server, and explains the operating-system and deployment trade-offs.

This document is about **hosting** — where the software runs. It is distinct from **connectivity** — how the host reaches the pool panel over RS-485 — which is covered separately in [Hardware requirements for connectivity](hardware-rs485-connectivity.md).

## Choosing a host

Aqualink Automate is a long-running service. It is most useful when it is always on: it tracks equipment state, publishes to MQTT/Home Assistant, and (optionally) bridges to Matter. Picking a host is mostly about one requirement and a few trade-offs.

- **Always-on.** The machine must stay powered and running. A desktop you switch off at night will miss state changes and break automations that depend on live data.
- **A free serial path to the panel.** Either a USB-to-RS485 adapter the host can reach, or a serial-over-ethernet adapter on the same network. See [connectivity](hardware-rs485-connectivity.md) for the options.
- **Low resource needs.** The application is a single native binary with a small footprint, so the CPU/RAM budget is modest. The host choices below all have enough headroom.

The options below run from cheapest to most capable. Any of them works; pick based on what you already own and whether you want Matter (which constrains the choice — see [Deployment paths](#deployment-paths)).

## Single-board computers

A single-board computer (SBC) is the cheapest dedicated host. It draws little power, runs silently, and is easy to leave on permanently next to the pool equipment.

- **Raspberry Pi Zero (or equivalent).** The lowest-cost option. It is enough to run the application and serve the web UI to your LAN. Suitable when you want a small, low-power box dedicated to pool control and nothing else.
- **Full-size Raspberry Pi (Pi 4/5 or equivalent).** More CPU, RAM, and I/O headroom. Worth it if you also want to run the Docker image with the bundled Matter bridge, or co-host other home services on the same board. The Pi has a wired Ethernet port, which is the most reliable choice for Matter (see below).

**Note:** Both run Linux, so they support the native binary *and* the Docker image, including the Matter bridge. An SBC is the natural home for a "set and forget" pool controller.

## x86 mini-PC, NAS, or existing always-on server

If you already run an always-on machine, you may not need to buy anything.

- **x86 mini-PC.** A small fanless mini-PC running Linux gives more performance than an SBC for a similar footprint, and is a good fit if you want everything in one tidy Docker stack.
- **NAS.** Many NAS devices run Docker (or a container manager). If yours does and it stays on, it can host the runtime image. Confirm your NAS can attach a USB serial adapter or reach a serial-over-ethernet adapter on the LAN.
- **Existing home server.** If you already run a Linux home server (for Home Assistant, media, etc.), add Aqualink Automate there — native or in a container — rather than buying separate hardware.

**Important:** If you want Matter and you are deploying with Docker, the host must be **Linux** and Docker must support **host networking**. NAS appliances and mini-PCs running native Linux Docker qualify; Docker Desktop on macOS/Windows does not (see [Deployment paths](#deployment-paths)).

## Operating system and footprint

Aqualink Automate is cross-platform. Native binaries are published for **Windows, Linux, and macOS** — the project's original goal was a Windows/macOS-capable variant of [AqualinkD](https://github.com/sfeakes/AqualinkD). Install packages are produced per platform:

| Platform | Package types | Best for |
|----------|---------------|----------|
| Linux | `.deb`, `.rpm`, `.tgz` | Servers and SBCs — the primary, always-on target |
| Windows | `.exe` (NSIS installer), `.zip` | Desktop use and Windows-only households |
| macOS | `.dmg`, `.tgz` | Desktop use on a Mac |

Download links and per-platform install steps are in [INSTALL.md](INSTALL.md#pre-built-binaries).

- **Linux is the primary hosting target.** It runs as a lightweight background service, supports the Docker image, and is the only platform where the Matter bridge can be commissioned. Choose Linux for any headless, always-on deployment.
- **Windows and macOS are for desktop use.** The native binaries are real and supported, which makes a desktop a fine place to run or evaluate the application. They are not ideal as a 24/7 unattended host, and they cannot commission Matter over Docker (the host-networking requirement is Linux-only).

**Footprint:** the runtime is a single native binary plus its bundled web assets and shared libraries. The Docker runtime image is based on `ubuntu:26.04` and runs the application as a non-root user (uid/gid `10000`, named `aqualink`). It bundles the Matter bridge sidecar and exposes HTTP on port `80`. A Raspberry Pi or any always-on Linux host can run it.

## Deployment paths

There are two ways to run Aqualink Automate. Full instructions for both are in [INSTALL.md](INSTALL.md); this section covers how the choice affects your host.

### Native binary

Install the package for your platform and run the binary directly. This works on Linux, Windows, and macOS, and is the simplest path for desktop use or a single SBC. The Matter bridge is a separate Node.js sidecar that is **bundled in the Docker image**, not in the native packages — so if you want Matter, use Docker on Linux.

By default the server binds to **`127.0.0.1`** (localhost only) for security. On a headless server you almost always want to reach the web UI from another machine on your LAN, so bind to all interfaces:

```bash
# Expose the web UI on the LAN (default is 127.0.0.1, localhost-only)
aqualink-automate --address 0.0.0.0 --disable-https
```

**Security:** binding to `0.0.0.0` makes the web UI reachable by anything on your network. API authentication is off by default. Run the server on a trusted network, and consider enabling the bearer-token auth (`--api-auth-token`) if you expose it more widely.

CLI options can also be supplied from a config file. Config-file keys are the option long names **without the leading dashes** — so `--address 0.0.0.0` on the command line is `address = 0.0.0.0` in the file. See the worked examples in [`examples/config-network.conf`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/examples/config-network.conf) and [`examples/config-docker.conf`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/examples/config-docker.conf), and run `aqualink-automate --help` for the full option list.

### Docker

The runtime image packages the application *and* the Matter bridge. It is the recommended path on a Linux host, especially if you want smart-home (Matter) integration.

```bash
# Build and run the production image (bridge-network port mapping).
# The image sets MATTER_ENABLED=true, so the Matter sidecar still starts — it just
# cannot be commissioned over bridge networking (see the host-networking section
# below). Add -e MATTER_ENABLED=false to skip the sidecar entirely.
docker build --target runtime -t aqualink-automate .
docker run -p 80:80 aqualink-automate
```

The container binds port `80` as the non-root `aqualink` user (uid `10000`). On most hosts the published port maps fine, but some require `CAP_NET_BIND_SERVICE` or a low `net.ipv4.ip_unprivileged_port_start` for a non-root process to bind below 1024 — or just remap the host side, e.g. `-p 8080:80`.

To pass a physical serial adapter into the container:

```bash
# Map a USB-to-RS485 adapter into the container
docker run -p 80:80 --device /dev/ttyUSB0 aqualink-automate --serial-port /dev/ttyUSB0
```

The image already runs the application with `--address 0.0.0.0 --disable-https`, so the web UI is reachable on the mapped port without extra flags.

### Host networking for Matter

Matter commissioning uses **IPv6 mDNS (UDP `5540` + `5353`)**. Docker's bridge network driver does not forward that link-local multicast traffic, so a port-mapped container cannot be paired. The container must therefore run with **host networking**:

```bash
# Compose sets network_mode: host + a persistent matter-data volume
docker compose up
```

```bash
# Equivalent plain-docker form
docker run --network host -v aqualink-matter:/data/matter aqualink-automate
```

The bundled [`docker-compose.yml`](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/docker-compose.yml) already sets `network_mode: host` and a `matter-data` volume mounted at `/data/matter`, so commissioned fabrics survive container restarts. Open the web UI's **Diagnostics → Matter** panel to scan the pairing QR.

**Important — this constrains your host choice:** host networking is **Linux-only**. On Docker Desktop (macOS/Windows), host networking is limited and Matter **cannot be commissioned from the LAN**. If you want Matter, host on Linux (an SBC, mini-PC, NAS, or server). On macOS/Windows, disable the bridge with `MATTER_ENABLED=false` (equivalently `--matter false`) and keep the normal `-p 80:80` port mapping; the application still runs, just without Matter.

For the full Matter pairing walkthrough and networking detail, see [Matter integration](MATTER.md#networking-requirement-docker).

## See also

- [Hardware requirements for connectivity](hardware-rs485-connectivity.md) — wiring the host to the pool panel over RS-485.
- [INSTALL.md](INSTALL.md) — installing pre-built binaries, building from source, and Docker.
- [Matter integration](MATTER.md) — the bundled smart-home bridge and its host-networking requirement.
