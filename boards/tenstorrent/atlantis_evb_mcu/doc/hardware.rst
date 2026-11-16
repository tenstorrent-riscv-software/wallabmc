.. SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
..
.. SPDX-License-Identifier: Apache-2.0

Atlantis EVB BMC Hardware Reference
***********************************

This document maps the STM32F746ZGT6 pins to board signals on the
Tenstorrent Atlantis EVB and records what WallaBMC does with them. The MCU circuit is the SLT one with a different power
scheme, so the devicetree is shared
(``boards/tenstorrent/common/atlantis_mcu.dtsi``) and this document
concentrates on what the EVB does differently. Subsystems that are
identical point at the SLT reference,
``boards/tenstorrent/atlantis_slt_mcu/doc/hardware.rst``.

Differences from the SLT
========================

- **Supply**: the MCU, PHY, TMP117, EEPROM and the MCU pull-ups run
  from D3V3_SB, made from the ATX +5VSB standby rail. The BMC is alive
  while the host is off and owns the PSU through PS_ON#.
- **Power control**: POWER_5V_EN (PE9) and PWR_GOOD_5V (PG6) are not
  connected; ATX_PSON_MCU (PB10) and ATX_PWR_OK (PB9) replace them. There
  is no on-board 12 V eFuse, no 12 V to 5 V/3.3 V bucks and no button B1.
- **New outputs**: M2_VCC_EN (PC13) switches the M.2 slot 3.3 V;
  SOC_BTM0/1/2 (PD7/PF5/PG14) can pull the SoC BOOT_MODE bits low; on the
  SLT only DIP switch J42 sets those bits.
- **No DIP switches** on the JTAG/UART mux selects: their pull-ups leave
  every mux at "no source" until the MCU drives the selects.
- **I2C**: the link to the SoC is SoC I2C0 (through U105), not I2C2; the
  PMIC bus reaches the MCU only after a 0R rework (R930-R933) instead of
  jumpers J67/J68.
- **Handshakes**: PWR1_GD, PWR2_GD and OTP_PWRRDY are plain 15K/18K
  dividers with no level shifter attached at all.
- **BOOT0**: J69 must be fitted to boot from flash; open means the ST
  system bootloader. On the SLT the jumper has no effect because R591 is
  0R.
- **Board ID** reads 11 (SLT: 00). **Console header** is J86, not J70.
- **VCORE enable** goes straight to the regulators (no J17).

MCU overview
============

- **MCU**: STM32F746ZGT6 (U23, LQFP144), Cortex-M7 at 216 MHz, 1 MB flash,
  256 KB SRAM + 64 KB DTCM
- **Supply**: D3V3_SB. ATX +5VSB (J84 pin 9) passes eFuse U91 (TPS25200,
  always enabled, ILIM about 2.4 A) and LDO U92 (TLV75801, 3.3 V, 500 mA).
  VDDA/VREF+ are filtered by L64. The whole BMC domain shares
  the LDO's 500 mA.
- **Clocks**: 8 MHz crystal Y12 (HSE), 32.768 kHz crystal Y13 (LSE, not yet
  enabled: the RTC runs from LSI until the crystal is verified)
- **RTC backup**: VBAT is the raw CR1220 coin cell in J62, shared
  with the SoC-side S35390A RTC. Without a cell the MCU RTC loses time on
  every power cycle.
- **BOOT0**: 2-pin jumper J69. R590 10K pulls BOOT0 to D3V3_SB; the jumper
  grounds it through R591 1K (about 0.3 V). **Fit the jumper** to boot from
  flash; without it the MCU sits in the ST system bootloader.
- **Reset**: button B3 on NRST
- **Board ID**: PD10/PD11 pulled up (R911/R912), ID1:ID0 = 11

Debug and console
=================

The FT4232H U42 on USB-C port J39 is USB powered and provides:

.. list-table::
   :header-rows: 1

   * - Channel
     - Function
     - Notes
   * - A (ADBUS0-2, ADBUS5)
     - MCU SWD
     - ADBUS0 SWCLK, ADBUS1 (DO) to SWDIO via 470R, ADBUS2 (DI) to SWDIO.
       ADBUS5 is wired to PC0 (``ft232_gpio``), purpose undefined. See
       ``support/README.md``.
   * - B
     - unused
     -
   * - C (CDBUS0/1)
     - MCU console
     - UART8 (PE1 TX, PE0 RX), 115200 8N1. Zephyr console and shell.
   * - D
     - unused
     -

Headers: J61 SWD (1 SWCLK, 2 SWDIO, 3 D3V3_SB, 4 GND). J86 (20-pin):
1 MCU RX, 2 MCU TX, 3 PA0, 4 PB15, 7 PD4, 9/10/19/20 GND; pins
12/14/16/18 are SoC I2S0 outputs. Pins 5, 6 and 8 are labelled MCU_PG3,
MCU_PG4 and MCU_PD7 but are not connected (those MCU pins are the SD mux
inputs and SOC_BTM0). There is no 3V3 pin on J86.

Pin map
========

Implemented in WallaBMC
-----------------------

