.. zephyr:board:: hifive_premier_p550_mcu

Overview
********

The SiFive HiFive Premier P550 board features an ARM Cortex-M4 based STM32F407XE MCU
with a wide range of connectivity support and configurations.

Hardware
********

SiFive HiFive Premier P550 board provides the following hardware components:

- Quad core Sifive P550 RISC-V processor (Unused here)
- STM32F407XE microcontroller
- ARM reg 32-bit Cortex reg -M4 CPU with FPU
- 168 MHz max CPU frequency
- VDD from 1.8 V to 3.6 V
- 8MHz system crystal
- 32.768KHz RTC crystal
- JTAG/SWD header
- 512 kB Flash
- 192+4 KB SRAM including 64-Kbyte of core coupled memory
- GPIO with external interrupt capability

Supported Features
==================

.. zephyr:board-supported-hw::

System Clock
============

The System Clock could be driven by internal or external oscillator,
as well as main PLL clock. By default System clock is driven by PLL clock
at 168MHz, driven by 8MHz high speed external clock.

Serial Port
===========

The Zephyr console output is assigned to UART1. Default settings are 115200 8N1.

Programming and Debugging
*************************

.. zephyr:board-supported-runners::

Applications for the ``hifive_premier_p550_mcu`` board configuration can be built and
flashed in the usual way (see :ref:`build_an_application` and
:ref:`application_run` for more details).

Flashing
========

SiFive HiFive Premier board includes an ST-LINK/V2 embedded debug tool interface.
This interface is supported by the openocd version included in Zephyr SDK.

Flashing an application to SiFive HiFive Premier STM32
------------------------------------------------------

Here is an example for the :zephyr:code-sample:`blinky` application.

Build and flash the application:

.. zephyr-app-commands::
   :zephyr-app: samples/basic/blinky
   :board: hifive_premier_p550_mcu
   :goals: build flash

Debugging
=========

You can debug an application in the usual way.  Here is an example for the
:zephyr:code-sample:`hello_world` application.

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: hifive_premier_p550_mcu
   :maybe-skip-config:
   :goals: debug
