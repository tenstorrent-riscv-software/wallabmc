# Flashing the Atlantis EVB MCU

The STM32F746 SWD port is wired to channel A of the FT4232H (U42) behind
the USB-C debug port J39, and to the 4-pin header J61. The MCU console
(UART8, 115200 8N1) is FT4232H channel C and pins 1/2 of the 20-pin
header J86. The FT4232H is powered from the USB cable; the MCU runs from
the ATX standby rail, so it can be flashed with the PSU in standby.

| Header | Pins |
| --- | --- |
| J61 SWD | 1 SWCLK, 2 SWDIO, 3 3V3 (standby), 4 GND |
| J86 | 1 MCU RX, 2 MCU TX, 9/10/19/20 GND |

The BOOT0 jumper J69 must be fitted: without it the MCU boots into the ST
system bootloader instead of flash.

## Via the USB debug port

Connect a USB-C cable to J39. OpenOCD drives SWD over the FT4232H with the
board config in this directory:

```
west flash --runner openocd
```

or with the files from a build:

```
openocd -f boards/tenstorrent/atlantis_evb_mcu/support/openocd.cfg \
  -c 'init; halt; flash write_image erase build/mcuboot/zephyr/zephyr.hex; flash write_image erase build/wallabmc/zephyr/zephyr.signed.hex; reset; exit'
```

The `wallabmc-firmware-atlantis_evb_mcu` artifact of a GitHub Actions run
holds the same images as `mcuboot.hex` and `wallabmc.signed.hex`.

The board wires the FT4232H data output (ADBUS1) to SWDIO through 470R and
the data input (ADBUS2) straight to SWDIO, the usual OpenOCD "resistor
hack". The dummy `SWDIO_OE` signal in `openocd.cfg` makes the ftdi driver
switch ADBUS1 to an input while the target drives SWDIO; this path is
untested on hardware. Both FT4232H bridges on the board (U42 on J39, U73
on J71) enumerate as 0403:6011 with no serial number unless their EEPROMs
are programmed; with both on one host pick U42 with `adapter usb location`
in `openocd.cfg`, or program serial numbers and use `adapter serial`.

## Via the SWD header

Any SWD probe on J61 works with the stock OpenOCD STM32F7 target config:

```
openocd -f interface/stlink.cfg -f target/stm32f7x.cfg \
  -c 'init; halt; flash write_image erase build/mcuboot/zephyr/zephyr.hex; flash write_image erase build/wallabmc/zephyr/zephyr.signed.hex; reset; exit'
```
