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

ZMS works on any `modm::BlockDevice`, for example a range of the internal
flash or an external flash, with offsets relative to the block device:

```cpp
modm::platform::BdFlash<Flash::Size - 16*1024> storage;
// Four sectors of one erase block each
modm::zms::setup<storage>(fs, 0, 4);
```

`zms_mount()` formats an erased area. The API is documented in
`<zephyr/kvss/zms.h>`, see also the [Zephyr documentation][zms].

modm copies ZMS from the Zephyr release in [modm-ext/zephyr-partial][partial]
and applies `zms.c.patch` with ten fixes that make it survive power losses
during flash operations, also on STM32 flash with ECC: an interrupted program
or erase can leave words behind that read wrong data with an ECC error and
cannot be programmed again. They were found with a [power-loss fuzzer][fuzzer]
calibrated on STM32 hardware and are meant to be upstreamed to Zephyr:

1. Mount skips non-erased bytes behind the last data of an interrupted write.
2. `zms_recover_last_ate()` also reads the first ATE of a sector.
3. An ATE only becomes the last ATE after its data range was checked.
4. A damaged close ATE is rebuilt from the last valid ATE.
5. A failed close ATE write in `zms_sector_close()` is handled.
6. The empty ATE is also checked for its magic and format.
7. Mount erases a non-blank next sector and an open sector without a valid
   empty ATE.
8. ATEs are written block by block in reverse when the write block is smaller.
9. A damaged delete ATE in the last slot no longer fails the mount.
10. The close ATE is rebuilt even if the sector has no valid ATE.

On the internal flash ZMS relies on `BdFlash`, whose `program()` refuses
non-erased targets and skips units of only erased bytes, and on
`modm::readMarked()`, which reads words with a double ECC error as invalid.

Sectors must consist of whole erase blocks of the same size. Every value costs
one ATE of 16 bytes (or `BlockSizeWrite`, if larger) plus the value rounded
up to `BlockSizeWrite` bytes. A sector holds at most `sector_size - 5 ATEs`
bytes of data.

The ZMS functions are not thread-safe. If several fibers or interrupts access
the same `zms_fs`, implement `k_mutex_lock()` and `k_mutex_unlock()`, which
are weak empty functions.

[zms]: https://docs.zephyrproject.org/latest/services/storage/zms/zms.html
[partial]: https://github.com/modm-ext/zephyr-partial
[fuzzer]: https://github.com/salkinium/modm/tree/feature/zms-fuzzer/tools/flash_storage_fuzz
