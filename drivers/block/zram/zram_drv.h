/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#ifndef _ZRAM_DRV_H_
#define _ZRAM_DRV_H_

#include <linux/spinlock.h>
#include <linux/zsmalloc.h>

#include "zcomp.h"

/*
 * Some arbitrary value. This is just to catch
 * invalid value for num_devices module parameter.
 */
static const unsigned max_num_devices = 32;

#define SECTOR_SHIFT		9
#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define ZRAM_LOGICAL_BLOCK_SHIFT 12
#define ZRAM_LOGICAL_BLOCK_SIZE	(1 << ZRAM_LOGICAL_BLOCK_SHIFT)
#define ZRAM_SECTOR_PER_LOGICAL_BLOCK	\
	(1 << (ZRAM_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))


/* Zero means empty, PAGE_SIZE is raw, and a uniform slot stores its word. */
#define ZRAM_UNIFORM_SIZE (PAGE_SIZE + 1)
#if PAGE_SHIFT < 16
typedef u16 zram_slot_size_t;
#else
typedef u32 zram_slot_size_t;
#endif

struct zram_slot_group {
	unsigned long locks;
	unsigned long handles[BITS_PER_LONG];
	zram_slot_size_t sizes[BITS_PER_LONG];
};

struct zram_stats {
	atomic64_t compr_data_size;	/* compressed size of pages stored */
	atomic64_t num_reads;	/* failed + successful */
	atomic64_t num_writes;	/* --do-- */
	atomic64_t num_migrated;	/* no. of migrated object */
	atomic64_t failed_reads;	/* can happen when memory is too low */
	atomic64_t failed_writes;	/* can happen when memory is too low */
	atomic64_t invalid_io;	/* non-page-aligned I/O requests */
	atomic64_t notify_free;	/* no. of swap slot free notifications */
	atomic64_t zero_pages;		/* no. of zero filled pages */
	atomic64_t same_pages;		/* all uniform pages, including zero */
	atomic64_t pages_stored;	/* no. of pages currently stored */
	atomic_long_t max_used_pages;	/* no. of maximum pages stored */
};

struct zram_meta {
	struct zram_slot_group *table;
	struct zs_pool *mem_pool;
	size_t max_comp_size;
};

struct zram {
	struct zram_meta *meta;
	struct zcomp *comp;
	struct gendisk *disk;
	/* Prevent concurrent execution of device init */
	struct rw_semaphore init_lock;
	/*
	 * the number of pages zram can consume for storing compressed data
	 */
	unsigned long limit_pages;
	int max_comp_streams;

	struct zram_stats stats;
	atomic_t refcount; /* refcount for zram_meta */
	/* wait all IO under all of cpu are done */
	wait_queue_head_t io_done;
	/*
	 * This is the limit on amount of *uncompressed* worth of data
	 * we can store in a disk.
	 */
	u64 disksize;	/* bytes */
	char compressor[10];
};
#endif
