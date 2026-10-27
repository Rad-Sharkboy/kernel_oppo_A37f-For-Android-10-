/*
 * zcomp_cinnamon.c - Cinnamon Compression Engine
 *
 * Optimizations:
 * 1. Branchless Zero Check: OR-based comparison to reduce pipeline stalls.
 * 2. Compiler Hints: likely/unlikely for better branch prediction.
 * 3. Prefetching: Load next blocks into cache to minimize latency.
 * 4. 4 Prev Blocks: Ring buffer for periodicity matching (10-15% more matches).
 * 5. Delta Compression: Detect blocks that differ in 1-2 bytes from previous blocks.
 * 6. 3-Bit Header: Encode specific match indices, header size 48 bytes.
 * 7. Local Counters: Batch atomic updates to reduce bus contention.
 * 8. Proc Monitoring: /proc/ccompress exposes stats, ratios, and savings.
 * 9. Zero-Run Encoding: Combine consecutive zero blocks into one run.
 * 10. Partial Match (16B): Encode when first half matches a previous block.
 * 11. Repeat‑Byte Run: Encode runs of identical bytes (any value) – useful for bitmaps.
 * 12. ARM64 NEON Fast Path: Accelerated zero check, block match, and diff counting using inline assembly.
 * 13. Batching NEON: Use kernel_neon_begin() once per page to reduce context‑switch overhead.
 * 14. Branch prediction hint for raw mode in decompression.
 * 15. Stride‑based Prediction: After a match at distance d, predict future matches at the same offset.
 *
 * Block size is tunable via module parameter `cinnamon_block_size` (default 32).
 * It must be a divisor of PAGE_SIZE (typically 16, 32, or 64).
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <asm/unaligned.h>
#include <crypto/cinnamon.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/ktime.h>
#include <linux/timex.h>        /* for get_cycles() */
#include <linux/sched.h>

#ifdef CONFIG_ARM64
#include <asm/neon.h>
#endif

#include "zcomp.h"
#include "zcomp_cinnamon.h"

/* Compression statistics */
atomic_t cinnamon_pages_compressed = ATOMIC_INIT(0);
atomic_t cinnamon_zero_blocks = ATOMIC_INIT(0);
atomic_t cinnamon_match_blocks = ATOMIC_INIT(0);
atomic_t cinnamon_delta_blocks = ATOMIC_INIT(0);
atomic_t cinnamon_raw_blocks = ATOMIC_INIT(0);
atomic_t cinnamon_partial_blocks = ATOMIC_INIT(0);
atomic_t cinnamon_fallback_pages = ATOMIC_INIT(0);        /* pages that fell back to raw */

atomic64_t cinnamon_bytes_in = ATOMIC64_INIT(0);
atomic64_t cinnamon_bytes_out = ATOMIC64_INIT(0);

/* Base block size (tunable) */
static int cinnamon_block_size = 32;
module_param(cinnamon_block_size, int, 0644);
MODULE_PARM_DESC(cinnamon_block_size,
    "Block size in bytes (must divide PAGE_SIZE, 16/32/64 recommended).");

/* Control NEON usage (ARM64 only) */
#ifdef CONFIG_ARM64
static bool use_neon = true;
module_param(use_neon, bool, 0644);
MODULE_PARM_DESC(use_neon, "Use NEON optimizations if available and safe");
#endif

#define SIG_THRESHOLD 32
#define MIN_BLOCK_SIZE 16
#define MAX_BLOCK_SIZE 64

/* Mode constants (4‑bit, values 0‑15) */
#define CINNAMON_MODE_RAW           0
#define CINNAMON_MODE_ZERO          1
#define CINNAMON_MODE_MATCH0        2
#define CINNAMON_MODE_MATCH1        3
#define CINNAMON_MODE_MATCH2        4
#define CINNAMON_MODE_MATCH3        5
#define CINNAMON_MODE_DELTA1        6   /* 1-byte difference */
#define CINNAMON_MODE_DELTA2        7   /* 2-byte difference */
#define CINNAMON_MODE_DELTA3        8   /* 3-byte difference */
#define CINNAMON_MODE_ZERO_RUN      9  /* zero run: followed by 1 byte length */
#define CINNAMON_MODE_MATCH16       10 /* first half match: ref + half block tail */
#define CINNAMON_MODE_REPEAT_BYTE_RUN 11 /* repeat‑byte run: value + length */
#define CINNAMON_MODE_UNUSED1        12 /* remove global match*/
#define CINNAMON_MODE_LONG_ZERO_RUN 13 /* long zero run: followed by 2 bytes length (LE) */
#define CINNAMON_MODE_UNUSED2        14
#define CINNAMON_MODE_UNUSED3        15

/* ========== Generic (C) versions – always available ========== */

/* Check if a block is all zero (generic) */
static inline int is_zero_block_generic(const u8 *block, int block_size)
{
	int i;
	const u64 *p = (const u64 *)block;
	int n = block_size / sizeof(u64);
	for (i = 0; i < n; i++) {
		if (get_unaligned_le64(&p[i]) != 0)
			return 0;
	}
	return 1;
}

/* Exact match of two blocks (generic) – using 64‑bit word comparison */
static inline int blocks_match_generic(const u8 *b1, const u8 *b2, int block_size)
{
	const u64 *p1 = (const u64 *)b1;
	const u64 *p2 = (const u64 *)b2;
	int n = block_size / sizeof(u64);
	int i;

	for (i = 0; i < n; i++) {
		if (get_unaligned_le64(&p1[i]) != get_unaligned_le64(&p2[i]))
			return 0;
	}
	return 1;
}

/* Count differing bytes between two blocks, early exit if >3 (generic) */
static inline int count_diff_bytes_generic(const u8 *b1, const u8 *b2, int block_size)
{
	int i, total = 0;
	const u64 *p1 = (const u64 *)b1;
	const u64 *p2 = (const u64 *)b2;
	int n = block_size / sizeof(u64);

	for (i = 0; i < n; i++) {
		u64 xor = get_unaligned_le64(&p1[i]) ^ get_unaligned_le64(&p2[i]);
		if (xor == 0)
			continue;

		/* Count non-zero bytes in this 8-byte chunk */
		total += (xor & 0xFF00000000000000ULL) ? 1 : 0;
		total += (xor & 0x00FF000000000000ULL) ? 1 : 0;
		total += (xor & 0x0000FF0000000000ULL) ? 1 : 0;
		total += (xor & 0x000000FF00000000ULL) ? 1 : 0;
		total += (xor & 0x00000000FF000000ULL) ? 1 : 0;
		total += (xor & 0x0000000000FF0000ULL) ? 1 : 0;
		total += (xor & 0x000000000000FF00ULL) ? 1 : 0;
		total += (xor & 0x00000000000000FFULL) ? 1 : 0;

		if (total > 3)
			return total;
	}
	return total;
}

/* Check if all bytes in a block equal the first byte (generic) */
static inline int is_repeat_byte_block_generic(const u8 *block, int block_size, u8 *value)
{
	u8 first = block[0];
	int i;
	for (i = 1; i < block_size; i++) {
		if (block[i] != first)
			return 0;
	}
	*value = first;
	return 1;
}

