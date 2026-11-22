## Nucleo F767ZI Atlantis Variant Documentation

`nucleo_f767zi/stm32f767xx/atlantis` is the [Nucleo F767ZI](nucleo_f7.md)
limited to what the STM32F746ZG on the Atlantis
[SLT](atlantis_slt_mcu.md) and [EVB](atlantis_evb_mcu.md) boards has. The
Nucleo's STM32F767ZI has twice the flash and half as much RAM again, so a
build that fits and runs on the plain Nucleo does not show that it fits on
Atlantis. This variant uses the Atlantis flash and RAM sizes and flash
layout on the Nucleo hardware.

It is still a Nucleo: the pins, console, Ethernet PHY, LEDs and button are
those of the Nucleo, and there are no Atlantis GPIOs, sensors or EEPROM.
See [nucleo_f7.md](nucleo_f7.md) for the consoles.

### Building
```
cd wallabmc
west build -p --sysbuild -b nucleo_f767zi/stm32f767xx/atlantis
```

### Flashing

As for the Nucleo F767ZI. The flash layout differs from the plain
`nucleo_f767zi` build, so when changing between the two flash both MCUboot
and WallaBMC.

Do not expect the stored settings to carry over between board variants.

For a clean setup run `config clear_and_reboot` in the BMC shell after changing
builds. Or bulk erase the entire flash with openocd.

### What is limited to match the STM32F746ZG

| Item | nucleo_f767zi | atlantis variant | Atlantis (STM32F746ZG) |
| --- | --- | --- | --- |
| Flash size (`flash0`, `CONFIG_FLASH_SIZE`) | 2048 KB | 1024 KB | 1024 KB |
| RAM (`sram0`, `CONFIG_SRAM_SIZE`) | 384 KB at 0x20020000 | 256 KB at 0x20020000 | 256 KB at 0x20010000 |
| DTCM | 128 KB | 64 KB | 64 KB |
| MCUboot `CONFIG_BOOT_MAX_IMG_SECTORS` | 12 | 3 | 3 |
| FPU (`CPU_HAS_FPU_DOUBLE_PRECISION`) | y (FPv5-D16) | n (FPv5-SP-D16) | n |
| `dfsdm` and `pdm0` to `pdm3` nodes | present, disabled | deleted | not on the F746 |
| `sdmmc2` node | present, disabled | deleted | not on the F746 |
| `mipi_dsi` node | present, disabled | deleted | not on the F746 |
| `jpeg` node | present, disabled | deleted | not on the F746 |
| `soc` compatible | `st,stm32f767` | `st,stm32f746` | `st,stm32f746` |
| Twister metadata | ram 384, flash 2048 | ram 256, flash 1024 | ram 256, flash 1024 |

`CONFIG_FPU` is off in WallaBMC, so the FPU type changes nothing today. It
stops a later FPU enable from generating double-precision instructions that
the F746 does not have.

### What cannot be limited

These belong to the STM32F767 silicon or to the Nucleo board, so a result on
this variant does not cover them:

- RAM base address. SRAM1 starts at 0x20020000 on the F767 and at 0x20010000
  on the F746. The variant keeps 0x20020000 and only reduces the size.
- Instruction and data cache size (16 KB each, 4 KB on the F746) and the
  Cortex-M7 revision (r1p0, r0p1 on the F746). Timing will differ.
- The SoC Kconfig stays `SOC_STM32F767XX` because the HAL headers and flash
  driver must match the real part. The HAL has `FLASH_SECTOR_TOTAL` of 24, so
  the flash driver's page layout still describes 12 sectors and 2 MB. The
  partitions and `CONFIG_FLASH_SIZE` keep WallaBMC below 1 MB, but code that
  walks the driver's page layout will see the larger device.
- F767 peripherals that Zephyr has no devicetree node for (CAN3 for example)
  are still in the silicon. Nothing in the build can enable them.
- Board wiring: console on USART3 (Atlantis UART8), host console on USART6
  (Atlantis USART2), RMII LAN8742 PHY (Atlantis MII DP83826E), HSE from the
  ST-Link in bypass mode (Atlantis has a crystal).

The clock tree (216 MHz), RTC on LSI, ADC and the WallaBMC Kconfig options
are the same on all three.

### Keeping the boards in sync

This applies to people and to coding agents. The variant is only useful
while it matches Atlantis, and nothing in the build checks that it does.

Where things are defined:

| What | Atlantis | atlantis variant |
| --- | --- | --- |
| SoC memory sizes and peripherals | Zephyr `stm32f746Xg.dtsi` via `boards/tenstorrent/common/atlantis_mcu.dtsi` | `boards/st/nucleo_f767zi/nucleo_f767zi_atlantis.dts` |
| Flash partitions | `boards/tenstorrent/common/atlantis_wallabmc.dtsi` | `boards/nucleo_f767zi_atlantis.overlay` |
| WallaBMC Kconfig | `boards/atlantis_slt_mcu.conf`, `boards/atlantis_evb_mcu.conf` | `boards/nucleo_f767zi_atlantis.conf` |
| MCUboot Kconfig | `sysbuild/mcuboot/boards/atlantis_*_mcu.conf` | `sysbuild/mcuboot/boards/nucleo_f767zi_atlantis.conf` |
| Nucleo pins, aliases, JTAG | | `boards/nucleo_stm32f7_wallabmc.dtsi`, shared with the other Nucleo boards |

Rules:

1. When the flash partitions change in `atlantis_wallabmc.dtsi`, make the
   same change in `nucleo_f767zi_atlantis.overlay`, and update
   `CONFIG_BOOT_MAX_IMG_SECTORS` in both MCUboot `.conf` files if the number
   of sectors in the application slot changes. Update the flash layout table
   above.
2. When a Kconfig option that affects size or memory use (optimisation
   level, stack and heap sizes, buffer counts, features) changes in an
   Atlantis `.conf`, make the same change in `nucleo_f767zi_atlantis.conf`.
   Options that only name the product, such as the Redfish strings, do not
   need copying.
3. When a change is made for the Nucleo itself (pins, aliases, a new
   peripheral), put it in `nucleo_stm32f7_wallabmc.dtsi` so that every
   Nucleo build gets it. Only partitions belong in the two `.overlay` files.
4. A variant does not inherit `nucleo_f767zi_defconfig`,
   `boards/nucleo_f767zi.conf` or `boards/nucleo_f767zi.overlay`. Zephyr only
   reads files named for the variant. `nucleo_f767zi_atlantis_defconfig` is a
   copy of Zephyr's `nucleo_f767zi_defconfig`; compare them when the Zephyr
   revision in `west.yml` changes. The board `.dts` includes Zephyr's
   `nucleo_f767zi.dts`, so it follows upstream by itself, but check that the
   deleted nodes still exist and that no new F767-only node has been added
   to `stm32f765.dtsi` or `stm32f767.dtsi`.
5. Do not use this variant to work around a limit. If something does not
   fit, it will not fit on Atlantis either.
6. Update the tables in this file in the same commit as the change.

To check, build both and compare:

```
west build -p --sysbuild -b nucleo_f767zi/stm32f767xx/atlantis
west build -p --sysbuild -b atlantis_evb_mcu -d build-atlantis
```

- The `FLASH`, `RAM` and `DTCM` region sizes the linker prints for the
  `wallabmc` and `mcuboot` images should be the same in both builds.
- The `partition@` nodes in `build/wallabmc/zephyr/zephyr.dts` should have
  the same `reg` values.
- `CONFIG_FLASH_SIZE`, `CONFIG_SRAM_SIZE` and `CONFIG_BOOT_MAX_IMG_SECTORS`
  should match.
