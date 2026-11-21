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

#define KMSG_COMPONENT "zram"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#ifdef CONFIG_ZRAM_DEBUG
#define DEBUG
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>

#include "zram_drv.h"

#ifdef CONFIG_ZRAM_CINNAMON_COMPRESS
#include "zcomp_cinnamon.h"
#endif

/* Globals */
static int zram_major;
static struct zram *zram_devices;
static const char *default_compressor = "lzo";

/* Module params (documentation at end) */
static unsigned int num_devices = 1;

static inline void deprecated_attr_warn(const char *name)
{
	pr_warn_once("%d (%s) Attribute %s (and others) will be removed. %s\n",
			task_pid_nr(current),
			current->comm,
			name,
			"See zram documentation.");
}

#define ZRAM_ATTR_RO(name)						\
static ssize_t name##_show(struct device *d,		\
				struct device_attribute *attr, char *b)	\
{									\
	struct zram *zram = dev_to_zram(d);				\
									\
	deprecated_attr_warn(__stringify(name));			\
	return scnprintf(b, PAGE_SIZE, "%llu\n",			\
		(u64)atomic64_read(&zram->stats.name));			\
}									\
static DEVICE_ATTR_RO(name);

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static ssize_t compact_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	unsigned long nr_migrated;
	struct zram *zram = dev_to_zram(dev);
	struct zram_meta *meta;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	meta = zram->meta;
	nr_migrated = zs_compact(meta->mem_pool);
	atomic64_add(nr_migrated, &zram->stats.num_migrated);
	up_read(&zram->init_lock);

	return len;
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	u64 size;

	down_read(&zram->init_lock);
	size = zram->disksize;
	up_read(&zram->init_lock);
	return scnprintf(buf, PAGE_SIZE, "%llu\n", size);
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = init_done(zram);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t orig_data_size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	deprecated_attr_warn("orig_data_size");
	return scnprintf(buf, PAGE_SIZE, "%llu\n",
		(u64)(atomic64_read(&zram->stats.pages_stored)) << PAGE_SHIFT);
}

static ssize_t mem_used_total_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val = 0;
	struct zram *zram = dev_to_zram(dev);

	deprecated_attr_warn("mem_used_total");
	down_read(&zram->init_lock);
	if (init_done(zram)) {
		struct zram_meta *meta = zram->meta;
		val = zs_get_total_pages(meta->mem_pool);
	}
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val << PAGE_SHIFT);
}

static ssize_t max_comp_streams_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = zram->max_comp_streams;
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t mem_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val;
	struct zram *zram = dev_to_zram(dev);

	deprecated_attr_warn("mem_limit");
	down_read(&zram->init_lock);
	val = zram->limit_pages;
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val << PAGE_SHIFT);
}

/* Accept byte counts and the conventional binary size suffixes. */
static int zram_parse_size(const char *buf, u64 *bytes)
{
	char value[32];
	char *number, *suffix;
	int shift, ret;

	if (strlcpy(value, buf, sizeof(value)) >= sizeof(value))
		return -EINVAL;
	number = strim(value);

	ret = kstrtoull(number, 0, bytes);
	if (ret != -EINVAL)
		return ret;

	suffix = number + strlen(number);
	if (suffix > number && (suffix[-1] == 'B' || suffix[-1] == 'b')) {
		suffix--;
		if (suffix > number && (suffix[-1] == 'I' || suffix[-1] == 'i'))
			suffix--;
	}
	if (suffix == number)
		return -EINVAL;
	switch (suffix[-1]) {
	case 'K': case 'k': shift = 10; break;
	case 'M': case 'm': shift = 20; break;
	case 'G': case 'g': shift = 30; break;
	case 'T': case 't': shift = 40; break;
	case 'P': case 'p': shift = 50; break;
	case 'E': case 'e': shift = 60; break;
	default: return -EINVAL;
	}

	*--suffix = '\0';
	ret = kstrtoull(number, 0, bytes);
	if (ret)
		return ret;
	if (*bytes > (U64_MAX >> shift))
		return -EOVERFLOW;
	*bytes <<= shift;
	return 0;
}

static ssize_t mem_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 limit;
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = zram_parse_size(buf, &limit);
	if (ret)
		return ret;
	if (limit > U64_MAX - (PAGE_SIZE - 1))
		return -EOVERFLOW;
	limit = PAGE_ALIGN(limit) >> PAGE_SHIFT;
	if (limit > ULONG_MAX)
		return -EINVAL;

	down_write(&zram->init_lock);
	ACCESS_ONCE(zram->limit_pages) = limit;
	up_write(&zram->init_lock);

	return len;
}

