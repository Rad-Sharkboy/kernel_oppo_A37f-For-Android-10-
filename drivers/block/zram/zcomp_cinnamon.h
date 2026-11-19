/*
 * zcomp_cinnamon.h - Cinnamon Compression for zRAM
 */

#ifndef _ZCOMP_CINNAMON_H_
#define _ZCOMP_CINNAMON_H_

#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

/* Maximum block size we support (must fit in prev_blocks) */
#define MAX_BLOCK_SIZE 64

/* Signature threshold: max differing bits to consider delta */
#define SIG_THRESHOLD 32

/* Context structure for each compression thread */
struct cinnamon_ctx {
	u8 prev_blocks[4][MAX_BLOCK_SIZE] __aligned(16); /* căn chỉnh 16 byte cho NEON */
	u64 prev_sig[4];                      /* 64‑bit signatures of previous blocks */
	int prev_index;                        /* ring buffer index (0..3) */
	void *lzo_workmem;
	int pred_stride;                        /* stride for prediction (0 = none) */
};

/* Compression statistics (exported for proc) */
extern atomic_t cinnamon_pages_compressed;
extern atomic_t cinnamon_zero_blocks;
extern atomic_t cinnamon_match_blocks;
extern atomic_t cinnamon_partial_blocks;
extern atomic_t cinnamon_delta_blocks;
extern atomic_t cinnamon_raw_blocks;

extern atomic64_t cinnamon_bytes_in;
extern atomic64_t cinnamon_bytes_out;

/* Proc entry functions */
extern void cinnamon_proc_init(void);
extern void cinnamon_proc_exit(void);

/* Backend structure for zram */
extern struct zcomp_backend zcomp_cinnamon;

#endif /* _ZCOMP_CINNAMON_H_ */