.. list-table::
   :header-rows: 1

   * - STM32 pin
     - Signal
     - DTS node / alias
     - Description
   * - PE1 / PE0
     - STM32_UART_TX / RX
     - ``uart8``
     - MCU console, FT4232 channel C and J86 pins 2/1
   * - PD5 / PD6
     - STM_USART2_TX / RX
     - ``usart2`` / ``host-console-uart``
     - SoC UART0 through mux U82
   * - PD8 / PD9
     - STM_USART3_TX / RX
     - ``usart3``
     - SoC UART1 through mux U83 (enabled, unused by code)
   * - PD15
     - SYS_UART_SEL0
     - ``sys_uart_sel0`` / ``host-console-uart-muxsel``
     - UART mux S0, driven high while WallaBMC runs
   * - PD14
     - SYS_UART_SEL1
     - ``sys_uart_sel1`` + gpio-hog
     - UART mux S1, held low
   * - PG15
     - LED DS4
     - ``green_led`` / ``status-led``
     - Green LED, active high, Morse "OK" blinker
   * - PG8
     - button B2
     - ``user_button`` / ``reset-button``
     - Active low, internal pull-up. Hold 10 s to clear config
       and reboot
   * - PG5
     - SYS_RSTN_3V3
     - ``sys_rstn`` / ``reset-gpio-1``
     - SoC reset request, active high (see SoC reset)
   * - PB10
     - ATX_PSON_MCU
     - ``atx_pson``, rail ``atx`` (step 1)
     - ATX PS_ON, high = PSU on (see Power)
   * - PB9
     - ATX_PWR_OK
     - ``atx_pwr_ok``, rail ``atx`` (step 1) ``power-good``
     - ATX power good, high. Reads 2.5 V (a marginal high) when the PSU
       does not drive it, so step 1 fails without a PSU
   * - PE13
     - VCORE_0V8_EN
     - ``vcore_0v8_en``, rail ``vcore_0v8`` (step 2)
     - TPS546E25 x2 CNTL through R940 (0.8 V, 80 A)
   * - PE12
     - MPM54304_EN
     - ``mpm54304_en``, rail ``mpm54304`` (step 3)
     - DVDD_3V3, DVDD_1V8, AVDD_1V8, AVDD_0V8
   * - PE15
     - DDR_VDD_EN
     - ``ddr_vdd_en``, rail ``ddr_vdd`` (step 4)
     - TPS51488 VDD1 1.8 V and VDD2 1.05 V
   * - PB14
     - DDR_VDD2L_EN
     - ``ddr_vdd2l_en``, rail ``ddr_vdd2l`` (step 5)
     - TPS746 0.9 V (from VDD1)
   * - PE14
     - DDR_VDDQ_EN
     - ``ddr_vddq_en``, rail ``ddr_vddq`` (step 6)
     - TPS51488 VDDQ 0.5 V
   * - PE10
     - PCIE_VP_EN
     - ``pcie_vp_en``, rail ``pcie_vp`` (step 7)
     - TPS628681 PCIE_0V8 (from P5V)
   * - PE11
     - PCIE_VPH_EN
     - ``pcie_vph_en``, rail ``pcie_vph`` (step 8)
     - TPS628682 PCIE_1V8 (from P5V)
   * - PC13
     - M2_VCC_EN
     - ``m2_vcc_en`` + gpio-hog, rail ``m2`` (step 9)
     - M.2 slot 3.3 V enable, hogged low from MCUboot until step 9
   * - PC7 / PC6
     - PG_PH1 / PG_PH2
     - ``pg_ph1`` / ``pg_ph2``, rail ``vcore_0v8`` (step 2) ``power-good``
       / ``power-good-optional``
     - VCORE phases in regulation, high; both clamped by LEDs to about
       2 V. PG_PH1 is required and fails step 2 when it does not read
       high; PG_PH2 only warns (see Known issues)
   * - PG7
     - PWR_GOOD_LPDDR5
     - ``pwr_good_lpddr5``, rail ``ddr_vdd`` (step 4) ``power-good``
     - VDD1 and VDD2 good, high. Pulled up to D3V3_SB, so it reads high
       whenever the regulator is unpowered
   * - PA10
     - PWR_GOOD_0V9
     - ``pwr_good_0v9``, rail ``ddr_vdd2l`` (step 5) ``power-good``
     - DDR_VDD2L good, high. Same standby pull-up caveat
   * - PF10
     - MPM54304_GPIO
     - ``mpm54304_gpio``, rail ``mpm54304`` (step 3) ``power-good``
     - MPM54304 power good (open drain, internal pull-up enabled)
   * - PG1 / PG2
     - PWR1_GD_3V3 / PWR2_GD_3V3
     - ``pwr1_gd_out`` / ``pwr2_gd_out`` + gpio-hogs, rails ``pwr1_gd`` /
       ``pwr2_gd`` (steps 10 / 11)
     - Power-good handshakes to the SoC through 15K/18K dividers
       (push-pull only), hogged low from MCUboot until their step
   * - PF2
     - OTP_PWRRDY_3V3
     - ``otp_pwrrdy_out`` + gpio-hog, rail ``otp_pwrrdy`` (step 12)
     - OTP power-ready handshake to the SoC through a 15K/18K divider,
       hogged low from MCUboot until step 12
   * - PG0
     - TJ_MAX_3V3
     - ``tj_max``, ``fault`` of ``power_sequencer``
     - SoC junction over-temperature output through U103; a rising edge
       while the host runs powers it off (see Power)
   * - PG10/PG9/PG11/PG12/PG13
     - STM32_JTAG1_TCK/TMS/TDI/TDO/TRST
     - ``jtag1_*``, ``jtag_ascalon``
     - Ascalon JTAG bit-bang, TCP port 7777, the default chain (see JTAG)
   * - PA11 / PA12
     - SYS_JTAG1_SEL0 / SEL1
     - ``sys_jtag1_sel*`` + gpio-hogs, ``select`` of ``jtag_ascalon``
     - Ascalon JTAG mux select: hogged to 00 (FT4232 U73), 01 (MCU) while
       a client is connected
   * - PF12/PF11/PF13/PF14/PF15
     - STM32_JTAG0_TCK/TMS/TDI/TDO/TRST
     - ``jtag0_*``, ``jtag_rcpu``
     - RCPU JTAG bit-bang, TCP port 7778
   * - PA5 / PA6
     - SYS_JTAG0_SEL0 / SEL1
     - ``sys_jtag0_sel*`` + gpio-hogs, ``select`` of ``jtag_rcpu``
     - RCPU JTAG mux select, same scheme
   * - PA1,PA2,PA7,PB0,PB1,PB11-PB13,PC1-PC5,PE2
     - ETH_MII_* / ETH_MDC / ETH_MII_MDIO
     - ``mac``, ``mdio``
     - MII to DP83826E PHY
   * - PE3
     - ETH_RSTN
     - ``eth_phy`` reset-gpios
     - PHY reset, active low
   * - PF1 / PF0
     - MCU_M_SCL / SDA
     - ``i2c2``
     - TMP117 and M24M01 EEPROM, 4.7K pull-ups (R875/R874)
   * - PB8 / PB7
     - MCU_S_SCL / SDA
     - ``i2c1``
     - Level-shifted link to SoC I2C0 (enabled, unused)
   * - PE5, PE6, PE7
     - QSPI_MUX_EN, IN1, IN2
     - ``qspi_mux_*`` + gpio-hogs
     - Boot flash given to the SoC (all low)
   * - PE8, PG3, PG4
     - SDIO_MUX_EN, IN1, IN2
     - ``sdio_mux_*`` + gpio-hogs
     - SD slot S2 given to the SoC (all low)

