# FlashDB

FlashDB is a database library for flash memory with a key-value database and a
time series database. It appends entries and checks them with a CRC32, wears the
sectors evenly and recovers after a power loss during a write.

- https://github.com/armink/FlashDB
- Documentation: http://armink.gitee.io/flashdb/#/

Enable the `:flashdb:kvdb` and/or `:flashdb:tsdb` submodules. The databases are
stored in partitions that you define with the classes of
`<modm/ext/flashdb/flashdb.hpp>`, the partition name is the `path` argument of
`fdb_kvdb_init()` and `fdb_tsdb_init()`:

```cpp
#include <modm/ext/flashdb/flashdb.hpp>

// Two 2 kB pages of the internal flash at offset 0x7f000
modm::flashdb::FlashPartition settings{"settings", 0x7f000, 2 * 2048};
// 64 kB of an external SPI flash
modm::BdSpiFlash<SpiMaster1, Cs, 8_MiB> spiflash;
modm::flashdb::BlockDevicePartition<spiflash> logs{"logs", 0, 64 * 1024};

fdb_kvdb kvdb;
fdb_kvdb_init(&kvdb, "settings", "settings", nullptr, nullptr);
```

A database needs at least two sectors, by default a sector is the erase block
size of the partition (`fdb_kvdb_control(FDB_KVDB_CTRL_SET_SEC_SIZE)` before
init sets a multiple of it). On the internal flash all pages of the partition
must have the same size, so on the STM32F2/F4/F7 the partition must be in the
16 kB, 64 kB or 128 kB sectors. Partitions in the other bank of a dual-bank
device do not stall the program during erases.

The module generates a `fdb_cfg.h` with `FDB_WRITE_GRAN` set to the write size
of the internal flash (`Flash::WriteSize`), which applies to all databases.
The log output of FlashDB goes to `MODM_LOG_INFO` if the `modm:debug` module is
used. To change the configuration, create a `<fdb_cfg_local.h>` file, which is
included at the *beginning* of the config, for example:

```c
#define FDB_KV_CACHE_TABLE_SIZE 16
#define FDB_SECTOR_CACHE_TABLE_SIZE 4
#define FDB_KV_NAME_MAX 32
```

FlashDB is not thread-safe: if several fibers use the same database, set a lock
with `FDB_KVDB_CTRL_SET_LOCK`/`FDB_TSDB_CTRL_SET_LOCK`.
