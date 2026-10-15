# somd — SOM Protocol Daemon

Host-side daemon for the HiFive Premier P550 that responds to the BMC's
UART protocol. Without it, the BMC has no way to know the host OS is running.

## Background

The P550 carrier board has a dedicated UART link between the STM32 BMC
(MCU UART4, PC10/PC11) and the EIC7700X SoC (SoC UART2, 0x50920000).
The BMC sends 267-byte framed messages to query host status, read
temperatures, and coordinate shutdown/reboot. The host must respond —
otherwise the BMC marks the SOM as offline.

The original ESWIN vendor Linux image included a proprietary daemon for
this purpose. Since WallaBMC replaces the vendor MCU firmware and targets
standard Linux distributions (RHEL, Fedora) that lack the vendor daemon,
`somd` provides the host-side implementation.

For the full protocol specification (frame format, command table, checksum
algorithm), see `boards/sifive/hifive_premier_p550_mcu/doc/hardware.rst`,
section "MCU-SoC UART4 protocol".

## What it does

- Listens on `/dev/ttyS2` (SoC UART2) at 115200 8N1
- Receives 267-byte framed requests from the BMC
- Replies to `CMD_BOARD_STATUS` (0x06) — keepalive acknowledgment
- Replies to `CMD_PVT_INFO` (0x05) — CPU/NPU temperatures from sysfs
- Replies `UNSUPPORTED` to unrecognized commands

When somd is running, the BMC reports `SOM daemon state: ONLINE`.
When somd stops, the BMC detects the loss after 5 missed keepalives
(~5 seconds) and reports `SOM daemon state: OFFLINE`.

## Build

Native build on the RISC-V host (no dependencies beyond libc):

```bash
make
```

Cross-compile from another machine:

```bash
make CC=riscv64-linux-gnu-gcc
```

## Install

```bash
sudo make install
sudo systemctl enable --now somd
```

This installs the binary to `/usr/local/sbin/somd` and the systemd unit
to `/etc/systemd/system/somd.service`.

## Usage

```
somd [-d /dev/ttyS2] [-f] [-v]
  -d  Serial device (default: /dev/ttyS2)
  -f  Run in foreground (log to stderr instead of syslog)
  -v  Verbose (repeat for more detail)
```

Test interactively:

```bash
sudo systemctl stop somd
sudo ./somd -f -v -d /dev/ttyS2
```

## Serial port identification

The correct serial port depends on the kernel's device tree enumeration.
On the P550 with the ESWIN EIC7700X SoC:

| Port | Address | Function |
|------|---------|----------|
| ttyS0 | 0x50900000 | SoC UART0 — kernel console |
| ttyS1 | 0x50910000 | SoC UART1 — unused |
| ttyS2 | 0x50920000 | SoC UART2 — BMC protocol link |

Verify with: `sudo cat /proc/tty/driver/serial`
