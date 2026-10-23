## Seeed Studio XIAO ESP32-C6 Documentation

The ESP32-C6 (RISC-V, Wi-Fi 6) on Seeed Studio's 21×17.5 mm XIAO form
factor with a USB-C connector. The BMC console runs over the SoC's
built-in USB Serial/JTAG (the XIAO's USB-C port), with a separate UART
(UART0) used for the host serial bridge. Only three GPIOs on the
XIAO connector are needed for host power control; the host UART pads stay
clear of the default I2C bus (D4/D5) and SPI bus (D8/D9/D10) so those
remain free for other uses.

### Building

The XIAO uses Espressif's built-in Simple Boot rather than MCUboot,
so the build skips ``--sysbuild``. Plug the XIAO into your build host
over USB-C — the same USB cable provides power, flashes the firmware,
and carries the BMC console.

```
# One-time: fetch the Espressif Wi-Fi/PHY binary blobs.
west blobs fetch hal_espressif

cd zephyr
west build -p -b xiao_esp32c6/esp32c6/hpcore ../wallabmc
```

### Flashing
```
west flash
```

Alternatively, use the `build/zephyr/zephyr.bin` file and run this
command:

```
esptool write-flash 0 zephyr.bin
```

### Running

#### Wiring

WARNING! Connecting your board to something else or itself could
damage it or components it is connected to. These are not tested
and not guaranteed to be safe or correct.

Four wires plus ground between the XIAO ESP32-C6 and the host board:

| XIAO pin | GPIO | Direction | Host signal | Notes |
| --- | --- | --- | --- | --- |
| D6 | GPIO16 | out | host UART RX | UART0 TX — host serial console (115200 8N1) |
| D7 | GPIO17 | in | host UART TX | UART0 RX — host serial console (115200 8N1) |
| D1 | GPIO1 | out (open-drain) | PWRBTN# input | open-drain, active-low |
| D2 | GPIO2 | out (open-drain) | SYSRESET# input | open-drain, active-low |
| GND | — | — | GND | Common reference, required |

> **Voltage warning.** ESP32-C6 GPIOs are **not** 5 V tolerant. The SYSRESET#
> pin is driven open-drain, so it only sinks — the host's internal pull-up sets
> the un-asserted level. If the host holds that line above 3.3 V when
> un-asserted, use a non-inverting, open-drain-compatible level shifter rated
> for both voltages, The UART0 RX line (D7 / GPIO17) sees the host's TX voltage
> directly; if the host UART runs above 3.3 V, level-shift it.

#### Optional: host power-good input

A momentary button gives the BMC no feedback, so by default it cannot
tell whether the host is on. If the host has a 3.3 V logic power-good
(PWROK) output, wire it to a spare XIAO pin, for example D3 / GPIO21,
and add a `gpio-keys` node for it to the board overlay:

```
host_status {
	compatible = "gpio-keys";
	host_pwrok: host_pwrok {
		gpios = <&gpio0 21 (GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN)>;
	};
};
```

Then add `host-power-good = &host_pwrok;` to the overlay's existing
`aliases` node. The BMC reads the logical level, so an active-low
PWROK# signal takes `(GPIO_ACTIVE_LOW | GPIO_PULL_UP)` instead; either
way the pull makes the input read "off" when no host is connected.

With it, Redfish `PowerState` reflects the host, `power on` /
`power off` / `power force-off` are skipped when the host already
reads the requested state, `reset` is refused when the host reads
off, and host auto power-on (`config host auto_poweron enable`,
Redfish `PowerRestorePolicy=AlwaysOn`) presses at BMC boot only when
the host reads off. Without it every press is sent as asked: a
`power on` or Redfish `On` sent to a running host is a shutdown
request, so check the host console first. Redfish `PowerState` is
then `null` (the web UI shows Unknown), `power state` reports unknown,
and host auto power-on is ignored, with a warning at boot, because a
press could shut down a host that is already running.

#### Host control from the shell

Without the power-good input every press is sent as asked; with it, a
press is skipped when the host already reads the requested state.

```
power on              # 200 ms press on GPIO1; a running host takes it as a shutdown request
power off             # 200 ms press on GPIO1; asks the host OS to shut down
power force-off       # 6 s press on GPIO1
power state           # on / off from the power-good input, otherwise unknown
reset                 # 1 s low pulse on GPIO2 (SYSRESET#)
```