Defined in the DTS, not yet used
--------------------------------

.. list-table::
   :header-rows: 1

   * - STM32 pin
     - Signal
     - DTS node
     - Direction
     - Description
   * - PD7 / PF5 / PG14
     - SOC_BTM0 / 1 / 2
     - ``soc_btm*``
     - Out
     - High forces SoC BOOT_MODE[n] low (see Boot mode)
   * - PA3, PA4, PD1, PD0
     - GPIO2/8/11/12_3V3
     - ``soc_gpio*``
     - Bidir
     - SoC GPIO_DATA[2/8/11/12] through auto-direction shifter U61. PD0
       is also wired to header J52 pin 18 (see Known issues)
   * - PF6-PF9, PB2, PB6
     - STM_QSPI_*
     - ``quadspi`` (disabled)
     -
     - MCU access to the boot flash when the mux selects it
   * - PC8-PC12, PD2
     - STM_SDIO_*
     - ``sdmmc1`` (disabled)
     -
     - MCU access to SD slot S2 when the mux selects it
   * - PA8 / PA9
     - SDIO0_DET / SDIO1_DET
     - ``sdio0_det`` / ``sdio1_det``
     - In
     - Card detect for slots S1 / S2, assumed active low
   * - PD3
     - SNR_ALERT
     - ``tmp117`` alert-gpios
     - In
     - TMP117 alert, active low, 4.7K pull-up
   * - PD12 / PD13
     - MCU_P_SCL / SDA
     - ``i2c4`` (disabled)
     -
     - PMIC bus, connected only after the R930-R933 rework
   * - PF3 / PF4
     - FT4232_GPIO2 / GPIO3
     - ``ft4232_gpio*``
     -
     - Spare lines from the SoC-side FT4232 U73 channel A
   * - PC0
     - FT232_GPIO
     - ``ft232_gpio``
     - In
     - MCU-side FT4232 U42 ADBUS5 through R849, purpose undefined; never
       drive it
   * - PD10 / PD11
     - STM32_ID0 / ID1
     - ``stm32_id*``
     - In
     - Board ID straps, both high on the EVB
   * - PC14 / PC15
     - Y13
     - ``clk_lse`` (disabled)
     -
     - 32.768 kHz crystal, enable once it is seen to start

Not connected or spare
----------------------

PA0, PB15 and PD4 reach header J86 pins 3, 4 and 7 and are free. PB3 and
PB5 are no-connects. PE9 (POWER_5V_EN) and PG6 (PWR_GOOD_5V) carry labels
that go nowhere: P5V is an ATX rail. PA15 and PB4 have 4.7K pull-ups only.
PE4 (ETH_MII_INTR) is wired but not in the DTS.

Ethernet
========

The Ethernet circuit is the SLT one (DP83826E U25, MII, 25 MHz crystal,
RJ45 J20) with every 3.3 V net moved to D3V3_SB, so the network is up
while the host is off. The straps differ from the SLT: R680/R681
and R233 are 1.5K, and the LED0/LED1 2.49K pull-ups R235/R236 are fitted. The SLT reference's MDIO address and reset notes
apply unchanged. ModeSelect (pin 1) is still pulled down through 4.7K
(R679) rather than shorted to ground: against the PHY's internal
pull-up the pin sits in the undefined bootstrap band, so BASIC mode is
not guaranteed and must be measured; the SLT uses 1K.
In ENHANCED mode the MII and address straps give the same result, pin
21 becomes PWRDN/INT (held high by R225) and pin 31 a 25 MHz CLKOUT on
the LED1 net.

Host console UART mux
=====================

SoC UART0 and UART1 are level shifted and routed by
SN74CB3Q3253 muxes U82/U83 with the SLT encoding:

.. list-table::
   :header-rows: 1

   * - SEL1 (PD14) : SEL0 (PD15)
     - UART0 / UART1 connected to
   * - 0 0
     - SoC-side FT4232 U73 channels C / D (USB-C J71)
   * - 0 1
     - MCU USART2 / USART3
   * - 1 0
     - Test points M24/M25 (UART0) and M28/M29 (UART1)
   * - 1 1
     - nothing (SoC RX lines float)

The selects have 4.7K pull-ups to P3V3 (R726/R727); the 499R grounding
resistors are not fitted and there is no DIP switch, so the muxes idle at
11 until the MCU drives them. WallaBMC hogs SEL1 low and drives SEL0 high
as ``host-console-uart-muxsel`` at init, so while the BMC runs the SoC
console (UART0) is on the console logger, console bridge (TCP port 22) and
web terminal. The muxes are on P3V3 and only exist after PS_ON.

SoC reset
=========

Same circuit as the SLT: SYS_RSTN_3V3 (PG5) and
FT4232_SYS_RSTN_3V3 (U73 ADBUS4) are diode-ORed into Q1, which pulls the
SoC RESETN low; **either input high asserts reset**. R589 (to D3V3_SB)
pulls PG5 high, so the SoC is held in reset throughout standby until
firmware drives the pin low. WallaBMC maps it as ``reset-gpio-1`` with
``GPIO_ACTIVE_HIGH``: ``reset_init()`` drives it high at BMC boot, so the
SoC stays in reset until ``power on`` releases it after the last sequence
step. ``power off`` asserts it before the rails drop, and ``reset`` pulses
it for one second while the host is on.

