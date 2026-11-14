# Power-loss fuzzing of flash storage on STM32

modm needs a place in flash for settings and the occasional fault dump, and
both must survive a power loss at any moment. We compared FlashDB and the
Zephyr Memory Storage (ZMS) and found that both lose data after the first
power loss at the wrong time. This folder holds the tool that found it: a
power-loss fuzzer with an emulated flash that behaves like real STM32 flash,
because it was calibrated by resetting real chips in the middle of programs
and erases.

With it we fixed ten bugs in ZMS ([`zms.c.patch`](../../ext/zephyrproject-rtos/zms/zms.c.patch)),
which modm now uses as `modm:zephyr-zms`. The fixed ZMS survives every model
of the fuzzer and hardware power-loss tests on six STM32 families. The fixes
are meant to go upstream to Zephyr, and this writeup and the fuzzer are meant
to help with that, and for everyone who is curious before then.

## What happens to STM32 flash when the power fails

The flash driver session of modm-io/modm#1424 measured it: a SysTick
interrupt with a handler in RAM calls `NVIC_SystemReset()` a fixed time after
a `Flash::program()` or `Flash::erase()` started, and the delay is swept over
the whole operation in 1/400 (program) or 1/100 (erase) steps, about 450-480
resets per board. After each reset the word is read without faulting
(`Flash::read()` reports double ECC errors instead of raising an NMI or bus
error), classified and programmed again. A system reset is not a power loss
(no supply ramp), so the numbers are indicative, but they are the best data we
have.

**An interrupted program** leaves the unit in one of these states:

| Family | erased | complete | ECC corrected (wrong data) | ECC double error |
|:-------|-------:|---------:|---------------------------:|-----------------:|
| G0 / G4 | 18% | 55-63% | not checked | 18-26% |
| L4 / L5 | 19% | 55% | 0-0.2% | 25% |
| U3 | 15% | 48% | 25% | 13% |
| U5 | 10% | 83% | 2% | 5% |
| H5 | 26% | 60% | 12% | 1% |
| H7 | 7% | 18% | 0.2% | 75% |

- No unit had wrong data without an ECC flag, but "corrected" units can hold
  wrong data, so a CRC is still needed.
- Programming the unit again works on the L4/L5/U3/U5 only if it reads as
  erased, everything else fails with PROGERR. The H5/H7 accept any program.
  The G0 raises an NMI when you program over a double ECC error.
- A unit programmed with all ones reads as erased without any flag, but on
  the L4 and U5 the next program fails with PROGERR once the cells were
  touched (after ~45% of the program time on the L476, ~12% on the U575).
- Multi-unit programs are written unit by unit in address order, also the
  128 byte burst of the U5, never all-or-nothing.

**An interrupted erase**:
- L476, H563 and F429: a system reset never interrupted an erase.
- U575: in the first 10% the bits drift towards one with ECC errors, from 12%
  erased words appear scattered over the page, from ~60% it is clean.
- H743: the sector is first programmed to zero in address order (0-15%), then
  all data is garbage, then mostly double ECC errors, and clean from ~50%.
  An interrupted erase can destroy all data and erase nothing.

**Reading a double ECC error** returns the raw uncorrected data on the NMI
families (L4, G0, G4, U5, L5, U3, H5) and garbage on the H7, so the library
must treat such a unit as invalid, never as erased.

**Timings**, which decide where a power loss most likely hits:

| Board | Erase unit | Erase time | Program unit | Program time |
|:------|:-----------|-----------:|:-------------|-------------:|
| L476 | 2 kB page | 22.1 ms | 8 B | 92 µs |
| U575 | 8 kB page | 1.54 ms | 16 B | 125 µs |
| H563 | 8 kB sector | 1.55 ms | 16 B | 36 µs |
| H743 | 128 kB sector | 0.93 s | 32 B | 86 µs |
| F429 | 16/64/128 kB sector | 0.19-0.93 s | 4 B | 15 µs |
| L053 | 128 B page | 3.3 ms | 4 B | 3.2 ms |

## The fuzzer

The emulated flash (`fuzz.h`) implements these measurements as models: how a
cut program unit ends up, how an interrupted erase looks, which units cannot
be programmed again, how double ECC errors read, and when the power fails
(after a number of operations, or weighted by the time each operation takes).
Presets combine them per family (`family=l4|u5|h5|h7|f4|l0`).

