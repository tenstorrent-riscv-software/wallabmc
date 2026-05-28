.. SPDX-FileCopyrightText: © 2025-2026 Tenstorrent USA, Inc.
..
.. SPDX-License-Identifier: Apache-2.0

HiFive Premier P550 BMC Hardware Reference
*******************************************

This document maps the STM32F407VET6 MCU pins to board signals on the SiFive
HiFive Premier P550 carrier board (HF106C), based on the carrier board
schematic v3.1 (page 20 "MCU MISC" and "MCU IO FUNCTION USED LIST"), and
verified by reverse engineering the original ESWIN BMC firmware ELF
(``STM32F407VET6_BMC.elf`` from the MCU tools repo). It serves as a reference
for implementing missing BMC features in WallaBMC.

**Carrier board schematic**: `Premier P550 Carrier Board Schematic <https://www.sifive.com/document-file/premier-p550-carrier-board-schematic>`_ (v3.1)

**SOM schematic**: `HiFive Premier P550 SOM Schematic v3.0 <https://sifive.cdn.prismic.io/sifive/ZwMDL7VsGrYSwa3P_HiFivePremierP550SOMSchematicv3.0.pdf>`_

**Getting started / user guide**: `HF106 User Guide V1.2 <https://sifive.cdn.prismic.io/sifive/Z1h2p5bqstJ98RbK_HF106_user_guide_V1p2_en.pdf>`_

**MCU user manual**: `Premier P550 MCU User Manual <https://www.sifive.com/document-file/premier-p550-mcu-user-manual>`_

**MCU tools repo**: `hifive-premier-p550-tools <https://github.com/sifiveinc/hifive-premier-p550-tools>`_

MCU overview
============

- **MCU**: STM32F407VET6 (schematic designator U51A/U51B, page 20 "MCU MISC")
- ARM Cortex-M4, 168 MHz, 512 KB flash, 192+4 KB SRAM
- Powered from ATX 5V standby rails (always-on when ATX PSU has standby power)
- MCU I/O voltage: 1.8V (VDD18_MCU from SY8113B buck off 5VSB)
- RTC backup: 3.3V coin cell battery (J27) OR VDD33_VSB via BAT54CW diode

Complete STM32 pin map
======================

Source: "MCU IO FUNCTION USED LIST" table from carrier board schematic v3.1,
page 20.

Currently implemented in WallaBMC
----------------------------------

.. list-table::
   :header-rows: 1

   * - STM32 Pin
     - Signal
     - DTS node / alias
     - Description
   * - PD4
     - DCDC POWER ON (DC_EN)
     - ``dcen`` / ``power-gpio-1``
     - DC-DC converter enable (main power rails). Active HIGH.
   * - PE3
     - ATX POWER ON REQUEST
     - ``atxpson`` / ``power-gpio-2``
     - ATX PSU power-on signal. Active HIGH.
   * - PE13
     - LED_PWM3 (Blue)
     - ``blue_led`` / ``status-led``
     - RGB LED blue channel, Morse "OK" blinker
   * - PE11
     - LED_PWM2 (Green)
     - ``green_led``
     - RGB LED green channel (defined, unused by code)
   * - PE9
     - LED_PWM1 (Red)
     - ``red_led``
     - RGB LED red channel (defined, unused by code)
   * - PB1
     - RECOVERY KEY (K_REC)
     - ``user_button_recovery`` / ``sw0``
     - Recovery button. Active LOW. **Bug**: ``reset-button`` alias missing in overlay, so ``button_init()`` silently skips.
   * - PB15
     - UART MUX SEL
     - ``uartmuxsel``
     - TS5A23157 mux control. Active HIGH. (see UART mux section)
   * - PD8/PD9
     - USART3 TX/RX
     - ``usart3``
     - MCU debug console (via FT4232H ch D -> USB debug)
   * - PC6/PC7
     - USART6 TX/RX
     - ``usart6``
     - Console bridge to SoC UART0 (DMA-enabled)
   * - PC10/PC11
     - UART4 TX/RX
     - ``uart4``
     - Connected to SoC UART2. BMC-SoC protocol (som_protocol.c). Host-side daemon: host-somd/
   * - PE0
     - JTAG TCK
     - ``jtagtck``
     - SoC JTAG bit-bang clock (TCP port 7777)
   * - PE1
     - JTAG TMS
     - ``jtagtms``
     - SoC JTAG bit-bang mode select
   * - PE2
     - JTAG TDO
     - ``jtagtdo``
     - SoC JTAG bit-bang data out (input, pull-up)
   * - PE4
     - JTAG TDI
     - ``jtagtdi``
     - SoC JTAG bit-bang data in
   * - PB2
     - BOOT1
     - ``boot1``
     - STM32 BOOT1 pin (defined but unused)
   * - PA1,PA2,PA7
     - RMII + MDIO
     - ``mac``, ``mdio``
     - LAN8720A PHY at addr 0, 100 Mbit BMC Ethernet
   * - PB11-PB13
     - ETH TX
     - (part of mac)
     - Ethernet TX enable, TXD0, TXD1
   * - PC1,PC4,PC5
     - ETH RX + MDC
     - (part of mac/mdio)
     - Ethernet RXD0, RXD1, MDC
   * - PC0
     - EPHY RESETN
     - (part of mac)
     - Ethernet PHY reset. Active LOW.