SYS_RSTN also pulls the SoC-side PCIE_X16_PERSTN and PCIE_X4_PERSTN low
through D10/D11 (the EVB has no x8 slot), where jumpers
J47/J49 can add the SoC's own RST_PCIE GPIOs or a manual strap. Whether
the slot and M.2 PERST# pins follow depends on shifter U108, which is
strapped in the wrong direction (see Known issues).

U73 ADBUS4 idles high as DTR# in the FT4232H's default UART mode, so a
USB-C cable on J71 to a host that does not drive it low holds the SoC in
reset regardless of PG5 (see Known issues).

Power
=====

ATX input
---------

The 24-pin ATX connector J84 supplies P3V3, P5V and ATX_12V
(fuse F1 to P12V, "12V Ready" LED DS3) directly: there are no on-board
bucks for them and no eFuse. All three exist only while PS_ON# is low. The
standby rail +5VSB feeds U91 and U92 to make D3V3_SB for the BMC domain.

- **PS_ON**: ATX_PSON_MCU (PB10) drives Q12 through R917 1K with R918 100K
  holding the gate low; Q12 pulls PS_ON# to ground. High = PSU on. The MCU
  pin is the only thing holding the PSU on: an MCU reset, reflash or crash
  releases Q12 and the host loses power. Jumper J85 is a manual PS_ON, but
  through R834 10K it probably cannot pull PS_ON# below the ATX VIL.
- **PWR_OK**: J84 PWR_OK reaches PB9 through R935 204R with D33 ESD
  protection. R915/R916 (10K to +5VSB, 10K to GND) bias the node to about
  2.5 V when the PSU leaves it open, which the STM32 reads as a marginal
  high. The sequencer requires PB9 low before it asserts PS_ON and high
  within 2 s afterwards (ATX T1 + T3 can reach 1 s). It does not yet watch
  for it going low while the host is on (see Feature work).
- **In standby** (PS_ON off): the MCU, PHY, TMP117, EEPROM, SWD and console
  work. Everything on P3V3/P5V/P12V is dead: the UART/JTAG muxes, the QSPI
  flash and its mux, the 3.3 V sides of the level shifters other than
  U105/U107, the M.2 switch, the SoC regulators and the fan. Driving mux
  selects and QSPI lines into those unpowered parts is harmless (their
  inputs are specified for VCC = 0).

SoC rails controlled by the MCU
-------------------------------

Every SoC regulator enable has a pull-down, so the SoC rails stay off until
the firmware drives them. The MCU pins are the SLT ones; only step 1 differs,
and the M.2 slot is new. The rails are the ``wallabmc,power-rail`` nodes in
``boards/tenstorrent/common/atlantis_wallabmc.dtsi`` (shared with the SLT) and
``boards/atlantis_evb_mcu.overlay``; ``power-on-sequence`` in the overlay's
``power_sequencer`` node lists them in this order; power off walks it in
reverse.

.. list-table::
   :header-rows: 1

   * - Step
     - Enable
     - Regulator
     - Rails
     - Power good
   * - 1
     - ATX_PSON_MCU (PB10)
     - ATX PSU
     - P12V, P5V, P3V3
     - ATX_PWR_OK (PB9)
   * - 2
     - VCORE_0V8_EN (PE13)
     - U22 + U2 TPS546E25
     - VCORE_0V8 80 A
     - PG_PH1 (PC7); PG_PH2 (PC6) optional
   * - 3
     - MPM54304_EN (PE12)
     - U53 MPM54304
     - DVDD_1V8, DVDD_3V3, AVDD_1V8, AVDD_0V8
     - MPM54304_GPIO (PF10)
   * - 4
     - DDR_VDD_EN (PE15)
     - U26 TPS51488
     - VDD1 1.8 V, VDD2 1.05 V
     - PWR_GOOD_LPDDR5 (PG7)
   * - 5
     - DDR_VDD2L_EN (PB14)
     - U27 TPS746
     - DDR_VDD2L 0.9 V
     - PWR_GOOD_0V9 (PA10)
   * - 6
     - DDR_VDDQ_EN (PE14)
     - U26 TPS51488
     - VDDQ 0.5 V
     - none
   * - 7
     - PCIE_VP_EN (PE10)
     - U51 TPS628681
     - PCIE_0V8
     - none (I2C status only)
   * - 8
     - PCIE_VPH_EN (PE11)
     - U52 TPS628682
     - PCIE_1V8
     - none (I2C status only)
   * - 9
     - M2_VCC_EN (PC13)
     - U90 TPS25200
     - P3V3_M2 (M.2 slot)
     - none; the 120 ms settle covers the TPS25200 turn-on and the
       100 ms PCIe T_PVPERL before reset release
   * - 10
     - PWR1_GD (PG1)
     -
     - handshake to the SoC
     - none
   * - 11
     - PWR2_GD (PG2)
     -
     - handshake to the SoC
     - none
   * - 12
     - OTP_PWRRDY (PF2)
     -
     - handshake to the SoC
     - none

The SoC requires only the rail order given in the SLT reference: step 3
(DVDD_3V3) comes before the DDR and PCIe groups, which are not ordered
relative to each other; DDR before PCIe and the M.2 slot after them are the
firmware's choice. The LPDDR5 sub-order VDD1 > VDD2 > 0.9 V > VDDQ
and the handshake constraints from the SLT reference still apply. Notes
on the power-good inputs:

- PWR_GOOD_LPDDR5 and PWR_GOOD_0V9 are pulled up to D3V3_SB (R684, R761),
  so they read high while their regulators are unpowered. The sequencer
  requires each to read low before its enable. The TPS51488, powered
  from P5V since step 1, drives PWR_GOOD_LPDDR5 low until enabled, and
  the TPS746, powered from VDD1 after step 4, drives PWR_GOOD_0V9 low
  until enabled (TPS51488 and TPS746 datasheets), so each reads low at
  its own step's pre-enable check; one still high there means its
  regulator has no input, and the power on fails instead of passing on
  the pull-up.
