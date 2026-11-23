.. SPDX-FileCopyrightText: © 2026 Tenstorrent USA, Inc.
..
.. SPDX-License-Identifier: Apache-2.0

Atlantis SLT BMC Hardware Reference
***********************************

This document maps the STM32F746ZGT6 pins to board signals on the
Tenstorrent Atlantis SLT board and records what WallaBMC does with them.
Board signal names are used throughout.

MCU overview
============

- **MCU**: STM32F746ZGT6 (U23, LQFP144), Cortex-M7 at 216 MHz, 1 MB flash,
  256 KB SRAM + 64 KB DTCM
- **Supply**: P3V3 from U50 (TPS548B28, 12 V to 3.3 V). U50 is enabled by a
  fixed divider, so the MCU is powered whenever 12 V is present and cannot
  power itself off. ATX standby power for the BMC was deferred to the EVB.
- **Clocks**: 8 MHz crystal Y12 (HSE), 32.768 kHz crystal Y13 (LSE, not yet
  enabled: the RTC runs from LSI until the crystal is verified)
- **RTC backup**: VBAT is P_VBATL, the D18 diode-OR of the CR1220 coin cell
  in J62 and P3V3, shared with the SoC-side S35390A RTC. While
  12 V is present VBAT sits at P3V3 minus a diode drop and the cell is
  not loaded; the cell carries the backup domain only with the board
  unpowered, and VBAT never floats. Without a cell the MCU RTC loses time
  on every power cycle. The ADC VBAT channel reads P_VBATL, not the cell.
- **BOOT0**: strapped low by R591 (0R), so the MCU always boots from flash
  (MCUboot). The J69 jumper only adds R590 4.7K to P3V3 and does nothing
  while R591 is fitted; the ST system bootloader needs R591 removed and
  the jumper fitted.
- **Reset**: button B3 on NRST
- **Board ID**: PD10/PD11 strapped low on SLT ("STM32ID: 00")

Debug and console
=================

The FT4232H U42 on USB-C port J39 provides:

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

