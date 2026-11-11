// Power-loss fuzzer for FlashDB KVDB + TSDB on an emulated flash, outside of modm:
//
//   F=ext/armink/flashdb
//   printf '#define FDB_USING_KVDB\n#define FDB_USING_TSDB\n#define FDB_USING_FAL_MODE\n#define FDB_PRINT(...) ((void)0)\n' > fdb_cfg.h
//   cc -O2 -DFDB_WRITE_GRAN=64 -DECC=1 -I. -Iext/armink -I$F/inc -o fuzz ext/armink/host_fuzz.c \
//      $F/src/fdb.c $F/src/fdb_kvdb.c $F/src/fdb_tsdb.c $F/src/fdb_utils.c && ./fuzz 3000 1
//
// A write or erase is interrupted after a random number of operations:
// writes are cut after a random byte, erases leave random garbage.
// ECC=1 emulates write-once words of FDB_WRITE_GRAN bits (PROGERR on rewrite).
#include <flashdb.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ECC
#define ECC 0
#endif
#define SEC 4096
#define WG ((FDB_WRITE_GRAN + 7) / 8)
static uint8_t flash[4 * SEC];
static uint8_t written[4 * SEC / WG]; // ECC: word programmed since erase
static long ops_left = -1;            // operations until the power loss
static jmp_buf reboot;
static unsigned long progerr, kv_erases, kv_bytes, kv_updates;
static int no_logs;
static const char *cut_op = "";
static uint32_t cut_addr;

static uint32_t rng = 1;
static uint32_t rnd(uint32_t max) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng % max; }

static int rd(const struct fal_partition *p, uint32_t a, uint8_t *b, size_t n)
{ memcpy(b, flash + p->offset + a, n); return n; }
static int wr(const struct fal_partition *p, uint32_t a, const uint8_t *b, size_t n)
{
	a += p->offset;
	size_t cut = n;
	if (ops_left >= 0 && ops_left-- == 0) cut = rnd(n);
	if (ECC) {
		if (a % WG || n % WG) { printf("unaligned write 0x%x+%zu\n", (unsigned)a, n); exit(1); }
		for (size_t w = a / WG; w < (a + n) / WG; w++) if (written[w]) { progerr++; return -1; }
	}
	for (size_t i = 0; i < cut; i++) flash[a + i] &= b[i];
	if (a < 2 * SEC) kv_bytes += cut;
	if (ECC) for (size_t w = a / WG; w < (a + cut + WG - 1) / WG; w++) written[w] = 1;
	if (cut < n) { cut_op = "write"; cut_addr = a; longjmp(reboot, 1); }
	return n;
}
static int er(const struct fal_partition *p, uint32_t a, size_t n)
{
	a += p->offset;
	if (ops_left >= 0 && ops_left-- == 0 && !getenv("NOERASE")) {
		cut_op = "erase"; cut_addr = a;
		for (size_t i = 0; i < n; i++) flash[a + i] |= rnd(256); // partially erased
		longjmp(reboot, 1);
	}
	memset(flash + a, 0xff, n);
	if (a < 2 * SEC) kv_erases += n / SEC;
	memset(written + a / WG, 0, n / WG);
	return n;
}
static struct fal_partition parts[2] = {{"kv", "kv", 0, 2 * SEC, {SEC, rd, wr, er}, 0},
										{"ts", "ts", 2 * SEC, 2 * SEC, {SEC, rd, wr, er}, 0}};
int fal_init(void) { return 0; }
const struct fal_partition *fal_partition_find(const char *n) { return &parts[n[0] == 't']; }
const struct fal_flash_dev *fal_flash_device_find(const char *n) { return &fal_partition_find(n)->dev; }
int fal_partition_read(const struct fal_partition *p, uint32_t a, uint8_t *b, size_t n) { return rd(p, a, b, n); }
int fal_partition_write(const struct fal_partition *p, uint32_t a, const uint8_t *b, size_t n) { return wr(p, a, b, n); }
int fal_partition_erase(const struct fal_partition *p, uint32_t a, size_t n) { return er(p, a, n); }

