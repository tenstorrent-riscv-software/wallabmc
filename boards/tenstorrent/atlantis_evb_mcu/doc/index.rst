.. zephyr:board:: atlantis_evb_mcu

Overview
********

The Tenstorrent Atlantis EVB carries an STM32F746ZGT6 microcontroller that
acts as the board management controller for the Atlantis SoC. This board
definition targets that MCU; the Atlantis SoC itself is not covered. The
MCU circuit is shared with the Atlantis SLT board; the EVB adds ATX power
control, M.2 slot power and SoC boot-mode control.

Hardware
********

- STM32F746ZGT6 (ARM Cortex-M7, 216 MHz, 1 MB flash, 256 KB SRAM + 64 KB DTCM)
  powered from the ATX standby rail, so the BMC runs while the host is off
- 8 MHz HSE crystal, 32.768 kHz LSE crystal, coin-cell backed RTC
- TI DP83826E 10/100 Ethernet PHY (MII), also on standby power
- MCU console UART on the FT4232H (channel C) and header J86
- USART2/USART3 to the Atlantis SoC UART0/UART1 through SN74CB3Q3253 muxes
- SWD on the FT4232H (channel A) and header J61
- TMP117 temperature sensor and M24M01 EEPROM on I2C
- GPIO control of the ATX PS_ON, the SoC power rails, reset, boot mode,
  M.2 slot power and the JTAG/UART/QSPI/SD muxes

See :doc:`hardware` for the complete pin map.

Supported Features
==================

.. zephyr:board-supported-hw::

System Clock
============

The system clock is driven by the main PLL at 216 MHz from the 8 MHz HSE
crystal. The RTC runs from LSI until the LSE crystal is verified.

Serial Port
===========

The Zephyr console is UART8 (PE1 TX, PE0 RX) at 115200 8N1, reachable through
FT4232H channel C of the USB debug port or pins 1/2 of header J86.

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

Applications for the ``atlantis_evb_mcu`` board configuration can be built and
flashed in the usual way (see :ref:`build_an_application` and
:ref:`application_run` for more details). The BOOT0 jumper J69 must be
fitted for the MCU to boot from flash.

Flashing
========

The STM32 SWD port is wired to FT4232H channel A of the USB debug connector
(J39) and to header J61. See ``support/README.md`` for OpenOCD instructions.

Flashing an application to Atlantis EVB MCU
--------------------------------------------

Here is an example for the :zephyr:code-sample:`blinky` application.

.. zephyr-app-commands::
   :zephyr-app: samples/basic/blinky
   :board: atlantis_evb_mcu
   :goals: build flash

Debugging
=========

You can debug an application in the usual way.  Here is an example for the
:zephyr:code-sample:`hello_world` application.

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: atlantis_evb_mcu
   :maybe-skip-config:
   :goals: debug
