### WIZnet W5500-EVB-Pico2 Documentation

The W5500-EVB-Pico2 is a Raspberry Pi Pico 2 class board with a
RP2350A Cortex-M33 application core, an integrated W5500 Ethernet
controller connected over SPI, and a USB-C connector.  The BMC console
appears as a USB CDC ACM serial device, and the onboard W5500 provides
10/100 Ethernet with no extra wiring.

> **Board variant.** Use the Cortex-M33 target, not the Hazard3 target:
> `w5500_evb_pico2/rp2350a/m33`.

#### Wiring

The default overlay exposes the following signals on the Pico2 GPIO
headers:

| Pico2 pin | GPIO | Direction | Host signal | Notes |
| --- | --- | --- | --- | --- |
| GP6 | 6 | out | host UART RX | UART1 TX — host serial console (115200 8N1) |
| GP7 | 7 | in | host UART TX | UART1 RX — host serial console |
| GP2 | 2 | out (open-drain) | host power switch | Active-low 200 ms pulse on `power on/off` |
| GP3 | 3 | out (open-drain) | host reset input | Active-low 1 s low pulse on `reset` / `power force-restart` |
| GND | — | — | GND | Common reference, required |

> **Voltage warning.** RP2350 GPIOs are **not** 5 V tolerant.  If the
> host pulls any of these signals above 3.3 V, level-shift or buffer
> them appropriately.

#### Build and flash

The Pico2 uses the native UF2 bootloader rather than MCUboot, so the
build skips ``--sysbuild`` and must pass the board conf/overlay
explicitly:

```
cd zephyr
west build -b w5500_evb_pico2/rp2350a/m33 ../wallabmc --pristine \
    -- -DCONFIG_DEFAULT_ADMIN_PASSWORD='"admin"'
```

The build produces `build/zephyr/zephyr.uf2`.  To flash it:

1. Hold **BOOTSEL** on the Pico2 and tap **RUN** (or plug the board
   in while holding BOOTSEL).
2. Copy `build/zephyr/zephyr.uf2` to the `RPI-RP2` USB drive.

The USB-C port is used for both UF2 flashing and the BMC USB-CDC
console.  After the board boots, look for a new `/dev/ttyACM0` device.

#### Network

The W5500 Ethernet controller is enabled by the board's devicetree and
Kconfig defaults.  On boot WallaBMC requests a DHCPv4 lease and prints
the address on the BMC console.  Browse to `http://<ip>/` for the web
UI; the BMC shell is available on `/dev/ttyACM0` and (once the board
is on the network) through the web UI.

#### Known limitations

* The host serial bridge is compiled in and exposed through the web
  terminal and TCP port 22, but it has not yet been runtime-tested on
  this board.
* HTTPS is disabled in the current board configuration to reduce heap
  pressure; the web UI and Redfish run over plain HTTP.
* The USB device stack may log `usbd_ch9: protocol error` / `80 06 00
  06 00 00 0a 00` lines while the host enumerates.  This is harmless
  noise from unsupported Device Qualifier Descriptor requests.

#### Host control from the shell

```
power on              # 200 ms pulse on GPIO2 (if BMC believes host is off)
power off             # 200 ms pulse on GPIO2 (if BMC believes host is on)
power force-off       # 6 s pulse on GPIO2
power force-restart   # 1 s low pulse on GPIO3
reset                 # alias for power force-restart
```
