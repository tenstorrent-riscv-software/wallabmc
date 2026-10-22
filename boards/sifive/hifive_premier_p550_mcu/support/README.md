# HiFive Premier P550 Debug & Flashing

The P550 debug USB-C port exposes an FTDI FT4232H with four channels:

| Channel | Interface | Linux | macOS | Function |
|---------|-----------|-------|-------|----------|
| if00 | SoC JTAG | `/dev/ttyUSB0` | `/dev/tty.usbserial-*00` | EIC7700X RISC-V debug |
| if01 | BMC JTAG | `/dev/ttyUSB1` | `/dev/tty.usbserial-*01` | STM32F407 flash/debug |
| if02 | SoC UART | `/dev/ttyUSB2` | `/dev/tty.usbserial-*02` | Host serial console (115200 8N1) |
| if03 | BMC UART | `/dev/ttyUSB3` | `/dev/tty.usbserial-*03` | Zephyr shell (115200 8N1) |

## OpenOCD Configuration

`p550_openocd.cfg` supports two targets via a `TARGET` variable:

- **`bmc`** (default) — STM32F407 on FTDI channel 1
- **`soc`** — EIC7700X RISC-V on FTDI channel 0

The target defaults to `bmc` if not specified.

## Prerequisites

```bash
# macOS
brew install open-ocd

# Fedora
sudo dnf install openocd gdb

# Ubuntu/Debian
sudo apt-get install openocd gdb-multiarch
```

## Flash BMC firmware

From a local build:

```bash
openocd -f boards/sifive/hifive_premier_p550_mcu/support/p550_openocd.cfg \
  -c "init; halt; \
      program build/mcuboot/zephyr/zephyr.hex verify; \
      program build/wallabmc/zephyr/zephyr.signed.hex verify; \
      reset; exit"
```

From CI artifacts (after extracting the firmware zip):

```bash
openocd -f boards/sifive/hifive_premier_p550_mcu/support/p550_openocd.cfg \
  -c "init; halt; \
      program zephyr.hex verify; \
      program zephyr.signed.hex verify; \
      reset; exit"
```

## Debug the RISC-V SoC

Start OpenOCD targeting the SoC (4 P550 cores):

```bash
openocd -c 'set TARGET soc' \
  -f boards/sifive/hifive_premier_p550_mcu/support/p550_openocd.cfg
```

In a separate terminal, connect GDB:

```bash
gdb
(gdb) target extended-remote localhost:3333
(gdb) info threads       # List all 4 harts
(gdb) monitor reset halt # Reset and halt
(gdb) x/4i $pc           # Disassemble at PC
```

For source-level kernel debugging, point GDB at the debuginfo tree:

```gdb
(gdb) set debug-file-directory /path/to/debuginfo/usr/lib/debug
(gdb) set sysroot /path/to/debuginfo
```

## Serial Console

Monitor the host boot or interact with the BMC shell:

```bash
# Host serial console (SoC UART, channel C)
picocom -b 115200 /dev/ttyUSB2       # Linux
screen /dev/tty.usbserial-*02 115200  # macOS

# BMC shell (MCU UART, channel D)
picocom -b 115200 /dev/ttyUSB3       # Linux
screen /dev/tty.usbserial-*03 115200  # macOS
```

## Troubleshooting

**BMC won't halt (DAP WAIT stalls):**

The flash may be read-protected. Unlock it first (erases all flash):

```bash
sudo openocd -f boards/sifive/hifive_premier_p550_mcu/support/p550_openocd.cfg \
  -c "init; halt; stm32f4x unlock 0; reset; exit"
```

Then flash normally.

**USB permission issues:** Close serial console sessions before flashing.
The FTDI kernel driver claims all channels. On macOS, `sudo` may be required.
On Linux, add your user to the `dialout` group:

```bash
sudo usermod -a -G dialout $(whoami)
# Re-login required
```

## Restore SiFive vendor firmware

```bash
openocd -f boards/sifive/hifive_premier_p550_mcu/support/p550_openocd.cfg \
  -c "init; halt; program STM32F407VET6_BMC.elf verify; reset; exit"
```
