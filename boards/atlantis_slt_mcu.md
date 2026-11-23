## Atlantis SLT MCU Documentation

The STM32F746ZGT6 BMC on the Tenstorrent Atlantis SLT board. The board
definition, pin map and bring-up notes are in
[tenstorrent/atlantis_slt_mcu/doc](tenstorrent/atlantis_slt_mcu/doc/index.rst).
The port has so far run only on a
NUCLEO-F756ZG stand-in, not on an Atlantis board; see Known issues and the
bring-up checklist in the hardware reference.

### BMC serial console

The MCU console (UART8) is channel C of the FT4232H behind the USB-C debug
port and header J70.

### Host serial console

USART2 reaches the Atlantis SoC UART0 through the board's UART mux; the
MCU drives the mux select.

### Building
```
cd zephyr
west build -p --sysbuild -b atlantis_slt_mcu ../wallabmc
```

### Flashing

```
west flash --runner openocd
```

Also see instructions on
[flashing the Atlantis SLT](tenstorrent/atlantis_slt_mcu/support/README.md)
