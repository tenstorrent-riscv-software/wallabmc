# Quick installation

SiFive made an openocd cfg public [here](https://github.com/sifiveinc/hifive-premier-p550-tools/blob/master/mcu-firmware/stm32_openocd.cfg)
```
wget https://raw.githubusercontent.com/sifiveinc/hifive-premier-p550-tools/refs/heads/master/mcu-firmware/stm32_openocd.cfg
```

Connect a USB-C cable to the P550 debug port.

You can download the WallaBMC p550 images from a successful github actions run from [here](https://github.com/tenstorrent/wallabmc/actions).
Look for the `wallabmc-firmware-hifive_premier_p550_mcu` artifact (github doesn't provide a static link). Unzip this and do:
```
openocd -f stm32_openocd.cfg -c 'init; halt; flash write_image erase mcuboot.hex ; flash write_image erase wallabmc.signed.hex ; reset; exit'
```

This should flash and then restart the BMC. You should then see output on the USB UART. (_NOTE: There are two UARTS on the USB. One for the host p550 and one for the STM32_)

If you are building yourself with west, follow the build instructions from the [README](../README.md) and then flash with:
```
openocd -f stm32_openocd.cfg -c 'init; halt; flash write_image erase build/mcuboot/zephyr/zephyr.hex ; flash write_image erase build/wallabmc/zephyr/zephyr.signed.hex ; reset; exit'
```

# Flash standard firmware from SiFive

If you want to go back to the original firmware, do this:

```
wget https://raw.githubusercontent.com/sifiveinc/hifive-premier-p550-tools/refs/heads/master/mcu-firmware/stm32_openocd.cfg
wget https://raw.githubusercontent.com/sifiveinc/hifive-premier-p550-tools/refs/heads/master/mcu-firmware/STM32F407VET6_BMC.elf
openocd -f stm32_openocd.cfg -c 'init; halt; program STM32F407VET6_BMC.elf; reset; exit'
```