Headers: J61 SWD (1 SWCLK, 2 SWDIO, 3 3V3, 4 GND), J70 UART (1 MCU TX,
2 MCU RX, 3 3V3, 4 GND).

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
     - MCU console, FT4232 channel C and J70
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
     - Active low, internal pull-up. Hold 10 s to clear config and reboot
   * - PG5
     - SYS_RSTN_3V3
     - ``sys_rstn`` / ``reset-gpio-1``
     - SoC reset request, active high (see SoC reset)
   * - PE9
     - POWER_5V_EN
     - ``power_5v_en``, rail ``p5v`` (step 1)
     - P5V enable (see Power)
   * - PE13
     - VCORE_0V8_EN
     - ``vcore_0v8_en``, rail ``vcore_0v8`` (step 2)
     - TPS546E25 x2 CNTL via J17 (0.8 V, 80 A)
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
     - TPS628681 PCIE_0V8
   * - PE11
     - PCIE_VPH_EN
     - ``pcie_vph_en``, rail ``pcie_vph`` (step 8)
     - TPS628682 PCIE_1V8
   * - PG6
     - PWR_GOOD_5V
     - ``pwr_good_5v``, rail ``p5v`` (step 1) power good
     - P5V in regulation (high)
   * - PC7 / PC6
     - PG_PH1 / PG_PH2
     - ``pg_ph1`` / ``pg_ph2``, rail ``vcore_0v8`` (step 2) power good /
       optional power good
     - VCORE phases in regulation, high. PG_PH1 gates the step, PG_PH2
       only warns. PG_PH2 sits at the ~4.4 V LDO; PG_PH1 is clamped by
       LED DS2 to about 2 V (see Known issues)
   * - PF10
     - MPM54304_GPIO
     - ``mpm54304_gpio``, rail ``mpm54304`` (step 3) power good
     - MPM54304 power good (open drain with the -0001 MTP, no board
       pull-up, internal pull-up enabled)
   * - PG7
     - PWR_GOOD_LPDDR5
     - ``pwr_good_lpddr5``, rail ``ddr_vdd`` (step 4) power good
     - VDD1 and VDD2 good (high, ~3.4 V level)
   * - PA10
     - PWR_GOOD_0V9
     - ``pwr_good_0v9``, rail ``ddr_vdd2l`` (step 5) power good
     - DDR_VDD2L good (high)
   * - PG1 / PG2
     - PWR1_GD_3V3 / PWR2_GD_3V3
     - ``pwr1_gd_out`` / ``pwr2_gd_out`` + gpio-hogs, rails ``pwr1_gd`` /
       ``pwr2_gd`` (steps 9 / 10)
     - Power-good handshakes to the SoC through 15K/18K dividers
       (push-pull only), raised at step 9 / 10 of a power on
   * - PF2
     - OTP_PWRRDY_3V3
     - ``otp_pwrrdy_out`` + gpio-hog, rail ``otp_pwrrdy`` (step 11)
     - OTP power-ready handshake to the SoC through a 15K/18K divider,
       raised at step 11 of a power on
   * - PG0
     - TJ_MAX_3V3
     - ``tj_max``, ``fault`` of ``power_sequencer``
     - SoC junction over-temperature output through U61; a rising edge
       while the host runs powers it off (see Power)
   * - PF12/PF11/PF13/PF14
     - STM32_JTAG0_TCK/TMS/TDI/TDO
     - ``jtag0_*`` / ``jtagtck`` etc.
     - RCPU JTAG bit-bang (TCP port 7777), needs mux select 01
   * - PA1,PA2,PA7,PB0,PB1,PB11-PB13,PC1-PC5,PE2
     - ETH_MII_* / ETH_MDC / ETH_MII_MDIO
     - ``mac``, ``mdio``
     - MII to DP83826E PHY
   * - PE3
     - ETH_RSTN
     - ``eth_phy`` reset-gpios
     - PHY reset, active low
   * - PF1 / PF0
     - I2C6_SCL / SDA
     - ``i2c2``
     - TMP117 and M24M01 EEPROM, 4.7K pull-ups (R873/R874)
   * - PB8 / PB7
     - I2C2_SCL_3V3 / SDA_3V3
     - ``i2c1``
     - Level-shifted link to SoC I2C2 (enabled, unused)
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
   * - PA3, PA4, PD1, PD0
     - GPIO2/8/11/12_3V3
     - ``soc_gpio*``
     - Bidir
     - SoC GPIO_DATA[2/8/11/12] through auto-direction shifters. GPIO11/12
       also on header J53 pins 3/1
   * - PA5 / PA6
     - SYS_JTAG0_SEL0 / SEL1
     - ``sys_jtag0_sel*``
     - Out
     - RCPU JTAG mux select
   * - PA11 / PA12
     - SYS_JTAG1_SEL0 / SEL1
     - ``sys_jtag1_sel*``
     - Out
     - Ascalon JTAG mux select
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
     - Sensor_ALERT
     - ``tmp117`` alert-gpios
     - In
     - TMP117 alert, active low, 4.7K pull-up
   * - PD12 / PD13
     - I2C5_SCL / SDA
     - ``i2c4`` (disabled)
     -
     - PMIC bus when J67/J68 are jumpered 1-2
   * - PF3 / PF4
     - FT4232_GPIO2 / GPIO3
     - ``ft4232_gpio*``
     -
     - Spare lines from the SoC-side FT4232 U73 channel A
   * - PC0
     - FT232_GPIO
     - ``ft232_gpio``
     - In
     - MCU-side FT4232 U42 ADBUS5 through R815, purpose undefined; never
       drive it
   * - PD10 / PD11
     - STM32_ID0 / ID1
     - ``stm32_id*``
     - In
     - Board ID straps, both low on SLT
   * - PC14 / PC15
     - Y13
     - ``clk_lse`` (disabled)
     -
     - 32.768 kHz crystal, enable once it is seen to start

Not connected or spare
----------------------

PA0, PB5, PB9, PB10, PB15, PD4, PD7, PF5 and PG14 only reach test points
M24-M32. The eFuse enable from U42 ADBUS4 is behind the
unpopulated 0R link R814. PC13 is unconnected. PA15 and PB4 have 4.7K
pull-ups only. PF15 (STM32_JTAG0_TRST) and the JTAG1 pins PG9 to PG13 are
wired but not in the DTS (see JTAG).

Ethernet
========

