# RS-485 connectivity and wiring

*For pool owners wiring a server to an Aqualink RS control panel. This covers the physical RS-485 link and the CLI options that select it. Full option detail lives in [Configuration reference](configuration.md).*

Before Aqualink Automate can talk to a pool controller, you need a physical RS-485 connection between the control panel and the server running the software. This document explains the two supported transports, the command-line options that select them, and how to wire the adapter to the panel.

**Note:** Both supported RS-485 protocols — Jandy/Zodiac and Pentair — ride the same physical connection. The transport is protocol-agnostic; the software auto-detects which protocol is on the bus. See the [supported-equipment summary](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/README.md#features) for the equipment covered.

## Contents

- [Serial connectivity overview](#serial-connectivity-overview)
- [Connecting a USB-2-RS485 adapter](#connecting-a-usb-2-rs485-adapter)
- [Connecting a serial-over-ethernet adapter](#connecting-a-serial-over-ethernet-adapter)
- [Serial connection guide — the wires](#serial-connection-guide--the-wires)
- [Wiring and cabling recommendations](#wiring-and-cabling-recommendations)

## Serial connectivity overview

There are two ways to connect the server to the RS Aqualink panel:

- **USB-RS485 adapter** — the cheapest and most direct option. A USB-to-RS485 adapter plugs into the server over USB and into the panel's RS-485 terminals over wire. The software talks to it through a local serial port (`--serial-port`).
- **Serial-over-ethernet adapter** — for installations where the server is far from the panel. A serial-to-ethernet adapter sits at the panel, and the server reaches it over the network (`--remote-serial-port`). This is useful when you cannot run a USB cable the full distance.

The two transports map to two implementations in the code: `PhysicalSerialPortImpl` for the local USB serial port, and `NetworkSerialPortImpl` for the TCP/RFC2217 network port (RFC2217 itself is handled by a separate protocol component). You select one or the other on the command line; they are mutually exclusive — passing both `--serial-port` and `--remote-serial-port` is rejected as a conflict.

In both cases the line is configured at **9600 baud, 8 data bits, no parity, 1 stop bit, no flow control (9600 8N1)**. These settings are fixed in the code to match the Aqualink RS bus and are not user-tunable.

Config-file keys are the option long name without the leading dashes. For example, the CLI flag `--serial-port` is written `serial-port` in a config file, and `--remote-serial-port` is written `remote-serial-port`.

## Connecting a USB-2-RS485 adapter

Use `--serial-port` (short form `-s`) to point the software at the local serial device the adapter enumerates as.

| Option | Short | Type | Default | Description |
|--------|-------|------|---------|-------------|
| `--serial-port` | `-s` | string | `/dev/ttyS0` (POSIX), `COM1` (Windows) | Local serial device for the RS-485 connection. |

**Note:** The default is a platform value resolved at runtime: `/dev/ttyS0` on Linux and macOS, `COM1` on Windows. These are placeholder defaults — a real USB-RS485 adapter usually enumerates as `/dev/ttyUSB0` on Linux or a `COMn` port (for example `COM3`) on Windows, so you will almost always set this explicitly.

A `--baudrate` option exists but is currently ignored — the bus is always opened at 9600 8N1, no flow control (`serial_initialise.cpp` hardcodes it and never reads the setting). Parity, data bits, stop bits and flow control have no options at all.

### Worked examples

Linux:

```bash
aqualink-automate --serial-port /dev/ttyUSB0   # typical USB-RS485 device on Linux
```

Windows:

```powershell
aqualink-automate --serial-port COM3   # the COM port your adapter enumerated as
```

The equivalent config file (`examples/config-serial.conf`):

```ini
# Physical RS-485 serial port
serial-port = /dev/ttyUSB0
```

Run it with:

```bash
aqualink-automate --config config-serial.conf
```

## Connecting a serial-over-ethernet adapter

Use `--remote-serial-port` (short form `-r`) with a `host:port` target to reach a serial-to-ethernet adapter over the network. When `--remote-serial-port` is set to a non-empty value, the software uses the network transport instead of a local serial port.

| Option | Short | Type | Default | Description |
|--------|-------|------|---------|-------------|
| `--remote-serial-port` | `-r` | string | _(unset)_ | Remote serial endpoint as `host:port`. Selects the network transport when non-empty. |
| `--rfc2217` | | flag | on by default (only applies to remote ports) | Use the RFC2217 telnet serial protocol. |
| `--no-rfc2217` | | flag | off | Disable RFC2217 and use a plain socket. |
| `--rawtcp` | | flag | off | Use raw TCP for the remote serial port. |

Behavior:

- A remote port uses **RFC2217 by default**. Many serial-to-ethernet adapters speak RFC2217, which carries serial line settings over the connection.
- Pass `--no-rfc2217` to fall back to a plain socket if your adapter does not support RFC2217.
- Pass `--rawtcp` to use raw TCP.
- These choices conflict with each other. `--rfc2217` cannot be combined with `--rawtcp` or `--no-rfc2217`, and `--no-rfc2217` cannot be combined with `--rawtcp`; the software rejects contradictory combinations at startup.

### Worked examples

RFC2217 (the default — shown explicitly here):

```bash
aqualink-automate --remote-serial-port 192.168.1.50:8899 --rfc2217
```

Plain socket (adapter without RFC2217 support):

```bash
aqualink-automate --remote-serial-port 192.168.1.50:8899 --no-rfc2217
```

Raw TCP:

```bash
aqualink-automate --remote-serial-port 192.168.1.50:8899 --rawtcp
```

The equivalent config file (`examples/config-network.conf`):

```ini
# Remote serial (host:port)
remote-serial-port = 192.168.1.50:8899
rfc2217 = true
```

Run it with:

```bash
aqualink-automate --config config-network.conf
```

To capture traffic from a live network connection for later replay, see [Recording and replay](RECORD_REPLAY.md).

## Serial connection guide — the wires

**Important:** This section is hardware-installation field guidance based on common RS Aqualink installations. It is not derived from the software and the software cannot verify your wiring. Always consult your pool equipment manual for the labeling specific to your panel.

1. **Identifying connections:** The simplest configuration is to connect the Data+ and Data- from a USB-2-RS485 adapter to the Data+ and Data- lines of the RS-485 interface of the RS Aqualink panel.
2. **Cabling:** Standard RS485 pool equipment cable includes 4 wires — Data+, Data-, Power+, and Ground-. Wire colors may vary depending on your installation, and labeling has evolved over the years. We recommend consulting your pool equipment manual for accurate information.
3. **Connecting to the control panel:** The control panel usually has a 4-post connector at the top right (some might have 2). This is the RS485 interface. Although the wires will be labeled, they won't indicate their functionality. The most common configuration is Green, Yellow, Black, Red.

Note that some USB adapters might have 3, 4, or even 6 connections. The third connection is typically used for ground (which can be crucial or problematic depending on your specific setup), and the fourth one is typically used for power and should not be used with USB-2-RS485 adapters.

RS Aqualink wiring most commonly uses the Green, Yellow, Black, Red color configuration. These wires are found at the top of the RS Aqualink panel. When following the standard color configuration, the table below shows the typical installation:

| Wire colour  | USB-2-RS485 pin | Description | Function                  |
|--------------|-----------------|-------------|---------------------------|
| Black        | Pin A           | Data+       | Data signaling line (+ve) |
| Yellow/White | Pin B           | Data-       | Data signaling line (-ve) |
| Green        | GND             | Ground      | Common/shared ground line |
| Red          | Not Used        | Power       | Not Used                  |

Note that the ground may be considered optional, as RS-485 is a differential signal (no common ground is required). Adding that third wire between the server and panel may still be done to limit the common-mode voltage and help with noise suppression.

## Wiring and cabling recommendations

**Important:** This section is hardware-installation field guidance, not derived from the software.

The wiring and connections to the adapter and panel are critical to getting a good signal. Many issues with poor message checksums — that is, lots of rejected messages — usually occur due to wiring problems. The recommended wire is 24AWG twisted pair; below is a paragraph taken from [RS Cable Selection](http://www.bb-elec.com/Learning-Center/All-White-Papers/Serial/Cable-Selection-for-RS422-and-RS485-Systems/Cable-Selection-for-RS-422-and-RS-485-Systems.PDF):

> The RS-422 specification recommends 24AWG twisted pair cable with a shunt capacitance of 16 pF per foot and 100 ohm characteristic impedance. While the RS-485 specification does not specify cabling, these recommendations should be used for RS-485 systems as well.

## See also

- [Configuration reference](configuration.md) — full detail on every serial and runtime option.
- [Server and automation host](hardware-server-automation.md) — the host that runs Aqualink Automate.
- [Recording and replay](RECORD_REPLAY.md) — capture bus traffic from a live connection.
- [Supported equipment](https://github.com/iainchesworthlabs/aqualink-automate/blob/main/README.md#features) — Jandy/Zodiac and Pentair coverage.
