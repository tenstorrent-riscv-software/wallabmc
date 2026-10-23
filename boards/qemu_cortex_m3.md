## QEMU Cortex M3 Documentation

The Zephyr QEMU Cortex M3 board is very limited but usable
for basic testing. It is used by CI, so that can show some
hints for how to use the machine, see
[scripts/run_qemu_ci.py](../scripts/run_qemu_ci.py).

### Building

QEMU does not use sysbuild.

```
cd zephyr
west build -p always -b qemu_cortex_m3 ../wallabmc
```