Also implemented in WallaBMC
------------------------------

.. list-table::
   :header-rows: 1

   * - STM32 Pin
     - Signal
     - DTS node / alias
     - Description
   * - PD5
     - SOM WARM RESET
     - ``som_reset`` / ``reset-gpio-1``
     - SOM warm reset. Assert during power-off, release after power-good.
   * - PD6
     - SOM RESET OUT DETECT
     - ``som_rst_detect``
     - SOM reset feedback input (defined, not yet used in code).
   * - PE5
     - BUCK POWER GOOD DETECTION
     - ``pwrok`` / ``power-good``
     - DC power good input. Polled during power-on with configurable timeout.
   * - PA12
     - POWER ON KEY
     - ``power_button`` / ``power-button``
     - Front-panel power button. Toggles power state on press (graceful shutdown if SOM protocol available).
   * - PD0-PD3
     - SOM BOOT MODE CTRL 0-3
     - ``bootsel0``-``bootsel3`` / ``bootsel-0``-``bootsel-3``
     - Boot source selection. Hardware mode (follow DIP switch) or software mode (MCU drives).
   * - PD12/PD13
     - TIM4 CH1/CH2 PWM
     - ``fan0``/``fan1`` (via ``pwm4``)
     - Fan PWM speed control (25 kHz). Shell: ``fan set <0|1> <0-100>``.
   * - PD10
     - POWER LED
     - ``pwr_led`` / ``power-led``
     - Front-panel power LED. On when host power is on.
   * - PD11
     - SLEEP LED
     - ``slp_led`` / ``sleep-led``
     - Front-panel sleep LED. On when host power is off (standby).
   * - PA3
     - I2C_MUX_EN
     - ``i2c_mux_en`` / ``i2c-mux-en``
     - TMUX1574 mux control. Enabled before EEPROM access.
   * - PC8
     - EEPROM WRITE PROTECT
     - ``eeprom_wp`` / ``eeprom-wp``
     - EEPROM write-protect control.
   * - PB6/PB7
     - I2C1 SCL/SDA
     - ``&i2c1``
     - AT24C02C EEPROM (carrier board identity, MACs). Deferred init.
   * - PA8/PC9
     - I2C3 SCL/SDA
     - ``&i2c3``
     - INA226 power monitor (12V rail). Deferred init.

Hardware reference
===================

EIC7700X boot modes (PD0-PD3)
------------------------------

Implemented in ``bootsel.c``. Shell: ``bootsel get``, ``bootsel set <0-15|hw>``.

The MCU drives BOOT_SEL[3:0] (PD0-PD3) to select the SoC boot source.
In hardware mode, pins are high-impedance inputs following the DIP switch (SW1).
In software mode, the MCU drives the pins as outputs.

When OTP security bit = 1 (only lower 2 bits matter):

.. list-table::
   :header-rows: 1

   * - SEL[3:0]
     - Boot CPU
     - First Boot
     - Second Boot
   * - xx00
     - SCPU
     - ROM
     - UART
   * - xx01
     - SCPU
     - ROM
     - eMMC
   * - xx10
     - SCPU
     - ROM
     - SPI NOR
   * - xx11
     - SCPU
     - ROM
     - USB

When OTP security bit = 0 (all 4 bits used):

.. list-table::
   :header-rows: 1

   * - SEL[3:0]
     - Boot CPU
     - First Boot
     - Second Boot
   * - 0000
     - SCPU
     - ROM
     - UART
   * - 0001
     - SCPU
     - ROM
     - eMMC
   * - 0010
     - SCPU
     - ROM
     - SPI NOR
   * - 0011
     - SCPU
     - ROM
     - USB
   * - 0100
     - SCPU
     - SPI NOR
     - UART
   * - 0101
     - SCPU
     - SPI NOR
     - eMMC
   * - 0110
     - SCPU
     - SPI NOR
     - SPI NOR
   * - 0111
     - SCPU
     - SPI NOR
     - USB
   * - 1x00
     - U84
     - SPI NOR
     - UART
   * - 1x01
     - U84
     - SPI NOR
     - eMMC
   * - 1x10
     - U84
     - SPI NOR
     - SPI NOR
   * - 1x11
     - U84
     - SPI NOR
     - USB

I2C bus architecture
---------------------

**I2C3** (PA8 SCL, PC9 SDA) — dedicated to carrier board peripherals:

.. list-table::
   :header-rows: 1

   * - Device
     - Address
     - Description
   * - INA226 power monitor
     - 0x44
     - 12V input rail. Implemented in ``power_monitor.c`` (deferred init).

**I2C1** (PB6 SCL, PB7 SDA) — shared bus (MCU / SoC / FT4232H):