The harness keeps a model of every value in RAM. It writes, deletes and reads
random IDs, cuts the power, mounts again and checks that every value is
either the old or the new one. A run stops at the first failure, so that
later failures, which are only consequences, do not count. A simple reference
store (`golden_fuzz.c`, erase the older of two sectors, write everything with
a CRC) checks the harness itself: it survives every model, so the failures
are in the library.

There are two modes:
- **Random**: seeds of random operations and cut points, `seed=N trace=1`
  prints every flash operation of one seed, which is how most bugs were traced.
- **libFuzzer** (`zms-lf`, with ASan and UBSan): the fuzzer input chooses the
  operations, IDs, values, cut times and damage, coverage guided. The damaged
  bits come from a separate generator, and a failure only counts if 6 of 8
  damage seeds repeat it, so that libFuzzer cannot build CRC collisions.

## Results

| | Random, 10 seeds x 1000 resets per preset | libFuzzer, 30 min per preset |
|:--|:--|:--|
| ZMS v4.5.0 (`PATCH=0`) | fails after 1-2 resets in every model | - |
| ZMS with `zms.c.patch` | 10/10 seeds survive, all six presets | no failure (369k-627k inputs each) |

On hardware, `test/integration/zms` with `ZMS_POWER_LOSS_CYCLES` resets the
chip with the independent watchdog every 50-300 ms while ZMS works, and every
value must be the old or the new one after each reset. It passes on the
NUCLEO-G474RE (51 resets), -L476RG (70), -U575ZI-Q (87), -H563ZI (47),
-H743ZI (100) and -L053R8 (100). It found no further ZMS bug, only two bugs in
the flash driver.

