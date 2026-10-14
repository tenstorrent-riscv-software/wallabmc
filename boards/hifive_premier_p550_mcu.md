## P550 MCU Documentation

### Host serial console

The P550 host serial console UART is connected to an actual serial console
UART on the host CPU.

### Building
```
cd zephyr
west build -p --sysbuild -b hifive_premier_p550_mcu ../wallabmc
```

### Flashing

```
west flash --runner openocd
```

Alternatively, use the `build/wallabmc/zephyr/zephyr.signed.hex` and
`build/mcuboot/zephyr/zephyr.hex` files, and run the openocd commands:

```
flash write_image erase build/mcuboot/zephyr/zephyr.hex
flash write_image erase build/wallabmc/zephyr/zephyr.signed.hex
```
Also see instructions on [flashing the p550](boards/sifive/hifive_premier_p550_mcu/support/README.md)