.. list-table::
   :header-rows: 1

   * - Device
     - Address
     - Description
   * - AT24C02C EEPROM
     - 0x50
     - Carrier board identity (serial, MACs). Implemented in ``board_identity.c`` (deferred init, vendor CRC32).

The I2C1 bus is shared via a TMUX1574 mux (U77). PA3 (I2C_MUX_EN) must be
driven to route the bus to the MCU before EEPROM access. PC8 (EEPROM_WP)
controls write-protect (HIGH = protected).

Fan control reference
----------------------

PWM implemented in ``fan.c``. Shell: ``fan get``, ``fan set <0|1> <0-100>``.

PD12 (TIM4_CH1) drives the SOM fan, PD13 (TIM4_CH2) drives the chassis fan.
25 kHz PWM, 0-100% duty cycle.

Tachometer inputs PE6 (CHASS_FAN_TACH1) and PB14 (CHASS_FAN_TACH2) are
defined in the schematic but not yet implemented in code.

NOT yet implemented
--------------------

SPI to SoC (SPI2: PB9, PB10, PC2, PC3)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - STM32 Pin
     - Signal (from schematic)
     - Direction
     - Description
   * - **PB9**
     - MCU SPI2 NSS
     - Output
     - SPI2 chip select to SOM
   * - **PB10**
     - MCU SPI2 SCK
     - Output
     - SPI2 clock
   * - **PC2**
     - MCU SPI2 MISO
     - Input
     - SPI2 MISO from SOM
   * - **PC3**
     - MCU SPI2 MOSI
     - Output
     - SPI2 MOSI to SOM

Direct SPI bus between MCU and SoC through the SOM edge connector. PB9
(SPI2_CS) is single-purpose -- connected straight to the SOM connector and
fed into the main EIC7700X SoC. Could be used for high-bandwidth MCU-to-SoC
communication (e.g., firmware updates, IPMI/MCTP transport). Lower priority
-- requires SoC-side driver support.

SPI1 / onboard flash (PA4, PA5, PA6, PB5) - optional
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - STM32 Pin
     - Signal (from schematic)
     - Direction
     - Description
   * - **PA4**
     - MCU SPI1 NSS
     - Output
     - SPI1 chip select (to W25Q32 flash, U52)
   * - **PA5**
     - MCU SPI1 SCK
     - Output
     - SPI1 clock
   * - **PA6**
     - MCU SPI1 MISO
     - Input
     - SPI1 MISO
   * - **PB5**
     - MCU SPI1 MOSI
     - Output
     - SPI1 MOSI

The W25Q32 (32 Mbit / 4 MB SPI flash) footprint exists on the carrier board
but is **not mounted** by default. If populated, could provide additional
storage for BMC firmware, logs, or configuration beyond the 512 KB internal
flash.

Other/misc GPIO (PE15)
~~~~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - STM32 Pin
     - Signal (from schematic)
     - Direction
     - Description
   * - **PE15**
     - DC_POWER_EN1
     - Output
     - Duplicates the DC_EN signal (same as PD4) through a not-mounted 0-ohm resistor. On DVB-2 boards this is a second DC enable output. In practice the 0R is NM so this pin has no effect and does not need to be driven.

FT4232H channel mapping and debug USB
======================================

The FTDI FT4232HL (U46) connects via USB-C debug port (J23) and provides 4
independent channels, each appearing as a separate USB serial/JTAG interface:

.. list-table::
   :header-rows: 1

   * - Channel
     - STM32 Pins
     - Function
     - Notes
   * - **A** (ADBUS)
     - \- (to SoC)
     - SoC JTAG
     - SoC JTAG via TMUX1574 mux (U70). Jumper J53 selects between FT4232H or MIPI-20 connector (J22/J59).
   * - **B** (BDBUS)
     - PA13, PA14, PA15, PB3
     - MCU JTAG/SWD
     - Direct connection to STM32 JTAG/SWD. Used by OpenOCD for BMC flashing. Level-shifted via TXB0108 (U45) between 3.3V and 1.8V.
   * - **C** (CDBUS)
     - via mux to PC6/PC7
     - UART (muxed)
     - Goes through TS5A23157 mux (U64). UART_MUX_SEL selects MCU USART6 or SoC UART0.
   * - **D** (DDBUS)
     - PD8/PD9
     - MCU console UART
     - Direct connection to STM32 USART3. This is the Zephyr shell.

The FT4232H also provides I2C via BCBUS4/BCBUS5, connected through a PCA9306
level shifter (U49) to the shared EEPROM I2C bus (I2C1).

UART mux architecture
======================

**Channel C UART mux (U64, TS5A23157)**:

- ``UART_MUX_SEL = 0`` (PB15 LOW, default): FT4232 ch C talks directly to SoC UART0
- ``UART_MUX_SEL = 1`` (PB15 HIGH): FT4232 ch C talks to MCU USART6

When a network client connects to the console bridge (TCP port 22), WallaBMC
sets UART_MUX_SEL=1 so that:

- TCP client <-> MCU USART6 <-> (via mux) <-> SoC UART0

When no TCP client is connected (MUX_SEL=0):

- USB debug ch C <-> (direct) <-> SoC UART0

This means the USB debug UART channel C is disconnected from the SoC during
a console bridge TCP session. This is by design - it prevents contention
between USB and network console users.

Power architecture and sequencing
==================================

The MCU controls the board's power-on sequence:

#. MCU is always powered from ATX 5V standby (VDD_5VSB -> VDD18_MCU via SY8113B buck)
#. On ``power on``:

   - Assert **DC_EN** (PD4 HIGH) -> enables main DC-DC converters
     (SQ20056RAC for 5V/3.3V carrier, SY81012VDC for 5V SOM/SYS)
   - Assert **ATX_PS_ON** (PE3 HIGH) -> tells ATX PSU to turn on main 12V

#. Monitor **DCDC_PWR_OK** (PE5) -> goes HIGH when power rails are stable
#. On ``power off``: deassert both DC_EN and ATX_PS_ON
#. **Auto power-on jumper** J61 (2-pin) can force power-on at ATX standby

**Original firmware power-on sequence** (from ``hf_power_task`` reverse engineering):

#. Assert **ATX_PS_ON** (PE3 HIGH) first
#. Assert **DC_EN** (PD4 HIGH)
#. Poll **DCDC_PWR_OK** (PE5) up to 10 times with 200ms delays (2 second timeout)
#. If DC good: init I2C3, check carrier board EEPROM info, release SOM reset (PD5 HIGH)
#. Set MCU LED to green, chassis power LED on / sleep LED off
#. If DC good timeout: abort power-on, go back to off state

**Original firmware power-off sequence** (from ``set_power_off``):

#. Assert SOM reset (PD5 LOW via ``som_reset_control(1)``)
#. Delay 10ms
#. DC power off (PD4 LOW)
#. Delay 10ms
#. ATX power off (PE3 LOW)
#. Delay 10ms
#. Set MCU LED to red, power LED off / sleep LED on

**Original firmware SOM reset behavior** (from ``som_reset_control``):

- Assert reset: PD5=LOW, reconfigure as output, deinit UART4+USART6, PB9=LOW
- Release reset: PD5=HIGH, reconfigure as IT_FALLING (interrupt input to detect
  SoC-initiated resets), reinit UART4+USART6, PB9=HIGH, LED=green, chassis LED=power on

**Current WallaBMC implementation gaps**:

- Does not monitor DCDC_PWR_OK (PE5) to confirm power-on success
- Does not sequence ATX_PS_ON before DC_EN (both asserted simultaneously)
- Does not hold SOM in reset during power-on, nor release it after DC good
- Does not drive front panel LEDs (PD10/PD11) to reflect power state
- Does not detect power button presses (PA12)
- Does not detect SoC-initiated resets via PD6 interrupt
- Does not deinit/reinit UARTs during SOM reset
- ``power_get_state()`` returns software-requested state, not actual hardware state

Known bugs in current DTS/overlay
===================================

#. **Missing reset-button alias**: The P550 overlay does not define
   ``reset-button = &user_button_recovery``. The DTS defines the
   ``user_button_recovery`` node and the ``sw0`` alias, but ``button.c`` looks for
   ``DT_ALIAS(reset_button)``. This means the recovery button **does nothing**
   on P550. Fix: add ``reset-button = &user_button_recovery;`` to the overlay
   aliases section.

#. **JTAG node compatible**: Both P550 and nucleo overlays use
   ``compatible = "gpio-leds"`` for the JTAG node with a comment "What is a
   better setting for this?". This works because ``gpio-leds`` provides the
   ``gpios`` property, but is semantically incorrect. A custom compatible like
   ``"gpio-jtag"`` or just removing the compatible and using raw GPIO nodes
   would be cleaner.

#. **JTAG pins "not connected for now"**: Comment in overlay suggests the SoC
   JTAG bit-bang pins (PE0-PE4) are not physically wired. The schematic does
   show these STM32 pins routed out, but the SoC JTAG chain also goes through
   a separate TMUX1574 mux (U70) to the FT4232H channel A. It is unclear
   whether the MCU bit-bang path connects to the same JTAG chain or is a
   separate debug interface. Hardware verification needed.

Carrier board EEPROM (AT24C02C on I2C1)
=========================================

The AT24C02C EEPROM at I2C1 address 0x50 stores carrier board identity and MAC
addresses. The original firmware stores two copies for redundancy.

EEPROM layout
--------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Description
   * - 0x00-0x32
     - 51
     - Primary board info record
   * - 0x50-0x82
     - 51
     - Backup board info record (identical format)