/* Compute 64‑bit XOR signature (generic) */
static inline u64 compute_block_signature_generic(const u8 *block, int block_size)
{
	const u64 *p = (const u64 *)block;
	int n = block_size / sizeof(u64);
	u64 sig = 0;
	int i;
	for (i = 0; i < n; i++)
		sig ^= p[i];
	return sig;
}

/* Find first differing byte offset (generic) */
static inline int find_first_diff_byte_generic(const u8 *b1, const u8 *b2, int block_size)
{
	int i;
	for (i = 0; i < block_size; i++) {
		if (b1[i] != b2[i])
			return i;
	}
	return 0;
}

/* ========== NEON implementations (ARM64 only) ========== */
#ifdef CONFIG_ARM64

/* NEON version: check if a block is all zero (block size multiple of 16) – with begin/end */
static inline int __attribute__((optimize("O0"))) neon_is_zero_block(const u8 *block, int block_size)
{
	int i;
	uint8_t max_val;

	kernel_neon_begin();
	asm volatile(
		"movi v0.16b, #0\n"                /* accumulator = 0 */
		: : : "v0"
	);
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v1.16b}, [%0]\n"         /* load 16 bytes */
			"orr v0.16b, v0.16b, v1.16b\n" /* OR into accumulator */
			: : "r"(block + i) : "v1", "v0"
		);
	}
	asm volatile(
		"umaxv b2, v0.16b\n"               /* find max byte in accumulator */
		"umov %w0, v2.b[0]\n"               /* move to general register */
		: "=r"(max_val) : : "v2"
	);
	kernel_neon_end();
	return max_val == 0;
}

/* NEON batched version – assumes NEON context already active */
static inline int __attribute__((optimize("O0"))) neon_is_zero_block_batched(const u8 *block, int block_size)
{
	int i;
	uint8_t max_val;
	/* No kernel_neon_begin/end here */
	asm volatile(
		"movi v0.16b, #0\n"
		: : : "v0"
	);
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v1.16b}, [%0]\n"
			"orr v0.16b, v0.16b, v1.16b\n"
			: : "r"(block + i) : "v1", "v0"
		);
	}
	asm volatile(
		"umaxv b2, v0.16b\n"
		"umov %w0, v2.b[0]\n"
		: "=r"(max_val) : : "v2"
	);
	return max_val == 0;
}

/* NEON version: exact match of two blocks – with begin/end */
static inline int __attribute__((optimize("O0"))) neon_blocks_match(const u8 *b1, const u8 *b2, int block_size)
{
	int i;
	uint8_t min_val;

	kernel_neon_begin();
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v0.16b}, [%1]\n"
			"ld1 {v1.16b}, [%2]\n"
			"cmeq v2.16b, v0.16b, v1.16b\n"
			"uminv b3, v2.16b\n"
			"umov %w0, v3.b[0]\n"
			: "=r"(min_val)
			: "r"(b1 + i), "r"(b2 + i)
			: "v0", "v1", "v2", "v3"
		);
		if (min_val != 0xFF) {
			kernel_neon_end();
			return 0;
		}
	}
	kernel_neon_end();
	return 1;
}

/* NEON batched version – assumes NEON context active */
static inline int __attribute__((optimize("O0"))) neon_blocks_match_batched(const u8 *b1, const u8 *b2, int block_size)
{
	int i;
	uint8_t min_val;
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v0.16b}, [%1]\n"
			"ld1 {v1.16b}, [%2]\n"
			"cmeq v2.16b, v0.16b, v1.16b\n"
			"uminv b3, v2.16b\n"
			"umov %w0, v3.b[0]\n"
			: "=r"(min_val)
			: "r"(b1 + i), "r"(b2 + i)
			: "v0", "v1", "v2", "v3"
		);
		if (min_val != 0xFF)
			return 0;
	}
	return 1;
}

/* NEON version: count differing bytes – with begin/end */
static inline int __attribute__((optimize("O0"))) neon_count_diff_bytes(const u8 *b1, const u8 *b2, int block_size)
{
	int i, total = 0;
	uint16_t sum;

	kernel_neon_begin();
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v0.16b}, [%1]\n"
			"ld1 {v1.16b}, [%2]\n"
			"eor v2.16b, v0.16b, v1.16b\n"
			"cmeq v3.16b, v2.16b, #0\n"    /* v3 = 0xFF where equal */
			"mvn v3.16b, v3.16b\n"         /* v3 = 0xFF where differ */
			"ushr v3.16b, v3.16b, 7\n"     /* v3 = 1 where differ, 0 elsewhere */
			"uaddlv h4, v3.16b\n"          /* sum across vector */
			"umov %w0, v4.h[0]\n"
			: "=r"(sum)
			: "r"(b1 + i), "r"(b2 + i)
			: "v0", "v1", "v2", "v3", "v4"
		);
		total += sum;
		if (total > 3) {
			kernel_neon_end();
			return total;
		}
	}
	kernel_neon_end();
	return total;
}

/* NEON batched version – assumes NEON context active */
static inline int __attribute__((optimize("O0"))) neon_count_diff_bytes_batched(const u8 *b1, const u8 *b2, int block_size)
{
	int i, total = 0;
	uint16_t sum;
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v0.16b}, [%1]\n"
			"ld1 {v1.16b}, [%2]\n"
			"eor v2.16b, v0.16b, v1.16b\n"
			"cmeq v3.16b, v2.16b, #0\n"
			"mvn v3.16b, v3.16b\n"
			"ushr v3.16b, v3.16b, 7\n"
			"uaddlv h4, v3.16b\n"
			"umov %w0, v4.h[0]\n"
			: "=r"(sum)
			: "r"(b1 + i), "r"(b2 + i)
			: "v0", "v1", "v2", "v3", "v4"
		);
		total += sum;
		if (total > 3)
			return total;
	}
	return total;
}

/* NEON: check if all bytes in a block equal the first byte – with begin/end */
static inline int __attribute__((optimize("O0"))) neon_is_repeat_byte_block(const u8 *block, int block_size, u8 *value)
{
	int i;
	uint8_t first = block[0];
	uint8_t all_equal;
	*value = first;

	kernel_neon_begin();
	/* Duplicate first byte into v0 */
	asm volatile(
		"dup v0.16b, %w0\n"
		:
		: "r"(first)
		: "v0"
	);
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v1.16b}, [%1]\n"
			"cmeq v2.16b, v1.16b, v0.16b\n"
			"uminv b3, v2.16b\n"
			"umov %w0, v3.b[0]\n"
			: "=r"(all_equal)
			: "r"(block + i)
			: "v1", "v2", "v3"
		);
		if (all_equal != 0xFF) {
			kernel_neon_end();
			return 0;
		}
	}
	kernel_neon_end();
	return 1;
}

/* NEON batched version – assumes NEON context active */
static inline int __attribute__((optimize("O0"))) neon_is_repeat_byte_block_batched(const u8 *block, int block_size, u8 *value)
{
	int i;
	uint8_t first = block[0];
	uint8_t all_equal;
	*value = first;

	asm volatile(
		"dup v0.16b, %w0\n"
		:
		: "r"(first)
		: "v0"
	);
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v1.16b}, [%1]\n"
			"cmeq v2.16b, v1.16b, v0.16b\n"
			"uminv b3, v2.16b\n"
			"umov %w0, v3.b[0]\n"
			: "=r"(all_equal)
			: "r"(block + i)
			: "v1", "v2", "v3"
		);
		if (all_equal != 0xFF)
			return 0;
	}
	return 1;
}