static ssize_t mem_used_max_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val = 0;
	struct zram *zram = dev_to_zram(dev);

	deprecated_attr_warn("mem_used_max");
	down_read(&zram->init_lock);
	if (init_done(zram))
		val = atomic_long_read(&zram->stats.max_used_pages);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val << PAGE_SHIFT);
}

static ssize_t mem_used_max_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int err;
	unsigned long val;
	struct zram *zram = dev_to_zram(dev);

	err = kstrtoul(buf, 10, &val);
	if (err || val != 0)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		struct zram_meta *meta = zram->meta;
		atomic_long_set(&zram->stats.max_used_pages,
				zs_get_total_pages(meta->mem_pool));
	}
	up_read(&zram->init_lock);

	return len;
}

static ssize_t max_comp_streams_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int num;
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = kstrtoint(buf, 0, &num);
	if (ret < 0)
		return ret;
	if (num < 1)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		if (!zcomp_set_max_streams(zram->comp, num)) {
			pr_info("Cannot change max compression streams\n");
			ret = -EINVAL;
			goto out;
		}
	}

	zram->max_comp_streams = num;
	ret = len;
out:
	up_write(&zram->init_lock);
	return ret;
}

static ssize_t comp_algorithm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t sz;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->compressor, buf);
	up_read(&zram->init_lock);

	return sz;
}

static ssize_t comp_algorithm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	const char *algorithm;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		up_write(&zram->init_lock);
		pr_info("Can't change algorithm for initialized device\n");
		return -EBUSY;
	}

	algorithm = zcomp_algorithm_name(buf);
	if (!algorithm) {
		up_write(&zram->init_lock);
		return -EINVAL;
	}
	strlcpy(zram->compressor, algorithm, sizeof(zram->compressor));
	up_write(&zram->init_lock);
	return len;
}

static inline u64 zram_table_groups(u64 pages)
{
	return (pages + BITS_PER_LONG - 1) / BITS_PER_LONG;
}

static inline struct zram_slot_group *zram_group(struct zram_meta *meta, u32 index)
{
	return &meta->table[index / BITS_PER_LONG];
}

static inline int is_partial_io(struct bio_vec *bvec)
{
	return bvec->bv_len != PAGE_SIZE;
}

/*
 * Check if request is within bounds and aligned on zram logical blocks.
 */
static inline int valid_io_request(struct zram *zram,
		sector_t start, unsigned int size)
{
	u64 end, bound;

	/* unaligned request */
	if (unlikely(start & (ZRAM_SECTOR_PER_LOGICAL_BLOCK - 1)))
		return 0;
	if (unlikely(size & (ZRAM_LOGICAL_BLOCK_SIZE - 1)))
		return 0;

	end = start + (size >> SECTOR_SHIFT);
	bound = zram->disksize >> SECTOR_SHIFT;
	/* out of range range */
	if (unlikely(start >= bound || end > bound || start > end))
		return 0;

	/* I/O request is valid */
	return 1;
}

static void zram_meta_free(struct zram_meta *meta, u64 disksize)
{
	size_t num_pages = disksize >> PAGE_SHIFT;
	size_t index;

	/* Free all pages that are still in this zram device */
	for (index = 0; index < num_pages; index++) {
		struct zram_slot_group *group = zram_group(meta, index);
		unsigned int slot = index % BITS_PER_LONG;
		unsigned long handle = group->handles[slot];

		if (!group->sizes[slot] || group->sizes[slot] == ZRAM_UNIFORM_SIZE)
			continue;

		zs_free(meta->mem_pool, handle);
	}

	zs_destroy_pool(meta->mem_pool);
	vfree(meta->table);
	kfree(meta);
}

static struct zram_meta *zram_meta_alloc(int device_id, u64 disksize)
{
	size_t num_pages;
	char pool_name[8];
	struct zram_meta *meta = kmalloc(sizeof(*meta), GFP_KERNEL);

	if (!meta)
		return NULL;

	num_pages = disksize >> PAGE_SHIFT;
	meta->table = vzalloc(zram_table_groups(num_pages) * sizeof(*meta->table));
	if (!meta->table) {
		pr_err("Error allocating zram address table\n");
		goto out_error;
	}