// ----------------------------------------------------------------------------
#define KEYS 16
#define MAXV 64
#define MAXL 128
static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
static size_t fill(uint32_t seed, uint8_t *b, size_t max)
{
	size_t n = 1 + mix(seed) % max;
	for (size_t i = 0; i < n; i++) b[i] = mix(seed ^ (i * 0x01000193));
	// values ending with 0xFF bytes, like -1 or erased structs
	if (seed % 8 == 0) memset(b + n / 2, 0xff, n - n / 2);
	return n;
}
static struct fdb_kvdb kvdb;
static struct fdb_tsdb tsdb;
static uint32_t seeds[KEYS]; // 0 = deleted
static int pending_key = -1;
static uint32_t pending_seed;
static fdb_time_t log_time;
static unsigned long failures, cycles, kv_recovered, ts_recovered, ts_prewrite, steps;

static fdb_time_t get_time(void) { return log_time + 1; }
#define FAIL(...) do { printf("cycle %lu (%s 0x%x): ", cycles, cut_op, (unsigned)cut_addr); printf(__VA_ARGS__); failures++; } while (0)

static int matches(uint32_t seed, const uint8_t *v, size_t n)
{
	uint8_t e[MAXV];
	if (!seed) return n == 0;
	return n == fill(seed, e, MAXV) && !memcmp(v, e, n);
}

static void verify_key(int k)
{
	char name[8]; uint8_t v[MAXV]; struct fdb_blob blob;
	snprintf(name, sizeof name, "key%02d", k);
	size_t n = fdb_kv_get_blob(&kvdb, name, fdb_blob_make(&blob, v, sizeof v));
	if (k == pending_key) {
		int is_new = matches(pending_seed, v, n);
		if (!is_new && !matches(seeds[k], v, n)) FAIL("%s interrupted: neither old nor new (%zu bytes)\n", name, n);
		if (is_new) seeds[k] = pending_seed;
		pending_key = -1;
		kv_recovered++;
	} else if (!matches(seeds[k], v, n)) FAIL("%s: wrong value (%zu bytes)\n", name, n);
}

struct it { fdb_time_t last; };
static bool verify_log(fdb_tsl_t tsl, void *arg)
{
	struct it *it = arg;
	if (tsl->status == FDB_TSL_PRE_WRITE || tsl->status == FDB_TSL_UNUSED) { ts_prewrite++; return false; }
	uint8_t e[MAXL], a[MAXL]; struct fdb_blob blob;
	size_t n = fill(tsl->time, e, MAXL);
	size_t r = fdb_blob_read((fdb_db_t)&tsdb, fdb_tsl_to_blob(tsl, fdb_blob_make(&blob, a, sizeof a)));
	if (tsl->time <= it->last) FAIL("log %d after %d\n", (int)tsl->time, (int)it->last);
	if (r != n || memcmp(a, e, n)) FAIL("log %d: wrong data (%zu of %zu bytes)\n", (int)tsl->time, r, n);
	it->last = tsl->time;
	return false;
}

static void init(void)
{
	uint32_t sec = SEC;
	fdb_kvdb_control(&kvdb, FDB_KVDB_CTRL_SET_SEC_SIZE, &sec);
	fdb_tsdb_control(&tsdb, FDB_TSDB_CTRL_SET_SEC_SIZE, &sec);
	if (fdb_kvdb_init(&kvdb, "kv", "kv", NULL, NULL) != FDB_NO_ERR) FAIL("kvdb init\n");
	if (fdb_tsdb_init(&tsdb, "ts", "ts", get_time, MAXL, NULL) != FDB_NO_ERR) FAIL("tsdb init\n");
	fdb_time_t last = 0;
	fdb_tsdb_control(&tsdb, FDB_TSDB_CTRL_GET_LAST_TIME, &last);
	if (last == log_time + 1) { log_time = last; ts_recovered++; }
	if (last != log_time) FAIL("tsdb last %d instead of %d\n", (int)last, (int)log_time);
}

