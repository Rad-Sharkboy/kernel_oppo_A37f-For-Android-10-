/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_

#include <linux/mutex.h>

struct zcomp_strm {
	/* compression/decompression buffer */
	void *buffer;
	/*
	 * The private data of the compression stream, only compression
	 * stream backend can touch this (e.g. compression algorithm
	 * working memory)
	 */
	void *private;
	/* used in multi stream backend, protected by backend strm_lock */
	struct list_head list;
};

/* static compression backend */
struct zcomp_backend {
	int (*compress)(const unsigned char *src, unsigned char *dst,
			size_t *dst_len, size_t max_size, void *private);

	int (*decompress)(const unsigned char *src, size_t src_len,
			unsigned char *dst, void *dev_private);  /* thêm dev_private */

	void *(*create)(void *dev_private);    /* thêm tham số dev_private */
	void (*destroy)(void *private);

	void *(*dev_create)(void);              /* tạo ngữ cảnh thiết bị */
	void (*dev_destroy)(void *dev_private);

	const char *name;
};

/* dynamic per-device compression frontend */
struct zcomp {
	void *stream;
	struct zcomp_backend *backend;
	void *dev_private;                       /* ngữ cảnh thiết bị (dùng chung) */

	struct zcomp_strm *(*strm_find)(struct zcomp *comp);
	void (*strm_release)(struct zcomp *comp, struct zcomp_strm *zstrm);
	bool (*set_max_streams)(struct zcomp *comp, int num_strm);
	void (*destroy)(struct zcomp *comp);
};

ssize_t zcomp_available_show(const char *comp, char *buf);
const char *zcomp_algorithm_name(const char *compress);

struct zcomp *zcomp_create(const char *comp, int max_strm);
void zcomp_destroy(struct zcomp *comp);

struct zcomp_strm *zcomp_strm_find(struct zcomp *comp);
void zcomp_strm_release(struct zcomp *comp, struct zcomp_strm *zstrm);

int zcomp_compress(struct zcomp *comp, struct zcomp_strm *zstrm,
		const unsigned char *src, size_t *dst_len, size_t max_size);

int zcomp_decompress(struct zcomp *comp, const unsigned char *src,
		size_t src_len, unsigned char *dst);

bool zcomp_set_max_streams(struct zcomp *comp, int num_strm);
#endif /* _ZCOMP_H_ */