	snprintf(pool_name, sizeof(pool_name), "zram%d", device_id);
	meta->mem_pool = zs_create_pool(pool_name, GFP_NOIO | __GFP_HIGHMEM);
	if (!meta->mem_pool) {
		pr_err("Error creating memory pool\n");
		goto out_error;
	}

	meta->max_comp_size = zs_max_packed_size(meta->mem_pool);
	return meta;

out_error:
	vfree(meta->table);
	kfree(meta);
	return NULL;
}

static inline bool zram_meta_get(struct zram *zram)
{
	if (atomic_inc_not_zero(&zram->refcount))
		return true;
	return false;
}

static inline void zram_meta_put(struct zram *zram)
{
	if (atomic_dec_and_test(&zram->refcount))
		wake_up(&zram->io_done);
}

static void update_position(u32 *index, int *offset, struct bio_vec *bvec)
{
	if (*offset + bvec->bv_len >= PAGE_SIZE)
		(*index)++;
	*offset = (*offset + bvec->bv_len) % PAGE_SIZE;
}

static int page_same_filled(void *ptr, unsigned long *element)
{
	unsigned int pos, last = PAGE_SIZE / sizeof(unsigned long) - 1;
	unsigned long *page = ptr;
	unsigned long value = page[0];

	/* Quickly rule out pages with a long uniform prefix. */
	if (page[last] != value)
		return 0;
	for (pos = 1; pos < last; pos++)
		if (page[pos] != value)
			return 0;

	*element = value;
	return 1;
}

static void zram_fill_page(void *ptr, unsigned long element)
{
	unsigned int pos;
	unsigned long *page = ptr;

	for (pos = 0; pos != PAGE_SIZE / sizeof(*page); pos++)
		page[pos] = element;
}

static void zram_copy_partial_same(struct bio_vec *bvec, int offset,
				   unsigned long element)
{
	struct page *page = bvec->bv_page;
	const u8 *word = (const u8 *)&element;
	unsigned int remaining = bvec->bv_len;
	u8 *base = kmap_atomic(page);
	u8 *dst = base + bvec->bv_offset;

	if (!element) {
		memset(dst, 0, remaining);
	} else {
		u8 pattern[sizeof(element)];
		unsigned int pos = offset % sizeof(element);
		unsigned int tail = sizeof(element) - pos;
		unsigned int copied = min_t(unsigned int, sizeof(element), remaining);

		memcpy(pattern, word + pos, tail);
		memcpy(pattern + tail, word, pos);
		memcpy(dst, pattern, copied);
		while (copied < remaining) {
			unsigned int count = min(copied, remaining - copied);

			memcpy(dst + copied, dst, count);
			copied += count;
		}
	}
	kunmap_atomic(base);
	flush_dcache_page(page);
}


/* Caller owns the slot's lock bit. */
static void zram_free_page(struct zram *zram, struct zram_slot_group *group,
			   unsigned int slot)
{
	size_t size = group->sizes[slot];
	unsigned long handle = group->handles[slot];

	if (!size)
		return;
	if (size == ZRAM_UNIFORM_SIZE) {
		if (!handle)
			atomic64_dec(&zram->stats.zero_pages);
		atomic64_dec(&zram->stats.same_pages);
	} else {
		zs_free(zram->meta->mem_pool, handle);
		atomic64_sub(size, &zram->stats.compr_data_size);
	}
	atomic64_dec(&zram->stats.pages_stored);
	group->handles[slot] = 0;
	group->sizes[slot] = 0;
}

static void zram_discard_slot(struct zram *zram, u32 index)
{
	struct zram_slot_group *group = zram_group(zram->meta, index);
	unsigned int slot = index % BITS_PER_LONG;

	bit_spin_lock(slot, &group->locks);
	zram_free_page(zram, group, slot);
	bit_spin_unlock(slot, &group->locks);
}

