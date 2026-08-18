# Running on a Raspberry Pi

Aqualink Automate runs well on a Raspberry Pi sitting next to your pool equipment,
wired to the AquaLink RS-485 bus. This page is the Pi-specific quick start; for the
full option reference see [configuration.md](configuration.md), and for non-Pi hosts
see [hardware-server-automation.md](hardware-server-automation.md).

## Which Pi

| Model | Notes |
|-------|-------|
| **Pi 5 / Pi 4** | Plenty of headroom; also fine for the Docker image with the Matter bridge. |
| **Pi 3 / Zero 2 W** | Ample for the app + web UI. The Zero 2 W (512 MB RAM) is the lowest-cost always-on option. |

Use the **64-bit (arm64) Raspberry Pi OS** (Bookworm or newer). The `.deb`/`.rpm`/`.tgz`
are built for `arm64` (and `amd64`); 32-bit Raspberry Pi OS is not currently supported.

## Install

Download the `arm64` `.deb` from the [latest release](https://github.com/iainchesworthlabs/aqualink-automate/releases)
and install it — the package brings its own runtime libraries, so it works on a stock
Raspberry Pi OS with no extra dependencies:

```bash
sudo apt install ./aqualink-automate_*_arm64.deb
```

This installs the binary, a `systemd` service, the `aqualink` service account (added to
the `dialout` group for serial access), and a default config at
`/etc/aqualink-automate/aqualink-automate.conf`. The service is **enabled on boot but not
started yet** — it needs your serial port first (below).

## Connect the RS-485 bus

You need an RS-485 transceiver between the Pi and the AquaLink bus. Two common ways:

### A. USB-RS485 adapter (easiest)

Plug in a USB-RS485 adapter (FTDI, CH340, or CP2102 based). It appears as `/dev/ttyUSB0`:

```bash
ls -l /dev/serial/by-id/      # confirm it's detected
```

Because `/dev/ttyUSB0` can renumber across reboots/replugs, pin a stable name. Copy the
shipped udev template and uncomment the line for your adapter:

```bash
sudo cp /usr/share/doc/aqualink-automate/examples/60-aqualink-automate.rules.example \
        /etc/udev/rules.d/60-aqualink-automate.rules
sudoedit /etc/udev/rules.d/60-aqualink-automate.rules     # uncomment your adapter
sudo udevadm control --reload-rules && sudo udevadm trigger
ls -l /dev/aqualink-rs485                                 # -> your adapter
```

### B. GPIO UART + RS-485 HAT (no USB port used)

If you use a MAX485/transceiver HAT on the 40-pin header, enable the **PL011** UART and
free it from the serial login console:

```bash
sudo raspi-config        # Interface Options -> Serial Port:
                         #   login shell over serial?  NO
                         #   serial hardware enabled?  YES
```

For full-rate PL011 on `/dev/ttyAMA0` (rather than the lower-quality mini-UART), add to
`/boot/firmware/config.txt`:

```
enable_uart=1
dtoverlay=disable-bt        # frees PL011 (/dev/ttyAMA0) on Pi 3/4/Zero 2 W
```

Reboot, then use `serial-port = /dev/ttyAMA0`.

## Configure and start

Set your serial device in the config, then start the service:

```bash
sudoedit /etc/aqualink-automate/aqualink-automate.conf
#   serial-port = /dev/aqualink-rs485      (or /dev/ttyUSB0, or /dev/ttyAMA0)

sudo systemctl start aqualink-automate
sudo systemctl status aqualink-automate
journalctl -u aqualink-automate -f         # live logs
```

The web UI is at `http://<pi-address>:9000`. To use port 80 instead, set
`http-port = 80` in the config (the service is granted `CAP_NET_BIND_SERVICE`, so it
does not need to run as root). If you expose the UI to your LAN, consider setting
`api-auth-token`.

## Updating / uninstalling

```bash
sudo apt install ./aqualink-automate_<newer>_arm64.deb   # upgrade (config is preserved)
sudo systemctl status aqualink-automate                   # restarts automatically on upgrade

sudo apt remove aqualink-automate                         # remove (keeps /etc config)
sudo apt purge  aqualink-automate                         # remove config + state too
```

## Troubleshooting

- **`status` shows the serial port can't be opened** — check the device path, that the
  adapter is plugged in, and that the `aqualink` user is in `dialout`
  (`id aqualink`). After adding a udev rule, re-run `udevadm trigger`.
- **UI not reachable from another machine** — the default binds `0.0.0.0`; check the
  Pi's firewall and that you used the Pi's LAN address and port (`9000` by default).
- **Logs** — everything goes to the journal: `journalctl -u aqualink-automate -e`.