Board info record structure (51 bytes)
---------------------------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Type
     - Field
     - CLI name
     - Factory default
   * - 0
     - 4
     - uint32_t LE
     - magicNumber
     - ``magic``
     - 0x45505EF1
   * - 4
     - 1
     - uint8_t
     - formatVersionNumber
     - ``format``
     - 0x03
   * - 5
     - 2
     - uint16_t LE
     - productIdentifier
     - ``productid``
     - 0x0004
   * - 7
     - 1
     - uint8_t
     - pcbRevision
     - ``pcbr``
     - 0x10
   * - 8
     - 1
     - uint8_t
     - bomRevision
     - ``bomr``
     - 0x10
   * - 9
     - 1
     - uint8_t
     - bomVariant
     - ``bomv``
     - 0x10
   * - 10
     - 18
     - char[18]
     - boardSerialNumber
     - ``boardsn``
     - (manufacturing)
   * - 28
     - 1
     - uint8_t
     - manufacturingTestStatus
     - ``manu``
     - 0x00
   * - 29
     - 6
     - uint8_t[6]
     - MAC0 (SoC ETH0)
     - ``setmac 0``
     - (manufacturing)
   * - 35
     - 6
     - uint8_t[6]
     - MAC1 (SoC ETH1)
     - ``setmac 1``
     - (manufacturing)
   * - 41
     - 6
     - uint8_t[6]
     - MCU MAC (BMC ETH)
     - ``setmac 2``
     - (manufacturing)
   * - 47
     - 4
     - uint32_t LE
     - CRC32 checksum
     - \-
     - (computed)

CRC32 is standard CRC-32 (same polynomial as Ethernet/zlib) computed over
bytes 0-46, then bitwise inverted (``~crc``). The original firmware function
``hf_crc32()`` initializes CRC to 0, runs a table-based CRC, then inverts.

EEPROM access requirements
----------------------------

#. **I2C mux**: Assert PA3 (I2C_MUX_EN) to route I2C1 bus to MCU before
   any EEPROM access. The bus is shared with SoC (I2C10) and FT4232H.
#. **Write protect**: PC8 must be driven LOW to allow writes. The original
   firmware initializes PC8=HIGH (protected) at boot and only lowers it
   during explicit write operations.
#. **Mutex**: The original firmware uses a FreeRTOS mutex (``gEEPROM_Mutex``
   at 0x20000584) to serialize access.
#. **Dual copy**: On boot, ``es_check_carrier_board_info`` reads primary
   (offset 0x00), validates CRC32. If corrupt, falls back to backup
   (offset 0x50). If both corrupt, restores factory defaults.

Original firmware cbinfo-g output format
-----------------------------------------

::

   [Carrierboard Information:]
   magicNumber:0x45505ef1
   formatVersionNumber:0x3
   productIdentifier:0x4
   pcbRevision:0x10
   bomRevision:0x10
   bomVariant:0x10
   SN:HF106C-XXXXX
   manufacturingTestStatus:0x0

Original firmware setmac command
---------------------------------

::

   setmac <index,0-2> <mac,like a1:26:39:91:b0:22>

- Index 0: SoC ETH0 MAC (at EEPROM offset 29)
- Index 1: SoC ETH1 MAC (at EEPROM offset 35)
- Index 2: BMC MCU MAC (at EEPROM offset 41)

After setting, both primary and backup EEPROM records are updated with new CRC.
The original firmware prints: ``The MAC setting will be valid after rebooting the carrier board!!!``

WallaBMC implementation notes
------------------------------

A ``boardinfo`` shell command should:

#. Enable I2C1 in device tree (PB6 SCL, PB7 SDA)
#. Configure PA3 as output for I2C mux control
#. Configure PC8 as output for EEPROM write protect
#. Read 51 bytes from EEPROM at I2C addr 0x50, offset 0x00
#. Validate CRC32; fallback to offset 0x50 if primary corrupt
#. Parse and display fields per the struct above
#. The MCU MAC could optionally be used to set the BMC Ethernet MAC
   address at boot instead of using the compiled-in default

MCU-SoC UART4 protocol
========================

The MCU and SoC communicate over UART4 (PC10 TX, PC11 RX, 115200 8N1) using
a fixed-length binary protocol. This is used to query SoC status, get SoM
board info, read temperatures, and exchange keepalive messages.

The protocol is defined in the `public OpenSBI patch for EIC7700
<https://lore.kernel.org/opensbi/20251218104243.562667-8-ganboing@gmail.com/>`_,
which is the authoritative reference for field names and command types used
below.

Packet format (267 bytes, 0x10B)
---------------------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Field
     - Description
   * - 0-3
     - 4
     - magic
     - ``55 AA 5A A5``
   * - 4-7
     - 4
     - xTaskToNotify
     - Task ID (used by FreeRTOS on MCU side to match replies)
   * - 8
     - 1
     - msg_type
     - Message type (see message types below)
   * - 9
     - 1
     - cmd_type
     - Command type enum (see command table below)
   * - 10
     - 1
     - cmd_result
     - Result code: ``0x00`` = success, ``0x01`` = error, ``0x02`` = invalid, ``0x03`` = not supported
   * - 11
     - 1
     - data_len
     - Payload data length in bytes (0-250)
   * - 12-261
     - 250
     - payload
     - Request/response payload, zero-padded
   * - 262
     - 1
     - checksum
     - XOR of bytes [8, 9, 11] XOR payload[0..N-1]
   * - 263-266
     - 4
     - tail
     - ``BA BD BA BD``