static int zram_decompress_page(struct zram *zram, char *mem, u32 index)
{
	int ret = 0;
	unsigned char *cmem;
	struct zram_meta *meta = zram->meta;
	struct zram_slot_group *group = zram_group(meta, index);
	unsigned int slot = index % BITS_PER_LONG;
	unsigned long handle;
	size_t size;

	bit_spin_lock(slot, &group->locks);
	handle = group->handles[slot];
	size = group->sizes[slot];

	if (!size || size == ZRAM_UNIFORM_SIZE) {
		bit_spin_unlock(slot, &group->locks);
		if (size && handle)
			zram_fill_page(mem, handle);
		else
			clear_page(mem);
		return 0;
	}

	cmem = zs_map_object(meta->mem_pool, handle, ZS_MM_RO);
	if (size == PAGE_SIZE)
		copy_page(mem, cmem);
	else
		ret = zcomp_decompress(zram->comp, cmem, size, mem);
	zs_unmap_object(meta->mem_pool, handle);
	bit_spin_unlock(slot, &group->locks);

	/* Should NEVER happen. Return bio error if it does. */
	if (unlikely(ret)) {
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);
		return ret;
	}

	return 0;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
			  u32 index, int offset)
{
	struct page *page = bvec->bv_page;
	unsigned char *user_mem, *uncmem = NULL;
	bool partial = is_partial_io(bvec);
	int ret;

	if (partial) {
		struct zram_meta *meta = zram->meta;
		struct zram_slot_group *group = zram_group(meta, index);
		unsigned int slot = index % BITS_PER_LONG;
		unsigned long element;
		size_t size;
		bool same;

		bit_spin_lock(slot, &group->locks);
		size = group->sizes[slot];
		same = !size || size == ZRAM_UNIFORM_SIZE;
		element = group->handles[slot];
		bit_spin_unlock(slot, &group->locks);
		if (same) {
			zram_copy_partial_same(bvec, offset, element);
			return 0;
		}
		uncmem = kmalloc(PAGE_SIZE, GFP_NOIO);
		if (!uncmem) {
			pr_info("Unable to allocate temp memory\n");
			return -ENOMEM;
		}
	}

	user_mem = kmap_atomic(page);
	ret = zram_decompress_page(zram, partial ? uncmem : user_mem, index);
	if (!ret) {
		if (partial)
			memcpy(user_mem + bvec->bv_offset, uncmem + offset,
			       bvec->bv_len);
		flush_dcache_page(page);
	}
	kunmap_atomic(user_mem);
	if (partial)
		kfree(uncmem);

	return ret;
}

static inline void update_used_max(struct zram *zram,
					const unsigned long pages)
{
	unsigned long old_max, cur_max;

	old_max = atomic_long_read(&zram->stats.max_used_pages);

	do {
		cur_max = old_max;
		if (pages > cur_max)
			old_max = atomic_long_cmpxchg(
				&zram->stats.max_used_pages, cur_max, pages);
	} while (old_max != cur_max);
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec, u32 index,
			   int offset)
{
	int ret = 0;
	size_t clen;
	unsigned long handle, element = 0;
	struct page *page;
	unsigned char *user_mem, *cmem, *src, *uncmem = NULL;
	struct zram_meta *meta = zram->meta;
	struct zram_slot_group *group = zram_group(meta, index);
	unsigned int slot = index % BITS_PER_LONG;
	struct zcomp_strm *zstrm = NULL;
	bool partial = is_partial_io(bvec);
	bool same;
	unsigned long alloced_pages, limit;

	page = bvec->bv_page;
	if (partial) {
		/*
		 * This is a partial IO. We need to read the full page
		 * before to write the changes.
		 */
		uncmem = kmalloc(PAGE_SIZE, GFP_NOIO);
		if (!uncmem) {
			ret = -ENOMEM;
			goto out;
		}
		ret = zram_decompress_page(zram, uncmem, index);
		if (ret)
			goto out;
		user_mem = kmap_atomic(page);
		memcpy(uncmem + offset, user_mem + bvec->bv_offset,
		       bvec->bv_len);
		kunmap_atomic(user_mem);
		same = page_same_filled(uncmem, &element);
	} else if (PageHighMem(page)) {
		user_mem = kmap_atomic(page);
		same = page_same_filled(user_mem, &element);
		kunmap_atomic(user_mem);
	} else {
		same = page_same_filled(page_address(page), &element);
	}

	if (same) {
		/* Free memory associated with this sector now. */
		bit_spin_lock(slot, &group->locks);
		zram_free_page(zram, group, slot);
		group->handles[slot] = element;
		group->sizes[slot] = ZRAM_UNIFORM_SIZE;
		if (!element)
			atomic64_inc(&zram->stats.zero_pages);
		atomic64_inc(&zram->stats.same_pages);
		atomic64_inc(&zram->stats.pages_stored);
		bit_spin_unlock(slot, &group->locks);
		ret = 0;
		goto out;
	}

	zstrm = zcomp_strm_find(zram->comp);
	src = partial ? uncmem : kmap_atomic(page);
	ret = zcomp_compress(zram->comp, zstrm, src, &clen,
			     meta->max_comp_size);
	if (!partial)
		kunmap_atomic(src);

	if (unlikely(ret)) {
		pr_err("Compression failed! err=%d\n", ret);
		goto out;
	}
	src = zstrm->buffer;
	if (unlikely(clen > meta->max_comp_size)) {
		clen = PAGE_SIZE;
		if (partial)
			src = uncmem;
	}

	handle = zs_malloc(meta->mem_pool, clen);
	if (!handle) {
		pr_info("Error allocating memory for compressed page: %u, size=%zu\n",
			index, clen);
		ret = -ENOMEM;
		goto out;
	}

	alloced_pages = zs_get_total_pages(meta->mem_pool);
	limit = ACCESS_ONCE(zram->limit_pages);
	if (limit && alloced_pages > limit) {
		zs_free(meta->mem_pool, handle);
		ret = -ENOMEM;
		goto out;
	}

	update_used_max(zram, alloced_pages);

	cmem = zs_map_object(meta->mem_pool, handle, ZS_MM_WO);

	if (clen == PAGE_SIZE && !partial) {
		src = kmap_atomic(page);
		copy_page(cmem, src);
		kunmap_atomic(src);
	} else {
		memcpy(cmem, src, clen);
	}

	zs_unmap_object(meta->mem_pool, handle);
	zcomp_strm_release(zram->comp, zstrm);
	zstrm = NULL;

	/*
	 * Free memory associated with this sector
	 * before overwriting unused sectors.
	 */
	bit_spin_lock(slot, &group->locks);
	zram_free_page(zram, group, slot);

	group->handles[slot] = handle;
	group->sizes[slot] = clen;
	atomic64_add(clen, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);
	bit_spin_unlock(slot, &group->locks);
out:
	if (zstrm)
		zcomp_strm_release(zram->comp, zstrm);
	if (partial)
		kfree(uncmem);
	return ret;
}

