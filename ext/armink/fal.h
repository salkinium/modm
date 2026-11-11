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

// Replaces the Flash Abstraction Layer (FAL) of FlashDB: databases are stored in
// the partitions that are defined with the classes in <modm/ext/flashdb.hpp>.

#ifndef MODM_FDB_FAL_H
#define MODM_FDB_FAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fal_partition;

struct fal_flash_dev
{
	/// Erase block size, the sector size of a database is a multiple of it
	size_t blk_size;
	/// @return number of bytes or a negative value on error
	int (*read)(const struct fal_partition *part, uint32_t addr, uint8_t *buf, size_t size);
	int (*write)(const struct fal_partition *part, uint32_t addr, const uint8_t *buf, size_t size);
	int (*erase)(const struct fal_partition *part, uint32_t addr, size_t size);
};

struct fal_partition
{
	/// The `path` argument of `fdb_kvdb_init()` and `fdb_tsdb_init()`
	const char *name;
	/// Same as name, FlashDB looks up the erase block size with it
	const char *flash_name;
	/// Offset and size of the partition on the device
	uint32_t offset;
	size_t len;
	struct fal_flash_dev dev;
	struct fal_partition *next;
};

int fal_init(void);
const struct fal_partition *fal_partition_find(const char *name);
const struct fal_flash_dev *fal_flash_device_find(const char *name);
int fal_partition_read(const struct fal_partition *part, uint32_t addr, uint8_t *buf, size_t size);
int fal_partition_write(const struct fal_partition *part, uint32_t addr, const uint8_t *buf, size_t size);
int fal_partition_erase(const struct fal_partition *part, uint32_t addr, size_t size);

#ifdef __cplusplus
}
#endif

#endif // MODM_FDB_FAL_H