/* NEON: compute 64‑bit XOR signature – with begin/end */
static inline u64 __attribute__((optimize("O0"))) neon_compute_block_signature(const u8 *block, int block_size)
{
	u64 sig_lo, sig_hi;
	const u8 *ptr = block;
	int len = block_size;

	kernel_neon_begin();
	asm volatile(
		"movi v0.16b, #0\n"
		"1:\n"
		"subs %w[len], %w[len], #16\n"
		"ld1 {v1.2d}, [%[ptr]], #16\n"
		"eor v0.16b, v0.16b, v1.16b\n"
		"b.gt 1b\n"
		"umov %[sig_lo], v0.d[0]\n"
		"umov %[sig_hi], v0.d[1]\n"
		: [ptr] "+r"(ptr), [len] "+r"(len),
		  [sig_lo] "=r"(sig_lo), [sig_hi] "=r"(sig_hi)
		:
		: "v0", "v1", "cc"
	);
	kernel_neon_end();
	return sig_lo ^ sig_hi;
}

/* NEON batched version – assumes NEON context active */
static inline u64 __attribute__((optimize("O0"))) neon_compute_block_signature_batched(const u8 *block, int block_size)
{
	u64 sig_lo, sig_hi;
	const u8 *ptr = block;
	int len = block_size;

	asm volatile(
		"movi v0.16b, #0\n"
		"1:\n"
		"subs %w[len], %w[len], #16\n"
		"ld1 {v1.2d}, [%[ptr]], #16\n"
		"eor v0.16b, v0.16b, v1.16b\n"
		"b.gt 1b\n"
		"umov %[sig_lo], v0.d[0]\n"
		"umov %[sig_hi], v0.d[1]\n"
		: [ptr] "+r"(ptr), [len] "+r"(len),
		  [sig_lo] "=r"(sig_lo), [sig_hi] "=r"(sig_hi)
		:
		: "v0", "v1", "cc"
	);
	return sig_lo ^ sig_hi;
}

/* NEON: find first differing byte offset – with begin/end */
static inline int __attribute__((optimize("O0"))) neon_find_first_diff_byte(const u8 *b1, const u8 *b2, int block_size)
{
	int i, j;
	uint8_t cmp_result;

	kernel_neon_begin();
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v0.16b}, [%1]\n"
			"ld1 {v1.16b}, [%2]\n"
			"cmeq v2.16b, v0.16b, v1.16b\n"   /* v2 = 0xFF where equal */
			"uminv b3, v2.16b\n"               /* min across vector */
			"umov %w0, v3.b[0]\n"
			: "=r"(cmp_result)
			: "r"(b1 + i), "r"(b2 + i)
			: "v0", "v1", "v2", "v3"
		);
		if (cmp_result != 0xFF) {
			kernel_neon_end();
			/* Found a difference – scan bytewise */
			for (j = 0; j < 16; j++) {
				if (b1[i + j] != b2[i + j])
					return i + j;
			}
			/* Should never happen */
			break;
		}
	}
	kernel_neon_end();
	return 0; /* all equal */
}

/* NEON batched version – assumes NEON context active */
static inline int __attribute__((optimize("O0"))) neon_find_first_diff_byte_batched(const u8 *b1, const u8 *b2, int block_size)
{
	int i, j;
	uint8_t cmp_result;
	for (i = 0; i < block_size; i += 16) {
		asm volatile(
			"ld1 {v0.16b}, [%1]\n"
			"ld1 {v1.16b}, [%2]\n"
			"cmeq v2.16b, v0.16b, v1.16b\n"
			"uminv b3, v2.16b\n"
			"umov %w0, v3.b[0]\n"
			: "=r"(cmp_result)
			: "r"(b1 + i), "r"(b2 + i)
			: "v0", "v1", "v2", "v3"
		);
		if (cmp_result != 0xFF) {
			for (j = 0; j < 16; j++) {
				if (b1[i + j] != b2[i + j])
					return i + j;
			}
			break;
		}
	}
	return 0;
}

/* Macro to check if NEON can be used safely for a given pointer */
#ifdef CONFIG_ARM64
#include <asm/hwcap.h>

static inline bool cinnamon_cpu_has_neon(void)
{
    return elf_hwcap & HWCAP_ASIMD;
}

#define NEON_SAFE(ptr, size) \
    (use_neon && cinnamon_cpu_has_neon() && ((unsigned long)(ptr) & 0xF) == 0)

#else /* !CONFIG_ARM64 */
#define NEON_SAFE(ptr, size) (0)
#endif /* CONFIG_ARM64 */

/* Dispatch macros: use NEON if safe, otherwise generic */
#define is_zero_block(b, sz) \
    (NEON_SAFE(b, sz) ? neon_is_zero_block(b, sz) : is_zero_block_generic(b, sz))

#define blocks_match(b1, b2, sz) \
    (NEON_SAFE(b1, sz) && NEON_SAFE(b2, sz) ? neon_blocks_match(b1, b2, sz) : blocks_match_generic(b1, b2, sz))

#define count_diff_bytes(b1, b2, sz) \
    (NEON_SAFE(b1, sz) && NEON_SAFE(b2, sz) ? neon_count_diff_bytes(b1, b2, sz) : count_diff_bytes_generic(b1, b2, sz))

#define is_repeat_byte_block(b, sz, val) \
    (NEON_SAFE(b, sz) ? neon_is_repeat_byte_block(b, sz, val) : is_repeat_byte_block_generic(b, sz, val))

#define compute_block_signature(b, sz) \
    (NEON_SAFE(b, sz) ? neon_compute_block_signature(b, sz) : compute_block_signature_generic(b, sz))

#define find_first_diff_byte(b1, b2, sz) \
    (NEON_SAFE(b1, sz) && NEON_SAFE(b2, sz) ? neon_find_first_diff_byte(b1, b2, sz) : find_first_diff_byte_generic(b1, b2, sz))

#else /* !CONFIG_ARM64 */

/* No NEON support – use generic versions directly */
#define is_zero_block(b, sz) is_zero_block_generic(b, sz)
#define blocks_match(b1, b2, sz) blocks_match_generic(b1, b2, sz)
#define count_diff_bytes(b1, b2, sz) count_diff_bytes_generic(b1, b2, sz)
#define is_repeat_byte_block(b, sz, val) is_repeat_byte_block_generic(b, sz, val)
#define compute_block_signature(b, sz) compute_block_signature_generic(b, sz)
#define find_first_diff_byte(b1, b2, sz) find_first_diff_byte_generic(b1, b2, sz)

#endif /* CONFIG_ARM64 */

/* 4-bit nibble header helpers */
static inline void set_header_mode(unsigned char *header, int block_idx,
                                   unsigned char mode)
{
	int byte_idx = block_idx >> 1;
	int shift = (block_idx & 1) ? 0 : 4;
	unsigned char mask = 0xF << shift;
	unsigned char value = (mode & 0xF) << shift;
	header[byte_idx] = (header[byte_idx] & ~mask) | value;
}