Both request and response use the same 267-byte frame format. The SoC runs a
matching daemon that receives on its UART2 and responds in the same format.
See ``host-somd/`` for WallaBMC's implementation of this daemon.

Message types
--------------

.. list-table::
   :header-rows: 1

   * - Value
     - Name
     - Description
   * - 0x01
     - REQUEST
     - Request from MCU to SoC
   * - 0x02
     - REPLY
     - Reply from SoC to MCU
   * - 0x03
     - NOTIFY
     - Asynchronous notification

Checksum calculation
---------------------

.. code-block:: c

   uint8_t checksum = packet[8] ^ packet[9] ^ packet[11];
   for (int i = 0; i < data_len; i++)
       checksum ^= packet[12 + i];
   packet[262] = checksum;

Where ``data_len`` is the value of ``packet[11]``. Since the payload is zero-padded
to 250 bytes, iterating over all 250 bytes (as OpenSBI does) produces the same
result as XORing zero bytes is a no-op.

Command table
--------------

.. list-table::
   :header-rows: 1

   * - cmd_type
     - Name
     - Direction
     - Payload size
     - Description
   * - 0x00
     - Keepalive
     - MCU->SoC->MCU
     - 0
     - SoC daemon liveness check. Sent periodically (every 6 seconds via ``SomRestartTimer``). If no response after 5 seconds, SoC is considered down.
   * - 0x01
     - POWER_OFF
     - SoC->MCU
     - 0
     - Power off the board
   * - 0x02
     - REBOOT
     - ?
     - ?
     - Warm reboot
   * - 0x03
     - READ_BOARD_INFO
     - ?
     - 33
     - Get SoM board identity (same struct as EEPROM)
   * - 0x04
     - CONTROL_LED
     - ?
     - ?
     - LED control
   * - 0x05
     - PVT_INFO
     - ?
     - 12
     - Get CPU temp, NPU temp, and fan speed from SoC
   * - 0x06
     - BOARD_STATUS
     - ?
     - ?
     - Board power status
   * - 0x07
     - POWER_INFO
     - ?
     - ?
     - Detailed power info
   * - 0x08
     - RESTART
     - ?
     - 0
     - Cold reboot (power cycle)

Response handling
------------------

The response arrives asynchronously via UART4 RX interrupt
(``HAL_UARTEx_RxEventCallback``). The ``uart4_protocol_task`` matches responses to
pending requests using a linked list and notifies the waiting task via
FreeRTOS task notifications. Timeout is configurable per command (typically
1000ms).

Response status is at offset 0x130 in the response context:

- ``0x00`` = success, response data follows at offset 0x131
- ``0xFF`` = no response / timeout
- Other values = error codes

READ_BOARD_INFO command (cmd_type=3)
--------------------------------------

