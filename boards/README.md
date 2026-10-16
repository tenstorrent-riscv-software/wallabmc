## Board specifics

### Seeed XIAO ESP32-C6

The ESP32-C6 (RISC-V, Wi-Fi 6) on Seeed Studio's 21×17.5 mm XIAO form
factor with a USB-C connector. The BMC console runs over the SoC's
built-in USB Serial/JTAG (the XIAO's USB-C port), with a separate UART
(UART0) used for the host serial bridge. Only three GPIOs on the
XIAO connector are needed for host power control; the host UART pads stay
clear of the default I2C bus (D4/D5) and SPI bus (D8/D9/D10) so those
remain free for other uses.

#### Wiring

Four wires plus ground between the XIAO ESP32-C6 and the host board:

| XIAO pin | GPIO | Direction | Host signal | Notes |
| --- | --- | --- | --- | --- |
| D6 | GPIO16 | in | host UART RX | UART0 TX — host serial console (115200 8N1) |
| D7 | GPIO17 | out | host UART TX | UART0 RX — host serial console (115200 8N1) |
| D1 | GPIO1 | out (open-drain) | PWRBTN# input | open-drain, active-low |
| D2 | GPIO2 | out (open-drain) | SYSRESET# input | open-drain, active-low |
| GND | — | — | GND | Common reference, required |

> **Voltage warning.** ESP32-C6 GPIOs are **not** 5 V tolerant. The
> SYSRESET# pin is driven open-drain, so it only sinks — the host's
> internal pull-up sets the un-asserted level. If the host holds that
> line above 3.3 V when un-asserted, add a small N-MOSFET (gate = ESP32
> GPIO, drain = host pin, source = GND) between them. The UART0 RX
> line (D7 / GPIO17) sees the host's TX voltage directly; if the host
> UART runs above 3.3 V, level-shift it.

#### Build and flash

The XIAO uses Espressif's built-in Simple Boot rather than MCUboot,
so the build skips ``--sysbuild``. Plug the XIAO into your build host
over USB-C — the same USB cable provides power, flashes the firmware,
and carries the BMC console. Then follow the build and flashing
commands contained in XXX main README.md.

#### Host control from the shell

```
power on              # 200 ms press on GPIO1 (if BMC believes host is off)
power off             # 200 ms press on GPIO1 (if BMC believes host is on)
power force-off       # 6 s press on GPIO1
reset                 # 1 s low pulse on GPIO2 (SYSRESET#)
```