- PG_PH1 and PG_PH2 are clamped by LED chains (R185/DS2, R936/DS18) to
  roughly 2 V (the LED part is unknown), between the STM32's
  design-guaranteed VIH of 1.8 V and its production-tested 2.3 V. PG_PH1
  is the primary TPS546E25's power good, actively pulled low while CNTL
  is low and VCC is present (TPS546E25 datasheet); the datasheet does
  not say what the secondary device's PG pin does in a stacked pair.
  ``boards/tenstorrent/common/atlantis_wallabmc.dtsi`` lists ``pg_ph1`` under
  ``power-good``
  so that a VCORE that never starts fails step 2 and backs out instead
  of letting the IO rails, handshakes and reset release proceed with no
  core supply.
  ``pg_ph2`` is under ``power-good-optional``: step 2 checks it only
  after the enable and a timeout logs
  ``VCORE_0V8_EN: optional PG_PH2 not good after 1000 ms`` as a warning.
  If PC7 reads low with VCORE up, see Known issues. PMBus STATUS_WORD at
  0x0D is the authoritative source.
- MPM54304_GPIO is the module's open-drain power good (0.2 ms delay) with
  no board pull-up; the DTS enables the internal one. It gates step 3.
- The MPM54304 AVDD_0V8 divider (R716 12K / R717 26K) assumes V_Ref4 has
  been programmed to 0.55 V; with the -0001 MTP's 0.6 V reference the rail
  comes up at about 0.88 V at the first enable after each PS_ON (see Known
  issues).

**Power on** (``power on``, Redfish ``On``, or ``config host
auto_poweron`` at BMC boot) runs the table. If the
host is already on it does nothing and logs ``System Power State
already: ON``. Otherwise it asserts SYS_RSTN and takes the steps in
order. Each step first waits for its required power-good inputs to read
low, failing with ``<enable>: <input> still good after N ms`` if one does
not. It then drives the enable high and waits for the same inputs to
read high, failing with ``<enable>: <input> not good after N ms``.
Optional inputs are only checked after the enable; their timeout logs
``<enable>: optional <input> not good after N ms`` as a warning and the
sequence continues. The step then sleeps its settle time. Each step logs
``<enable> on`` at debug level. After step 12 the sequencer releases
SYS_RSTN, marks the host on and logs ``System Power State changed to:
ON``. Any failure logs ``Power on failed at <enable>, powering off``,
drives every enable low in reverse ``power-on-sequence`` order with
the same 100 ms off delay before step 1 as ``power off``, leaves SYS_RSTN
asserted and returns the error to the shell or Redfish.

The power-good timeout is 1 s for every step except step 1, which allows
2 s because ATX T1 + T3 can reach 1 s. The settle times are 1 ms after
step 6, 5 ms after steps 7 and 8 and 120 ms after step 9; the other
steps have none. The SoC requires only the rail order and specifies no
inter-rail delays, so these are the firmware's choice: they cover the
soft-start of the regulators that have no power-good input and are not
yet verified on hardware.

The pre-enable check also applies to step 1: with no PSU attached PB9
sits at about 2.5 V, so ``power on`` fails after 2 s with
``ATX_PSON_MCU: ATX_PWR_OK still good after 2000 ms`` and no enable is
driven high.

**Power off** (``power off``, Redfish ``ForceOff``) does nothing and logs
``System Power State already: OFF`` if the host is already off. Otherwise it
asserts SYS_RSTN and walks ``power-on-sequence`` in reverse:
enables 12 to 2 go low with no delays between them, then after the 100 ms
``off-delay-ms`` of step 1 ATX_PSON_MCU goes low and the host is marked off.
The off delay keeps the PSU, and so P5V, up while the TPS51488 drops VDD2 at
once and then holds VDD1 for 30 to 60 ms before discharging it (TPS51488
datasheet); the collapse order of the other rails is set by the regulators
and the load. A power off therefore takes about 100 ms. Redfish
``PowerCycle`` is off, 2 s, on. The shell, Redfish and the boot-time auto
power-on take one host power lock without waiting: a second operation during
another is refused with ``-EBUSY`` (503 from Redfish) rather than queued.
``power force-off`` and Redfish ``ForceOff`` are the same power off here,
and ``power state`` reports the state the BMC drives.

Host power state is not persisted across BMC reboots. ``power_init()``
drives every enable low and configures every power-good input as an input
with its devicetree pull, so a BMC reset powers the host off (on the EVB the
PS_ON circuit does that anyway, see Known issues). MCUboot's devicetree
carries the rail nodes too (the sysbuild build shares the application's
bindings) but MCUboot has no code for them; the gpio-hogs on M2_VCC_EN and
the three handshakes still apply in MCUboot and the app, so those pins are
low from the bootloader until the sequencer reconfigures them.

**Thermal trip**: TJ_MAX_3V3 (PG0, ``tj_max``) is the SoC's junction
over-temperature output, listed as a ``fault`` of the ``power_sequencer``
node. U103 (see Level shifters) is high-Z while DVDD_1V8 is off, so the DTS
pull-down holds PG0 low until step 3; after that it repeats the SoC pin,
whose level in reset is undocumented (see Known issues), so the sequencer
does not act on the level. After step 12, once SYS_RSTN is released, it arms
a rising-edge interrupt on PG0 and disarms it again before ``power off`` or
``reset`` asserts SYS_RSTN. Every rising edge logs ``TJ_MAX_3V3 asserted,
powering off`` and runs the normal power off from a thread of its own (the
system work queue can be busy with NTP for seconds): the handshakes and the
SoC enables drop within about a millisecond, PS_ON 100 ms later, and
``System Power State changed to: OFF`` follows. ``power on`` brings the host
back. The edge is not qualified by reading the line again, because a SoC
that resets itself on a trip may stop driving the pin at once. PG0 already
high when it is armed logs ``TJ_MAX_3V3 asserted at reset release`` and does
not stop the power on; an alarm already active then is not seen until it
drops and returns. The polarity is an assumption (see Known issues). A pin-0
EXTI line is needed, so no other PA0 to PK0 interrupt can be added while
``tj_max`` is a fault.

Boot mode
=========