static int zram_bvec_rw(struct zram *zram, struct bio_vec *bvec, u32 index,
			int offset, int rw)
{
	int ret;

	if (rw == READ) {
		atomic64_inc(&zram->stats.num_reads);
		ret = zram_bvec_read(zram, bvec, index, offset);
	} else {
		atomic64_inc(&zram->stats.num_writes);
		ret = zram_bvec_write(zram, bvec, index, offset);
	}

	if (unlikely(ret)) {
		if (rw == READ)
			atomic64_inc(&zram->stats.failed_reads);
		else
			atomic64_inc(&zram->stats.failed_writes);
	}

	return ret;
}

/*
 * zram_bio_discard - handler on discard request
 * @index: physical block index in PAGE_SIZE units
 * @offset: byte offset within physical block
 */
static void zram_bio_discard(struct zram *zram, u32 index,
			     int offset, struct bio *bio)
{
	size_t n = bio->bi_size;
	unsigned long freed = 0;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */
	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			return;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_discard_slot(zram, index);
		freed++;
		index++;
		n -= PAGE_SIZE;
	}
	if (freed)
		atomic64_add(freed, &zram->stats.notify_free);
}

static void zram_reset_device(struct zram *zram)
{
	struct zram_meta *meta;
	struct zcomp *comp;
	u64 disksize;

	down_write(&zram->init_lock);

	zram->limit_pages = 0;

	if (!init_done(zram)) {
		up_write(&zram->init_lock);
		return;
	}

	meta = zram->meta;
	comp = zram->comp;
	disksize = zram->disksize;
	/*
	 * Refcount will go down to 0 eventually and r/w handler
	 * cannot handle further I/O so it will bail out by
	 * check zram_meta_get.
	 */
	zram_meta_put(zram);
	/*
	 * We want to free zram_meta in process context to avoid
	 * deadlock between reclaim path and any other locks.
	 */
	wait_event(zram->io_done, atomic_read(&zram->refcount) == 0);
	/* Order teardown after the last I/O drops its reference. */
	smp_mb();

	/* Reset stats */
	memset(&zram->stats, 0, sizeof(zram->stats));
	zram->meta = NULL;
	zram->comp = NULL;
	zram->disksize = 0;
	zram->max_comp_streams = 1;

	set_capacity(zram->disk, 0);
	part_stat_set_all(&zram->disk->part0, 0);

	up_write(&zram->init_lock);
	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(meta, disksize);
	zcomp_destroy(comp);
}

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize;
	struct zcomp *comp;
	struct zram_meta *meta;
	struct zram *zram = dev_to_zram(dev);
	int err;

	err = zram_parse_size(buf, &disksize);
	if (err)
		return err;
	if (!disksize)
		return -EINVAL;
	if (disksize > U64_MAX - (PAGE_SIZE - 1))
		return -EOVERFLOW;

	disksize = PAGE_ALIGN(disksize);
	if ((disksize >> PAGE_SHIFT) > U32_MAX ||
	    zram_table_groups(disksize >> PAGE_SHIFT) > SIZE_MAX / sizeof(struct zram_slot_group) ||
	    (disksize >> SECTOR_SHIFT) > (sector_t)-1)
		return -EOVERFLOW;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_unlock;
	}

	meta = zram_meta_alloc(zram->disk->first_minor, disksize);
	if (!meta) {
		err = -ENOMEM;
		goto out_unlock;
	}

	comp = zcomp_create(zram->compressor, zram->max_comp_streams);
	if (IS_ERR(comp)) {
		pr_info("Cannot initialise %s compressing backend\n",
				zram->compressor);
		err = PTR_ERR(comp);
		zram_meta_free(meta, disksize);
		goto out_unlock;
	}

	zram->meta = meta;
	zram->comp = comp;
	zram->disksize = disksize;
	/* Publish the initialized device before I/O can take a reference. */
	smp_wmb();
	atomic_set(&zram->refcount, 1);
	set_capacity(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);

	/*
	 * Revalidate disk out of the init_lock to avoid lockdep splat.
	 * It's okay because disk's capacity is protected by init_lock
	 * so that revalidate_disk always sees up-to-date capacity.
	 */
	revalidate_disk(zram->disk);

	return len;

