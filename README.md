![WallaBMC logo](img/logo.png)

# WallaBMC

## Overview

WallaBMC is a simple, lightweight Baseboard Management Controller (BMC) firmware suitable for STM32 and similar class microcontrollers. Built on the Zephyr RTOS, WallaBMC provides essential BMC functionality including network management, host power control, and web-based administration through a Redfish-compliant interface.

WallaBMC is designed for embedded systems requiring BMC capabilities without the complexity of full-featured BMC solutions. It provides core functionality for monitoring and managing host systems through industry-standard interfaces.

### Features

* **LED Status Indicators**: Visual feedback for system status
* **IPv4 Networking**: Static IP or DHCP with mDNS hostname resolution
* **Redfish Interface**: Industry-standard RESTful API for management
* **Web Interface**: HTTP/HTTPS web UI for administration
* **BMC Console**: Management console accessible via serial or web interface
* **Persistent Configuration**: Settings stored across reboots
* **Host Power Control**: Power on/off management for host systems
* **Host Console**: Serial console access

### Hardware Support

WallaBMC currently supports the following hardware platforms:

| Hardware | Zephyr board name | Description |
| --- | --- | --- |
| **SiFive HiFive Premier P550 MCU** | hifive_premier_p550_mcu | RISC-V based platform |
| **STM32 Nucleo F767ZI** | nucleo_f767zi | ARM Cortex-M7 development board (standalone, no host CPU) |
| **Seeed Studio XIAO ESP32-C6** | xiao_esp32c6/esp32c6/hpcore | RISC-V, Wi-Fi 6, on a 21×17.5 mm board with USB-C; BMC console over USB Serial/JTAG (see [below](#seeed-xiao-esp32-c6)) |
| **qemu** | qemu_cortex_m3 | see [run_qemu_ci.py](scripts/run_qemu_ci.py) |

### Screenshot

The main page of the web interface is shown below, click to enlarge.

[![Web UI screenshot](img/web-ui-thumbnail.png)](img/web-ui.png)

## Using

### Prerequisites

Before getting started, ensure you have a proper Zephyr development environment. Follow the official [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html).

Required tools:

* West (Zephyr's meta-tool)
* CMake (version 3.20.0 or later)
* Python 3
* A toolchain for your target platform (ARM or RISC-V)
* OpenOCD or appropriate flashing tool for your hardware

### Installation

#### Initialize Workspace

The first step is to initialize the workspace folder where WallaBMC and all Zephyr modules will be cloned:

```
# Initialize workspace for WallaBMC (main branch)
west init -m https://github.com/tenstorrent/wallabmc.git --mr main workspace
# update Zephyr modules
cd workspace
west update
```

### Configuring

WallaBMC and Zephyr have some configuration options. Defaults generally should be used,
though there are some particular options for interest.

`CONFIG_DEFAULT_ADMIN_PASSWORD` - By default a random default admin password is generated
by the build. This can be overidden.

`CONFIG_DEFAULT_WIFI_SSID` - If the board supports WIFI, provide a default SSID. This can
be overidden by runtime configuration.

`CONFIG_DEFAULT_WIFI_PASSWORD` - If the board supports WIFI, provide a default passowrd.
This can be overidden by runtime configuration.

### Building

Append config options to the end of the build string
as such:

    `-- -DCONFIG_DEFAULT_ADMIN_PASSWORD='"admin"'`

Or run west `build -b <board> -t menuconfig` to change
these options.

### Building

Different boards have different build steps. Refer to the
board specific documentation for details.

[Nucleo F767ZI](boards/nucleo_f767zi.md)
[P550 MCU](boards/hifive_premier_p550_mcu.md)
[XIAO ESP32-C6](boards/xiao_esp32c6_hpcore.md)
[QEMU](boards/qemu_cortex_m3.md)

## Running

Each board has different features and capabilities, full details in
the board specific documentation. In general though, boards tend to:

* Be either standalone, that must have pins connected to a target to monitor
  and interact with it as a BMC. Or they are integrated MCU controllers on a
  system board that includes main CPUs, in the case of the P550 MCU.

* Have a status LED, which blinks slowly to indicate WallaBMC is running.

* Have a UART and power/reset control GPIOs.

* Have some networking link, either ethernet or Wi-Fi. WallaBMC by default uses
  DHCP with the hostname ``wallabmc`` to get an IP address lease.

* Support HTTP (and some HTTPS), and provide Redfish and a Web UI which can
  control the BMC and host system.

* Support an MCU serial console, JTAG, and flashing interface over a USB
  connection.  See board specific documentation for details about how to use
  JTAG, console, flash, etc.
  
### BMC shell

Most MCUs provide serial consoles via USB serial and via the Web UI. The Zephyr
application shell and logging is connected to this console. The shell includes
wallabmc commands.

The websocket BMC shell endpoint URL is /console/bmc and supports ws and
wss if ``CONFIG_APP_HTTPS`` (e.g., ``wss://wallabmc.local.net/console/bmc``).

### Host serial console

The websocket host console endpoint URL is /console/host and supports ws and
wss if ``CONFIG_APP_HTTPS`` (e.g., ``wss://wallabmc.local.net/console/host``).

### BMC shell settings and configuration

* ``help`` command listing with hierarchical help (e.g., ``help config``).
* ``config`` shell command can be used to configure the BMC.
* ``power`` can power the host on and off. On the Nucleo board there is no
  host CPU so one of the LEDs is a stand-in for a host power GPIO.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for information on how to contribute to this project.

## License

This project is licensed under the terms described in:

* [LICENSE](LICENSE) – code license
* [LICENSE_understanding.txt](LICENSE_understanding.txt) – license summary and clarification
* [LICENSE-DOCS](LICENSE-DOCS) – Creative Commons license for all documentation and logos