static inline unsigned char get_header_mode(const unsigned char *header,
                                            int block_idx)
{
	int byte_idx = block_idx >> 1;
	int shift = (block_idx & 1) ? 0 : 4;
	return (header[byte_idx] >> shift) & 0xF;
}

/* Find two differing bytes (generic) */
static inline void find_two_diff_bytes(const u8 *b1, const u8 *b2, int block_size,
                                       int *off1, u8 *val1,
                                       int *off2, u8 *val2)
{
	int i, found = 0;
	for (i = 0; i < block_size && found < 2; i++) {
		if (b1[i] != b2[i]) {
			if (found == 0) {
				*off1 = i;
				*val1 = b1[i];
			} else {
				*off2 = i;
				*val2 = b1[i];
			}
			found++;
		}
	}
}

/* Find three differing bytes (generic) */
static inline void find_three_diff_bytes(const u8 *b1, const u8 *b2, int block_size,
                                         int *off1, u8 *val1,
                                         int *off2, u8 *val2,
                                         int *off3, u8 *val3)
{
	int i, found = 0;
	for (i = 0; i < block_size && found < 3; i++) {
		if (b1[i] != b2[i]) {
			if (found == 0) {
				*off1 = i;
				*val1 = b1[i];
			} else if (found == 1) {
				*off2 = i;
				*val2 = b1[i];
			} else {
				*off3 = i;
				*val3 = b1[i];
			}
			found++;
		}
	}
}

