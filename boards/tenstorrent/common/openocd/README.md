# Debugging the Atlantis SoC through the BMC

WallaBMC bit-bangs both SoC JTAG chains for OpenOCD's `remote_bitbang`
driver, one TCP server each, and points the board mux at the MCU only
while a client is connected (see the JTAG section of each board's
`doc/hardware.rst`). The Ascalon CPU is the default chain: it has the
port the daemon has always used and OpenOCD's default gdb, telnet and
tcl ports. One OpenOCD process drives one adapter, so run one per chain;
the configs use different ports so both can run at once.

| Chain | SoC port | BMC TCP port | OpenOCD config | gdb / telnet / tcl |
| --- | --- | --- | --- | --- |
| Ascalon (default) | JTAG1 | 7777 | `ascalon.cfg` | 3333 / 4444 / 6666 |
| RCPU | JTAG0 | 7778 | `rcpu.cfg` | 3334 / 4445 / 6667 |

The configs are `ascalon.cfg` and `rcpu.cfg` in this directory. Their
TAP layout, targets and events were validated on the Atlantis FPGA
prototype and emulator and are untested on silicon; the final `halt`
and `virt2phys_mode` are guarded so that OpenOCD stays up when they
fail. They need an OpenOCD built with the
`remote_bitbang` driver: the Debian/Ubuntu `openocd` packages,
riscv-openocd, or upstream git built with its defaults
(the 0.12.0 release needs `--enable-remote-bitbang`). The Zephyr SDK's
openocd that `west flash` uses omits it; `openocd -c 'adapter list'`
shows which drivers a build has. `BMC` is the BMC's address and
defaults to `wallabmc`:

```
openocd -c 'set BMC 10.0.0.5' \
  -f boards/tenstorrent/common/openocd/ascalon.cfg
openocd -c 'set BMC 10.0.0.5' \
  -f boards/tenstorrent/common/openocd/rcpu.cfg
gdb -ex 'target extended-remote :3333'   # Ascalon
gdb -ex 'target extended-remote :3334'   # RCPU
```

The Ascalon chain is the cluster's route controller TAP (`asc_top.jrc`,
6-bit IR) with the RISC-V debug TAP (`asc.cpu`, 5-bit IR) behind it; the
config opens the debug TAP after `init` and again after every reset,
since a cluster reset hides it. Halting needs the CPL firmware loaded
and the cores out of `no_fetch`; both configs end with a guarded `halt`,
so OpenOCD stays up when that fails and the raw check `irscan asc.cpu
0x7f0; drscan asc.cpu 33 0` can be run from the telnet port (it reads
back `0x7071`). On the prototype the RCPU chain is a single RISC-V debug
TAP with one RV32 hart; on silicon a DFD TAP is expected to share that
port in function mode, so autoprobe first on silicon and add a
placeholder `jtag newtap` if OpenOCD reports a second TAP. No IDCODE is
confirmed for Atlantis silicon, so the configs check none; the
JTAG-disable OTP fuse makes an IDCODE read 0.

Both chains wire TRST and the daemon drives it from the remote_bitbang
reset bytes, but the configs keep `reset_config none`, so OpenOCD resets
the TAPs through TMS and never asserts TRST (`reset_config trst_only`
would use the line). On the RCPU chain `reset` goes through the debug
module. On the Ascalon chain `reset`, `reset halt` and gdb's `monitor
reset halt` only reset the TAPs and re-open the debug TAP; they neither
reset the cluster nor halt it. Reset the SoC with `reset`, or `power
off` and `power on`, on the BMC shell or Redfish; SRST is never driven
by the daemon. The driver has no speed setting, the bit-bang rate is the
BMC's.
