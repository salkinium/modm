/*
 * Copyright (c) 2026, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Randomized stress test of FlashDB on the internal flash. A key-value database
// and a time series database each use two sectors of at least 2 kB behind the
// program image. The test sets, deletes and reads random keys and appends random
// logs, compares both databases with a model in RAM and re-initializes them from
// time to time, like after a reboot. It reports the time per operation and the
// number of erased pages.
//
// With FLASHDB_POWER_LOSS_CYCLES defined, the independent watchdog resets the
// device after a random time of up to 2s instead, mostly in the middle of
// writing or erasing the flash. The model survives in the .noinit RAM and after
// every reset each key must have its old or its new value, and the logs must
// end with the last or the interrupted log.

#include <modm/board.hpp>
#include <modm/ext/flashdb/flashdb.hpp>
#include <modm/platform/flash/flash.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../integration_test.hpp"

using namespace std::chrono_literals;

#ifndef FLASHDB_STRESS_SECONDS
#define FLASHDB_STRESS_SECONDS 30
#endif
// Limits the wear of the flash per run
#ifndef FLASHDB_MAX_ERASES
#define FLASHDB_MAX_ERASES 40
#endif

extern "C" const uint8_t __rom_end[];

namespace
{

constexpr uint32_t
mix(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352d;
	x ^= x >> 15; x *= 0x846ca68b;
	x ^= x >> 16;
	return x;
}

/// Fills the value of a seed, which also determines its length
size_t
fill(uint32_t seed, uint8_t *buffer, size_t max)
{
	const size_t length = 1 + mix(seed) % max;
	for (size_t ii{0}; ii < length; ii++) buffer[ii] = mix(seed ^ (ii * 0x01000193));
	return length;
}

struct Timing
{
	uint32_t count{}, sum{}, max{};
	void add(uint32_t us) { count++; sum += us; if (us > max) max = us; }
	void print(const char *name)
	{
		MODM_LOG_INFO.printf("%s: %lu x, avg %luus, max %luus\n", name, count,
							 count ? sum / count : 0, max);
	}
};

template< class F >
auto
measure(Timing &timing, F &&f)
{
	const auto start = modm::chrono::micro_clock::now();
	auto result = f();
	timing.add((modm::chrono::micro_clock::now() - start).count());
	return result;
}

// ----------------------------------------------------------------------------
constexpr size_t Keys{16};
constexpr size_t MaxValue{64};
constexpr size_t MaxLog{128};
constexpr int NoKey{-1};

/// The model of both databases, which survives a reset
struct State
{
	uint32_t magic;
	uint32_t rng;
	uint32_t kv_seed[Keys]; // 0 = deleted
	int pending_key;        // key that is set or deleted right now
	uint32_t pending_seed;
	fdb_time_t log_time;    // last appended log
	uint32_t cycles, steps, erases, kv_recovered, ts_recovered;
};
alignas(32) modm_section(".noinit") State state;
// Changes with every build, so that a new program starts from scratch
constexpr uint32_t Magic = mix(__TIME__[0] | __TIME__[1] << 8 | __TIME__[3] << 16 | __TIME__[4] << 24) ^ __TIME__[7];

uint32_t random(uint32_t max) { return (state.rng = mix(state.rng + 0x9e3779b9)) % max; }

bool failed{false};
#define CHECK(cond, ...) if (not (cond)) { MODM_LOG_ERROR.printf(__VA_ARGS__); failed = true; }

// ----------------------------------------------------------------------------
// The partitions use the last free pages behind the image
/// The reset can hit any flash operation: the model must be in RAM by then,
/// not only in the write-back data cache of a Cortex-M7
void
persist()
{
#if defined(__DCACHE_PRESENT) && __DCACHE_PRESENT
	SCB_CleanDCache_by_Addr(reinterpret_cast<uint32_t *>(&state), sizeof(state));
#endif
}

int (*erase_page)(const fal_partition*, uint32_t, size_t);
int (*write_flash)(const fal_partition*, uint32_t, const uint8_t*, size_t);
int
count_erase(const fal_partition *part, uint32_t addr, size_t size)
{
	// FlashDB erases one sector per call, which is several pages on L0/L1
	state.erases++;
	persist();
	return erase_page(part, addr, size);
}
int
persist_write(const fal_partition *part, uint32_t addr, const uint8_t *data, size_t size)
{
	persist();
	return write_flash(part, addr, data, size);
}

/// Two sectors of at least 2 kB below the page end, made of pages of the same size
/// @return offset of the first page and the sector size, or zero
uint32_t
findPages(uint16_t &end, uint32_t &sector)
{
	const uint16_t image = Flash::getPage(uintptr_t(__rom_end) - 1);
	while (end > image + 1)
	{
		const size_t size = Flash::getSize(end - 1);
		const uint16_t count = 2 * std::max<size_t>(1, 2048 / size);
		uint16_t page = end - 1;
		// Equally sized and continuous pages
		while (page > image + 1 and end - page < count and Flash::getSize(page - 1) == size and
			   Flash::getOffset(page - 1) + size == Flash::getOffset(page)) page--;
		if (end - page == count)
		{
			end = page;
			sector = count / 2 * size;
			return Flash::getOffset(page);
		}
		end = page;
	}
	return 0;
}

// ----------------------------------------------------------------------------
fdb_kvdb kvdb;
Timing kv_set, kv_get, kv_del, kv_init;

void
keyName(size_t key, char *name)
{ std::snprintf(name, 16, "key%02u", unsigned(key)); }

/// @return the length of the value, or zero if the key does not exist
size_t
readKey(size_t key, uint8_t *value)
{
	char name[16];
	keyName(key, name);
	fdb_blob blob;
	return measure(kv_get, [&] {
		return fdb_kv_get_blob(&kvdb, name, fdb_blob_make(&blob, value, MaxValue)); });
}

bool
matches(uint32_t seed, const uint8_t *value, size_t length)
{
	if (seed == 0) return length == 0;
	uint8_t expected[MaxValue];
	return length == fill(seed, expected, MaxValue) and std::memcmp(value, expected, length) == 0;
}

void
verifyKey(size_t key)
{
	uint8_t value[MaxValue]{};
	const size_t length = readKey(key, value);
	if (int(key) == state.pending_key)
	{
		// Interrupted by a reset: the old or the new value
		const bool is_new = matches(state.pending_seed, value, length);
		CHECK(is_new or matches(state.kv_seed[key], value, length),
			  "key%02u: interrupted, but neither old nor new value (%u bytes)\n", unsigned(key), unsigned(length));
		if (is_new) state.kv_seed[key] = state.pending_seed;
		state.pending_key = NoKey;
		state.kv_recovered++;
		return;
	}
	CHECK(matches(state.kv_seed[key], value, length),
		  "key%02u: read %u bytes, wrong length or data\n", unsigned(key), unsigned(length));
}

void
stepKv()
{
	const size_t key = random(Keys);
	char name[16];
	keyName(key, name);
	const uint32_t op = random(10);
	if (op < 6)
	{
		const uint32_t seed = random(0xffff'fffe) + 1;
		uint8_t value[MaxValue];
		fdb_blob blob;
		const size_t length = fill(seed, value, MaxValue);
		state.pending_seed = seed;
		state.pending_key = key;
		const fdb_err_t err = measure(kv_set, [&] {
			return fdb_kv_set_blob(&kvdb, name, fdb_blob_make(&blob, value, length)); });
		CHECK(err == FDB_NO_ERR, "%s: set failed with %d\n", name, err);
		state.kv_seed[key] = seed;
		state.pending_key = NoKey;
	}
	else if (op < 7)
	{
		state.pending_seed = 0;
		state.pending_key = key;
		const fdb_err_t err = measure(kv_del, [&] { return fdb_kv_del(&kvdb, name); });
		CHECK(err == FDB_NO_ERR or (err == FDB_KV_NAME_ERR and state.kv_seed[key] == 0),
			  "%s: delete failed with %d\n", name, err);
		state.kv_seed[key] = 0;
		state.pending_key = NoKey;
	}
	verifyKey(key);
}

// ----------------------------------------------------------------------------
fdb_tsdb tsdb;
Timing ts_append, ts_iter, ts_init;
fdb_time_t get_time() { return state.log_time + 1; }

struct Iteration
{
	fdb_time_t last;
	size_t count;
};

bool
verifyLog(fdb_tsl_t tsl, void *arg)
{
	auto &it = *static_cast<Iteration *>(arg);
	uint8_t expected[MaxLog], actual[MaxLog]{};
	const size_t length = fill(tsl->time, expected, MaxLog);
	fdb_blob blob;
	fdb_blob_make(&blob, actual, sizeof(actual));
	const size_t read = fdb_blob_read((fdb_db_t) &tsdb, fdb_tsl_to_blob(tsl, &blob));
	CHECK(tsl->time > it.last, "log %ld after %ld\n", long(tsl->time), long(it.last));
	CHECK(read == length and std::memcmp(actual, expected, length) == 0,
		  "log %ld: read %u bytes instead of %u or wrong data\n", long(tsl->time), unsigned(read), unsigned(length));
	it.last = tsl->time;
	it.count++;
	return false;
}

void
verifyLogs()
{
	Iteration it{0, 0};
	measure(ts_iter, [&] { fdb_tsl_iter(&tsdb, verifyLog, &it); return 0; });
	CHECK(it.last == state.log_time, "logs end at %ld instead of %ld\n", long(it.last), long(state.log_time));
}

bool
markLog(fdb_tsl_t tsl, void *)
{
	// Marks every 8th log as uploaded, like a fault report
	if (tsl->time % 8 == 0 and tsl->status == FDB_TSL_WRITE)
		fdb_tsl_set_status(&tsdb, tsl, FDB_TSL_USER_STATUS1);
	return false;
}

void
stepTs()
{
	uint8_t value[MaxLog];
	fdb_blob blob;
	const size_t length = fill(state.log_time + 1, value, MaxLog);
	const fdb_err_t err = measure(ts_append, [&] {
		return fdb_tsl_append(&tsdb, fdb_blob_make(&blob, value, length)); });
	CHECK(err == FDB_NO_ERR, "append %ld failed with %d\n", long(state.log_time + 1), err);
	state.log_time++;
}

// ----------------------------------------------------------------------------
uint32_t kv_sector, ts_sector;

void
init(bool format)
{
	fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &kv_sector);
	fdb_tsdb_control(&tsdb, FDB_TSDB_CTRL_SET_SEC_SIZE, &ts_sector);
	fdb_err_t err = measure(kv_init, [] { return fdb_kvdb_init(&kvdb, "kv", "kv", nullptr, nullptr); });
	CHECK(err == FDB_NO_ERR, "KVDB init failed with %d\n", err);
	if (format) fdb_kv_set_default(&kvdb);
	err = measure(ts_init, [] { return fdb_tsdb_init(&tsdb, "ts", "ts", get_time, MaxLog, nullptr); });
	CHECK(err == FDB_NO_ERR, "TSDB init failed with %d\n", err);
	if (format) fdb_tsl_clean(&tsdb);
	fdb_time_t last{};
	fdb_tsdb_control(&tsdb, FDB_TSDB_CTRL_GET_LAST_TIME, &last);
	if (last == state.log_time + 1) {
		// The append was interrupted after writing the log
		state.log_time = last;
		state.ts_recovered++;
	}
	CHECK(last == state.log_time, "TSDB last time %ld instead of %ld\n", long(last), long(state.log_time));
}

void
verify()
{
	for (size_t key{0}; key < Keys; key++) verifyKey(key);
	verifyLogs();
}

void
reinit()
{
	fdb_kvdb_deinit(&kvdb);
	fdb_tsdb_deinit(&tsdb);
	init(false);
	verify();
}

void
step()
{
	stepKv();
	if (random(4) == 0) stepTs();
	if (random(64) == 0) fdb_tsl_iter(&tsdb, markLog, nullptr);
	state.steps++;
}

void
report()
{
	MODM_LOG_INFO.printf("%lu steps, %lu erases, %ld logs, %lu cycles, recovered %lu keys and %lu logs\n",
						 state.steps, state.erases, long(state.log_time), state.cycles,
						 state.kv_recovered, state.ts_recovered);
	kv_set.print("KV set");
	kv_get.print("KV get");
	kv_del.print("KV del");
	kv_init.print("KV init");
	ts_append.print("TS append");
	ts_iter.print("TS iterate");
	ts_init.print("TS init");
}

} // namespace

// ----------------------------------------------------------------------------
int
main()
{
	Board::initialize();
	// A failed run must not restart and wear the flash further
	if (state.magic == ~Magic) return finishTest(false);
	const bool fresh = state.magic != Magic;
	if (fresh) state = State{Magic, 0x12345678, {}, NoKey, 0, 0, 0, 0, 0, 0, 0};
	else state.cycles++;
	if (fresh) modm::delay(1s); // let RTT attach before the flash stalls the bus

	uint16_t end{0};
	for (size_t size{0}; size < Flash::Size; size += Flash::getSize(end++)) ;
	const uint32_t ts_offset = findPages(end, ts_sector);
	const uint32_t kv_offset = findPages(end, kv_sector);
	if (not ts_offset or not kv_offset) {
		MODM_LOG_ERROR << "No free pages!" << modm::endl;
		return finishTest(false);
	}
	static modm::flashdb::FlashPartition ts_part("ts", ts_offset, 2 * ts_sector);
	static modm::flashdb::FlashPartition kv_part("kv", kv_offset, 2 * kv_sector);
	erase_page = kv_part.dev.erase;
	kv_part.dev.erase = ts_part.dev.erase = count_erase;
	write_flash = kv_part.dev.write;
	kv_part.dev.write = ts_part.dev.write = persist_write;

	if (fresh)
	{
		MODM_LOG_INFO << "\n\nFlashDB stress test, write granularity " << FDB_WRITE_GRAN << " bits\n"
					  << "KVDB at " << Flash::getAddr(Flash::getPage(Flash::OriginAddr + kv_offset)) << " with 2x " << kv_sector
					  << " bytes, TSDB at " << Flash::getAddr(Flash::getPage(Flash::OriginAddr + ts_offset)) << " with 2x "
					  << ts_sector << " bytes" << modm::endl;
	}
	init(fresh);
	if (not fresh) verify();
	if (failed) { state.magic = ~Magic; return finishTest(false); }

#ifdef FLASHDB_POWER_LOSS_CYCLES
	if (fresh)
		MODM_LOG_INFO << "Power loss test with " << FLASHDB_POWER_LOSS_CYCLES << " resets" << modm::endl;
	else if (state.cycles % 10 == 0)
		MODM_LOG_INFO << "cycle " << state.cycles << ", " << state.steps << " steps" << modm::endl;
	if (state.cycles < FLASHDB_POWER_LOSS_CYCLES and state.erases < FLASHDB_MAX_ERASES)
	{
		// The watchdog resets after 50-300ms, the LSI clocks it with ~32kHz / 32.
		// It keeps running after the reset, the startup must be shorter.
		Iwdg::enable();
		Iwdg::initialize(Iwdg::Prescaler::Div32, 50 + random(250));
		Iwdg::trigger();
		while (not failed) step();
		state.magic = ~Magic;
		printTestResult(false);
		while (true) Iwdg::trigger();
	}
#else
	const auto start = modm::Clock::now();
	uint32_t reinits{0};
	while (not failed and modm::Clock::now() - start < std::chrono::seconds(FLASHDB_STRESS_SECONDS)
		   and state.erases < FLASHDB_MAX_ERASES)
	{
		step();
		if (random(256) == 0) { reinit(); reinits++; }
	}
	MODM_LOG_INFO << reinits << " reinits" << modm::endl;
	reinit();
#endif
	report();
	state.magic = 0;
	return finishTest(not failed);
}