- **PHY**: TI DP83826E (U25) in BASIC pin mode, 25 MHz crystal, RJ45 J20
- **Interface**: MII (straps "100M Auto negotiation / MII MAC mode / Full
  Duplex"). The fitted strap resistors are R680/R681 1.5K (MII) and R233
  1.5K (full duplex); auto-negotiation and 100M are the PHY's internal
  defaults, the LED0/LED1 pull-ups R235/R236 are not fitted. RX_ER, CRS
  and COL are not wired to the MCU; fine for full duplex.
- **MDIO address**: no address straps are fitted, so the address is the
  datasheet default. SNLS647I rev I gives PHY_ADD0 = 1 (RX_D3 has an
  internal pull-up) so the DTS uses address 1. Older datasheet revisions
  and app note SNLA338A say 0. **Verify at bring-up** by reading
  PHYIDR1/PHYIDR2 (0x2000/0xA131) at addresses 0 and 1; a wrong address
  logs "No PHY found" and leaves the BMC without network.
- **Reset**: ETH_RSTN (PE3), active low, 4.7K pull-up. The DTS asserts it
  for 100 us and waits 5 ms before the first MDIO access, per the datasheet.
- **Interrupt**: ETH_MII_INTR (PE4), open drain, active low, 4.7K pull-up.
  Not in the DTS: the generic ``ethernet-phy`` driver polls.
- **ModeSelect**: pin 1 is pulled down through 1K (R679), which holds it
  below the 0.6 V bootstrap VIL for any internal pull-up above 4.5K, so
  BASIC mode is fixed. The RJ45 LEDs are PHY-driven, active low, with no
  firmware involvement.

Host console UART mux
=====================

SoC UART0 and UART1 are 1.8 V signals level shifted by U89 and then routed
by two SN74CB3Q3253 muxes (U82 for UART0, U83 for UART1) that share the
select lines:

.. list-table::
   :header-rows: 1

   * - SEL1 (PD14) : SEL0 (PD15)
     - UART0 / UART1 connected to
   * - 0 0
     - SoC-side FT4232 U73 channels C / D (USB-C J71)
   * - 0 1
     - MCU USART2 / USART3
   * - 1 0
     - Headers J77 / J78 (1 TX in, 2 RX out, 3 3V3, 4 GND)
   * - 1 1
     - nothing (SoC RX lines float)

Both lines have 4.7K pull-ups and a DIP switch (SW4) that grounds them
through 499R when ON. WallaBMC hogs SEL1 low and drives SEL0 high as
``host-console-uart-muxsel`` at init, so while the BMC runs the SoC
console (UART0) is on the console logger, console bridge (TCP port 22) and
web terminal, and the USB channels see nothing. The MCU pushes about 7 mA
into a closed SW4 section; set SW4 OFF when the BMC owns the selects.

SoC reset
=========

SYS_RSTN_3V3 (PG5) and FT4232__SYS_RSTN_3V3 (U73 ADBUS4) are diode-ORed
into Q1, which pulls the SoC RESETN low: **either input high asserts
reset**. R589 pulls PG5 high, so the SoC is held in reset whenever the MCU
is unpowered, in reset, or has not configured the pin. WallaBMC maps the
pin as ``reset-gpio-1`` with ``GPIO_ACTIVE_HIGH``. ``reset_init()`` drives
it high at BMC boot, so the SoC stays in reset until ``power on`` releases
it after the last sequence step. ``power off`` asserts it before the rails
drop, and ``reset`` pulses it for one second while the host is on. SYS_RSTN
also asserts PERST# on the x8 and x4 slots through the diodes D9 and D11
and on the x16 slot through the AND gate U100 when jumper
J81 is fitted (fit it when the SoC is a root port). The x16 PERST# shifter
U84 takes its direction from the PCIe strap: with J51 open
(strap high, root port) the SoC side drives the slot; with J51 shorted
(strap low, endpoint) the slot drives the SoC's PERSTN, J81 must be open
and the BMC cannot reset that slot. Neither strap level is
documented; root port with J51 open is inferred from the U84 direction
(strap high drives the slot) and the purpose of J81. RESETN has an internal
pull-up in the SoC.

U73 ADBUS4 has no series resistor or pull-down. It is DTR# in the
FT4232H's default UART mode and sits high unless the host actively drives
it low (a port open with DTR asserted, or an MPSSE layout with that bit
low), so a USB-C cable on J71 to an idle host can hold the SoC in reset
no matter what PG5 does. See Known issues.

Power
=====

What comes up by itself
-----------------------

ATX 12 V (J30) passes fuse F1 and the TPS259830 eFuse U54 (UVLO about
10.8 V, ILIM 18 A) to P12V ("12V Ready" LED DS3), and U50 makes P3V3 for
the MCU, PHY and the QSPI flash. Both FT4232H bridges (U42 on J39, U73 on
J71) run from their own USB VBUS, so they enumerate with or without 12 V.
The eFuse enable is reachable only from push-button B1 and the SoC-side
FT4232 U73 (BDBUS7, R816 fitted) through MOSFET Q12, so **BDBUS7 high
disables the eFuse**; the MCU-side path R814 is not populated. ATX PS_ON
is a jumper on J31. So the MCU cannot power-cycle the board or itself:
"power off" means dropping the SoC rails.

SoC rails controlled by the MCU
-------------------------------

Every enable has a pull-down, so all of these rails stay off until the
firmware drives them. The SoC requires only the rail order
P12V > P3V3 > P5V > VCORE_0V8 > DVDD_3V3 > DDR / PCIe, with LPDDR5
VDD1 > VDD2 > 0.9 V > VDDQ.

.. list-table::
   :header-rows: 1

   * - Step
     - Enable
     - Regulator
     - Rails
     - Power good
   * - 1
     - POWER_5V_EN (PE9)
     - U57 TPS548B28
     - P5V 10 A (USB, HDMI, DDR regulators)
     - PWR_GOOD_5V (PG6)
   * - 2
     - VCORE_0V8_EN (PE13)
     - U22 + U2 TPS546E25
     - VCORE_0V8 80 A
     - PG_PH1 (PC7), PG_PH2 (PC6, optional)
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
     - PWR1_GD (PG1)
     -
     - handshake to the SoC
     -
   * - 10
     - PWR2_GD (PG2)
     -
     - handshake to the SoC
     -
   * - 11
     - OTP_PWRRDY (PF2)
     -
     - handshake to the SoC
     -

Steps 3 to 8 follow the rail order above, which does not order the DDR and
PCIe groups relative to each other; DDR before PCIe is the firmware's
choice.

The SoC also constrains the handshakes: OTP_PWRRDY, PWR1_GD and
PWR2_GD must be driven low before DVDD_1V8 ramps and asserted only once
VDD/VDDIO are good (200 ns setup for PWRRDY). No inter-rail delays, no
other handshake or reset timing and no power-off order are specified.
The VCORE enable reaches the regulators through jumper J17.
As drawn, the printed default ("3-4 short") would tie CNTL to ground, so
STM32 control needs the VCORE_0V8_EN pin (2 as drawn) linked to a CNTL
pin; check the real header.

The rails are the ``wallabmc,power-rail`` nodes in
``boards/tenstorrent/common/atlantis_wallabmc.dtsi`` (shared with the EVB) and
``boards/atlantis_slt_mcu.overlay``; ``power-on-sequence`` in the overlay's
``power_sequencer`` node lists them in the table's order; power off walks
it in reverse. ``power on`` walks the sequence forwards.
SYS_RSTN is asserted first. Each step waits for its required power-good
inputs to read low (a power good that is already high before its enable
cannot be trusted), asserts the enable, waits for the inputs to read high
and then sleeps the step's settle time. PWR_GOOD_5V gates step 1, PG_PH1
step 2, MPM54304_GPIO step 3, PWR_GOOD_LPDDR5 step 4 and PWR_GOOD_0V9 step
5. PG_PH1 is the primary TPS546E25's power good and is required so that a
VCORE that never starts stops the sequence instead of letting the IO rails,
the handshakes and the reset release go ahead with no core supply; the
TPS546E25 datasheet says PG is actively pulled low while CNTL is low with
VCC present, so the low reading before the enable is a valid check. PG_PH2
is ``power-good-optional`` on step 2: it is not checked before the enable
and a timeout logs ``VCORE_0V8_EN: optional PG_PH2 not good after 1000 ms``
and continues, because the TPS546E25 datasheet does not say what the
secondary device's PG pin does in a stacked pair (see Known issues). If the
LED DS2 clamp makes PC7 read low while VCORE is fine, step 2 fails with
``VCORE_0V8_EN: PG_PH1 not good after 1000 ms`` and backs out; VCORE is up
for that second, long enough to measure PC7. The fix is to remove or buffer
DS2, or to move ``pg_ph1`` to ``power-good-optional`` in
``boards/tenstorrent/common/atlantis_wallabmc.dtsi``, which leaves VCORE
ungated. Steps 6 to 11 have no power-good input. Every wait
allows 1 s (``power-good-timeout-ms``, polled each millisecond). The settle
times are 1 ms after VDDQ and 5 ms after each PCIe rail, nothing elsewhere;
they cover the soft-start of regulators with no power-good input, are the
firmware's choice and are not yet verified on hardware. After step 11 the
sequencer releases SYS_RSTN, marks the host on and logs ``System Power State
changed to: ON``; each step also logs ``<ENABLE> on`` at debug level, where
``<ENABLE>`` is the enable's DTS label.

A step that times out logs ``<ENABLE>: <PG> still good after 1000 ms`` or
``<ENABLE>: <PG> not good after 1000 ms``, then ``Power on failed at
<ENABLE>, powering off``: every enable is driven inactive in reverse
``power-on-sequence`` order as for ``power off``, SYS_RSTN stays asserted
and the error is returned to the shell or Redfish. ``power off`` asserts
SYS_RSTN and walks ``power-on-sequence`` in reverse. Steps 11 to 2
drop with no delay; step 1 has ``off-delay-ms`` 100, so P5V stays up for 100
ms after the SoC regulator enables have dropped. The TPS51488 is powered
from P5V and needs it while it shuts down: it drops VDD2 as soon as
DDR_VDD_EN goes low and holds VDD1 for 30 to 60 ms before discharging it
(TPS51488 datasheet). Power off therefore takes about 100 ms. Among the SoC
rails the order is by enable only and is not timed; the real collapse order
is set by the regulators and the load. ``power on`` with the host already on
and ``power off`` with it already off do nothing and log ``System Power
State already: ON`` (or ``OFF``) instead of ``changed to``. Power cycle is
off, 2 s, on. The shell, Redfish and the boot-time auto power-on (``config
host auto_poweron``) take one host power lock without waiting: a second
operation during another is refused with ``-EBUSY`` (503 from Redfish)
rather than queued. ``power force-off`` and Redfish ``ForceOff`` are the
same power off here, and ``power state`` reports the state the BMC drives.

The host power state is not persisted across BMC reboots: a BMC reset powers
the host off, and the sequence runs at boot only when auto power-on is
enabled. MCUboot's devicetree carries the rail nodes too (the sysbuild build
shares the application's bindings) but MCUboot has no code for them, so only
the gpio-hogs act and every enable stays low until the application runs.

**Thermal trip**: TJ_MAX_3V3 (PG0, ``tj_max``) is the SoC's junction
over-temperature output, listed as a ``fault`` of the ``power_sequencer``
node. It comes through U61 (see Level shifters): it floats until DVDD_1V8 is
up and reads high through the shifter's pull-up whenever the SoC does not
drive it, presumably in reset too (undocumented, see Known issues), so the
sequencer does not act on its level. After the last step, once SYS_RSTN is
released, it arms a rising-edge interrupt on PG0 and disarms it again before
``power off`` or ``reset`` asserts SYS_RSTN. Every rising edge logs
``TJ_MAX_3V3 asserted, powering off`` and runs the normal power off from a
thread of its own (the system work queue can be busy with NTP for seconds):
the handshakes and the SoC enables drop within about a millisecond, P5V 100
ms later, and ``System Power State changed to: OFF`` follows. ``power on``
brings the host back. The edge is not qualified by reading the line again,
because a SoC that resets itself on a trip may stop driving the pin at once.
PG0 already high when it is armed logs ``TJ_MAX_3V3 asserted at reset
release`` and does not stop the power on, since that is what the pull-up
gives before the SoC drives the pin; an alarm already active then is not
seen until it drops and returns. The polarity is an assumption (see Known
issues). A pin-0 EXTI line is needed, so no other PA0 to PK0 interrupt can
be added while ``tj_max`` is a fault.

PMIC I2C
--------

The regulators share one I2C/PMBus bus (board net I2C4): TPS546E25 at
0x0D (PMBus), TPS628681 at 0x49, TPS628682 at 0x41, MPM54304 at 0x68.
Headers J67 (SCL) and J68 (SDA) select the master: 1-2 puts the MCU's I2C5
(STM32 ``i2c4``, PD12/PD13) on the bus, 2-3 the SoC's I2C4. The pull-ups
R179/R180 to the TPS546E25 4.4 V LDO are not fitted (the MPM54304 pins are
rated to 4 V), so in the 1-2 position the only pull-ups are the MCU-side
R204/R205 (4.7K to P3V3) and in the 2-3 position only the U67 shifter's
internal ones while DVDD_1V8 is up.

JTAG
====

Two SoC TAPs, each behind SN74CB3Q3253 muxes with the same select
encoding as the UART mux (SEL1:SEL0 00 FT4232 U73, 01 MCU, 10 header,
11 none) and a DIP switch that forces 00 when ON:

.. list-table::
   :header-rows: 1

   * - Chain
     - MCU pins (TCK/TMS/TDI/TDO/TRST)
     - Select
     - Switch
     - Header
   * - JTAG0 (RCPU)
     - PF12/PF11/PF13/PF14/PF15
     - PA5 (SEL0), PA6 (SEL1)
     - SW2
     - J40
   * - JTAG1 (Ascalon)
     - PG10/PG9/PG11/PG12/PG13
     - PA11 (SEL0), PA12 (SEL1)
     - SW3
     - J73

WallaBMC's ``jtagtck``/``jtagtms``/``jtagtdi``/``jtagtdo`` aliases point at
JTAG0. The select lines are not driven by the firmware yet. With both SW2
sections OFF the selects idle at 11 and no source reaches the SoC, so until
``jtag.c`` drives PA6 low and PA5 high on client connect, give JTAG0 to the
MCU by hand: SW2 section on SEL1 ON, section on SEL0 OFF. Hogging the
selects was avoided because it would cut the USB JTAG path that bring-up
relies on.

Boot mode
=========

BOOT_MODE0/1/2 and TEST_MODE come from DIP switch J42 alone:
R395-R398 (4.7K) pull them up to DVDD_1V8 and sections 1 to 4 ground
BOOT_MODE0, BOOT_MODE1, BOOT_MODE2 and TEST_MODE when ON; sections 5 to 8
are not connected. No STM32 pin reaches these nets, so the BMC cannot select
the SoC boot mode on the SLT. The EVB turns PD7/PF5/PG14, test points here,
into SOC_BTM0/1/2, which can only pull a bit low. The BOOT_MODE[2:0]
encoding is not documented here.

Boot flash and SD muxes
=======================

- **QSPI**: the 16 MiB MX25L12833F (U19) sits behind TS3A27518E U21.
  EN (PE5) is pulled up, so the flash is isolated from everyone until the
  MCU drives it low; IN1/IN2 (PE6/PE7) both low route it to the SoC, both
  high to the MCU QUADSPI. Never drive IN1 and IN2 differently (it splits
  the bus). WallaBMC hogs all three low so the SoC can boot from NOR as
  soon as it is released from reset, in MCUboot as well as the app. The
  ``quadspi`` node is defined but disabled; programming the flash from the
  BMC is a follow-up.
- **SD**: slot S1 is wired straight to SoC SDIO0 (the MCU sees only its
  card detect on PA8). Slot S2 is behind U20 with the same control scheme
  (PE8 EN, PG3/PG4 IN); WallaBMC hogs them low so S2 belongs to SoC SDIO1.
  U20, both slots and their pull-ups are on DVDD_3V3 and are dead until
  MPM54304 is enabled, so the card-detect inputs read low until then.

I2C buses
=========

.. list-table::
   :header-rows: 1

   * - Board name
     - STM32 peripheral
     - Pins
     - Devices
   * - I2C6
     - I2C2 (``i2c2``)
     - PF1 SCL, PF0 SDA
     - TMP117 (0x49, alert PD3), M24M01 EEPROM (128 KiB at 0x56; the
       driver uses 0x57 for the upper half; write control tied low). 4.7K
       pull-ups R873/R874, fast mode.
   * - I2C2
     - I2C1 (``i2c1``)
     - PB8 SCL, PB7 SDA
     - SoC I2C2 through auto-direction shifter U67, 4.7K pull-ups both
       sides. Protocol and master undefined.
   * - I2C5
     - I2C4 (``i2c4``, disabled)
     - PD12 SCL, PD13 SDA
     - Headers J67/J68 (PMIC bus when jumpered 1-2)

Level shifters
==============

All TXS0108E/TXS0102 shifters between the SoC and the MCU have their OE
pulled to DVDD_1V8. Until MPM54304 is enabled, TJ_MAX_3V3 and
GPIO2/8/11/12_3V3 are undriven on the MCU side; firmware must ignore them
until DVDD_1V8 is up, and must not add pull-downs (they would fight the
shifters' internal pull-ups once enabled). Those pull-ups also make
TJ_MAX_3V3 read high whenever the SoC does not drive it, so the
sequencer acts only on its edges (see Power).

PWR1_GD, PWR2_GD and OTP_PWRRDY do not go through a shifter: 15K/18K
dividers (R864-R867, R870/R871) replace the
unfitted 0R links into U61/U85, so the SoC sees 1.8 V when the
MCU drives 3.3 V and 0 V when the pin is low or floating (MCU unpowered or
in reset). The pins must be push-pull: open drain would never reach a
high. The shifters' B ports stay attached to the 3.3 V nets, so once
DVDD_1V8 is up their internal pull-ups lift a floating pin to about 1.5 V
at the SoC; the gpio-hogs keep the three pins driven low from boot, in
MCUboot and the app, and ``power_init()`` reconfigures them as push-pull
outputs, still low, until steps 9 to 11 of the sequence raise them.

Jumpers and switches
====================

.. list-table::
   :header-rows: 1

   * - Ref
     - Function
     - Notes
   * - J69
     - STM32 BOOT0 pull-up
     - No effect while R591 (0R) straps BOOT0 low; with R591 removed, a
       jumper selects the ST system bootloader
   * - J51
     - PCIe strap
     - Open = PCIE_STRAP high (10K to DVDD_1V8), shorted = low (1K to
       GND); sets the x16 PERST# direction in U84 (see SoC reset)
   * - J80
     - PCIe strap to SoC GPIO_DATA[3]
     - Fitted applies PCIE_STRAP to the SoC pin (AC4), open leaves the
       GPIO to the SoC
   * - J81
     - x16 slot PERST# source
     - Fitted (root port, J51 open) lets SYS_RSTN, and with J47 2-3 the
       SoC's RST_PCIE_X16, drive the x16 PERST# through U100; open in
       endpoint mode (J51 shorted)
   * - J47
     - Second input of U100
     - Open = x16 PERST# follows SYS_RSTN alone (R439 pull-up), 2-3 adds
       the SoC's RST_PCIE_X16, 1-2 holds it asserted
   * - J42
     - SoC boot mode / TEST_MODE
     - Sections 1-4 ground BOOT_MODE0/1/2 and TEST_MODE when ON (4.7K
       pull-ups to DVDD_1V8), 5-8 not connected; no MCU path (see Boot mode)
   * - J17
     - VCORE enable source
     - VCORE_0V8_EN to CNTL (2-5 as drawn) for STM32 control; the printed
       default "3-4" would hold VCORE off
   * - J67 / J68
     - PMIC I2C master
     - 1-2 STM32 I2C5, 2-3 SoC I2C4
   * - J57 / J58
     - QSPI mux IN1 / IN2 straps
     - 4.7K to 3V3 or GND; MCU outputs override
   * - J79 / J56
     - SD mux IN1 / IN2 straps
     - same
   * - J31
     - ATX PS_ON
     - jumper turns the ATX supply on
   * - SW2 / SW3 / SW4
     - JTAG0 / JTAG1 / UART mux selects
     - One section per SEL line; ON grounds it (0), OFF pulls it up (1).
       Both ON = FT4232, both OFF = no source; the MCU overrides either
   * - B1
     - 12 V eFuse disable
     - board power cycle
   * - B2 / B3
     - MCU user button / MCU reset
     -
   * - J62
     - CR1220 for the MCU and SoC RTCs
     -

Flash layout
============

.. list-table::
   :header-rows: 1

   * - Sectors
     - Offset
     - Size
     - Partition
   * - 0-1 (2 x 32 KB)
     - 0x00000
     - 64 KB
     - ``mcuboot``
   * - 2-3 (2 x 32 KB)
     - 0x10000
     - 64 KB
     - ``storage`` (NVS, two sectors)
   * - 4 (128 KB)
     - 0x20000
     - 128 KB
     - unallocated
   * - 5-7 (3 x 256 KB)
     - 0x40000
     - 768 KB
     - ``image-0``

Known issues
============

None of these is fixed by firmware; the TJ_MAX entries need a
measurement before the DTS can be settled.

- **PG_PH1 level**: the TPS546E25 PG output is pulled up by
  R178 10K to the ~4.4 V LDO but also feeds LED DS2 through R185 1K, so
  when PG releases the node sits at the LED forward voltage plus a little,
  roughly 2 V (the LED part is unknown). That is between the
  STM32's design-guaranteed VIH of 1.8 V and its production-tested 2.3 V,
  so PC7 may read low or be noisy while VCORE is fine.
  PG_PH2 has no LED and is unaffected. The sequencer lists ``pg_ph1`` as
  ``power-good`` on step 2 so that a VCORE that never starts stops the
  power on; a bad reading therefore fails step 2 with ``VCORE_0V8_EN:
  PG_PH1 not good after 1000 ms`` and backs the rails out. VCORE is up
  for that second, long enough to measure PC7. The fix is to buffer the
  LED or remove DS2. Until then ``pg_ph1`` can move to
  ``power-good-optional`` in
  ``boards/tenstorrent/common/atlantis_wallabmc.dtsi``, which
  leaves VCORE ungated.
- **PG_PH2 in the stacked pair**: the TPS546E25 datasheet does not say
  what the secondary device's PG pin does when two are stacked (over PMBus
  a secondary always reports power good), so ``pg_ph2`` is
  ``power-good-optional`` until it is measured. If PC6 follows VCORE, move
  it to ``power-good`` so both phases gate the step.
- **SoC held in reset by the USB bridge**: U73 ADBUS4 drives
  the reset OR directly and is DTR#, high unless the host drives it low.
  A J71 cable to a host that has not asserted DTR, or any port close,
  asserts SoC reset. The fix is a pull-down and series resistor, an
  inverted sense, or an EEPROM/host configuration that keeps ADBUS4 low.
- **BOOT0 recovery**: R591 0R
  straps BOOT0 low whether or not J69 is fitted, so the ST system
  bootloader needs R591 removed; SWD (FT4232 channel A or J61) remains
  the recovery path. Ask hardware whether J69 should go or R591 should
  become a pull-down about ten times R590, as on the EVB.
- **TJ_MAX polarity**: nothing documents the SoC's TJ_MAX pin.
  The sequencer assumes it is driven high while the junction is at its
  maximum temperature and low otherwise. ``TJ_MAX_3V3 asserted at reset
  release`` on every ``power on`` means the SoC had not started driving the
  pin when SYS_RSTN was released (U61's pull-up) or the alarm is
  active low; in the second case the host would be powered off when the
  alarm clears, not when it starts, and ``tj_max`` in
  ``atlantis_mcu.dtsi`` needs ``GPIO_ACTIVE_LOW``. Measure PG0 with the
  SoC running.
- **TJ_MAX while the SoC is reset behind the BMC's back**:
  when U73 ADBUS4 resets the SoC with the host on (see above), a SoC
  that stops driving TJ_MAX lets U61's pull-up raise PG0, which the
  sequencer would take as a trip. Measure PG0 with the SoC held in reset
  by DTR#; if it rises, a J71 event ends in a power off until the reset
  path is fixed.

Bring-up checklist
==================

#. Read the PHY ID at MDIO addresses 0 and 1 and fix ``reg`` on
   ``eth_phy`` if needed.
#. Confirm SW2/SW3/SW4 positions, J42 sections 1-4, the J17 jumper, J67/J68,
   and the PCIe jumpers J51/J80/J81 (no default is printed; root-port mode,
   which the SYS_RSTN step below expects, is J51 open, J80 fitted, J81
   fitted).
#. Fit a CR1220 in J62 if the RTC should keep time.
#. Enable ``clk_lse`` and switch ``rtc`` to ``STM32_SRC_LSE RTC_SEL(1)``
   on a test build. If Y13 does not start, the STM32 clock driver spins
   waiting for it and neither MCUboot nor the app prints anything; raise
   ``driving-capability`` on ``clk_lse`` in that case. Make it the default
   once it boots.
#. Check the FT4232 channel A SWD path programs the MCU; J61 is the
   fallback.
#. Run ``power on`` and watch the console: ``System Power State changed
   to: ON`` means all eleven steps passed; ``Power on failed at <ENABLE>,
   powering off`` names the first rail whose power good did not rise
   (``<PG> not good``) or was already high (``<PG> still good``). The
   per-step ``<ENABLE> on`` lines are at debug level.
#. With the host running, read PG0 (TJ_MAX_3V3): it must be low and
   ``power on`` must not have logged ``TJ_MAX_3V3 asserted at reset
   release``; if it did, see Known issues. If the SoC can be made to
   raise TJ_MAX, check the BMC logs ``TJ_MAX_3V3 asserted, powering off``
   followed by ``System Power State changed to: OFF``. Also read PG0 with
   the SoC held in reset through J71 (Known issues). If the host powers
   off on its own with ``TJ_MAX_3V3 asserted`` in the log and the SoC
   was not hot, drop ``fault`` from ``power_sequencer`` in
   ``boards/atlantis_slt_mcu.overlay`` until the cause is found.
#. Measure PG_PH1 at PC7 and PG_PH2 at PC6 with VCORE on. PG_PH1 is
   required: if PC7 stays low, step 2 fails with ``VCORE_0V8_EN: PG_PH1
   not good after 1000 ms`` after holding VCORE up for 1 s, long enough
   to measure. Fix DS2, or move ``pg_ph1`` to ``power-good-optional`` on
   the ``vcore_0v8`` step in
   ``boards/tenstorrent/common/atlantis_wallabmc.dtsi``, which
   leaves VCORE ungated. PG_PH2 only warns; move it to ``power-good`` if
   PC6 follows VCORE.
#. Check MPM54304_GPIO (PF10) reads low before MPM54304_EN and high
   after it. The MPM54304 datasheet says the GPIO pin in PG mode is
   pulled low while EN is low or in UVLO, so step 3 failing with
   ``MPM54304_EN: MPM54304_GPIO still good after 1000 ms`` means U53 has
   no input or the fitted MTP does not configure the GPIO pin as PG;
   ``not good`` means the rails never came up.
#. Scope SYS_RSTN_3V3 (PG5) across ``power on`` and ``power off``: it must
   stay high until OTP_PWRRDY has risen and go high again before the
   enables drop.
   With J81 fitted and J51 open the x16 slot PERST# must follow it.
#. Measure SYS_RSTN with a USB-C cable on J71 to an idle host (Known
   issues).

Feature work
============

#. Monitor the power-good inputs while the host is on and act on a loss
   (power off, or at least report it); today they are read only during
   ``power on``.
#. Timed power-off order if the SoC turns out to need one (none is
   specified); today the enables drop in reverse order with only the
   100 ms P5V off delay.
#. Persist the host power state across BMC reboots so the host can be
   brought back after a BMC reset, which powers it off.
#. Drive the JTAG mux selects from the JTAG daemon.
#. TMP117 and EEPROM support on ``i2c2`` (board temperature, identity).
#. PMBus telemetry over ``i2c4`` when J67/J68 give the bus to the MCU.
#. QSPI flash programming and SD slot S2 access through the muxes.
#. RTC on the LSE crystal once Y13 is verified.
#. Report the fault inputs and the last trip in the shell and Redfish.
