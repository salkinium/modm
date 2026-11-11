/*
 * Copyright (c) 2026, Niklas Hauser
 *
 * This file is part of the modm project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
// ----------------------------------------------------------------------------

// Emulated flash with power loss and the fuzzing harness, see README.md.
// The storage library file defines the hooks below and includes this header.
#pragma once
#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- the model
enum cut_model { CUT_UNIT, CUT_PREFIX, CUT_BITS };   // what an interrupted write leaves
enum erase_model { ERASE_ATOMIC, ERASE_BITS };       // what an interrupted erase leaves
enum prog_model { PROG_NOR, PROG_CONTENT, PROG_TOUCHED }; // when programming a unit fails
enum when_model { WHEN_OPS, WHEN_TIME };             // when the power fails

static struct
{
	enum cut_model cut;
	enum erase_model erase;
	enum prog_model prog;
	enum when_model when;
	size_t unit;      // program unit in bytes
	size_t sector;    // erase unit in bytes
	size_t size;      // flash size in bytes
	uint32_t write_us, erase_us; // duration per unit and per sector
} model = {CUT_PREFIX, ERASE_BITS, PROG_NOR, WHEN_OPS, 8, 2048, 8192, 80, 20000};

// ---------------------------------------------------------------- the flash
#define FLASH_MAX (64 * 1024)
static uint8_t flash[FLASH_MAX];
static uint8_t touched[FLASH_MAX];     // unit programmed since erase (PROG_TOUCHED)
static uint8_t interrupted[FLASH_MAX]; // unit touched by an interrupted operation
static long power_left = -1;
static int trace;           // operations or microseconds until power loss
static jmp_buf power_loss;
static struct
{
	unsigned long erases, bytes, double_writes, residue_writes;
	const char *op;
	uint32_t addr;
} stats;

static uint32_t rng = 1;
static uint32_t rnd(uint32_t max) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return max ? rng % max : 0; }

static int unit_erased(uint32_t u)
{
	for (size_t i = 0; i < model.unit; i++) if (flash[u * model.unit + i] != 0xff) return 0;
	return 1;
}

/// @return the number of bytes (or 1 for erases) before the power fails, or -1
static long power_cut(size_t units, uint32_t us_per_unit)
{
	if (power_left < 0) return -1;
	if (model.when == WHEN_OPS) {
		if (power_left-- == 0) return rnd(units);
		return -1;
	}
	long duration = (long)units * us_per_unit;
	if (power_left < duration) { long cut = power_left / us_per_unit; power_left = -1; return cut; }
	power_left -= duration;
	return -1;
}

static int emu_read(uint32_t a, void *b, size_t n) { memcpy(b, flash + a, n); return 0; }

static int emu_write(uint32_t a, const void *data, size_t n)
{
	const uint8_t *b = data;
	if (a % model.unit || n % model.unit || a + n > model.size) {
		printf("FATAL: unaligned write 0x%x+%zu\n", (unsigned)a, n); exit(2);
	}
	for (uint32_t u = a / model.unit; u < (a + n) / model.unit; u++) {
		const int fails = (model.prog == PROG_CONTENT && !unit_erased(u)) ||
						  (model.prog == PROG_TOUCHED && touched[u]);
		if (fails) {
			if (interrupted[u]) stats.residue_writes++; else stats.double_writes++;
			return -EIO;
		}
	}
	const long cut = power_cut(n / model.unit, model.write_us); // in units
	if (trace) printf("    write 0x%04x+%zu%s\n", (unsigned)a, n, cut < 0 ? "" : " CUT");
	const size_t whole = cut < 0 ? n : (size_t)cut * model.unit;
	for (size_t i = 0; i < whole; i++) flash[a + i] &= b[i];
	for (size_t u = a / model.unit; u < (a + whole) / model.unit; u++) touched[u] = 1;
	stats.bytes += whole;
	if (cut < 0) return 0;
	// the interrupted unit
	const uint32_t u = (a + whole) / model.unit, base = u * model.unit;
	switch (model.cut) {
	case CUT_UNIT: break; // nothing of this unit was programmed
	case CUT_PREFIX: { // the first bytes, like the Zephyr flash simulator
		size_t k = rnd(model.unit);
		for (size_t i = 0; i < k; i++) flash[base + i] &= b[whole + i];
		touched[u] = k > 0;
		break;
	}
	case CUT_BITS: // a random subset of the bits that should be cleared
		for (size_t i = 0; i < model.unit; i++) flash[base + i] &= b[whole + i] | (uint8_t)rnd(256);
		touched[u] = 1; // the program pulse was applied
		break;
	}
	interrupted[u] = 1;
	stats.op = "write"; stats.addr = base;
	longjmp(power_loss, 1);
}

static int emu_erase(uint32_t a, size_t n)
{
	if (a % model.sector || n % model.sector) { printf("FATAL: unaligned erase\n"); exit(2); }
	const long cut = power_cut(n / model.sector, model.erase_us); // in sectors
	if (trace) printf("    erase 0x%04x+%zu%s\n", (unsigned)a, n, cut < 0 ? "" : " CUT");
	const size_t whole = cut < 0 ? n : (size_t)cut * model.sector;
	memset(flash + a, 0xff, whole);
	memset(touched + a / model.unit, 0, whole / model.unit);
	memset(interrupted + a / model.unit, 0, whole / model.unit);
	stats.erases += whole / model.sector;
	if (cut < 0) return 0;
	const uint32_t s = a + whole;
	if (model.erase == ERASE_BITS) {
		for (size_t i = 0; i < model.sector; i++) flash[s + i] |= rnd(256);
		for (size_t u = s / model.unit; u < (s + model.sector) / model.unit; u++) interrupted[u] = 1;
	}
	stats.op = "erase"; stats.addr = s;
	longjmp(power_loss, 1);
}

// ---------------------------------------------------------------- the hooks
static void lib_format(void);       // erase and initialize the storage
static int lib_mount(void);         // after a power loss, 0 on success
static void lib_step(void);         // one random operation, updates the model
static void lib_verify(void);       // compares all data with the model
static unsigned long lib_updates(void);

static char failure[256];
#define FAIL(...) do { if (!failure[0]) snprintf(failure, sizeof failure, __VA_ARGS__); } while (0)

// ---------------------------------------------------------------- the harness
static int parse(const char *arg, const char *name, const char *const *values, int *out)
{
	size_t len = strlen(name);
	if (strncmp(arg, name, len) || arg[len] != '=') return 0;
	for (int i = 0; values[i]; i++) if (!strcmp(arg + len + 1, values[i])) { *out = i; return 1; }
	printf("unknown %s\n", arg); exit(2);
}

int main(int argc, char **argv)
{
	unsigned long seeds = 10, cap = 1000, wear = 0;
	static const char *const cuts[] = {"unit", "prefix", "bits", 0}, *const erases[] = {"atomic", "bits", 0},
		*const progs[] = {"nor", "content", "touched", 0}, *const whens[] = {"ops", "time", 0};
	for (int i = 1; i < argc; i++) {
		int v;
		if (parse(argv[i], "cut", cuts, &v)) model.cut = v;
		else if (parse(argv[i], "erase", erases, &v)) model.erase = v;
		else if (parse(argv[i], "prog", progs, &v)) model.prog = v;
		else if (parse(argv[i], "when", whens, &v)) model.when = v;
		else if (!strncmp(argv[i], "unit=", 5)) model.unit = strtoul(argv[i] + 5, 0, 0);
		else if (!strncmp(argv[i], "seeds=", 6)) seeds = strtoul(argv[i] + 6, 0, 0);
		else if (!strncmp(argv[i], "cap=", 4)) cap = strtoul(argv[i] + 4, 0, 0);
		else if (!strncmp(argv[i], "trace=", 6)) trace = strtoul(argv[i] + 6, 0, 0);
		else if (!strncmp(argv[i], "wear=", 5)) wear = strtoul(argv[i] + 5, 0, 0);
		else { printf("unknown %s\n", argv[i]); return 2; }
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("%s cut=%s erase=%s prog=%s when=%s unit=%zu: ", LIB_NAME, cuts[model.cut], erases[model.erase],
		   progs[model.prog], whens[model.when], model.unit);

	if (wear) {
		// Without power loss: wear and double writes of the library itself
		rng = 1; memset(flash, 0xff, sizeof flash); memset(touched, 0, sizeof touched);
		lib_format();
		memset(&stats, 0, sizeof stats);
		unsigned long u0 = lib_updates();
		for (unsigned long i = 0; i < wear && !failure[0]; i++) lib_step();
		const double k = 1000.0 / (lib_updates() - u0);
		printf("per 1000 updates %.1f erases, %.0f bytes, %lu double writes%s%s\n", stats.erases * k,
			   stats.bytes * k, stats.double_writes, failure[0] ? ", FAILED: " : "", failure);
		return failure[0] != 0;
	}

	unsigned long survived[1000], capped = 0;
	for (unsigned long seed = 1; seed <= seeds && seed <= 1000; seed++) {
		rng = seed * 2654435761u;
		memset(flash, 0xff, sizeof flash); memset(touched, 0, sizeof touched);
		memset(interrupted, 0, sizeof interrupted); memset(&stats, 0, sizeof stats);
		failure[0] = 0; power_left = -1;
		lib_format();
		unsigned long resets = 0;
		while (!failure[0] && resets < cap) {
			if (setjmp(power_loss) == 0) {
				power_left = model.when == WHEN_OPS ? (long)rnd(200) : (long)rnd(2000000);
				while (!failure[0]) lib_step();
				break; // a failure without power loss
			}
			power_left = -1;
			resets++;
			if (trace) printf("  --- reset %lu\n", resets);
			if (lib_mount()) break;
			lib_verify();
		}
		survived[seed - 1] = resets;
		if (!failure[0]) capped++;
		else printf("\n  seed %lu after %lu resets (%s 0x%x, %lu double, %lu residue): %s", seed, resets,
					stats.op ? stats.op : "-", (unsigned)stats.addr, stats.double_writes,
					stats.residue_writes, failure);
	}
	unsigned long min = cap;
	for (unsigned long s = 0; s < seeds; s++) if (survived[s] < min) min = survived[s];
	printf("\n  => %lu of %lu seeds survived %lu resets, first failure after %lu resets\n",
		   capped, seeds, cap, capped == seeds ? cap : min);
	return capped != seeds;
}