static void verify(void)
{
	for (int k = 0; k < KEYS; k++) verify_key(k);
	struct it it = {0};
	fdb_tsl_iter(&tsdb, verify_log, &it);
	if (it.last != log_time) FAIL("logs end at %d instead of %d\n", (int)it.last, (int)log_time);
}

static void step(void)
{
	char name[8]; uint8_t v[MAXV]; struct fdb_blob blob;
	int k = rnd(KEYS);
	snprintf(name, sizeof name, "key%02d", k);
	uint32_t op = rnd(10);
	if (op < 6) {
		uint32_t seed = rnd(0xfffffffe) + 1;
		size_t n = fill(seed, v, MAXV);
		pending_seed = seed; pending_key = k;
		fdb_err_t e = fdb_kv_set_blob(&kvdb, name, fdb_blob_make(&blob, v, n));
		if (e != FDB_NO_ERR) FAIL("%s: set failed %d\n", name, e);
		seeds[k] = seed; pending_key = -1;
		kv_updates++;
	} else if (op < 7) {
		pending_seed = 0; pending_key = k;
		fdb_err_t e = fdb_kv_del(&kvdb, name);
		if (e != FDB_NO_ERR && !(e == FDB_KV_NAME_ERR && !seeds[k])) FAIL("%s: del failed %d\n", name, e);
		seeds[k] = 0; pending_key = -1;
		kv_updates++;
	}
	verify_key(k);
	if (!no_logs && rnd(4) == 0) {
		uint8_t l[MAXL];
		size_t n = fill(log_time + 1, l, MAXL);
		fdb_err_t e = fdb_tsl_append(&tsdb, fdb_blob_make(&blob, l, n));
		if (e != FDB_NO_ERR) FAIL("append %d failed %d\n", (int)log_time + 1, e);
		log_time++;
	}
	steps++;
}

int main(int argc, char **argv)
{
	unsigned long max_cycles = argc > 1 ? strtoul(argv[1], 0, 0) : 10000;
	rng = argc > 2 ? strtoul(argv[2], 0, 0) : 1;
	memset(flash, 0xff, sizeof flash);
	ops_left = -1;
	init();
	fdb_kv_set_default(&kvdb);
	fdb_tsl_clean(&tsdb);
	setvbuf(stdout, NULL, _IONBF, 0);
	for (cycles = 0; cycles < max_cycles && failures < 20; cycles++) {
		if (getenv("TRACE")) printf("cycle %lu steps %lu\n", cycles, steps);
		if (setjmp(reboot) == 0) {
			ops_left = rnd(200);
			while (failures < 20) step();
			break;
		}
		// power loss: reboot from scratch
		ops_left = -1;
		memset(&kvdb, 0, sizeof kvdb);
		memset(&tsdb, 0, sizeof tsdb);
		init();
		verify();
	}
	// Wear of the KVDB without power loss and without logs
	unsigned long e0 = kv_erases, b0 = kv_bytes, u0 = kv_updates;
	ops_left = -1;
	no_logs = 1;
	for (int i = 0; i < 20000 && failures < 20; i++) step();
	if (kv_updates > u0) printf("KVDB per 1000 updates: %.1f erases, %.0f bytes written\n",
		1000.0 * (kv_erases - e0) / (kv_updates - u0), 1000.0 * (kv_bytes - b0) / (kv_updates - u0));
	printf("GRAN %d ECC %d: %lu cycles, %lu steps, %lu logs, recovered %lu keys %lu logs, "
		   "%lu interrupted logs seen, %lu PROGERR, %lu failures\n", FDB_WRITE_GRAN, ECC, cycles, steps,
		   (unsigned long)log_time, kv_recovered, ts_recovered, ts_prewrite, progerr, failures);
	return failures != 0;
}
