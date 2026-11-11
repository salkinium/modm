# Zephyr Memory Storage (ZMS)

A key-value store for small values on flash, from the Zephyr RTOS, with 32-bit
IDs. ZMS writes entries round-robin into sectors: the data grows from the start
of a sector, the allocation table entries (ATE) with ID, length, CRCs and a
cycle counter grow from the end. Full sectors are closed and their newest
entries are moved by the garbage collector into the current sector, one sector
always stays erased. Values of up to 8 bytes are stored inside the ATE.

```cpp
#include <modm/ext/zephyr-zms/zms.hpp>

zms_fs fs;
// Four sectors of at least 2 kB at the end of the flash
modm::zms::setup(fs, Flash::Size - 4*2048, 4, 2048);
if (zms_mount(&fs)) { /* error */ }

uint32_t boot_count{};
zms_read(&fs, 1, &boot_count, sizeof(boot_count));
boot_count++;
zms_write(&fs, 1, &boot_count, sizeof(boot_count));
```

`zms_mount()` formats an erased area. The API is documented in
`<zephyr/kvss/zms.h>`, see also the [Zephyr documentation][zms].

This copy is changed to survive power losses during flash operations on STM32
flash with ECC: an interrupted program or erase can leave words behind that
read wrong data with an ECC error and cannot be programmed again. It was
fuzzed with a power-loss model calibrated on STM32 hardware and requires the
buffer `Flash::program()` of the `modm:platform:flash` module, which refuses
non-erased targets and skips units of only erased bytes.

Sectors must consist of whole flash pages of the same size. Every value costs
one ATE of 16 bytes (or `Flash::WriteSize`, if larger) plus the value rounded
up to `Flash::WriteSize` bytes. A sector holds at most `sector_size - 5 ATEs`
bytes of data.

The ZMS functions are not thread-safe. If several fibers or interrupts access
the same `zms_fs`, implement `k_mutex_lock()` and `k_mutex_unlock()`, which
are weak empty functions.

[zms]: https://docs.zephyrproject.org/latest/services/storage/zms/zms.html