SOC_BTM0/1/2 (PD7/PF5/PG14) each drive a BSS138 (Q13-Q15, 1K series, 100K
gate pull-down) whose drain is BOOT_MODE0/1/2. Those are pulled
to DVDD_1V8 through R396-R398 (4.7K) and grounded through DIP switch J44
sections 6/7/8 when ON. So BOOT_MODE[n] = NOT(SOC_BTM[n]) AND NOT(J44
section ON): the MCU can only force a bit to 0, and a DIP section that is ON
wins. With the MCU pins low or floating the DIP switch decides. The
BOOT_MODE[2:0] encoding and its sample point are not known here; they
must come from the SoC documentation. Drive the pins, if at all, after
DVDD_1V8 is up and before SYS_RSTN is released. On the SLT the same bits and
TEST_MODE come from DIP switch J42 with no MCU path; the EVB moves TEST_MODE
to jumper J79 (R395 10K pull-up, fitted = low).

PMIC I2C
========

The regulators share one PMBus (TPS546E25 at 0x0D, TPS628681 at 0x49,
TPS628682 at 0x41, MPM54304 at 0x68) pulled up by R919/R920 4.7K to P3V3.
0R links select the master: R930/R932 (fitted) connect the
SoC's I2C4 through shifter U107, R931/R933 (not fitted) would connect the
MCU's MCU_P (STM32 ``i2c4``, PD12/PD13, pull-ups R204/R205 to D3V3_SB).
Fitting R931/R933 without removing R930/R932 puts two masters on the bus
through an auto-direction shifter, so the rework must swap them. The
TPS546E25 SMB_ALERT lines reach only header J9. ``i2c4`` stays disabled.

JTAG
====

Two SoC TAPs, each behind SN74CB3Q3253 muxes with the UART mux encoding
(SEL1:SEL0 00 FT4232 U73, 01 MCU, 10 header, 11 none):

.. list-table::
   :header-rows: 1

   * - Chain
     - MCU pins (TCK/TMS/TDI/TDO/TRST)
     - Select
     - Header
   * - JTAG0 (RCPU)
     - PF12/PF11/PF13/PF14/PF15
     - PA5 (SEL0), PA6 (SEL1)
     - J40
   * - JTAG1 (Ascalon)
     - PG10/PG9/PG11/PG12/PG13
     - PA11 (SEL0), PA12 (SEL1)
     - J73

WallaBMC serves both chains to OpenOCD's ``remote_bitbang`` driver, one
TCP port each (``wallabmc,jtag`` nodes in
``boards/tenstorrent/common/atlantis_wallabmc.dtsi``):

.. list-table::
   :header-rows: 1

   * - Chain
     - Node
     - TCP port
     - OpenOCD config
   * - JTAG1 (Ascalon)
     - ``jtag_ascalon``
     - 7777
     - ``boards/tenstorrent/common/openocd/ascalon.cfg``
   * - JTAG0 (RCPU)
     - ``jtag_rcpu``
     - 7778
     - ``boards/tenstorrent/common/openocd/rcpu.cfg``

The Ascalon CPU is the default chain and keeps the port the daemon has
always used. The selects have 4.7K pull-ups to P3V3 and no switch (the
499R R626/R627 and R636/R637 are not fitted), so gpio-hogs hold all
four low from MCUboot onward and the daemons park them there between
sessions (``select-idle`` in the overlay): the USB path through U73
works whenever no BMC JTAG session is open, and unlike 11 that leaves
the SoC's JTAG inputs defined, since at 11 the mux outputs float into
the level shifters U64/U77. Each daemon leaves its
TCK/TMS/TDI/TDO/TRST pins as inputs until a client connects; it then
drives TCK and TDI low and TMS high, releases TRST, drives the selects
to 01, and undoes both when the client goes away. The ``remote_bitbang``
reset commands drive TRST; SRST is not wired to the daemon, the host
reset belongs to the sequencer (see SoC reset). The bridge has no TRST
on the Ascalon chain (U80 1B1 carries only pull-up R694), so the USB
path cannot assert it; the BMC (01) and header J73 (10) can.
``boards/tenstorrent/common/openocd/README.md`` covers the host side.

Boot flash and SD muxes
=======================

The same TS3A27518E muxes and control scheme as the SLT: U21 for the
MX25L12833F boot flash (PE5 EN, PE6/PE7 IN1/IN2) and U20 for SD
slot S2 (PE8 EN, PG3/PG4). WallaBMC hogs all six lines low so
the SoC owns both. The SLT's strap headers are gone: QSPI_MUX_IN1/IN2 have
4.7K pull-ups to P3V3 (R511/R513) that the hogs override, and the SD mux
inputs have no pulls. Slot S1 is wired straight to SoC SDIO0; both slots
gained ESD diodes. U21 and the flash are on P3V3, U20 and the slots on
DVDD_3V3.

I2C buses
=========

.. list-table::
   :header-rows: 1

   * - Board name
     - STM32 peripheral
     - Pins
     - Devices
   * - MCU_M
     - I2C2 (``i2c2``)
     - PF1 SCL, PF0 SDA
     - TMP117 (0x49, alert PD3), M24M01 EEPROM (128 KiB at 0x56; the
       driver uses 0x57 for the upper half; write control tied low). 4.7K
       pull-ups to D3V3_SB, fast mode.
   * - MCU_S
     - I2C1 (``i2c1``)
     - PB8 SCL, PB7 SDA
     - SoC I2C0 through TXS0102 U105 (3.3 V side on D3V3_SB, OE from
       DVDD_1V8), 4.7K pull-ups both sides, nothing else on the SoC side.
       Protocol and master undefined.
   * - MCU_P
     - I2C4 (``i2c4``, disabled)
     - PD12 SCL, PD13 SDA
     - PMIC bus after the R930-R933 rework

Not reachable by the MCU: the PCIe slot and M.2 SMBus (SoC I2C1), the
S35390A RTC (SoC I2C3), the IO connector J83 (SoC I2C2) and HDMI DDC.

Level shifters
==============

PWR1_GD, PWR2_GD and OTP_PWRRDY are 15K/18K dividers (R881-R886)
with nothing else attached: the SoC sees 1.8 V when the MCU drives
3.3 V and 0 V when the pin is low or floating. The pins must be push-pull.