out_unlock:
	up_write(&zram->init_lock);
	return err;
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned short do_reset;
	struct zram *zram;
	struct block_device *bdev;

	ret = kstrtou16(buf, 10, &do_reset);
	if (ret)
		return ret;
	if (!do_reset)
		return -EINVAL;

	zram = dev_to_zram(dev);
	bdev = bdget_disk(zram->disk, 0);

	if (!bdev)
		return -ENOMEM;

	mutex_lock(&bdev->bd_mutex);
	/* Do not reset an active device! */
	if (bdev->bd_openers) {
		ret = -EBUSY;
		goto out;
	}

	/* Failed writeback must leave the existing device intact. */
	ret = fsync_bdev(bdev);
	if (ret)
		goto out;
	zram_reset_device(zram);

	mutex_unlock(&bdev->bd_mutex);
	revalidate_disk(zram->disk);
	bdput(bdev);

	return len;

out:
	mutex_unlock(&bdev->bd_mutex);
	bdput(bdev);
	return ret;
}

static int __zram_make_request(struct zram *zram, struct bio *bio)
{
	unsigned long start_time;
	int i, offset, rw, ret = 0;
	u32 index;
	struct bio_vec *bvec;

	index = bio->bi_sector >> SECTORS_PER_PAGE_SHIFT;
	offset = (bio->bi_sector &
		  (SECTORS_PER_PAGE - 1)) << SECTOR_SHIFT;

	if (unlikely(bio->bi_rw & REQ_DISCARD)) {
		zram_bio_discard(zram, index, offset, bio);
		return 0;
	}

	rw = bio_data_dir(bio);
	start_time = jiffies;
	generic_start_io_acct(rw, bio->bi_size >> SECTOR_SHIFT,
			      &zram->disk->part0);
	bio_for_each_segment(bvec, bio, i) {
		int max_transfer_size = PAGE_SIZE - offset;

		if (bvec->bv_len > max_transfer_size) {
			/*
			 * zram_bvec_rw() can only make operation on a single
			 * zram page. Split the bio vector.
			 */
			struct bio_vec bv;

			bv.bv_page = bvec->bv_page;
			bv.bv_len = max_transfer_size;
			bv.bv_offset = bvec->bv_offset;

			ret = zram_bvec_rw(zram, &bv, index, offset, rw);
			if (ret < 0)
				break;

			bv.bv_len = bvec->bv_len - max_transfer_size;
			bv.bv_offset += max_transfer_size;
			ret = zram_bvec_rw(zram, &bv, index + 1, 0, rw);
		} else {
			ret = zram_bvec_rw(zram, bvec, index, offset, rw);
		}
		if (ret < 0)
			break;

		update_position(&index, &offset, bvec);
	}

	generic_end_io_acct(rw, &zram->disk->part0, start_time);
	return ret;
}

