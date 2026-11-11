// Power-loss fuzzer for Zephyr ZMS on an emulated flash, outside of modm:
//
//   D=ext/zephyrproject-rtos/zms
//   printf '#define CONFIG_ZMS_LOOKUP_CACHE 1\n#define CONFIG_ZMS_LOOKUP_CACHE_SIZE 32\n#define CONFIG_ZMS_LOOKUP_CACHE_MANUAL 0\n#define CONFIG_ZMS_DATA_CRC 1\n#define CONFIG_ZMS_NO_DOUBLE_WRITE 1\n' > zms_config.h
//   cc -O2 -DWG=8 -DECC=1 -I. -I$D/shim -I$D/zms -o fuzz $D/host_fuzz.c $D/zms/zms.c $D/zms_port.c && ./fuzz 3000 1
//
// A write or erase is interrupted after a random number of operations:
// writes are cut after a random byte, erases leave random bits set.
// ECC=1 emulates write-once words of WG bytes (PROGERR on rewrite) like on the
// STM32 L4/G4/U5/H5/H7. Also counts the erases and the written bytes per update.
#include <zephyr/kvss/zms.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WG
#define WG 8
#endif
#ifndef ECC
#define ECC 0
#endif
#ifndef SEC
#define SEC 2048
#endif
#ifndef SECTORS
#define SECTORS 4
#endif
static uint8_t flash[SECTORS * SEC];
static uint8_t written[SECTORS * SEC / WG];
static long ops_left = -1;
static jmp_buf reboot;
static unsigned long progerr, erases, bytes_written;
static const char *cut_op = "";
static uint32_t cut_addr;

static uint32_t rng = 1;
static uint32_t rnd(uint32_t max) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng % max; }

static int rd(const struct device *d, off_t a, void *b, size_t n)
{ (void)d; memcpy(b, flash + a, n); return 0; }
static int wr(const struct device *d, off_t a, const void *data, size_t n)
{
	(void)d;
	const uint8_t *b = data;
	size_t cut = n;
	if (ops_left >= 0 && ops_left-- == 0) cut = rnd(n);
	if (a % WG || n % WG) { printf("unaligned write 0x%x+%zu\n", (unsigned)a, n); exit(1); }
	if (ECC) for (size_t w = a / WG; w < (a + n) / WG; w++) if (written[w]) { progerr++; return -EIO; }
	for (size_t i = 0; i < cut; i++) flash[a + i] &= b[i];
	if (ECC) for (size_t w = a / WG; w < (a + cut + WG - 1) / WG; w++) written[w] = 1;
	bytes_written += cut;
	if (cut < n) { cut_op = "write"; cut_addr = a; longjmp(reboot, 1); }
	return 0;
}
static int er(const struct device *d, off_t a, size_t n)
{
	(void)d;
	if (ops_left >= 0 && ops_left-- == 0) {
		cut_op = "erase"; cut_addr = a;
		for (size_t i = 0; i < n; i++) flash[a + i] |= rnd(256);
		longjmp(reboot, 1);
	}
	memset(flash + a, 0xff, n);
	memset(written + a / WG, 0, n / WG);
	erases += n / SEC;
	return 0;
}
static const struct flash_parameters params = {WG, {0}, 0xff};
static const struct flash_parameters *gp(const struct device *d) { (void)d; return &params; }
static int pi(const struct device *d, off_t o, struct flash_pages_info *i)
{ (void)d; i->start_offset = o - o % SEC; i->size = SEC; i->index = o / SEC; return 0; }
static const struct flash_driver_api api = {rd, wr, er, gp, pi};
static const struct device dev = {&api, NULL};

// ----------------------------------------------------------------------------
#define KEYS 16
#define MAXV 64
static uint32_t mix(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
static size_t fill(uint32_t seed, uint8_t *b, size_t max)
{
	size_t n = 1 + mix(seed) % max;
	for (size_t i = 0; i < n; i++) b[i] = mix(seed ^ (i * 0x01000193));
	if (seed % 8 == 0) memset(b + n / 2, 0xff, n - n / 2);
	return n;
}
static struct zms_fs fs;
static uint32_t seeds[KEYS];
static int pending_key = -1;
static uint32_t pending_seed;
static unsigned long failures, cycles, recovered, steps, updates;
#define FAIL(...) do { printf("cycle %lu (%s 0x%x): ", cycles, cut_op, (unsigned)cut_addr); printf(__VA_ARGS__); failures++; } while (0)

static int matches(uint32_t seed, const uint8_t *v, ssize_t n)
{
	uint8_t e[MAXV];
	if (!seed) return n == -ENOENT;
	return n == (ssize_t)fill(seed, e, MAXV) && !memcmp(v, e, n);
}

static void verify_key(int k)
{
	uint8_t v[MAXV];
	ssize_t n = zms_read(&fs, k, v, sizeof v);
	if (k == pending_key) {
		int is_new = matches(pending_seed, v, n);
		if (!is_new && !matches(seeds[k], v, n)) FAIL("key %d interrupted: neither old nor new (%zd)\n", k, n);
		if (is_new) seeds[k] = pending_seed;
		pending_key = -1;
		recovered++;
	} else if (!matches(seeds[k], v, n)) FAIL("key %d: wrong value (%zd)\n", k, n);
}

static void mount(void)
{
	memset(&fs, 0, sizeof fs);
	fs.flash_device = &dev;
	fs.offset = 0;
	fs.sector_size = SEC;
	fs.sector_count = SECTORS;
	int e = zms_mount(&fs);
	if (e) FAIL("mount failed %d\n", e);
}

static void step(void)
{
	uint8_t v[MAXV];
	int k = rnd(KEYS);
	uint32_t op = rnd(10);
	if (op < 6) {
		uint32_t seed = rnd(0xfffffffe) + 1;
		size_t n = fill(seed, v, MAXV);
		pending_seed = seed; pending_key = k;
		ssize_t e = zms_write(&fs, k, v, n);
		if (e < 0) FAIL("key %d: write failed %zd\n", k, e);
		seeds[k] = seed; pending_key = -1;
		updates++;
	} else if (op < 7) {
		pending_seed = 0; pending_key = k;
		int e = zms_delete(&fs, k);
		if (e < 0) FAIL("key %d: delete failed %d\n", k, e);
		seeds[k] = 0; pending_key = -1;
		updates++;
	}
	verify_key(k);
	steps++;
}

int main(int argc, char **argv)
{
	unsigned long max_cycles = argc > 1 ? strtoul(argv[1], 0, 0) : 3000;
	rng = argc > 2 ? strtoul(argv[2], 0, 0) : 1;
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(flash, 0xff, sizeof flash);
	mount();
	for (cycles = 0; cycles < max_cycles && failures < 20; cycles++) {
		if (setjmp(reboot) == 0) {
			ops_left = rnd(200);
			while (failures < 20) step();
			break;
		}
		ops_left = -1;
		mount();
		for (int k = 0; k < KEYS; k++) verify_key(k);
	}
	// Wear without power loss
	unsigned long e0 = erases, b0 = bytes_written, u0 = updates;
	ops_left = -1;
	for (int i = 0; i < 20000 && failures < 20; i++) step();
	printf("WG %d ECC %d %dx%d: %lu cycles, %lu steps, recovered %lu, %lu PROGERR, %lu failures; "
		   "per 1000 updates: %.1f erases, %.0f bytes written\n", WG, ECC, SECTORS, SEC, cycles, steps,
		   recovered, progerr, failures, 1000.0 * (erases - e0) / (updates - u0),
		   1000.0 * (bytes_written - b0) / (updates - u0));
	return failures != 0;
}