GPIO2/8/11/12_3V3 go through TXS0108E U61 with OE from DVDD_1V8: undriven
until MPM54304 is enabled, and no pull-downs (they would fight the shifter's
pull-ups). TJ_MAX_3V3 comes through SN74AVC4T245 U103, a fixed direction
push-pull buffer that is high-Z while DVDD_1V8 is off, so the DTS gives PG0
a pull-down and the sequencer's ``fault`` input reads low until then (see
Power). The MCU-facing I2C shifters U105 and U107 have their 3.3 V side on
D3V3_SB, their 1.8 V side on DVDD_1V8 and OE from DVDD_1V8.

Jumpers, switches and headers
=============================

.. list-table::
   :header-rows: 1

   * - Ref
     - Function
     - Notes
   * - J69
     - STM32 BOOT0
     - 2-pin; fitted = flash boot, open = ST system bootloader
   * - J84
     - ATX 24-pin input
     - PS_ON# driven by the MCU (PB10)
   * - J85
     - Manual ATX PS_ON
     - 10K in series; probably too weak, see Known issues
   * - J44
     - SoC DIP switch
     - sections 6/7/8 ground BOOT_MODE0/1/2 when ON; 1-5 are I2S0 straps
   * - J79
     - SoC TEST_MODE
     - fitted = low
   * - J47 / J49, J48 / J50
     - PCIe x16 / M.2 PERST# source
     - 2-3 adds the SoC RST_PCIE GPIO, 1-2 the manual strap J48/J50
   * - J9
     - PMBus debug header
     - PMB_CLK, PMB_DATA, SMB_ALERT, PMB_CONTROL
   * - R930-R933
     - PMIC bus master
     - 0R links, SoC fitted, MCU not fitted
   * - R511-R514
     - QSPI mux IN1/IN2 straps
     - 4.7K pull-ups fitted; MCU outputs override
   * - J40 / J73
     - JTAG0 / JTAG1 headers
     - mux select 10
   * - J86
     - MCU console and spare GPIO
     - see Debug and console
   * - J61
     - MCU SWD
     - 1 SWCLK, 2 SWDIO, 3 D3V3_SB, 4 GND
   * - J64
     - Fan
     - power only (1 NC, 2 +12 V, 3 GND); runs whenever PS_ON is on
   * - B2 / B3
     - MCU user button / MCU reset
     -
   * - J62
     - CR1220 for the MCU and SoC RTCs
     -

Flash layout
============

Identical to the SLT: MCUboot in sectors 0-1, NVS storage in sectors 2-3,
sector 4 unallocated, ``image-0`` (768 KB) in sectors 5-7.

Known issues
============

Some of these change the BMC design, the rest are hardware questions.

- **No PS_ON latch**: PS_ON# is held only by Q12 from PB10. Any
  MCU reset (reflash, watchdog, debugger reset, MCUboot before GPIO init)
  drops the host power unsequenced. J85 cannot substitute: R834 10K in
  series leaves PS_ON# above the ATX VIL against the PSU's pull-up (the SLT
  strap uses 100R). Hardware should add a
  latch or pull-up option and change R834; firmware should re-assert PB10 as
  early as possible after a warm reset if the host was on, which WallaBMC
  cannot do until the host power state persists across BMC reboots (see
  Feature work).
- **M.2 enable floats**: U90 TPS25200 EN has no pull resistor
  and the datasheet forbids a floating EN, so the slot power is undefined
  whenever PC13 is not driven. A 100K pull-down at U90 matches every other
  enable on the board; until then the gpio-hog keeps it low from MCUboot
  on and the sequencer drives it only at step 9.
- **Power-good pull-ups on standby**: PWR_GOOD_LPDDR5 and
  PWR_GOOD_0V9 read high with the regulators off. The sequencer requires
  them low before their enables, so a regulator with no input fails the
  power on instead of passing; see Power.
- **PERST# shifter direction**: U108 SN74AVC4T245 carries both
  PERST# lines with 2DIR strapped low (R908), which is B to A: the x16 slot
  and the M.2 never receive PERST#, and the SoC-side PERSTN inputs are
  driven by the shifter against the D10/D11 reset diodes. R908 should move
  to DVDD_1V8. Until then nothing the BMC does with SYS_RSTN reaches the
  connectors.
- **MPM54304 AVDD_0V8**: R717 changed from 36K to 26K with the
  note "Set VRef4 = 0.55 V", but nothing on the board can write that
  register before the first MPM54304_EN after 12 V is applied, so the
  0.8 V rail starts at about 0.88 V. Hardware should confirm the MTP that
  is fitted and the SoC's AVDD_0V8 maximum; if the BMC is meant to program
  it, it needs the PMBus (R930-R933) first.
- **PG_PH1 / PG_PH2 levels**: both power-good nodes feed LEDs
  through 1K, clamping the high level to roughly 2 V with no margin to the
  STM32's production-tested VIH; what PG_PH2 does on the secondary of a
  stacked TPS546E25 pair is undocumented. PG_PH1 is required: if PC7
  reads low with VCORE up, step 2 fails with
  ``VCORE_0V8_EN: PG_PH1 not good after 1000 ms`` and backs out, and
  VCORE is up during that second for a measurement. The fix is to remove
  or buffer DS2, or to move ``pg_ph1`` to ``power-good-optional`` in
  ``boards/tenstorrent/common/atlantis_wallabmc.dtsi``, which leaves VCORE
  ungated. PG_PH2 is
  optional, so step 2 only warns when PC6 does not read high.
- **SoC held in reset by the USB bridge**: U73 ADBUS4 drives
  the reset OR directly and idles high as DTR#; see SoC reset.
- **U61 A5 label**: both A4 and A5 of U61 are labelled GPIO12,
  so SoC GPIO_DATA[13] is unconnected and header J52 pin 18 (GPIO13_3V3)
  is a second path onto PD0. Do not use J52 pin 18 until fixed.
- **BOOT0 default**: J69 open selects the ST bootloader; a
  board without the jumper looks dead.
