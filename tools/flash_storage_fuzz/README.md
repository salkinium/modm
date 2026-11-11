# Power-loss fuzzer for flash storage libraries

Runs FlashDB (KVDB + TSDB) and Zephyr ZMS (key-value) on an emulated flash in a
host program, cuts the power at random points and checks every byte of the
stored data against a reference model after each reboot.

```sh
OUT=build ./build.sh   # FDB=, ZMS=, SUFFIX= select other sources, e.g. upstream
build/zms seeds=10 cap=1000 cut=prefix erase=atomic prog=content when=ops
build/fdb8 wear=20000 prog=content   # no power loss: wear and double writes
```

## What is tested

- 16 keys, values of 1-64 bytes, 1/8 of the values end with 0xFF bytes (like
  -1, NaN or structs with erased defaults). Per step: 60% write, 10% delete,
  30% only read a random key. FlashDB also appends 1-128 byte logs (25%).
- After every reboot all keys are read: the key whose update was interrupted
  must have its old or its new value (or be deleted for an interrupted delete),
  every other key must be unchanged. The logs must be in time order, have the
  right data and end with the last or the interrupted log. Interrupted logs
  that FlashDB returns with status `FDB_TSL_PRE_WRITE` are skipped.
- A run stops at the **first** failure, later failures are only consequences.
  Each seed is a new run from erased flash; a seed that reaches `cap` resets
  without failure has survived.
- The databases use 4 sectors of 2 kB each (FlashDB: 4 for KVDB, 4 for TSDB).

## The power-loss model

`when`: when the power fails
- `ops`: after 0-199 flash operations, uniformly. Stresses rarely hit paths,
  like writing the close entry of a sector.
- `time`: after 0-2 s of flash activity, a write takes 80 µs per program unit
  and an erase 20 ms per sector. Realistic: most power losses hit an erase.

`cut`: what the interrupted program unit contains (units before it are written
completely, units after it are not touched)
- `unit`: nothing, the unit is still erased. The gentlest model.
- `prefix`: its first 0-7 bytes, like the `max_len` of the Zephyr flash simulator.
- `bits`: a random half of the bits that should be cleared. Closest to an
  interrupted program pulse on ECC flash, which programs a whole unit at once.

`erase`: what an interrupted erase leaves
- `atomic`: the sector is unchanged, the erase did not start.
- `bits`: random bits of the sector are set, a partially erased sector.

`prog`: when programming a unit fails with -EIO
- `nor`: never, bits are ANDed (NOR flash without ECC, STM32F2/F4/F7).
- `content`: if the unit does not read as erased, like the Zephyr flash simulator
  without `CONFIG_FLASH_SIMULATOR_DOUBLE_WRITES` (ECC flash with an erased check
  by reading, STM32 PROGERR).
- `touched`: if anything programmed the unit since the erase, even if it still
  reads as erased. The harshest model, only realistic if the flash controller
  remembers interrupted programs in the ECC bits.

Failed programs are counted as `double` (the library wrote a completely
written unit again: a bug on ECC flash even without power loss) or `residue`
(the unit was touched by an interrupted operation).

## Not modeled

- Single-bit ECC corrections and double-ECC read errors: reads return the raw
  bits. On hardware `Flash::read()` must not fault on them.
- Bits that read correctly but are weakly programmed and flip later.
- Power loss during the reboot itself (mount and recovery run uninterrupted).