FlashDB (with three fixes of our own) failed after 2 resets in the realistic
model, after ~6-10 resets on hardware, does not work on the H7 (36-byte write
at a 32-byte aligned address) and runs full in libFuzzer even without power
loss, so modm does not use it. Its harness is on
[salkinium:feature/flashdb](https://github.com/salkinium/modm/tree/feature/flashdb/tools/flash_storage_fuzz).

## The ten ZMS fixes

| # | Fix | Bug | Found with |
|--:|:----|:----|:-----------|
| 1 | Mount skips non-erased bytes behind the last data | The next value is written over the partial data of an interrupted write | random, cut writes |
| 2 | `zms_recover_last_ate()` reads the first ATE of a sector | The first entry moved by the garbage collector is missed and overwritten | trace of one seed |
| 3 | The data range is checked before an ATE becomes the last one | Orphaned data that passes CRC8 moves the ATE write address into the data | trace of seeds |
| 4 | A damaged close ATE is rebuilt from the last valid ATE | On ECC flash it cannot be programmed again, the sector never closes | STM32 model |
| 5 | A failed close write is handled in `zms_sector_close()` | The next sector's first ATE lands in the close slot | STM32 model |
| 6 | The empty ATE is also checked for magic and format | An interrupted erase leaves len/id all ones, only CRC8 (1/256) guards it | erase model |
| 7 | Mount erases a non-blank next sector and an open sector without a valid empty ATE | New entries become invisible, deleted values come back | random, erase model |
| 8 | ATEs are written in reverse when the write block is smaller than 16 B | A half-written ATE looks valid if the CRC block went first | cut inside units |
| 9 | A damaged delete ATE in the last slot no longer fails the mount | Mount returns `-ESPIPE`, the storage is unusable | STM32 model |
| 10 | The close ATE is rebuilt even without any valid ATE | The sector stays open, entries of the next sector are lost | libFuzzer, L0 preset |

Fixes 1-3 and 5-9 apply to any flash, 4 and 10 mostly to flash with ECC.
Zephyr's own tests miss these, because they cut the power at a few hand-picked
points and mostly only read after a remount.

## Upstreaming

All bugs were found on the host, so the Zephyr maintainers do not need our
boards. The plan: extend the Zephyr flash simulator with these measurements
(interrupted erases with random bits, damaged units that cannot be programmed
again, ECC read errors, all-ones programs on L4/U5, time-weighted cuts), port
the harness to Zephyr's libFuzzer support on `native_sim`, and send the fixes
one by one with a test each.

## Running it

```sh
OUT=build ./build.sh   # ZMS=, PATCH=0, SUFFIX= select other sources, e.g. upstream
build/zms seeds=10 cap=1000 family=l4
build/zms seeds=10 cap=1000 cut=prefix erase=atomic prog=content when=ops
build/zms wear=20000 prog=content   # no power loss: wear and double writes
build/zms seed=7 unit=32 trace=1 ...   # one seed with every flash operation and reset
```

## What is tested

- 16 keys, values of 1-64 bytes, 1/8 of the values end with 0xFF bytes (like
  -1, NaN or structs with erased defaults). Per step: 60% write, 10% delete,
  30% only read a random key.
- After every reboot all keys are read: the key whose update was interrupted
  must have its old or its new value (or be deleted for an interrupted delete),
  every other key must be unchanged.
- A run stops at the **first** failure, later failures are only consequences.
  Each seed is a new run from erased flash; a seed that reaches `cap` resets
  without failure has survived.
- ZMS uses 4 sectors of 2 kB each.

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
- `stm32`: measured on L4/L5/U3/U5/H5/H7 by resetting during `Flash::program()`:
  20% still erased, 50% complete, 30% damaged (wrong data
  with an ECC flag). A damaged unit cannot be programmed (`prog=content`). A unit
  that reads as erased never had an ECC flag on the hardware and could be
  programmed. Units of all ones data are assumed to behave the same (not
  measured, the hardware test used `0x5a`).

**The realistic models are the measured presets `family=l4|u5|h5|h7|f4|l0`**
(later options override them):

| family | unit | program | erase | interrupted unit: erased/complete/ECCC/ECCD | interrupted erase | reprogram fails | ECCD reads |
|---|---|---|---|---|---|---|---|
| l4 (L4/L5/G0/G4) | 8 | 92 µs | 22.1 ms | 19/56/0/25 | not seen, atomic | not erased, ECC, all ones before | raw |
| u5 (U5/U3) | 16 | 125 µs | 1.54 ms | 10/83/2/5 | bits drift to one, erased words from 12%, clean from 60% | like l4 | raw |
| h5 | 16 | 36 µs | 1.55 ms | 26/60/12/2 | not seen, atomic | not erased, ECC | raw |
| h7 | 32 | 86 µs | 0.93 s | 7/18/0/75 | zero in address order (0-15%), garbage (-36%), ECCD (-50%), clean | not erased, ECC | garbage |
| f4 (F2/F4/F7) | 1 | 4 µs | 0.19 s | 50/50/0/0 | not seen, atomic | never (NOR) | - |
| l0 (L0/L1) | 4 | 3.2 ms | 3.3 ms | 20/50/0/30 (unmeasured) | not seen, atomic | not erased, ECC | raw |

All presets cut by time (`when=time`), skip units of all ones like the driver
(`skipff=1`) and read ECCD units as zeros like the ports (`eccmap=1`). "Not seen"
means a system reset never interrupted the erase, real power loss is unmeasured.
The older `cut=stm32 erase=bits prog=stm32 when=time` is more pessimistic for
erases.

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

- ECC errors of neighbouring words: the driver must only report the ECC errors
  of the words it read (prefetch reads the next word too).
- Interrupted erases by power loss (only by system reset) and the L0/L1 program.
- Bits that read correctly but are weakly programmed and flip later.
- Power loss during the reboot itself (mount and recovery run uninterrupted).

## Coverage guided with libFuzzer

`build.sh` also builds `zms-lf` and `golden-lf` with libFuzzer,
ASan and UBSan if a Homebrew LLVM is installed (Apple clang has no libFuzzer
runtime). The fuzzer input replaces the random numbers that choose the
structure: operations, IDs, values (up to 1800 bytes),
when the power fails and which kind of damage the interrupted unit gets. The
damaged bits themselves come from a separate generator, so that the fuzzer
cannot build CRC collisions byte by byte; a failure is only reported if it
repeats with at least 6 of 8 damage seeds. Every input is one run from erased
flash with up to 32 power losses and a final regular reboot.

```sh
mkdir corpus crashes
FUZZ_MODEL="cut=stm32 erase=bits prog=stm32 when=time unit=8 skipff=1" \
    build/zms-lf -max_total_time=1800 -max_len=4096 -artifact_prefix=crashes/ corpus
FUZZ_MODEL="unit=8 nopower=1" build/zms-lf ...             # algorithm only, no power loss
FUZZ_MODEL="... trace=1" build/zms-lf crashes/crash-...     # replay with every flash operation
build/zms-lf -minimize_crash=1 -max_total_time=60 crashes/crash-...
```

A write may fail with `-ENOSPC` only if the live values (plus 32 bytes each)
exceed `lib_capacity()`, one sector for ZMS.
