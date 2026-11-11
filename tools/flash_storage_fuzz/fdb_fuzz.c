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

// FlashDB KVDB (4x 2 kB at 0) and TSDB (4x 2 kB at 8 kB) on the emulated flash
#define LIB_NAME "flashdb"
#include <flashdb.h>
#include "fuzz.h"

#define SEC 2048
static int fal_rd(const struct fal_partition *p, uint32_t a, uint8_t *b, size_t n)
{ return emu_read(p->offset + a, b, n) ? -1 : (int)n; }
static int fal_wr(const struct fal_partition *p, uint32_t a, const uint8_t *b, size_t n)
{ return emu_write(p->offset + a, b, n) ? -1 : (int)n; }
static int fal_er(const struct fal_partition *p, uint32_t a, size_t n)
{ return emu_erase(p->offset + a, n) ? -1 : (int)n; }
static struct fal_partition parts[2] = {{"kv", "kv", 0, 4 * SEC, {SEC, fal_rd, fal_wr, fal_er}, 0},
										{"ts", "ts", 4 * SEC, 4 * SEC, {SEC, fal_rd, fal_wr, fal_er}, 0}};
int fal_init(void) { return 0; }
const struct fal_partition *fal_partition_find(const char *n) { return &parts[n[0] == 't']; }
const struct fal_flash_dev *fal_flash_device_find(const char *n) { return &fal_partition_find(n)->dev; }
int fal_partition_read(const struct fal_partition *p, uint32_t a, uint8_t *b, size_t n) { return p->dev.read(p, a, b, n); }
int fal_partition_write(const struct fal_partition *p, uint32_t a, const uint8_t *b, size_t n) { return p->dev.write(p, a, b, n); }
int fal_partition_erase(const struct fal_partition *p, uint32_t a, size_t n) { return p->dev.erase(p, a, n); }

#include "model.h"

static struct fdb_kvdb kvdb;
static struct fdb_tsdb tsdb;
static fdb_time_t log_time;
static fdb_time_t get_time(void) { return log_time + 1; }

static void key_name(int k, char *name) { snprintf(name, 8, "key%02d", k); }

static ssize_t kv_read(int k, uint8_t *v)
{
	char name[8]; struct fdb_blob blob;
	key_name(k, name);
	size_t n = fdb_kv_get_blob(&kvdb, name, fdb_blob_make(&blob, v, MAXV));
	return n ? (ssize_t)n : -ENOENT;
}
static int kv_write(int k, const uint8_t *v, size_t n)
{
	char name[8]; struct fdb_blob blob;
	key_name(k, name);
	return fdb_kv_set_blob(&kvdb, name, fdb_blob_make(&blob, v, n)) == FDB_NO_ERR ? 0 : -EIO;
}
static int kv_delete(int k)
{
	char name[8];
	key_name(k, name);
	fdb_err_t e = fdb_kv_del(&kvdb, name);
	return (e == FDB_NO_ERR || e == FDB_KV_NAME_ERR) ? 0 : -EIO;
}

static int log_append(void)
{
	uint8_t l[MAXL]; struct fdb_blob blob;
	size_t n = fill(log_time + 1, l, MAXL);
	if (fdb_tsl_append(&tsdb, fdb_blob_make(&blob, l, n)) != FDB_NO_ERR) return -EIO;
	log_time++;
	return 0;
}

struct iteration { fdb_time_t last; };
static bool verify_log(fdb_tsl_t tsl, void *arg)
{
	struct iteration *it = arg;
	// Interrupted appends are returned too and must be skipped by the caller
	if (tsl->status == FDB_TSL_PRE_WRITE || tsl->status == FDB_TSL_UNUSED) return false;
	uint8_t e[MAXL], a[MAXL]; struct fdb_blob blob;
	size_t n = fill(tsl->time, e, MAXL);
	size_t r = fdb_blob_read((fdb_db_t)&tsdb, fdb_tsl_to_blob(tsl, fdb_blob_make(&blob, a, sizeof a)));
	if (tsl->time <= it->last) FAIL("log %d after %d", (int)tsl->time, (int)it->last);
	else if (r != n || memcmp(a, e, n)) FAIL("log %d: wrong data", (int)tsl->time);
	it->last = tsl->time;
	return failure[0];
}
static void logs_verify(void)
{
	struct iteration it = {0};
	fdb_tsl_iter(&tsdb, verify_log, &it);
	if (it.last != log_time) FAIL("logs end at %d instead of %d", (int)it.last, (int)log_time);
}

static int open_dbs(void)
{
	uint32_t sec = SEC;
	memset(&kvdb, 0, sizeof kvdb);
	memset(&tsdb, 0, sizeof tsdb);
	fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec);
	fdb_tsdb_control(&tsdb, FDB_TSDB_CTRL_SET_SEC_SIZE, &sec);
	if (fdb_kvdb_init(&kvdb, "kv", "kv", NULL, NULL) != FDB_NO_ERR) { FAIL("kvdb init"); return -1; }
	if (fdb_tsdb_init(&tsdb, "ts", "ts", get_time, MAXL, NULL) != FDB_NO_ERR) { FAIL("tsdb init"); return -1; }
	return 0;
}

static void lib_format(void)
{
	model.sector = SEC;
	model.size = 8 * SEC;
	model_reset();
	log_time = 0;
	open_dbs();
	fdb_kv_set_default(&kvdb);
	fdb_tsl_clean(&tsdb);
}

static int lib_mount(void)
{
	if (open_dbs()) return -1;
	fdb_time_t last = 0;
	fdb_tsdb_control(&tsdb, FDB_TSDB_CTRL_GET_LAST_TIME, &last);
	if (last == log_time + 1) log_time = last; // the interrupted append was written
	if (last != log_time) FAIL("tsdb last time %d instead of %d", (int)last, (int)log_time);
	return 0;
}

static void lib_verify(void) { keys_verify(); logs_verify(); }
static void lib_step(void) { model_step(1); }