/*
 * Handler function for all zram I/O requests.
 */
static void zram_make_request(struct request_queue *queue, struct bio *bio)
{
	struct zram *zram = queue->queuedata;
	int ret;

	if (unlikely(!zram_meta_get(zram)))
		goto error;

	if (!valid_io_request(zram, bio->bi_sector,
					bio->bi_size)) {
		atomic64_inc(&zram->stats.invalid_io);
		goto put_zram;
	}

	ret = __zram_make_request(zram, bio);
	zram_meta_put(zram);
	if (ret)
		goto error;
	set_bit(BIO_UPTODATE, &bio->bi_flags);
	bio_endio(bio, 0);
	return;
put_zram:
	zram_meta_put(zram);
error:
	bio_io_error(bio);
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;
	if (unlikely(!zram_meta_get(zram)))
		return;
	if (unlikely(index >= (zram->disksize >> PAGE_SHIFT)))
		goto out;

	zram_discard_slot(zram, index);
	atomic64_inc(&zram->stats.notify_free);
out:
	zram_meta_put(zram);
}

static const struct block_device_operations zram_devops = {
	.swap_slot_free_notify = zram_slot_free_notify,
	.owner = THIS_MODULE
};

static DEVICE_ATTR_WO(compact);
static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RO(initstate);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_RO(orig_data_size);
static DEVICE_ATTR_RO(mem_used_total);
static DEVICE_ATTR_RW(mem_limit);
static DEVICE_ATTR_RW(mem_used_max);
static DEVICE_ATTR_RW(max_comp_streams);
static DEVICE_ATTR_RW(comp_algorithm);

static ssize_t io_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8llu\n",
			(u64)atomic64_read(&zram->stats.failed_reads),
			(u64)atomic64_read(&zram->stats.failed_writes),
			(u64)atomic64_read(&zram->stats.invalid_io),
			(u64)atomic64_read(&zram->stats.notify_free));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t mm_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	u64 orig_size, mem_used = 0;
	u64 max_used;
	ssize_t ret;

	down_read(&zram->init_lock);
	if (init_done(zram))
		mem_used = zs_get_total_pages(zram->meta->mem_pool);

	orig_size = atomic64_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8llu %8llu %8llu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			(u64)zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.zero_pages),
			(u64)atomic64_read(&zram->stats.num_migrated));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t same_pages_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE, "%llu\n",
		(u64)atomic64_read(&zram->stats.same_pages));
	up_read(&zram->init_lock);
	return ret;
}

static DEVICE_ATTR_RO(same_pages);
static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
ZRAM_ATTR_RO(num_reads);
ZRAM_ATTR_RO(num_writes);
ZRAM_ATTR_RO(failed_reads);
ZRAM_ATTR_RO(failed_writes);
ZRAM_ATTR_RO(invalid_io);
ZRAM_ATTR_RO(notify_free);
ZRAM_ATTR_RO(zero_pages);
ZRAM_ATTR_RO(compr_data_size);

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_num_reads.attr,
	&dev_attr_num_writes.attr,
	&dev_attr_failed_reads.attr,
	&dev_attr_failed_writes.attr,
	&dev_attr_compact.attr,
	&dev_attr_invalid_io.attr,
	&dev_attr_notify_free.attr,
	&dev_attr_zero_pages.attr,
	&dev_attr_same_pages.attr,
	&dev_attr_orig_data_size.attr,
	&dev_attr_compr_data_size.attr,
	&dev_attr_mem_used_total.attr,
	&dev_attr_mem_limit.attr,
	&dev_attr_mem_used_max.attr,
	&dev_attr_max_comp_streams.attr,
	&dev_attr_comp_algorithm.attr,
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
	NULL,
};

static struct attribute_group zram_disk_attr_group = {
	.attrs = zram_disk_attrs,
};