- **Standby LDO budget**: U92 TLV75801 (SOT-23-5, 500 mA)
  drops 5 V to 3.3 V for the MCU at 216 MHz, the PHY, the LEDs and the
  pull-ups; about 0.4 W at 250 mA with no thermal design shown.
- **TJ_MAX polarity**: nothing documents the SoC's TJ_MAX pin.
  The sequencer assumes it is driven high while the junction is at its
  maximum temperature and low otherwise. ``TJ_MAX_3V3 asserted at reset
  release`` on every ``power on`` means the SoC had not started driving the
  pin when SYS_RSTN was released (U103 repeating an undriven input) or
  the alarm is active low; in the second case the host would be powered
  off when the alarm clears, not when it starts, and the ``&tj_max``
  override in ``atlantis_evb_mcu.dts`` needs ``GPIO_ACTIVE_LOW`` in
  place of ``GPIO_ACTIVE_HIGH``. Measure PG0 with the SoC running.
- **TJ_MAX undriven into U103**: the SN74AVC4T245 has no
  bus hold and TI requires a driven level on every data input, so
  whenever the SoC does not drive TJ_MAX with DVDD_1V8 up (for example
  in reset, including a reset through U73 ADBUS4 with the host on, or
  if its firmware has to enable the pin first; neither is documented)
  PG0 is undefined and may toggle, and a rising edge powers the host
  off. The STM32 pull-down only helps while
  U103 is unpowered. A pull-down on TJ_MAX_1V8 at the SoC side would
  give U103 a defined input; until then, if every ``power on`` ends in
  ``TJ_MAX_3V3 asserted, powering off``, drop ``fault`` from
  ``power_sequencer`` in ``boards/atlantis_evb_mcu.overlay``. R891 also
  strapping ``*2OE`` low lets 2B1 glitch while DVDD_1V8 ramps, which the
  sequencer does not see during a power on (a DVDD_1V8 dropout with the
  host on would trip it).

Bring-up checklist
==================

#. Fit J69. Check PB10 low keeps the PSU off and high turns it on, and
   that PB9 follows PWR_OK. Check what it reads with the PSU in standby:
   step 1 requires it low before PS_ON, so a PSU that leaves PWR_OK open
   in standby fails ``power on`` the same way as no PSU.
#. Read the PHY ID at MDIO addresses 0 and 1 and fix ``reg`` on
   ``eth_phy`` if needed; measure U25 pin 1 to learn BASIC vs ENHANCED mode.
#. Confirm J44 sections 6/7/8, J47/J49 and the R930-R933 fit.
#. Fit a CR1220 in J62 if the RTC should keep time.
#. Enable ``clk_lse`` and switch ``rtc`` to ``STM32_SRC_LSE RTC_SEL(1)``
   on a test build. If Y13 does not start, the STM32 clock driver spins
   waiting for it and neither MCUboot nor the app prints anything; raise
   ``driving-capability`` on ``clk_lse`` in that case. Make it the default
   once it boots.
#. Check the FT4232 channel A SWD path programs the MCU; J61 is the
   fallback.
#. Scope SYS_RSTN_3V3 (PG5): it is high from BMC boot and stays high
   through ``power on`` until the last step. Measure SYS_RSTN with a
   USB-C cable on J71 to an idle host, and the M.2 PERST# behaviour
   (Known issues).
#. Measure PWR_GOOD_LPDDR5 in standby and after PS_ON with the enables
   low, and PWR_GOOD_0V9 with VDD1 up and DDR_VDD2L_EN low: steps 4 and 5
   need them low at their pre-enable checks.
#. Run ``power on`` with debug logging. Expect ``<enable> on`` for each
   step, then ``System Power State changed to: ON``. A step that fails
   logs ``<enable>: <input> not good after N ms`` and ``Power on failed
   at <enable>, powering off``, and the enables before it go low again.
#. With the host running, read PG0 (TJ_MAX_3V3): it must be low and
   ``power on`` must not have logged ``TJ_MAX_3V3 asserted at reset
   release``; if it did, see Known issues. If the SoC can be made to
   raise TJ_MAX, check the BMC logs ``TJ_MAX_3V3 asserted, powering off``
   followed by ``System Power State changed to: OFF``. Also scope PG0
   with the SoC held in reset through J71 and right after SYS_RSTN is
   released (Known issues).
#. Measure PG_PH1/PG_PH2 at PC7/PC6 with VCORE on. PG_PH1 is required:
   if PC7 reads low, step 2 fails after 1 s with
   ``VCORE_0V8_EN: PG_PH1 not good after 1000 ms`` and backs out, so
   measure during that second. Remove or buffer DS2, or move ``pg_ph1``
   to ``power-good-optional`` in
   ``boards/tenstorrent/common/atlantis_wallabmc.dtsi``,
   which leaves VCORE ungated. PG_PH2 is optional: a low PC6 only warns
   ``VCORE_0V8_EN: optional PG_PH2 not good after 1000 ms``; move it
   into ``power-good`` if it reads a reliable high.
#. Scope the rails during ``power on`` and check that the settle times in
   ``boards/tenstorrent/common/atlantis_wallabmc.dtsi`` and the overlay cover
   each regulator's soft-start; they are not yet verified on hardware.

Feature work
============

#. Monitor ATX_PWR_OK and the power-good inputs while the host is on and
   power off (or at least report) when one drops; the sequencer only
   checks them during ``power on``.
#. Timed power-off order, if the SoC turns out to need one (none is
   specified): ``power off`` drops enables 12 to 2 in reverse order with
   no delays between them; only step 1 has an off delay.
#. Re-assert PB10 after a warm BMC reset if the host was on, which needs
   the host power state persisted across BMC reboots.
#. Boot-mode control through ``soc_btm*`` once the encoding is known.
#. TMP117 and EEPROM support on ``i2c2`` (board temperature, identity);
   use the board ID straps to tell EVB and SLT apart if one image serves
   both.
#. PMBus telemetry over ``i2c4`` after the R930-R933 rework.
#. QSPI flash programming and SD slot S2 access through the muxes.
#. RTC on the LSE crystal once Y13 is verified.
#. Report the fault inputs and the last trip in the shell and Redfish.
