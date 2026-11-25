## Atlantis EVB MCU Documentation

The STM32F746ZGT6 BMC on the Tenstorrent Atlantis EVB, the same MCU circuit
as the SLT run from the ATX standby rail. The board definition, pin map and
bring-up notes are in
[tenstorrent/atlantis_evb_mcu/doc](tenstorrent/atlantis_evb_mcu/doc/index.rst).
The port has so far run only on a
NUCLEO-F756ZG stand-in, not on an Atlantis board; see Known issues and the
bring-up checklist in the hardware reference.

### BMC serial console

The MCU console (UART8) is channel C of the FT4232H behind the USB-C debug
port and header J86.

### Host serial consoles

Two consoles come through the board's UART muxes, which the MCU points
at itself. USART3 carries SoC UART1, the OpenSBI/Linux console: TCP port
22 and the web UI's Host Console. USART2 carries SoC UART0, the RCPU
boot ROM and HILT console at 9600 baud (set ``current-speed`` on
``usart2`` to 115200 once the OTP option is fused): TCP port 23 and the
RCPU Console. The TCP ports are raw; use ``nc``, not ``telnet``.

### Building
```
cd zephyr
west build -p --sysbuild -b atlantis_evb_mcu ../wallabmc
```

### Flashing

```
west flash --runner openocd
```

Also see instructions on
[flashing the Atlantis EVB](tenstorrent/atlantis_evb_mcu/support/README.md)