static int create_device(struct zram *zram, int device_id)
{
	struct request_queue *queue;
	int ret = -ENOMEM;

	init_rwsem(&zram->init_lock);
	init_waitqueue_head(&zram->io_done);
	strlcpy(zram->compressor, default_compressor, sizeof(zram->compressor));
	zram->meta = NULL;
	zram->max_comp_streams = 1;

	queue = blk_alloc_queue(GFP_KERNEL);
	if (!queue) {
		pr_err("Error allocating disk queue for device %d\n",
			device_id);
		goto out;
	}

	blk_queue_make_request(queue, zram_make_request);

	 /* gendisk structure */
	zram->disk = alloc_disk(1);
	if (!zram->disk) {
		pr_warn("Error allocating disk structure for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_queue;
	}

	zram->disk->major = zram_major;
	zram->disk->first_minor = device_id;
	zram->disk->fops = &zram_devops;
	zram->disk->queue = queue;
	zram->disk->queue->queuedata = zram;
	zram->disk->private_data = zram;
	snprintf(zram->disk->disk_name, 16, "zram%d", device_id);

	/* Actual capacity set using syfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);
	/* zram devices sort of resembles non-rotational disks */
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, zram->disk->queue);
	queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, zram->disk->queue);
	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(zram->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(zram->disk->queue,
					ZRAM_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(zram->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(zram->disk->queue, PAGE_SIZE);
	zram->disk->queue->limits.discard_granularity = PAGE_SIZE;
	zram->disk->queue->limits.max_discard_sectors = UINT_MAX;
	/*
	 * zram_bio_discard() will clear all logical blocks if logical block
	 * size is identical with physical block size(PAGE_SIZE). But if it is
	 * different, we will skip discarding some parts of logical blocks in
	 * the part of the request range which isn't aligned to physical block
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		zram->disk->queue->limits.discard_zeroes_data = 1;
	else
		zram->disk->queue->limits.discard_zeroes_data = 0;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, zram->disk->queue);

	add_disk(zram->disk);

	ret = sysfs_create_group(&disk_to_dev(zram->disk)->kobj,
				&zram_disk_attr_group);
	if (ret < 0) {
		pr_warn("Error creating sysfs group");
		goto out_free_disk;
	}
	return 0;

out_free_disk:
	del_gendisk(zram->disk);
	put_disk(zram->disk);
out_free_queue:
	blk_cleanup_queue(queue);
out:
	return ret;
}

static void destroy_devices(unsigned int nr)
{
	struct zram *zram;
	unsigned int i;

	for (i = 0; i < nr; i++) {
		zram = &zram_devices[i];
		/*
		 * Remove sysfs first, so no one will perform a disksize
		 * store while we destroy the devices
		 */
		sysfs_remove_group(&disk_to_dev(zram->disk)->kobj,
				&zram_disk_attr_group);

		del_gendisk(zram->disk);
		zram_reset_device(zram);
		blk_cleanup_queue(zram->disk->queue);
		put_disk(zram->disk);
	}

	kfree(zram_devices);
	unregister_blkdev(zram_major, "zram");
	pr_info("Destroyed %u device(s)\n", nr);
}

static int __init zram_init(void)
{
	int ret, dev_id;

	if (!num_devices || num_devices > max_num_devices) {
		pr_warn("Invalid value for num_devices: %u\n",
				num_devices);
		return -EINVAL;
	}

	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_warn("Unable to get major number\n");
		return -EBUSY;
	}

	/* Allocate the device array and initialize each one */
	zram_devices = kzalloc(num_devices * sizeof(struct zram), GFP_KERNEL);
	if (!zram_devices) {
		unregister_blkdev(zram_major, "zram");
		return -ENOMEM;
	}

	for (dev_id = 0; dev_id < num_devices; dev_id++) {
		ret = create_device(&zram_devices[dev_id], dev_id);
		if (ret)
			goto out_error;
	}

	pr_info("Created %u device(s)\n", num_devices);
#ifdef CONFIG_ZRAM_CINNAMON_COMPRESS
	cinnamon_proc_init();
#endif
	return 0;

out_error:
	destroy_devices(dev_id);
	return ret;
}

static void __exit zram_exit(void)
{
	destroy_devices(num_devices);
#ifdef CONFIG_ZRAM_CINNAMON_COMPRESS
	cinnamon_proc_exit();
#endif
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of zram devices");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_DESCRIPTION("Compressed RAM Block Device");