/* ---------- NEON‑batched compression routine ---------- */
#ifdef CONFIG_ARM64
static int cinnamon_compress_neon_batched(const unsigned char *src, unsigned char *dst,
                                          size_t *dst_len, void *private,
                                          int block_size, int blocks_per_page)
{
	/* C90: all declarations at the top */
	int i;
	unsigned char *header_ptr;
	unsigned char *dst_data;
	size_t out_len;
	u64 final_checksum;
	int header_size;
	int half;
	unsigned char mode;
	int p;
	u64 cur_sig;
	int best_delta, best_ref;
	int off1, off2, off3;
	u8 val1, val2, val3;
	u64 xor_sig;
	int diff;
	int prev_ref;
	int run;
	u8 repeat_value;
	u8 next_val;
	u64 repeat_sig;
	int order[4];
	int idx;
	int local_zero, local_match, local_delta, local_raw, local_partial;
	struct cinnamon_ctx *ctx = private;
	int stride_ref;
	int d;

	local_zero = local_match = local_delta = local_raw = local_partial = 0;

	header_size = blocks_per_page / 2;

	if (*dst_len < 1 + header_size + 8)
		goto fallback_raw;

	dst[0] = (unsigned char)block_size;
	header_ptr = dst + 1;
	dst_data = header_ptr + header_size;
	out_len = 1 + header_size;

	memset(header_ptr, 0, header_size);
	memset(ctx->prev_blocks, 0, sizeof(ctx->prev_blocks));
	ctx->prev_index = 0;
	ctx->pred_stride = 0;   /* initialize stride prediction */

	i = 0;
	while (i < blocks_per_page) {
		const u8 *current_src = src + (i * block_size);

		if (i + 2 < blocks_per_page)
			__builtin_prefetch(src + ((i + 2) * block_size), 0, 0);

		/* --- Stride‑based prediction --- */
		if (ctx->pred_stride > 0 && i >= ctx->pred_stride) {
			stride_ref = (ctx->prev_index + (4 - ctx->pred_stride)) % 4;
			if (neon_blocks_match_batched(current_src,
					(const u8 *)ctx->prev_blocks[stride_ref], block_size)) {
				mode = CINNAMON_MODE_MATCH0 + stride_ref;
				cur_sig = ctx->prev_sig[stride_ref];
				local_match++;
				goto set_mode;
			} else {
				ctx->pred_stride = 0;   /* prediction failed, reset */
			}
		}

		/* --- Zero‑run detection --- */
		if (likely(neon_is_zero_block_batched(current_src, block_size))) {
			run = 1;
			while (i + run < blocks_per_page && run < 65535) {
				if (i + run + 1 < blocks_per_page)
					__builtin_prefetch(src + ((i + run + 1) * block_size), 0, 0);
				if (!neon_is_zero_block_batched(src + ((i + run) * block_size), block_size))
					break;
				run++;
			}

			if (run <= 255) {
				if (unlikely(out_len + 1 > *dst_len - sizeof(u64)))
					goto fallback_raw;

				mode = CINNAMON_MODE_ZERO_RUN;
				set_header_mode(header_ptr, i, mode);
				dst_data[0] = (unsigned char)run;
				dst_data += 1;
				out_len += 1;
			} else {
				if (unlikely(out_len + 2 > *dst_len - sizeof(u64)))
					goto fallback_raw;

				mode = CINNAMON_MODE_LONG_ZERO_RUN;
				set_header_mode(header_ptr, i, mode);
				put_unaligned_le16((u16)run, dst_data);
				dst_data += 2;
				out_len += 2;
			}

			local_zero += run;

			for (p = 0; p < run && p < 4; p++) {
				memset(ctx->prev_blocks[ctx->prev_index], 0, block_size);
				ctx->prev_sig[ctx->prev_index] = 0;
				ctx->prev_index = (ctx->prev_index + 1) % 4;
			}
			ctx->prev_index = (ctx->prev_index + run - p) & 3;

			ctx->pred_stride = 0;   /* run breaks stride pattern */
			i += run;
			continue;
		}

		/* --- Repeat‑byte run detection --- */
		if (neon_is_repeat_byte_block_batched(current_src, block_size, &repeat_value)) {
			run = 1;
			while (i + run < blocks_per_page && run < 255) {
				if (i + run + 1 < blocks_per_page)
					__builtin_prefetch(src + ((i + run + 1) * block_size), 0, 0);
				if (!neon_is_repeat_byte_block_batched(src + ((i + run) * block_size), block_size, &next_val) ||
				    next_val != repeat_value)
					break;
				run++;
			}

			if (unlikely(out_len + 2 > *dst_len - sizeof(u64)))
				goto fallback_raw;

			mode = CINNAMON_MODE_REPEAT_BYTE_RUN;
			set_header_mode(header_ptr, i, mode);
			dst_data[0] = repeat_value;
			dst_data[1] = (unsigned char)run;
			dst_data += 2;
			out_len += 2;

			repeat_sig = neon_compute_block_signature_batched(current_src, block_size);
			for (p = 0; p < run && p < 4; p++) {
				memset(ctx->prev_blocks[ctx->prev_index], repeat_value, block_size);
				ctx->prev_sig[ctx->prev_index] = repeat_sig;
				ctx->prev_index = (ctx->prev_index + 1) % 4;
			}
			ctx->prev_index = (ctx->prev_index + run - p) & 3;

			ctx->pred_stride = 0;   /* run breaks stride pattern */
			i += run;
			continue;
		}

		/* --- Not a run, process single block --- */

		/* Quick match with previous block (i-1) */
		if (i > 0) {
			prev_ref = (ctx->prev_index + 3) & 3;
			if (neon_blocks_match_batched(current_src,
					(const u8 *)ctx->prev_blocks[prev_ref], block_size)) {
				mode = CINNAMON_MODE_MATCH0 + prev_ref;
				cur_sig = ctx->prev_sig[prev_ref];
				local_match++;
				ctx->pred_stride = 1;
				goto set_mode;
			}
		}

		/* Match with any of the 4 history blocks, most recent first */
		order[0] = (ctx->prev_index + 3) % 4;
		order[1] = (ctx->prev_index + 2) % 4;
		order[2] = (ctx->prev_index + 1) % 4;
		order[3] = ctx->prev_index;
		/* The quick check already rejected the most recent block. */
		for (idx = (i > 0); idx < 4; idx++) {
			p = order[idx];
			if (neon_blocks_match_batched(current_src,
					(const u8 *)ctx->prev_blocks[p], block_size)) {
				mode = CINNAMON_MODE_MATCH0 + p;
				cur_sig = ctx->prev_sig[p];
				local_match++;
				/* compute stride distance (1..4) */
				d = ((ctx->prev_index + 3 - p) & 3) + 1;
				ctx->pred_stride = d;
				goto set_mode;
			}
		}

		/* Try partial match (first half) */
		half = block_size / 2;
		for (p = 0; p < 4; p++) {
			if (memcmp(current_src, ctx->prev_blocks[p], half) == 0) {
				if (unlikely(out_len + 1 + half > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_MATCH16;
				dst_data[0] = p;
				memcpy(dst_data + 1, current_src + half, half);
				dst_data += 1 + half;
				out_len += 1 + half;
				local_partial++;
				cur_sig = neon_compute_block_signature_batched(current_src, block_size);
				/* partial match does not set stride */
				ctx->pred_stride = 0;
				goto set_mode;
			}
		}

		/* No match, compute signature for delta */
		cur_sig = neon_compute_block_signature_batched(current_src, block_size);

		/* Try delta compression */
		best_delta = 4;
		best_ref = -1;
		for (p = 0; p < 4; p++) {
			xor_sig = cur_sig ^ ctx->prev_sig[p];
			if (__builtin_popcountll(xor_sig) > SIG_THRESHOLD)
				continue;

			diff = neon_count_diff_bytes_batched(current_src,
					(const u8 *)ctx->prev_blocks[p], block_size);
			if (diff < best_delta) {
				best_delta = diff;
				best_ref = p;
				if (best_delta == 0) break;
				if (best_delta == 1) break;
			}
		}
		if (best_ref >= 0 && best_delta <= 3) {
			if (best_delta == 1) {
				if (unlikely(out_len + 3 > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_DELTA1;
				off1 = neon_find_first_diff_byte_batched(current_src,
						(const u8 *)ctx->prev_blocks[best_ref], block_size);
				val1 = current_src[off1];
				dst_data[0] = best_ref;
				dst_data[1] = off1;
				dst_data[2] = val1;
				dst_data += 3;
				out_len += 3;
			} else if (best_delta == 2) {
				if (unlikely(out_len + 5 > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_DELTA2;
				find_two_diff_bytes(current_src,
				                    (const u8 *)ctx->prev_blocks[best_ref], block_size,
				                    &off1, &val1, &off2, &val2);
				dst_data[0] = best_ref;
				dst_data[1] = off1;
				dst_data[2] = val1;
				dst_data[3] = off2;
				dst_data[4] = val2;
				dst_data += 5;
				out_len += 5;
			} else { /* delta == 3 */
				if (unlikely(out_len + 7 > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_DELTA3;
				find_three_diff_bytes(current_src,
				                      (const u8 *)ctx->prev_blocks[best_ref], block_size,
				                      &off1, &val1, &off2, &val2, &off3, &val3);
				dst_data[0] = best_ref;
				dst_data[1] = off1;
				dst_data[2] = val1;
				dst_data[3] = off2;
				dst_data[4] = val2;
				dst_data[5] = off3;
				dst_data[6] = val3;
				dst_data += 7;
				out_len += 7;
			}
			local_delta++;
			/* delta does not set stride */
			ctx->pred_stride = 0;
			goto set_mode;
		}

		/* Fallback: raw block */
		mode = CINNAMON_MODE_RAW;
		local_raw++;
		if (unlikely(out_len + block_size > *dst_len - sizeof(u64)))
			goto fallback_raw;
		memcpy(dst_data, current_src, block_size);
		dst_data += block_size;
		out_len += block_size;
		ctx->pred_stride = 0;   /* raw block resets stride */

set_mode:
		set_header_mode(header_ptr, i, mode);

		memcpy(ctx->prev_blocks[ctx->prev_index], current_src, block_size);
		ctx->prev_sig[ctx->prev_index] = cur_sig;
		ctx->prev_index = (ctx->prev_index + 1) % 4;

		i++;
	}

	final_checksum = cinnamon_checksum(dst, out_len);
	if (likely(out_len + sizeof(u64) <= *dst_len)) {
		put_unaligned_le64(final_checksum, dst_data);
		*dst_len = out_len + sizeof(u64);
	} else {
		goto fallback_raw;
	}

	atomic64_add(PAGE_SIZE, &cinnamon_bytes_in);
	atomic64_add(*dst_len, &cinnamon_bytes_out);

	/* Update statistics */
	atomic_inc(&cinnamon_pages_compressed);
	atomic_add(local_zero, &cinnamon_zero_blocks);
	atomic_add(local_match, &cinnamon_match_blocks);
	atomic_add(local_partial, &cinnamon_partial_blocks);
	atomic_add(local_delta, &cinnamon_delta_blocks);
	atomic_add(local_raw, &cinnamon_raw_blocks);

	return 0;

fallback_raw:
	memcpy(dst, src, PAGE_SIZE);
	*dst_len = PAGE_SIZE;
	atomic_inc(&cinnamon_fallback_pages);
	atomic64_add(PAGE_SIZE, &cinnamon_bytes_in);
	atomic64_add(PAGE_SIZE, &cinnamon_bytes_out);
	return 0;
}
#endif /* CONFIG_ARM64 */

/* ---- Main compression function ---- */
static int cinnamon_compress(const unsigned char *src, unsigned char *dst,
                             size_t *dst_len, void *private)
{
	/* C90: all declarations at the top */
	int i;
	unsigned char *header_ptr;
	unsigned char *dst_data;
	size_t out_len;
	u64 final_checksum;
	int block_size;
	int blocks_per_page;
	int header_size;
	int half;
	unsigned char mode;
	int p;
	u64 cur_sig;
	int best_delta, best_ref;
	int off1, off2, off3;
	u8 val1, val2, val3;
	u64 xor_sig;
	int diff;
	int prev_ref;
	int run;
	u8 repeat_value;
	u8 next_val;
	u64 repeat_sig;
	int order[4];
	int idx;
	int local_zero, local_match, local_delta, local_raw, local_partial;
	struct cinnamon_ctx *ctx = private;
	u64 start_ns, end_ns, delta_ns;
	cycles_t start_cycles, end_cycles, delta_cycles;
#ifdef CONFIG_ARM64
	bool use_neon_batch;
	int ret;
#endif
	int stride_ref;
	int d;

	local_zero = local_match = local_delta = local_raw = local_partial = 0;

	if (unlikely(!ctx))
		return -EINVAL;

	/* Use the tunable block size, ensuring it divides PAGE_SIZE */
	block_size = cinnamon_block_size;
	if (PAGE_SIZE % block_size != 0) {
		block_size = 32;
		if (PAGE_SIZE % block_size != 0)
			block_size = 16;
	}

	blocks_per_page = PAGE_SIZE / block_size;
	header_size = blocks_per_page / 2;

	/* OPTIMIZATION: Try NEON batched path if conditions are met */
#ifdef CONFIG_ARM64
	use_neon_batch = use_neon && cinnamon_cpu_has_neon() &&
			 ((unsigned long)src & 0xF) == 0 &&
			 ((unsigned long)dst & 0xF) == 0;
	if (use_neon_batch) {
		kernel_neon_begin();
		ret = cinnamon_compress_neon_batched(src, dst, dst_len, private,
		                                     block_size, blocks_per_page);
		kernel_neon_end();
		return ret;
	}
#endif

	/* Fallback to default (per‑block NEON or generic) */
	if (*dst_len < 1 + header_size + 8)
		goto fallback_raw;

	dst[0] = (unsigned char)block_size;
	header_ptr = dst + 1;
	dst_data = header_ptr + header_size;
	out_len = 1 + header_size;

	memset(header_ptr, 0, header_size);
	memset(ctx->prev_blocks, 0, sizeof(ctx->prev_blocks));
	ctx->prev_index = 0;
	ctx->pred_stride = 0;   /* initialize stride prediction */

	start_ns = sched_clock();
	start_cycles = get_cycles();

	i = 0;
	while (i < blocks_per_page) {
		const u8 *current_src = src + (i * block_size);

		if (i + 2 < blocks_per_page)
			__builtin_prefetch(src + ((i + 2) * block_size), 0, 0);

		/* --- Stride‑based prediction --- */
		if (ctx->pred_stride > 0 && i >= ctx->pred_stride) {
			stride_ref = (ctx->prev_index + (4 - ctx->pred_stride)) % 4;
			if (blocks_match(current_src,
					(const u8 *)ctx->prev_blocks[stride_ref], block_size)) {
				mode = CINNAMON_MODE_MATCH0 + stride_ref;
				cur_sig = ctx->prev_sig[stride_ref];
				local_match++;
				goto set_mode;
			} else {
				ctx->pred_stride = 0;
			}
		}

		/* --- Zero‑run detection --- */
		if (likely(is_zero_block(current_src, block_size))) {
			run = 1;
			while (i + run < blocks_per_page && run < 65535) {
				if (i + run + 1 < blocks_per_page)
					__builtin_prefetch(src + ((i + run + 1) * block_size), 0, 0);
				if (!is_zero_block(src + ((i + run) * block_size), block_size))
					break;
				run++;
			}

			if (run <= 255) {
				if (unlikely(out_len + 1 > *dst_len - sizeof(u64)))
					goto fallback_raw;

				mode = CINNAMON_MODE_ZERO_RUN;
				set_header_mode(header_ptr, i, mode);
				dst_data[0] = (unsigned char)run;
				dst_data += 1;
				out_len += 1;
			} else {
				if (unlikely(out_len + 2 > *dst_len - sizeof(u64)))
					goto fallback_raw;

				mode = CINNAMON_MODE_LONG_ZERO_RUN;
				set_header_mode(header_ptr, i, mode);
				put_unaligned_le16((u16)run, dst_data);
				dst_data += 2;
				out_len += 2;
			}

			local_zero += run;

			for (p = 0; p < run && p < 4; p++) {
				memset(ctx->prev_blocks[ctx->prev_index], 0, block_size);
				ctx->prev_sig[ctx->prev_index] = 0;
				ctx->prev_index = (ctx->prev_index + 1) % 4;
			}
			ctx->prev_index = (ctx->prev_index + run - p) & 3;

			ctx->pred_stride = 0;
			i += run;
			continue;
		}

		/* --- Repeat‑byte run detection --- */
		if (is_repeat_byte_block(current_src, block_size, &repeat_value)) {
			run = 1;
			while (i + run < blocks_per_page && run < 255) {
				if (i + run + 1 < blocks_per_page)
					__builtin_prefetch(src + ((i + run + 1) * block_size), 0, 0);
				if (!is_repeat_byte_block(src + ((i + run) * block_size), block_size, &next_val) ||
				    next_val != repeat_value)
					break;
				run++;
			}

			if (unlikely(out_len + 2 > *dst_len - sizeof(u64)))
				goto fallback_raw;

			mode = CINNAMON_MODE_REPEAT_BYTE_RUN;
			set_header_mode(header_ptr, i, mode);
			dst_data[0] = repeat_value;
			dst_data[1] = (unsigned char)run;
			dst_data += 2;
			out_len += 2;

			repeat_sig = compute_block_signature(current_src, block_size);
			for (p = 0; p < run && p < 4; p++) {
				memset(ctx->prev_blocks[ctx->prev_index], repeat_value, block_size);
				ctx->prev_sig[ctx->prev_index] = repeat_sig;
				ctx->prev_index = (ctx->prev_index + 1) % 4;
			}
			ctx->prev_index = (ctx->prev_index + run - p) & 3;

			ctx->pred_stride = 0;
			i += run;
			continue;
		}

		/* --- Not a run, process single block --- */

		/* Quick match with previous block (i-1) */
		if (i > 0) {
			prev_ref = (ctx->prev_index + 3) & 3;
			if (blocks_match(current_src, (const u8 *)ctx->prev_blocks[prev_ref], block_size)) {
				mode = CINNAMON_MODE_MATCH0 + prev_ref;
				cur_sig = ctx->prev_sig[prev_ref];
				local_match++;
				ctx->pred_stride = 1;
				goto set_mode;
			}
		}

		/* Match with any of the 4 history blocks, most recent first */
		order[0] = (ctx->prev_index + 3) % 4;
		order[1] = (ctx->prev_index + 2) % 4;
		order[2] = (ctx->prev_index + 1) % 4;
		order[3] = ctx->prev_index;
		/* The quick check already rejected the most recent block. */
		for (idx = (i > 0); idx < 4; idx++) {
			p = order[idx];
			if (blocks_match(current_src, (const u8 *)ctx->prev_blocks[p], block_size)) {
				mode = CINNAMON_MODE_MATCH0 + p;
				cur_sig = ctx->prev_sig[p];
				local_match++;
				d = ((ctx->prev_index + 3 - p) & 3) + 1;
				ctx->pred_stride = d;
				goto set_mode;
			}
		}

		/* Try partial match (first half) */
		half = block_size / 2;
		for (p = 0; p < 4; p++) {
			if (memcmp(current_src, ctx->prev_blocks[p], half) == 0) {
				if (unlikely(out_len + 1 + half > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_MATCH16;
				dst_data[0] = p;
				memcpy(dst_data + 1, current_src + half, half);
				dst_data += 1 + half;
				out_len += 1 + half;
				local_partial++;
				cur_sig = compute_block_signature(current_src, block_size);
				ctx->pred_stride = 0;
				goto set_mode;
			}
		}

		/* No match, compute signature for delta */
		cur_sig = compute_block_signature(current_src, block_size);

		/* Try delta compression */
		best_delta = 4;
		best_ref = -1;
		for (p = 0; p < 4; p++) {
			xor_sig = cur_sig ^ ctx->prev_sig[p];
			if (__builtin_popcountll(xor_sig) > SIG_THRESHOLD)
				continue;

			diff = count_diff_bytes(current_src, (const u8 *)ctx->prev_blocks[p], block_size);
			if (diff < best_delta) {
				best_delta = diff;
				best_ref = p;
				if (best_delta == 0) break;
				if (best_delta == 1) break;
			}
		}
		if (best_ref >= 0 && best_delta <= 3) {
			if (best_delta == 1) {
				if (unlikely(out_len + 3 > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_DELTA1;
				off1 = find_first_diff_byte(current_src,
						(const u8 *)ctx->prev_blocks[best_ref], block_size);
				val1 = current_src[off1];
				dst_data[0] = best_ref;
				dst_data[1] = off1;
				dst_data[2] = val1;
				dst_data += 3;
				out_len += 3;
			} else if (best_delta == 2) {
				if (unlikely(out_len + 5 > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_DELTA2;
				find_two_diff_bytes(current_src,
				                    (const u8 *)ctx->prev_blocks[best_ref], block_size,
				                    &off1, &val1, &off2, &val2);
				dst_data[0] = best_ref;
				dst_data[1] = off1;
				dst_data[2] = val1;
				dst_data[3] = off2;
				dst_data[4] = val2;
				dst_data += 5;
				out_len += 5;
			} else { /* delta == 3 */
				if (unlikely(out_len + 7 > *dst_len - sizeof(u64)))
					goto fallback_raw;
				mode = CINNAMON_MODE_DELTA3;
				find_three_diff_bytes(current_src,
				                      (const u8 *)ctx->prev_blocks[best_ref], block_size,
				                      &off1, &val1, &off2, &val2, &off3, &val3);
				dst_data[0] = best_ref;
				dst_data[1] = off1;
				dst_data[2] = val1;
				dst_data[3] = off2;
				dst_data[4] = val2;
				dst_data[5] = off3;
				dst_data[6] = val3;
				dst_data += 7;
				out_len += 7;
			}
			local_delta++;
			ctx->pred_stride = 0;
			goto set_mode;
		}

		/* Fallback: raw block */
		mode = CINNAMON_MODE_RAW;
		local_raw++;
		if (unlikely(out_len + block_size > *dst_len - sizeof(u64)))
			goto fallback_raw;
		memcpy(dst_data, current_src, block_size);
		dst_data += block_size;
		out_len += block_size;
		ctx->pred_stride = 0;

set_mode:
		set_header_mode(header_ptr, i, mode);

		memcpy(ctx->prev_blocks[ctx->prev_index], current_src, block_size);
		ctx->prev_sig[ctx->prev_index] = cur_sig;
		ctx->prev_index = (ctx->prev_index + 1) % 4;

		i++;
	}

	end_ns = sched_clock();
	end_cycles = get_cycles();
	delta_ns = end_ns - start_ns;
	delta_cycles = end_cycles - start_cycles;

	/* Batch update statistics */
	atomic_inc(&cinnamon_pages_compressed);
	atomic_add(local_zero, &cinnamon_zero_blocks);
	atomic_add(local_match, &cinnamon_match_blocks);
	atomic_add(local_partial, &cinnamon_partial_blocks);
	atomic_add(local_delta, &cinnamon_delta_blocks);
	atomic_add(local_raw, &cinnamon_raw_blocks);

	final_checksum = cinnamon_checksum(dst, out_len);
	if (likely(out_len + sizeof(u64) <= *dst_len)) {
		put_unaligned_le64(final_checksum, dst_data);
		*dst_len = out_len + sizeof(u64);
	} else {
		goto fallback_raw;
	}

	atomic64_add(PAGE_SIZE, &cinnamon_bytes_in);
	atomic64_add(*dst_len, &cinnamon_bytes_out);
	return 0;

fallback_raw:
	end_ns = sched_clock();
	end_cycles = get_cycles();
	delta_ns = end_ns - start_ns;
	delta_cycles = end_cycles - start_cycles;

	memcpy(dst, src, PAGE_SIZE);
	*dst_len = PAGE_SIZE;
	atomic_inc(&cinnamon_fallback_pages);
	atomic64_add(PAGE_SIZE, &cinnamon_bytes_in);
	atomic64_add(PAGE_SIZE, &cinnamon_bytes_out);
	return 0;
}

/* ---- Decompression ---- */
static int cinnamon_decompress(const unsigned char *src, size_t src_len,
                               unsigned char *dst, void *dev_private)
{
	/* C90: all declarations at the top */
	int i;
	const unsigned char *header_ptr;
	const unsigned char *src_data;
	size_t consumed_bytes;
	u64 stored_checksum, actual_checksum;
	int block_size;
	int blocks_per_page;
	int header_size;
	int half;
	u8 *current_dst;
	unsigned char mode;
	int ref, offset, off1, off2, off3;
	u8 value, val1, val2, val3;
	u8 *run_dst;
	int run;
	u8 repeat_value;
	u8 prev_blocks[4][MAX_BLOCK_SIZE] __aligned(16);
	int prev_index;
	int j;

	if (unlikely(src_len == PAGE_SIZE)) {
		memcpy(dst, src, PAGE_SIZE);
		return 0;
	}
	if (unlikely(src_len < 1 + sizeof(u64)))
		return -EINVAL;

	block_size = src[0];
	if (block_size < MIN_BLOCK_SIZE || block_size > MAX_BLOCK_SIZE ||
	    PAGE_SIZE % block_size != 0)
		return -EINVAL;

	blocks_per_page = PAGE_SIZE / block_size;
	header_size = blocks_per_page / 2;

	if (unlikely(src_len < 1 + header_size + sizeof(u64)))
		return -EINVAL;

	header_ptr = src + 1;
	src_data = header_ptr + header_size;
	consumed_bytes = 1 + header_size;

	memset(prev_blocks, 0, sizeof(prev_blocks));
	prev_index = 0;

	stored_checksum = get_unaligned_le64(src + src_len - sizeof(u64));
	actual_checksum = cinnamon_checksum(src, src_len - sizeof(u64));
	if (unlikely(stored_checksum != actual_checksum))
		return -EIO;

	for (i = 0; i < blocks_per_page; i++) {
		current_dst = dst + (i * block_size);
		mode = get_header_mode(header_ptr, i);
		half = block_size / 2;

		/* OPTIMIZATION: raw mode is unlikely when compression is effective */
		if (unlikely(mode == CINNAMON_MODE_RAW)) {
			if (unlikely(consumed_bytes + block_size > src_len - sizeof(u64)))
				return -EINVAL;
			memcpy(current_dst, src_data, block_size);
			src_data += block_size;
			consumed_bytes += block_size;
		} else if (mode == CINNAMON_MODE_ZERO) {
			memset(current_dst, 0, block_size);
		} else if (mode == CINNAMON_MODE_ZERO_RUN) {
			if (unlikely(consumed_bytes + 1 > src_len - sizeof(u64)))
				return -EINVAL;
			run = src_data[0];
			src_data += 1;
			consumed_bytes += 1;

			for (j = 0; j < run; j++) {
				run_dst = dst + ((i + j) * block_size);
				memset(run_dst, 0, block_size);
				memcpy(prev_blocks[prev_index], run_dst, block_size);
				prev_index = (prev_index + 1) % 4;
			}
			i += run - 1;
			continue;
		} else if (mode == CINNAMON_MODE_LONG_ZERO_RUN) {
			if (unlikely(consumed_bytes + 2 > src_len - sizeof(u64)))
				return -EINVAL;
			run = get_unaligned_le16(src_data);
			src_data += 2;
			consumed_bytes += 2;

			for (j = 0; j < run; j++) {
				run_dst = dst + ((i + j) * block_size);
				memset(run_dst, 0, block_size);
				memcpy(prev_blocks[prev_index], run_dst, block_size);
				prev_index = (prev_index + 1) % 4;
			}
			i += run - 1;
			continue;
		} else if (mode == CINNAMON_MODE_REPEAT_BYTE_RUN) {
			if (unlikely(consumed_bytes + 2 > src_len - sizeof(u64)))
				return -EINVAL;
			repeat_value = src_data[0];
			run = src_data[1];
			src_data += 2;
			consumed_bytes += 2;

			for (j = 0; j < run; j++) {
				run_dst = dst + ((i + j) * block_size);
				memset(run_dst, repeat_value, block_size);
				memcpy(prev_blocks[prev_index], run_dst, block_size);
				prev_index = (prev_index + 1) % 4;
			}
			i += run - 1;
			continue;
		} else if (mode >= CINNAMON_MODE_MATCH0 && mode <= CINNAMON_MODE_MATCH3) {
			ref = mode - CINNAMON_MODE_MATCH0;
			memcpy(current_dst, prev_blocks[ref], block_size);
		} else if (mode == CINNAMON_MODE_MATCH16) {
			if (unlikely(consumed_bytes + 1 + half > src_len - sizeof(u64)))
				return -EINVAL;
			ref = src_data[0];
			if (ref < 0 || ref >= 4) return -EINVAL;
			memcpy(current_dst, prev_blocks[ref], half);
			memcpy(current_dst + half, src_data + 1, half);
			src_data += 1 + half;
			consumed_bytes += 1 + half;
		} else if (mode == CINNAMON_MODE_DELTA1) {
			if (unlikely(consumed_bytes + 3 > src_len - sizeof(u64)))
				return -EINVAL;
			ref = src_data[0];
			offset = src_data[1];
			value = src_data[2];
			if (ref < 0 || ref >= 4) return -EINVAL;
			memcpy(current_dst, prev_blocks[ref], block_size);
			current_dst[offset] = value;
			src_data += 3;
			consumed_bytes += 3;
		} else if (mode == CINNAMON_MODE_DELTA2) {
			if (unlikely(consumed_bytes + 5 > src_len - sizeof(u64)))
				return -EINVAL;
			ref = src_data[0];
			off1 = src_data[1];
			val1 = src_data[2];
			off2 = src_data[3];
			val2 = src_data[4];
			if (ref < 0 || ref >= 4) return -EINVAL;
			memcpy(current_dst, prev_blocks[ref], block_size);
			current_dst[off1] = val1;
			current_dst[off2] = val2;
			src_data += 5;
			consumed_bytes += 5;
		} else if (mode == CINNAMON_MODE_DELTA3) {
			if (unlikely(consumed_bytes + 7 > src_len - sizeof(u64)))
				return -EINVAL;
			ref = src_data[0];
			off1 = src_data[1];
			val1 = src_data[2];
			off2 = src_data[3];
			val2 = src_data[4];
			off3 = src_data[5];
			val3 = src_data[6];
			if (ref < 0 || ref >= 4) return -EINVAL;
			memcpy(current_dst, prev_blocks[ref], block_size);
			current_dst[off1] = val1;
			current_dst[off2] = val2;
			current_dst[off3] = val3;
			src_data += 7;
			consumed_bytes += 7;
		} else {
			return -EINVAL;
		}

		/* Update intra‑page history */
		memcpy(prev_blocks[prev_index], current_dst, block_size);
		prev_index = (prev_index + 1) % 4;
	}

	if (consumed_bytes != src_len - sizeof(u64))
		return -EINVAL;

	return 0;
}

/* ---- create/destroy ---- */
static void *cinnamon_create(void *dev_private)
{
	struct cinnamon_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;
	return ctx;
}

static void cinnamon_destroy(void *private)
{
	kfree(private);
}

/* ---- Proc file interface ---- */
static int cinnamon_proc_show(struct seq_file *m, void *v)
{
	unsigned long total_p;
	unsigned long long bytes_in;
	unsigned long long bytes_out;
	unsigned long fallback;

	total_p = (unsigned long)atomic_read(&cinnamon_pages_compressed);
	bytes_in = (unsigned long long)atomic64_read(&cinnamon_bytes_in);
	bytes_out = (unsigned long long)atomic64_read(&cinnamon_bytes_out);
	fallback = (unsigned long)atomic_read(&cinnamon_fallback_pages);

	seq_printf(m, "block_size: %d\n", cinnamon_block_size);
	seq_printf(m, "pages_compressed: %lu\n", total_p);
	seq_printf(m, "zero_blocks: %d\n",
		   atomic_read(&cinnamon_zero_blocks));
	seq_printf(m, "match_blocks: %d\n",
		   atomic_read(&cinnamon_match_blocks));
	seq_printf(m, "partial_blocks: %d\n",
		   atomic_read(&cinnamon_partial_blocks));
	seq_printf(m, "delta_blocks: %d\n",
		   atomic_read(&cinnamon_delta_blocks));
	seq_printf(m, "raw_blocks: %d\n",
		   atomic_read(&cinnamon_raw_blocks));
	seq_printf(m, "fallback_pages: %lu\n", fallback);
	seq_printf(m, "bytes_in: %llu\n", bytes_in);
	seq_printf(m, "bytes_out: %llu\n", bytes_out);

	if (total_p > 0) {
		unsigned int ratio = (unsigned int)((bytes_out * 100) / bytes_in);
		unsigned long long saved = (bytes_in > bytes_out) ? (bytes_in - bytes_out) : 0;
		unsigned long long saved_mb = saved / (1024ULL * 1024ULL);

		seq_printf(m, "compression_ratio: %u%%\n", ratio);
		seq_printf(m, "ram_saved: %llu MB\n", saved_mb);
	} else {
		seq_printf(m, "compression_ratio: N/A\n");
		seq_printf(m, "ram_saved: N/A MB\n");
	}

	return 0;
}

static int cinnamon_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cinnamon_proc_show, NULL);
}

static const struct file_operations cinnamon_proc_fops = {
	.open = cinnamon_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void cinnamon_proc_init(void)
{
	proc_create("ccompress", 0444, NULL, &cinnamon_proc_fops);
}

void cinnamon_proc_exit(void)
{
	remove_proc_entry("ccompress", NULL);
}

/* Export the backend structure */
struct zcomp_backend zcomp_cinnamon = {
	.compress = cinnamon_compress,
	.decompress = cinnamon_decompress,
	.create = cinnamon_create,
	.destroy = cinnamon_destroy,
	.dev_create = NULL,      /* không dùng device context */
	.dev_destroy = NULL,
	.name = "cinnamon",
};