# Power-loss fuzzer for flash storage libraries

Runs FlashDB (KVDB + TSDB) and Zephyr ZMS (key-value) on an emulated flash in a
host program, cuts the power at random points and checks every byte of the
stored data against a reference model after each reboot.

```sh
OUT=build ./build.sh   # FDB=, ZMS=, SUFFIX= select other sources, e.g. upstream
build/zms seeds=10 cap=1000 cut=prefix erase=atomic prog=content when=ops
build/fdb8 wear=20000 prog=content   # no power loss: wear and double writes
build/zms seed=7 unit=32 trace=1 ...   # one seed with every flash operation and reset
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
- `prefix`: its first 0 to unit-1 bytes, like the `max_len` of the Zephyr flash simulator.
- `bits`: a random half of the bits that should be cleared. Closest to an
  interrupted program pulse on ECC flash, which programs a whole unit at once.
- `stm32`: measured on L4/L5/U3/U5/H5/H7 (`modm flash storage.md`, "Interrupted
  program on hardware"): 20% still erased, 50% complete, 30% damaged (wrong data
  with an ECC flag). A damaged unit cannot be programmed (`prog=content`). A unit
  that reads as erased never had an ECC flag on the hardware and could be
  programmed. Units of all ones data are assumed to behave the same (not
  measured, the hardware test used `0x5a`).

**The realistic model for STM32 with ECC is `cut=stm32 erase=bits prog=stm32 when=time`**
(`prog=content` for H5/H7, `skipff=1` if the driver skips units of all ones).

`erase`: what an interrupted erase leaves
- `atomic`: the sector is unchanged, the erase did not start.
- `bits`: random bits of the sector are set, a partially erased sector.

`prog`: when programming a unit fails with -EIO
- `nor`: never, bits are ANDed (NOR flash without ECC, STM32F2/F4/F7).
- `content`: if the unit does not read as erased, like the Zephyr flash simulator
  without `CONFIG_FLASH_SIMULATOR_DOUBLE_WRITES` (ECC flash with an erased check
  by reading, STM32 PROGERR).
- `stm32`: like `content`, and also if the unit was programmed with all ones
  before (completed, or interrupted after the first 20%): measured on L4 and U5,
  the word reads as erased but the next program fails with PROGERR. H5/H7
  accept it. `skipff=1` emulates a driver that does not program units of all
  ones, which avoids this.
- `touched`: if anything programmed the unit since the erase, even if it still
  reads as erased. Not realistic: on no measured STM32 did a word that reads
  as erased fail to program.

Reads outside the flash fail like the range check of a port and are counted.
Failed programs are counted as `double` (the library wrote a completely
written unit again: a bug on ECC flash even without power loss) or `residue`
(the unit was touched by an interrupted operation).

## Not modeled

- ECC flags on reads: damaged units read as their wrong data, like the
  (corrected) data on the hardware. `Flash::read()` must not fault on them, and
  a storage library must not program them (G0 raises an NMI when programming
  a word with a double ECC error, L4/L5/U3/U5 set PROGERR).
- Interrupted erases are not measured on hardware yet, `erase=bits` is a guess.
- Bits that read correctly but are weakly programmed and flip later.
- Power loss during the reboot itself (mount and recovery run uninterrupted).

## Coverage guided with libFuzzer

`build.sh` also builds `zms-lf`, `fdb8-lf` and `golden-lf` with libFuzzer,
ASan and UBSan if a Homebrew LLVM is installed (Apple clang has no libFuzzer
runtime). The fuzzer input replaces the random numbers that choose the
structure: operations, IDs, values (ZMS up to 1800 bytes, FlashDB up to 256),
when the power fails and which kind of damage the interrupted unit gets. The
damaged bits themselves come from a separate generator, so that the fuzzer
cannot build CRC collisions byte by byte; a failure is only reported if it
repeats with at least 6 of 8 damage seeds. Every input is one run from erased
flash with up to 32 power losses and a final regular reboot.

```sh
mkdir corpus crashes
FUZZ_MODEL="cut=stm32 erase=bits prog=stm32 when=time unit=8 skipff=1" \
    build/zms-lf -max_total_time=1800 -max_len=4096 -artifact_prefix=crashes/ corpus
FUZZ_MODEL="unit=8 nopower=1" build/fdb8-lf ...            # algorithm only, no power loss
FUZZ_MODEL="... trace=1" build/zms-lf crashes/crash-...     # replay with every flash operation
build/zms-lf -minimize_crash=1 -max_total_time=60 crashes/crash-...
```

A write may fail with `-ENOSPC` only if the live values (plus 32 bytes each)
exceed `lib_capacity()`: one sector for ZMS, half a sector for FlashDB.