Sends cmd_type=3 (READ_BOARD_INFO) with data_len=33 to SoC, receives 33
bytes of board info in the same format as the EEPROM carrier board record
(but from the SoM's own storage):

::

   [Somboard Information:]
   magicNumber:0x...
   version:0x...
   id:0x...
   pcb:0x...
   bom_revision:0x...
   bom_variant:0x...
   SN:...
   status:0x...

The struct matches the first 33 bytes of the carrier board record (magic
through manufacturingTestStatus), without the MAC addresses or CRC.

**Requires**: SoC must be powered on and running the UART protocol daemon.
If SoC is off, ``web_cmd_handle`` returns error code 1 immediately.

PVT_INFO command (cmd_type=5)
-------------------------------

Sends cmd_type=5 (PVT_INFO) with data_len=12 to SoC, receives 12 bytes:

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Type
     - Field
   * - 0
     - 4
     - int32_t
     - cpu_temp (millidegrees Celsius)
   * - 4
     - 4
     - int32_t
     - npu_temp (millidegrees Celsius)
   * - 8
     - 4
     - int32_t
     - fan_speed (RPM)

Original firmware display format::

   cpu_temp(Celsius):XX.X  npu_temp(Celsius):XX.X  fan_speed(rpm):XXXX

Conversion: ``degrees = raw / 1000``, ``fraction = raw % 1000``.

SoC keepalive mechanism
------------------------

The original firmware runs a keepalive timer (``SomRestartTimer``, 6 second
interval). It sends a request with cmd_type=0 to the SoC. If no response is
received within 5 seconds (``SOM_STATUS_CHECK_STATE`` timer), the SoC is
considered unresponsive. The keepalive state is exposed via ``somwork`` CLI
command (``Som Work Status: ...``).

WallaBMC implementation notes
------------------------------

To implement SoC communication:

#. UART4 is already enabled in the DTS (PC10/PC11, 115200 baud) but unused
   by WallaBMC code
#. Implement the 267-byte packet framing with magic/checksum/tail
#. Add async TX/RX with timeout (similar to console_bridge pattern)
#. Register shell commands: ``sominfo``, ``temp`` (or integrate into Redfish)
#. Optionally implement keepalive for SoC liveness monitoring
#. The SoC-side daemon must be running (part of the SoC's Linux userspace)

Original firmware CLI command reference
=========================================

Complete list of CLI commands from the original ESWIN firmware, extracted from
string table and symbol analysis:

Power management
-----------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``sompower-g``
     - Get SOM power status ("ON" or "OFF")
   * - ``sompower-s``
     - Set SOM power on/off
   * - ``powerlost-g``
     - Get power-lost resume attribute ("enabled"/"disabled"). When enabled, SoC auto-powers-on after ATX power restore.
   * - ``powerlost-s``
     - Set power-lost resume (enable/disable auto restart after power loss)

SoC console routing
--------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``somconsole-g``
     - Get SoC console routing ("UART" or "Telnet")
   * - ``somconsole-s``
     - Set SoC console routing: ``0`` = via UART (FT4232H ch C), ``1`` = via Telnet (MCU bridges network to USART6). Controls UART_MUX_SEL (PB15).

Boot configuration
-------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``bootsel-g``
     - Get software boot select configuration (shows current BOOT_SEL[3:0] and whether HW or SW controlled)
   * - ``bootsel-s``
     - Set software boot select. When SW-controlled, MCU drives PD0-PD3 as outputs. When HW-controlled, MCU sets PD0-PD3 as inputs (follows DIP switch SW1).

Board identity (carrier board EEPROM)
--------------------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``cbinfo-g``
     - Display carrier board information from EEPROM (magic, format, product ID, PCB/BOM revision, serial number, test status)
   * - ``cbinfo-s``
     - Set carrier board EEPROM fields: ``cbinfo-s <field> <hex_value>``. Fields: magic, format, productid, pcbr, bomr, bomv, boardsn, manu
   * - ``eepromwp-s``
     - Set EEPROM write protect: ``0`` = disabled (writable), ``1`` = enabled (protected). Controls PC8.
   * - ``setmac``
     - Set MAC address: ``setmac <0-2> <aa:bb:cc:dd:ee:ff>``. Index 0=SoC ETH0, 1=SoC ETH1, 2=BMC MCU.

SoC status (requires SoC powered on)
--------------------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``sominfo``
     - Display SoM board information (magic, version, ID, PCB/BOM, serial, status). Queries SoC via UART4 cmd 0x21.
   * - ``somwork``
     - Get SoC kernel work status ("Normal"/"Abnormal"). Based on keepalive responses.
   * - ``temp-g``
     - Get CPU temperature, NPU temperature, and fan speed from SoC via UART4 cmd 0x0C.

System
-------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``version``
     - Display BMC firmware version (format: ``version:0x%x``)
   * - ``reboot``
     - Reboot the BMC MCU

Appendix: Original firmware verification
==========================================

The original ESWIN firmware (``STM32F407VET6_BMC.elf`` from
https://github.com/sifiveinc/hifive-premier-p550-tools) is not stripped and
contains full symbol names. The following confirms pin assignments by
cross-referencing function disassembly against GPIO register addresses.

GPIO port base addresses (from MX_GPIO_Init literal pool)
----------------------------------------------------------

.. list-table::
   :header-rows: 1

   * - Address
     - Port
   * - 0x40020000
     - GPIOA
   * - 0x40020400
     - GPIOB
   * - 0x40020800
     - GPIOC
   * - 0x40020C00
     - GPIOD
   * - 0x40021000
     - GPIOE

Pin verification from named functions
--------------------------------------

.. list-table::
   :header-rows: 1

   * - Function
     - Port (from disasm)
     - Pin bitmask
     - GPIO Pin
     - Confirmed
   * - ``atx_power_on``
     - 0x40021000 (GPIOE)
     - 0x08
     - **PE3**
     - ATX power control
   * - ``dc_power_on``
     - 0x40020C00 (GPIOD)
     - 0x10
     - **PD4**
     - DC power control
   * - ``get_dc_power_status``
     - 0x40021000 (GPIOE)
     - 0x20
     - **PE5**
     - DC power good input
   * - ``power_led_on``
     - 0x40020C00 (GPIOD)
     - 0x400
     - **PD10**
     - Chassis power LED
   * - ``sleep_led_on``
     - 0x40020C00 (GPIOD)
     - 0x800
     - **PD11**
     - Chassis sleep LED
   * - ``som_reset_control``
     - 0x40020C00 (GPIOD)
     - 0x20
     - **PD5**
     - SOM warm reset
   * - ``set_bootsel``
     - 0x40020C00 (GPIOD)
     - 0x0F
     - **PD0-PD3**
     - Boot mode select
   * - ``get_key_status``
     - 0x40020000 (GPIOA)
     - 0x1000
     - **PA12**
     - Power button
   * - ``get_user_key_status``
     - 0x40020400 (GPIOB)
     - 0x02
     - **PB1**
     - Recovery button
   * - ``prvCommandCBWpEEPROM``
     - 0x40020800 (GPIOC)
     - 0x100
     - **PC8**
     - EEPROM write protect

MX_GPIO_Init initial pin states
--------------------------------

.. list-table::
   :header-rows: 1

   * - Port.Pin
     - Initial State
     - Mode
     - Notes
   * - PE3, PE15
     - LOW
     - Output PP
     - ATX off, PE15 off at init
   * - PC0
     - LOW
     - Output PP
     - Ethernet PHY reset asserted
   * - PB15
     - LOW
     - Output PP
     - UART mux = SoC direct
   * - PD4, PD5, PD10
     - LOW
     - Output PP
     - DC off, SOM reset asserted, power LED off
   * - PD11
     - HIGH
     - Output PP
     - Sleep LED on at init
   * - PC8
     - HIGH
     - Output PP
     - EEPROM write-protected at init
   * - PD0-PD3
     - HIGH
     - Output PP
     - Boot select all high at init
   * - PA4
     - HIGH
     - Output PP
     - SPI1 NSS deselected
   * - PA12
     - \-
     - IT_FALLING
     - Power button interrupt
   * - PD6
     - \-
     - IT_FALLING
     - SOM reset feedback interrupt
   * - PB1
     - \-
     - IT_FALLING
     - Recovery button interrupt
   * - PE5
     - \-
     - Input
     - DC power good (polled, not interrupt)

NVIC interrupt mapping
-----------------------

.. list-table::
   :header-rows: 1

   * - IRQ
     - EXTI Line
     - GPIO Pin
     - Handler
   * - 7
     - EXTI1
     - PB1
     - Recovery button (10s hold = factory reset)
   * - 23
     - EXTI9_5
     - PD6
     - SOM reset feedback
   * - 40
     - EXTI15_10
     - PA12
     - Power button (short=toggle, 4s hold=force off)

Timer PWM configuration (from HAL_TIM_MspPostInit)
----------------------------------------------------

.. list-table::
   :header-rows: 1

   * - Timer
     - Peripheral
     - GPIO Port
     - Pin Bitmask
     - Pins
     - AF
     - Function
   * - TIM1
     - 0x40010000
     - GPIOE (0x40021000)
     - 0x2A00
     - PE9, PE11, PE13
     - AF2
     - RGB status LED (CH1=red, CH2=green, CH3=blue)
   * - TIM4
     - 0x40000800
     - GPIOD (0x40020C00)
     - 0x3000
     - PD12, PD13
     - AF2
     - Fan PWM (CH1=CPU fan, CH2=chassis fan)

RGB LED status codes (from set_mcu_led_status)
------------------------------------------------

.. list-table::
   :header-rows: 1

   * - Status
     - Color
     - Meaning
   * - 1
     - Red only
     - System off / power failure
   * - 2
     - Green only
     - System running normally
   * - 3
     - Blue / cycling
     - Boot in progress
   * - 4
     - White (all)
     - All LEDs on

I2C peripheral mapping
------------------------

.. list-table::
   :header-rows: 1

   * - Handle (RAM)
     - Peripheral
     - Address
     - Bus
     - Confirmed usage
   * - 0x20000b34
     - 0x40005400 (I2C1)
     - \-
     - I2C1
     - EEPROM access (0xA0 = 7-bit 0x50) via ``es_check_carrier_board_info``
   * - 0x20000b88
     - 0x40005C00 (I2C3)
     - \-
     - I2C3
     - INA226 (0x88 = 7-bit 0x44) via ``ina226_init``

INA226 configuration (from ina226_init)
----------------------------------------

- Config register (0x00): **0x4527** (stored as LE 0x2745, byte-swapped on I2C) = 16 averages, 1.1ms bus+shunt conversion, continuous shunt+bus mode
- Calibration register (0x05): **0x0800** (stored as LE 0x0008, byte-swapped on I2C) = 2048, giving Current_LSB = 2.5 mA with 1 mOhm shunt
- Shunt resistor: **1 mOhm**, max measurable current ~81.9 A
- I2C bus: **I2C3** (PA8 SCL, PC9 SDA)

Original firmware CLI commands (from strings)
----------------------------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Description
   * - ``sompower-g``
     - Get SOM power status (ON/OFF)
   * - ``sompower-s``
     - Set SOM power (on/off)
   * - ``somconsole-g``
     - Get SOM console config (UART/Telnet)
   * - ``somconsole-s``
     - Set SOM console (0=UART, 1=Telnet)
   * - ``bootsel-g``
     - Get software boot select configuration
   * - ``bootsel-s``
     - Set software boot select
   * - ``powerlost-g``
     - Get power-lost resume attribute
   * - ``powerlost-s``
     - Set power-lost resume (enable/disable auto restart)
   * - ``eepromwp-s``
     - Set EEPROM write protect (0=disabled, 1=enabled)
   * - ``sominfo``
     - Display SOM board information
   * - ``somwork``
     - Get SOM kernel work status
   * - ``version``
     - Get BMC firmware version
