# Configuration storage

WallaBMC's settings, eg. the admin password, hostname, network and NTP
settings, and the HTTPS key and certificate, are kept in a flash partition
separate from the firmware image. They're set with the `config` shell
command, the web UI or Redfish, and survive reboots and firmware updates.

## Where it's stored

The configuration is in the `storage` partition, formatted with Zephyr's
[NVS](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html)
(non-volatile storage), a simple key-value store designed for flash.

| Board | Partition | Sectors |
|---|---|---|
| nucleo_f767zi | 0x8000, 96 KiB | 3 × 32 KiB |
| nucleo_f767zi/stm32f767xx/atlantis | 0x10000, 64 KiB | 2 × 32 KiB |
| atlantis_evb_mcu, atlantis_slt_mcu | 0x10000, 64 KiB | 2 × 32 KiB |
| xiao_esp32c6 | 0x3b0000, 192 KiB | 48 × 4 KiB |
| hifive_premier_p550_mcu | 0x8000, 32 KiB | 2 × 16 KiB |

The partitions are defined in the board overlays in `boards/`, for the
Atlantis boards in `boards/tenstorrent/common/atlantis_wallabmc.dtsi`, which
their overlays include, or for xiao_esp32c6, by Zephyr's default Espressif
partition table. The atlantis variant of nucleo_f767zi uses the Atlantis
layout, see [its documentation](../boards/nucleo_f767zi_atlantis.md).
qemu_cortex_m3 has no persistent storage (`CONFIG_PERSISTENT_STORAGE=n`), so
it runs with the defaults, and changes are lost at reboot.

If the partition doesn't hold NVS yet, eg. on a new board or after a chip
erase, it's formatted at boot (`CONFIG_NVS_INIT_BAD_MEMORY_REGION`).

## How items are stored

Each setting is a separate NVS item, with a numeric ID from `enum
config_id` in `src/config.c`. Numbers are stored in their binary form,
strings without their NUL, and the HTTPS key and certificate as binary
blobs.

At boot `config_init()` in `src/config.c` reads each setting into RAM. A
setting that isn't in flash, or can't be read, gets its default, which is
written back to flash. So new firmware that adds an item starts with its
default, and the rest of the configuration is kept.

The HTTPS items are loaded by `https_cert.c` instead, and have no fixed
default. If the key is missing or invalid, a new one is generated. If the
certificate is missing or unusable, a new one is issued. Either way the
new one is saved. Missing extra names are left empty, and invalid ones
are deleted. See [What's checked at
boot](https-certificates.md#whats-checked-at-boot).

Changing a setting rewrites only its item. NVS never overwrites in place:
a write appends a new copy of the item, and a delete appends a record
saying it's deleted. The old copies stay in flash until NVS runs out of
space in a sector and garbage collects it, which erases a whole sector. So
deleted data isn't wiped from flash, see [Recovery in the HTTPS
certificates documentation](https-certificates.md#recovery).

## Versioning

Item 1 is the configuration version, `CFG_CURRENT_VERSION` in
`src/config.c`, currently 3. At boot:

- If there's no version item, the configuration is new, and the current
  version is written.
- If it matches the firmware's version, the configuration is loaded.
- If it differs, the whole configuration is erased, and every item gets its
  default. That includes the admin password, the network settings and the
  HTTPS key, so this is a flag day for every board. It applies to
  downgrades as well as upgrades.

Version 3 is the format with one NVS item per setting. Earlier versions
stored the whole configuration as a single item.

The version should almost never change. Compatibility comes from the item
IDs instead, following the rules in `enum config_id`:

- Add new items with new, increasing IDs.
- Never reuse an ID, even of an item that's no longer used.
- Never change what an item means or its format. Add a new item instead,
  convert the old one to it at boot, and delete the old one.
- Don't handle downgrades. Older firmware with the same configuration
  version ignores items it doesn't know. With a different version it erases
  the whole configuration, see above.

Older firmware also uses its default for an item that's longer than its
buffer for that item, and writes the default over it, eg. a string that
newer firmware allows to be longer.

## Clearing the configuration

`config clear_and_reboot` erases every sector of the partition and
reboots. Holding the user button for 10 seconds does the same, on
nucleo_f767zi and its atlantis variant the blue button, and on the Atlantis
boards button B2. Everything goes back to its default, and a new HTTPS key
is generated.

If the configuration can't be used at boot, eg. the version can't be
written, the BMC logs an error and runs with the defaults, without saving
changes. `config clear_and_reboot` still works then, and clears it. If the
partition can't be mounted at all, the BMC also runs with the defaults.

Erasing the whole chip, or changing the flash layout so the partition
moves, also loses the configuration. Updating only the firmware image, eg.
with `west flash`, keeps it. See [Firmware
updates](https-certificates.md#firmware-updates).

Changing a Nucleo between the nucleo_f767zi build and its atlantis variant
changes the layout, but the two partitions overlap: the variant's is the
last two sectors of the plain build's, and the variant's MCUboot partition
covers the first. Flashing doesn't erase the sectors they share, so the
configuration the other build finds there isn't defined: it may be lost, or
old settings may come back. Run `config clear_and_reboot` after changing
builds.
