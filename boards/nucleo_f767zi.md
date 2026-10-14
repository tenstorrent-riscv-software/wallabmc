## Nucleo F767ZI Documentation

### BMC serial console

The Nucleo exposes an STM32 UART as a serial device over the USB port.
WallaBMC puts the BMC console on this serial device that displays boot
and log messages, and can be used to query and configure the device.

### Host serial console

The Nucleo has USART6 connected to pins D0/D1 RX/TX on the CN10 connector.
WallaBMC uses this as the host serial console which can be accessed with
the WebUI terminal or telnet port 22. The pins would have to be connected
to something useful (e.g., each other have a loopback UART that echoes back
what is transmitted to it).

### Building
```
cd zephyr
west build -p --sysbuild -b nucleo_f767zi ../wallabmc
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
