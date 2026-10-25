/*
 * cinnamon-iosched.c - Cinnamon I/O Scheduler
 *
 * Optimizations:
 * 1. Zero-Alloc: Embeds timestamp directly in request pointer (no kmalloc).
 * 2. Anti-Starvation: Forces writes if delayed > 125ms to prevent FS locking.
 * 3. Smart Batching: Dynamic pressure reporting to zcomp backend.
 * 4. Dynamic Read-Ahead: Adjusts read-ahead based on I/O pressure.
 * 5. Hysteresis: Smooths pressure transitions with moving average.
 * 6. Per-Queue State: Ensures SMP safety with isolated state per device.
 * 7. Latency Monitoring: Tracks and exposes average request latency via sysfs.
 * 8. Moving Average: Smooth latency reporting with historical weighting.
 * 9. TRIM Priority: Dedicated queue for DISCARD commands (avoids fsync delays).
 * 10. Starvation Counter & Auto-Recovery: Detects severe write starvation (>150ms)
 *     and temporarily reduces read_batch to 4 for 2 seconds.
 * 11. Tiered Queuing: Critical (metadata/prio), Read/Write Foreground, Background.
 * 12. O(1) Merge: Limits merge scan to 8 entries per queue to avoid O(n) overhead.
 * 13. Latency-Aware Throttling: Adjusts batch sizes based on average latency.
 * 14. eMMC Sequential Write Boost: Detects sequential writes and prioritizes them.
 * 15. Background Starvation Prevention: Forces a background request after 16 foreground dispatches.
 * 16. Smooth Read-Ahead Transition: Gradual adjustment of ra_pages to avoid jitter.
 * 17. Priority Storage in Request: Combines timestamp and priority in elv.priv.
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/backing-dev.h>
#include <linux/ktime.h>
#include <linux/fs.h>
#include <linux/genhd.h>

/* Tunables */
#define CINNAMON_WRITE_EXPIRE      125           /* 125ms */
#define CINNAMON_READ_BATCH        8
#define CINNAMON_WRITE_BATCH        16
#define CINNAMON_PRESSURE_THRES     24
#define CINNAMON_HYSTERESIS_MS     500           /* 500ms hysteresis */
#define CINNAMON_HISTORY_SIZE       8
#define CINNAMON_TARGET_LATENCY_MS  50
#define CINNAMON_ADJUST_INTERVAL_MS 2000
#define WRITE_STARVE_THRESH_NS      150000000ULL  /* 150ms (stricter) */
#define MAX_MERGE_SCAN               8             /* O(1) merge scan limit */
#define FG_STARVE_LIMIT              16            /* Max foreground dispatches before forcing background */

#define EMMC_MAJOR                   179
#define EMMC_MAX_BATCH                16
#define EMMC_MIN_PRESSURE_THRES       12
#define EMMC_MAX_PRESSURE_THRES       24

/* Priority levels for tiered queuing */
enum io_priority {
	PRIO_CRITICAL = 0,   /* Metadata, high-priority */
	PRIO_TRIM,            /* Discard commands */
	PRIO_READ_FG,         /* Foreground reads */
	PRIO_WRITE_FG,        /* Foreground writes */
	PRIO_READ_BG,         /* Background reads */
	PRIO_WRITE_BG,        /* Background writes */
	PRIO_COUNT
};

/* Shift and mask for storing priority with timestamp */
#define CINNAMON_PRIO_SHIFT 8
#define CINNAMON_PRIO_MASK ((1 << CINNAMON_PRIO_SHIFT) - 1)

struct cinnamon_data {
	/* Tiered queues */
	struct list_head queues[PRIO_COUNT];
	unsigned int queue_counts[PRIO_COUNT];

	/* Batching state */
	unsigned int batch_count;
	unsigned int read_batch_count;   /* tracks reads in current cycle */
	unsigned int fg_dispatch_count;  /* consecutive foreground dispatches */

	/* Latency EWMA (ns), not a sum over num_requests. */
	atomic64_t total_latency_ns;
	atomic_t num_requests;

	/* Pressure & read-ahead */
	int last_ra_pressure;
	unsigned long last_pressure_change_jiffies;
	unsigned long last_manual_param_change_jiffies;

	/* Tunable parameters (per-queue) */
	int write_expire_ms;
	int read_batch;
	int write_batch;
	int pressure_thres;
	int hysteresis_ms;

	/* Read pattern history */
	sector_t read_history[CINNAMON_HISTORY_SIZE];
	int history_index;
	int history_count;
	int last_read_direction;
	sector_t predicted_sector;

	/* Write pattern history */
	sector_t write_history[CINNAMON_HISTORY_SIZE];
	int write_history_index;
	int write_history_count;
	int last_write_direction;
	sector_t predicted_write_sector;

	/* Adaptive tuning */
	unsigned long last_adjust_jiffies;
	int target_latency_ms;
	int adjust_interval_ms;

	/* Saved parameters for idle mode */
	int saved_read_batch;
	int saved_write_batch;
	int saved_write_expire_ms;

	struct request_queue *q;   /* for ra_pages access */
	int dispatch_seq;
	int empty_dispatch_count;
	bool emmc_mode;

	/* Starvation auto-recovery */
	unsigned int write_starved_count;
	unsigned long last_starved_jiffies;
	unsigned long starvation_recovery_until;
	int saved_read_batch_for_starve;

	/* Sequential write detection boost */
	unsigned int seq_write_count;
	sector_t last_write_sector;
};

/* Global pressure indicator */
atomic_t cinnamon_io_pressure = ATOMIC_INIT(0);
EXPORT_SYMBOL(cinnamon_io_pressure);

/* Helpers for storing/retrieving 64-bit timestamps and priority in elv.priv[] */
static inline void set_req_time_and_prio(struct request *rq, u64 time_ns, int prio)
{
	u64 val = (time_ns << CINNAMON_PRIO_SHIFT) | (prio & CINNAMON_PRIO_MASK);
#if BITS_PER_LONG == 64
	rq->elv.priv[0] = (void *)val;
#else
	rq->elv.priv[0] = (void *)(unsigned long)(val & 0xFFFFFFFF);
	rq->elv.priv[1] = (void *)(unsigned long)(val >> 32);
#endif
}

static inline u64 get_req_time(struct request *rq)
{
	u64 val;
#if BITS_PER_LONG == 64
	val = (u64)(unsigned long)rq->elv.priv[0];
#else
	u64 low = (u64)(unsigned long)rq->elv.priv[0];
	u64 high = (u64)(unsigned long)rq->elv.priv[1];
	val = (high << 32) | low;
#endif
	return val >> CINNAMON_PRIO_SHIFT;
}

static inline int get_req_prio(struct request *rq)
{
	u64 val;
#if BITS_PER_LONG == 64
	val = (u64)(unsigned long)rq->elv.priv[0];
#else
	u64 low = (u64)(unsigned long)rq->elv.priv[0];
	u64 high = (u64)(unsigned long)rq->elv.priv[1];
	val = (high << 32) | low;
#endif
	return val & CINNAMON_PRIO_MASK;
}

/* Clamp tunables for eMMC */
static void cinnamon_clamp_for_emmc(struct cinnamon_data *cd)
{
	if (!cd->emmc_mode)
		return;
	cd->read_batch = min(cd->read_batch, EMMC_MAX_BATCH);
	cd->write_batch = min(cd->write_batch, EMMC_MAX_BATCH);
	cd->pressure_thres = clamp(cd->pressure_thres,
				    EMMC_MIN_PRESSURE_THRES,
				    EMMC_MAX_PRESSURE_THRES);
}

/* Update read-ahead based on global pressure with smoothing */
static void cinnamon_update_ra(struct cinnamon_data *cd)
{
	int curr_press = atomic_read(&cinnamon_io_pressure);
	if (curr_press != cd->last_ra_pressure) {
		unsigned int ra_kb;
		unsigned long target_ra_pages;
		unsigned long current_ra = cd->q->backing_dev_info.ra_pages;

		switch (curr_press) {
		case 3:
			ra_kb = 128;
			break;
		case 2:
			ra_kb = 512;
			break;
		case 1:
			ra_kb = 1024;
			break;
		default:
			ra_kb = 2048;
			break;
		}
		target_ra_pages = ra_kb >> (PAGE_SHIFT - 10);

		/* Smooth transition: adjust by at most 16 pages per step */
		if (current_ra < target_ra_pages)
			current_ra = min(current_ra + 16, target_ra_pages);
		else if (current_ra > target_ra_pages)
			current_ra = max(current_ra - 16, target_ra_pages);

		cd->q->backing_dev_info.ra_pages = current_ra;
		cd->last_ra_pressure = curr_press;
	}
}

/* Set global pressure with hysteresis and idle mode */
static void cinnamon_set_pressure(struct cinnamon_data *cd, int new_pressure)
{
	int old_pressure, curr_pressure;
	int retries = 0;

	do {
		curr_pressure = atomic_read(&cinnamon_io_pressure);
		if (new_pressure == curr_pressure)
			return;

		/* Always allow dropping to idle (0) */
		if (new_pressure == 0) {
			old_pressure = atomic_cmpxchg(&cinnamon_io_pressure,
						       curr_pressure, 0);
			if (old_pressure == curr_pressure) {
				cd->last_pressure_change_jiffies = jiffies;

				/* Entering idle: save and halve parameters */
				cd->saved_read_batch = cd->read_batch;
				cd->saved_write_batch = cd->write_batch;
				cd->saved_write_expire_ms = cd->write_expire_ms;
				cd->read_batch = max(cd->read_batch / 2, 8);
				cd->write_batch = max(cd->write_batch / 2, 8);
				cd->write_expire_ms = min(cd->write_expire_ms + 100, 300);
				cinnamon_clamp_for_emmc(cd);
				cinnamon_update_ra(cd);
			}
			return;
		}

		/* Hysteresis check for non-idle transitions */
		if (!time_after(jiffies, cd->last_pressure_change_jiffies +
				msecs_to_jiffies(cd->hysteresis_ms)))
			return;

		/* Only allow increasing pressure or from 0 to non-zero */
		if (curr_pressure == 0 || new_pressure > curr_pressure) {
			old_pressure = atomic_cmpxchg(&cinnamon_io_pressure,
						       curr_pressure, new_pressure);
			if (old_pressure == curr_pressure) {
				cd->last_pressure_change_jiffies = jiffies;

				/* Leaving idle: restore saved */
				if (curr_pressure == 0) {
					cd->read_batch = cd->saved_read_batch;
					cd->write_batch = cd->saved_write_batch;
					cd->write_expire_ms = cd->saved_write_expire_ms;
				}
				cinnamon_clamp_for_emmc(cd);
				cinnamon_update_ra(cd);
				return;
			}
		} else {
			return;
		}
	} while (++retries < 10);
}

/* Update read history with confidence-based prediction */
static void cinnamon_update_read_history(struct cinnamon_data *cd, sector_t sector)
{
	cd->read_history[cd->history_index] = sector;
	cd->history_index = (cd->history_index + 1) % CINNAMON_HISTORY_SIZE;
	if (cd->history_count < CINNAMON_HISTORY_SIZE)
		cd->history_count++;

	if (cd->history_count >= 3) {
		int i, consistent = 0, total_diffs = 0;
		sector_t avg_diff = 0, min_diff = 0, max_diff = 0;
		int idx = (cd->history_index - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
		int confidence;

		for (i = 1; i < min(5, cd->history_count); i++) {
			int cur = (cd->history_index - i + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			int prev = (cur - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			sector_t diff = cd->read_history[cur] - cd->read_history[prev];

			if (i == 1) {
				min_diff = max_diff = diff;
				avg_diff = diff;
			} else {
				min_diff = min(min_diff, diff);
				max_diff = max(max_diff, diff);
				avg_diff = (avg_diff * (i-1) + diff) / i;
			}
			total_diffs++;
			if (abs(diff - avg_diff) < 1024)
				consistent++;
		}

		confidence = consistent * 100 / total_diffs;
		if (confidence >= 80 && abs(max_diff - min_diff) < 2048 &&
		    avg_diff != 0 && abs(avg_diff) < 1048576) {
			cd->predicted_sector = cd->read_history[idx] + avg_diff;
			cd->last_read_direction = avg_diff > 0 ? 1 : -1;
		} else {
			cd->predicted_sector = 0;
			cd->last_read_direction = 0;
		}
	} else {
		cd->predicted_sector = 0;
		cd->last_read_direction = 0;
	}
}

/* Update write history similarly */
static void cinnamon_update_write_history(struct cinnamon_data *cd, sector_t sector)
{
	cd->write_history[cd->write_history_index] = sector;
	cd->write_history_index = (cd->write_history_index + 1) % CINNAMON_HISTORY_SIZE;
	if (cd->write_history_count < CINNAMON_HISTORY_SIZE)
		cd->write_history_count++;

	if (cd->write_history_count >= 3) {
		int i, consistent = 0, total_diffs = 0;
		sector_t avg_diff = 0, min_diff = 0, max_diff = 0;
		int idx = (cd->write_history_index - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
		int confidence;

		for (i = 1; i < min(5, cd->write_history_count); i++) {
			int cur = (cd->write_history_index - i + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			int prev = (cur - 1 + CINNAMON_HISTORY_SIZE) % CINNAMON_HISTORY_SIZE;
			sector_t diff = cd->write_history[cur] - cd->write_history[prev];

			if (i == 1) {
				min_diff = max_diff = diff;
				avg_diff = diff;
			} else {
				min_diff = min(min_diff, diff);
				max_diff = max(max_diff, diff);
				avg_diff = (avg_diff * (i-1) + diff) / i;
			}
			total_diffs++;
			if (abs(diff - avg_diff) < 1024)
				consistent++;
		}

		confidence = consistent * 100 / total_diffs;
		if (confidence >= 80 && abs(max_diff - min_diff) < 2048 &&
		    avg_diff != 0 && abs(avg_diff) < 1048576) {
			cd->predicted_write_sector = cd->write_history[idx] + avg_diff;
			cd->last_write_direction = avg_diff > 0 ? 1 : -1;
		} else {
			cd->predicted_write_sector = 0;
			cd->last_write_direction = 0;
		}
	} else {
		cd->predicted_write_sector = 0;
		cd->last_write_direction = 0;
	}

	/* Sequential write detection for eMMC boost */
	if (cd->last_write_direction != 0 && cd->last_write_sector != 0) {
		if (sector == cd->last_write_sector + 1)
			cd->seq_write_count++;
		else
			cd->seq_write_count = 0;
	}
	cd->last_write_sector = sector;
}

/* Merge function with O(1) scan limit */
static int cinnamon_merge(struct request_queue *q, struct request **req,
			  struct bio *bio)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int dir = bio_data_dir(bio);
	sector_t sector = bio->bi_sector;
	sector_t last_sector = sector + bio_sectors(bio);
	struct list_head *queue_head;
	struct request *rq_iter;
	int scanned = 0;

	if (dir == READ) {
		if (!list_empty(&cd->queues[PRIO_CRITICAL]))
			queue_head = &cd->queues[PRIO_CRITICAL];
		else if (!list_empty(&cd->queues[PRIO_READ_FG]))
			queue_head = &cd->queues[PRIO_READ_FG];
		else if (!list_empty(&cd->queues[PRIO_READ_BG]))
			queue_head = &cd->queues[PRIO_READ_BG];
		else
			return ELEVATOR_NO_MERGE;
	} else {
		if (!list_empty(&cd->queues[PRIO_TRIM]))
			queue_head = &cd->queues[PRIO_TRIM];
		else if (!list_empty(&cd->queues[PRIO_WRITE_FG]))
			queue_head = &cd->queues[PRIO_WRITE_FG];
		else if (!list_empty(&cd->queues[PRIO_WRITE_BG]))
			queue_head = &cd->queues[PRIO_WRITE_BG];
		else
			return ELEVATOR_NO_MERGE;
	}

	list_for_each_entry(rq_iter, queue_head, queuelist) {
		sector_t rq_sector = blk_rq_pos(rq_iter);
		sector_t rq_last_sector = rq_sector + blk_rq_sectors(rq_iter);

		if (sector == rq_last_sector) {
			*req = rq_iter;
			return ELEVATOR_BACK_MERGE;
		}
		if (last_sector == rq_sector) {
			*req = rq_iter;
			return ELEVATOR_FRONT_MERGE;
		}
		if (++scanned >= MAX_MERGE_SCAN)
			break;
	}

	return ELEVATOR_NO_MERGE;
}

/* Handle merged requests - now correctly decrement queue count */
static void cinnamon_merged_requests(struct request_queue *q, struct request *rq,
				      struct request *next)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio = get_req_prio(next);

	if (prio >= 0 && prio < PRIO_COUNT) {
		if (cd->queue_counts[prio] > 0)
			cd->queue_counts[prio]--;
	}
	list_del_init(&next->queuelist);
}

/* Add request to appropriate tiered queue */
static void cinnamon_add_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio;

	if (rq->cmd_flags & REQ_DISCARD) {
		prio = PRIO_TRIM;
	} else if (rq->cmd_flags & (REQ_META | REQ_PRIO)) {
		prio = PRIO_CRITICAL;
	} else {
		int dir = rq_data_dir(rq);
		if (rq->cmd_flags & REQ_SYNC) {
			prio = (dir == READ) ? PRIO_READ_FG : PRIO_WRITE_FG;
		} else {
			prio = (dir == READ) ? PRIO_READ_BG : PRIO_WRITE_BG;
		}
	}

	set_req_time_and_prio(rq, ktime_to_ns(ktime_get()), prio);

	/* Sync requests go to head for faster service */
	if (rq->cmd_flags & REQ_SYNC)
		list_add(&rq->queuelist, &cd->queues[prio]);
	else
		list_add_tail(&rq->queuelist, &cd->queues[prio]);

	cd->queue_counts[prio]++;
}

/* Update latency moving average */
static void cinnamon_update_latency(struct cinnamon_data *cd, u64 latency_ns)
{
	u64 old_avg = atomic64_read(&cd->total_latency_ns);
	int old_count = atomic_read(&cd->num_requests);

	if (old_count > 0) {
		u64 new_avg = (old_avg * 7 + latency_ns * 3) / 10;
		atomic64_set(&cd->total_latency_ns, new_avg);
	} else {
		atomic64_set(&cd->total_latency_ns, latency_ns);
	}

	if (old_count < 100)
		atomic_set(&cd->num_requests, old_count + 1);
	else
		atomic_set(&cd->num_requests, old_count / 2);
}

/* Adaptive parameter tuning based on latency and queue depth */
static void cinnamon_adjust_params(struct cinnamon_data *cd)
{
	unsigned long now = jiffies;
	u64 total_ns;
	int num, avg_latency_ms;
	int queue_depth = 0;
	int i;
	int read_cnt, write_cnt, total_io;
	int read_ratio;

	if (time_before(now, cd->last_manual_param_change_jiffies +
			msecs_to_jiffies(5000)))
		return;

	if (time_before(now, cd->last_adjust_jiffies +
			msecs_to_jiffies(cd->adjust_interval_ms)))
		return;

	cd->last_adjust_jiffies = now;

	total_ns = atomic64_read(&cd->total_latency_ns);
	num = atomic_read(&cd->num_requests);
	if (num == 0)
		return;
	avg_latency_ms = div64_u64(total_ns, 1000000);

	for (i = 0; i < PRIO_COUNT; i++)
		queue_depth += cd->queue_counts[i];

	if (avg_latency_ms > cd->target_latency_ms * 2) {
		cd->read_batch = max(cd->read_batch / 2, 4);
		cd->write_batch = max(cd->write_batch / 2, 4);
		cd->write_expire_ms = max(cd->write_expire_ms / 2, 10);
		cd->pressure_thres = max(cd->pressure_thres / 2, 8);
	} else if (avg_latency_ms < cd->target_latency_ms / 2) {
		cd->read_batch = min(cd->read_batch * 2, 32);
		cd->write_batch = min(cd->write_batch * 2, 32);
		cd->write_expire_ms = min(cd->write_expire_ms * 2, 500);
		cd->pressure_thres = min(cd->pressure_thres * 2, 48);
	}

	read_cnt = cd->queue_counts[PRIO_READ_FG] +
		  cd->queue_counts[PRIO_READ_BG] +
		  cd->queue_counts[PRIO_CRITICAL];
	write_cnt = cd->queue_counts[PRIO_WRITE_FG] +
		   cd->queue_counts[PRIO_WRITE_BG];
	total_io = read_cnt + write_cnt;

	if (total_io > 0) {
		read_ratio = read_cnt * 100 / total_io;
		if (read_ratio > 70)
			cd->read_batch = min(cd->read_batch * 2, 32);
		else if (read_ratio < 30)
			cd->write_batch = min(cd->write_batch * 2, 32);
	}

	if (queue_depth > cd->pressure_thres * 2) {
		cd->read_batch = max(cd->read_batch / 2, 4);
		cd->write_batch = max(cd->write_batch / 2, 4);
	}

	cd->saved_read_batch = cd->read_batch;
	cd->saved_write_batch = cd->write_batch;
	cd->saved_write_expire_ms = cd->write_expire_ms;

	cinnamon_clamp_for_emmc(cd);
}

/* Dispatch function with tiered priority, starvation avoidance, and background anti-starvation */
static int cinnamon_dispatch(struct request_queue *q, int force)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	struct request *rq = NULL;
	u64 now_ns = ktime_to_ns(ktime_get());
	int target_pressure = 0;
	int pressure = atomic_read(&cinnamon_io_pressure);
	unsigned long write_expire_ns = cd->write_expire_ms * 1000000ULL;
	unsigned long read_batch = cd->read_batch;
	int prio, i;
	int total_pending;

	/* Starvation recovery check */
	if (cd->starvation_recovery_until &&
	    time_after(jiffies, cd->starvation_recovery_until)) {
		cd->read_batch = cd->saved_read_batch_for_starve;
		cd->starvation_recovery_until = 0;
		cinnamon_clamp_for_emmc(cd);
	}

	/* Dynamic expiration based on pressure and queue depth */
	if (unlikely(pressure == 3))
		write_expire_ns = 50 * 1000000ULL;
	else if (pressure <= 1)
		write_expire_ns = 250 * 1000000ULL;

	if (cd->queue_counts[PRIO_WRITE_FG] + cd->queue_counts[PRIO_WRITE_BG] > 16)
		write_expire_ns = 150 * 1000000ULL;

	/* Anti-starvation for background: force a background request after too many foreground */
	if (cd->fg_dispatch_count >= FG_STARVE_LIMIT) {
		if (!list_empty(&cd->queues[PRIO_READ_BG])) {
			rq = list_first_entry(&cd->queues[PRIO_READ_BG],
					      struct request, queuelist);
			target_pressure = 1;
			cd->fg_dispatch_count = 0;
			goto dispatch;
		}
		if (!list_empty(&cd->queues[PRIO_WRITE_BG])) {
			rq = list_first_entry(&cd->queues[PRIO_WRITE_BG],
					      struct request, queuelist);
			target_pressure = 2;
			cd->fg_dispatch_count = 0;
			goto dispatch;
		}
		/* No background, reset counter anyway? Keep it? */
		cd->fg_dispatch_count = 0;
	}

	/* 1. Critical queue */
	if (!list_empty(&cd->queues[PRIO_CRITICAL])) {
		rq = list_first_entry(&cd->queues[PRIO_CRITICAL],
				      struct request, queuelist);
		target_pressure = 1;
		cd->fg_dispatch_count++;
		goto dispatch;
	}

	/* 2. TRIM queue */
	if (!list_empty(&cd->queues[PRIO_TRIM])) {
		rq = list_first_entry(&cd->queues[PRIO_TRIM],
				      struct request, queuelist);
		target_pressure = 0;
		/* TRIM doesn't affect fg count */
		goto dispatch;
	}

	/* 3. Write starvation (foreground writes) */
	if (!list_empty(&cd->queues[PRIO_WRITE_FG])) {
		struct request *wrq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
							struct request, queuelist);
		u64 wait_time = now_ns - get_req_time(wrq);
		if (wait_time > write_expire_ns) {
			if (wait_time > WRITE_STARVE_THRESH_NS) {
				cd->write_starved_count++;
				cd->last_starved_jiffies = jiffies;
				if (!cd->starvation_recovery_until) {
					cd->saved_read_batch_for_starve = cd->read_batch;
					cd->read_batch = 4;
					cinnamon_clamp_for_emmc(cd);
					cd->starvation_recovery_until = jiffies + 2 * HZ;
				}
			}
			rq = wrq;
			target_pressure = 3;
			cd->fg_dispatch_count++;
			goto dispatch;
		}
	}

	/* 4. eMMC sequential write boost */
	if (cd->emmc_mode && cd->seq_write_count > 4) {
		if (!list_empty(&cd->queues[PRIO_WRITE_FG])) {
			rq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
					      struct request, queuelist);
			target_pressure = 2;
			cd->fg_dispatch_count++;
			goto dispatch;
		}
	}

	/* 5. Normal interleaving: read_fg first, then write_fg if read batch exceeded */
	if (!list_empty(&cd->queues[PRIO_READ_FG])) {
		if (cd->read_batch_count >= read_batch &&
		    !list_empty(&cd->queues[PRIO_WRITE_FG])) {
			rq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
					      struct request, queuelist);
			target_pressure = 2;
			cd->read_batch_count = 0;
			cd->fg_dispatch_count++;
			goto dispatch;
		} else {
			rq = list_first_entry(&cd->queues[PRIO_READ_FG],
					      struct request, queuelist);
			target_pressure = 1;
			cd->read_batch_count++;
			cd->fg_dispatch_count++;
			goto dispatch;
		}
	}

	/* 6. Foreground writes (if no reads) */
	if (!list_empty(&cd->queues[PRIO_WRITE_FG])) {
		rq = list_first_entry(&cd->queues[PRIO_WRITE_FG],
				      struct request, queuelist);
		target_pressure = 2;
		cd->fg_dispatch_count++;
		goto dispatch;
	}

	/* 7. Background reads */
	if (!list_empty(&cd->queues[PRIO_READ_BG])) {
		rq = list_first_entry(&cd->queues[PRIO_READ_BG],
				      struct request, queuelist);
		target_pressure = 1;
		cd->read_batch_count++;
		cd->fg_dispatch_count = 0;  /* reset on background */
		goto dispatch;
	}

	/* 8. Background writes */
	if (!list_empty(&cd->queues[PRIO_WRITE_BG])) {
		rq = list_first_entry(&cd->queues[PRIO_WRITE_BG],
				      struct request, queuelist);
		target_pressure = 2;
		cd->fg_dispatch_count = 0;
		goto dispatch;
	}

	/* No requests */
	cd->empty_dispatch_count++;
	cinnamon_set_pressure(cd, 0);

	if ((cd->empty_dispatch_count % 100) == 0)
		cinnamon_adjust_params(cd);

	/* Idle read-ahead adjustments */
	if ((cd->empty_dispatch_count % 50) == 0) {
		if (cd->predicted_sector != 0 &&
		    cd->queue_counts[PRIO_READ_FG] == 0 &&
		    cd->queue_counts[PRIO_READ_BG] == 0) {
			unsigned long current_ra = cd->q->backing_dev_info.ra_pages;
			unsigned long max_ra = min(current_ra * 2, 256UL);
			unsigned long new_ra = clamp(current_ra + 16, 16UL, max_ra);
			if (cd->last_read_direction != 0)
				cd->q->backing_dev_info.ra_pages = new_ra;
		} else if (cd->q->backing_dev_info.ra_pages > 64) {
			cd->q->backing_dev_info.ra_pages = 64;
		}
	}
	return 0;

dispatch:
	if (rq) {
		/* Decrement count for the queue this request belongs to */
		prio = get_req_prio(rq);
		if (prio >= 0 && prio < PRIO_COUNT && cd->queue_counts[prio] > 0)
			cd->queue_counts[prio]--;

		list_del_init(&rq->queuelist);

		/* Latency tracking */
		if ((cd->dispatch_seq % ((rq_data_dir(rq) == READ) ? 8 : 16)) == 0) {
			u64 lat = now_ns - get_req_time(rq);
			cinnamon_update_latency(cd, lat);
		}

		/* Update history */
		if ((cd->dispatch_seq++ % 4) == 0) {
			if (rq_data_dir(rq) == READ)
				cinnamon_update_read_history(cd, blk_rq_pos(rq));
			else
				cinnamon_update_write_history(cd, blk_rq_pos(rq));
		}

		/* Clear timestamp (optional) */
		set_req_time_and_prio(rq, 0, 0);

		/* Update batching counters */
		cd->batch_count = (target_pressure == 2) ? cd->batch_count + 1 : 0;
		if (target_pressure == 1)
			cd->read_batch_count++;
		else
			cd->read_batch_count = 0;

		/* Update pressure if needed */
		total_pending = 0;
		for (i = 0; i < PRIO_COUNT; i++)
			total_pending += cd->queue_counts[i];
		if (total_pending > cd->pressure_thres)
			target_pressure = 3;

		if (target_pressure != atomic_read(&cinnamon_io_pressure))
			cinnamon_set_pressure(cd, target_pressure);

		elv_dispatch_sort(q, rq);
		return 1;
	}
	return 0;
}

/* Former/latter request functions - optimized using stored priority */
static struct request *
cinnamon_former_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio = get_req_prio(rq);

	if (prio < 0 || prio >= PRIO_COUNT)
		return NULL;

	if (rq->queuelist.prev == &cd->queues[prio])
		return NULL;

	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
cinnamon_latter_request(struct request_queue *q, struct request *rq)
{
	struct cinnamon_data *cd = q->elevator->elevator_data;
	int prio = get_req_prio(rq);

	if (prio < 0 || prio >= PRIO_COUNT)
		return NULL;

	if (rq->queuelist.next == &cd->queues[prio])
		return NULL;

	return list_entry(rq->queuelist.next, struct request, queuelist);
}

/* Initialize queue */
static int cinnamon_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct cinnamon_data *cd;
	struct elevator_queue *eq;
	int i;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	cd = kmalloc_node(sizeof(*cd), GFP_NOIO, q->node);
	if (!cd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	for (i = 0; i < PRIO_COUNT; i++) {
		INIT_LIST_HEAD(&cd->queues[i]);
		cd->queue_counts[i] = 0;
	}

	cd->batch_count = 0;
	cd->read_batch_count = 0;
	cd->fg_dispatch_count = 0;
	atomic64_set(&cd->total_latency_ns, 0);
	atomic_set(&cd->num_requests, 0);
	cd->last_ra_pressure = -1;
	cd->last_pressure_change_jiffies = 0;
	cd->last_manual_param_change_jiffies = jiffies;
	cd->write_expire_ms = CINNAMON_WRITE_EXPIRE;
	cd->read_batch = CINNAMON_READ_BATCH;
	cd->write_batch = CINNAMON_WRITE_BATCH;
	cd->pressure_thres = CINNAMON_PRESSURE_THRES;
	cd->hysteresis_ms = CINNAMON_HYSTERESIS_MS;

	cd->history_index = 0;
	cd->history_count = 0;
	cd->last_read_direction = 0;
	cd->predicted_sector = 0;

	cd->write_history_index = 0;
	cd->write_history_count = 0;
	cd->last_write_direction = 0;
	cd->predicted_write_sector = 0;
	cd->seq_write_count = 0;
	cd->last_write_sector = 0;

	cd->last_adjust_jiffies = jiffies;
	cd->target_latency_ms = CINNAMON_TARGET_LATENCY_MS;
	cd->adjust_interval_ms = CINNAMON_ADJUST_INTERVAL_MS;
	cd->saved_read_batch = cd->read_batch;
	cd->saved_write_batch = cd->write_batch;
	cd->saved_write_expire_ms = cd->write_expire_ms;

	cd->q = q;
	cd->dispatch_seq = 0;
	cd->empty_dispatch_count = 0;

	cd->write_starved_count = 0;
	cd->last_starved_jiffies = 0;
	cd->starvation_recovery_until = 0;
	cd->saved_read_batch_for_starve = 0;

	cd->emmc_mode = false;
	if (q->backing_dev_info.dev) {
		dev_t devt = q->backing_dev_info.dev->devt;
		if (MAJOR(devt) == EMMC_MAJOR)
			cd->emmc_mode = true;
	}
	cinnamon_clamp_for_emmc(cd);

	eq->elevator_data = cd;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void cinnamon_exit_queue(struct elevator_queue *e)
{
	struct cinnamon_data *cd = e->elevator_data;
	int i;

	for (i = 0; i < PRIO_COUNT; i++)
		WARN_ON(!list_empty(&cd->queues[i]));

	kfree(cd);
}

/* Sysfs attributes (unchanged except for adding fg_dispatch_count maybe) */
static ssize_t cinnamon_pressure_show(struct elevator_queue *e, char *page)
{
	return sprintf(page, "%d\n", atomic_read(&cinnamon_io_pressure));
}

static ssize_t cinnamon_avg_latency_ns_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	u64 total_ns = atomic64_read(&cd->total_latency_ns);
	int num = atomic_read(&cd->num_requests);
	if (num == 0)
		return sprintf(page, "0\n");
	return sprintf(page, "%llu\n", (unsigned long long)total_ns);
}

static ssize_t cinnamon_write_expire_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->write_expire_ms);
}

static ssize_t cinnamon_write_expire_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val) || val <= 0)
		return -EINVAL;
	cd->write_expire_ms = val;
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_read_batch_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->read_batch);
}

static ssize_t cinnamon_read_batch_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val) || val <= 0)
		return -EINVAL;
	cd->read_batch = val;
	cinnamon_clamp_for_emmc(cd);
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_write_batch_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->write_batch);
}

static ssize_t cinnamon_write_batch_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val) || val <= 0)
		return -EINVAL;
	cd->write_batch = val;
	cinnamon_clamp_for_emmc(cd);
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_pressure_thres_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->pressure_thres);
}

static ssize_t cinnamon_pressure_thres_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val) || val < 0)
		return -EINVAL;
	cd->pressure_thres = val;
	cinnamon_clamp_for_emmc(cd);
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_hysteresis_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->hysteresis_ms);
}

static ssize_t cinnamon_hysteresis_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val) || val <= 0)
		return -EINVAL;
	cd->hysteresis_ms = val;
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_target_latency_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->target_latency_ms);
}

static ssize_t cinnamon_target_latency_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val) || val <= 0)
		return -EINVAL;
	cd->target_latency_ms = val;
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_adjust_interval_ms_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->adjust_interval_ms);
}

static ssize_t cinnamon_adjust_interval_ms_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val) || val < 100)
		return -EINVAL;
	cd->adjust_interval_ms = val;
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_predicted_sector_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%llu\n", (unsigned long long)cd->predicted_sector);
}

static ssize_t cinnamon_last_read_direction_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->last_read_direction);
}

static ssize_t cinnamon_history_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->history_count);
}

static ssize_t cinnamon_ra_pages_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%lu\n", cd->q->backing_dev_info.ra_pages);
}

static ssize_t cinnamon_batch_stats_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page,
		"read_batch_count: %u\nbatch_count: %u\nfg_dispatch: %u\n"
		"critical: %d\ntrim: %d\nread_fg: %d\nwrite_fg: %d\nread_bg: %d\nwrite_bg: %d\n",
		cd->read_batch_count, cd->batch_count, cd->fg_dispatch_count,
		cd->queue_counts[PRIO_CRITICAL],
		cd->queue_counts[PRIO_TRIM],
		cd->queue_counts[PRIO_READ_FG],
		cd->queue_counts[PRIO_WRITE_FG],
		cd->queue_counts[PRIO_READ_BG],
		cd->queue_counts[PRIO_WRITE_BG]);
}

static ssize_t cinnamon_emmc_mode_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->emmc_mode ? 1 : 0);
}

static ssize_t cinnamon_emmc_mode_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct cinnamon_data *cd = e->elevator_data;
	long val;
	if (kstrtol(page, 10, &val))
		return -EINVAL;
	cd->emmc_mode = (val != 0);
	cinnamon_clamp_for_emmc(cd);
	cd->last_manual_param_change_jiffies = jiffies;
	return count;
}

static ssize_t cinnamon_trim_cnt_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%d\n", cd->queue_counts[PRIO_TRIM]);
}

static ssize_t cinnamon_starvation_count_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%u\n", cd->write_starved_count);
}

static ssize_t cinnamon_last_starved_show(struct elevator_queue *e, char *page)
{
	struct cinnamon_data *cd = e->elevator_data;
	return sprintf(page, "%lu\n", cd->last_starved_jiffies);
}

static struct elv_fs_entry cinnamon_attrs[] = {
	__ATTR(pressure, 0444, cinnamon_pressure_show, NULL),
	__ATTR(avg_latency_ns, 0444, cinnamon_avg_latency_ns_show, NULL),
	__ATTR(write_expire_ms, 0644, cinnamon_write_expire_ms_show, cinnamon_write_expire_ms_store),
	__ATTR(read_batch, 0644, cinnamon_read_batch_show, cinnamon_read_batch_store),
	__ATTR(write_batch, 0644, cinnamon_write_batch_show, cinnamon_write_batch_store),
	__ATTR(pressure_thres, 0644, cinnamon_pressure_thres_show, cinnamon_pressure_thres_store),
	__ATTR(hysteresis_ms, 0644, cinnamon_hysteresis_ms_show, cinnamon_hysteresis_ms_store),
	__ATTR(target_latency_ms, 0644, cinnamon_target_latency_ms_show, cinnamon_target_latency_ms_store),
	__ATTR(adjust_interval_ms, 0644, cinnamon_adjust_interval_ms_show, cinnamon_adjust_interval_ms_store),
	__ATTR(predicted_sector, 0444, cinnamon_predicted_sector_show, NULL),
	__ATTR(last_read_direction, 0444, cinnamon_last_read_direction_show, NULL),
	__ATTR(history_count, 0444, cinnamon_history_count_show, NULL),
	__ATTR(ra_pages, 0444, cinnamon_ra_pages_show, NULL),
	__ATTR(batch_stats, 0444, cinnamon_batch_stats_show, NULL),
	__ATTR(emmc_mode, 0644, cinnamon_emmc_mode_show, cinnamon_emmc_mode_store),
	__ATTR(trim_cnt, 0444, cinnamon_trim_cnt_show, NULL),
	__ATTR(starvation_count, 0444, cinnamon_starvation_count_show, NULL),
	__ATTR(last_starved, 0444, cinnamon_last_starved_show, NULL),
	__ATTR_NULL
};

static struct elevator_type elevator_cinnamon = {
	.ops = {
		.elevator_merge_req_fn		= cinnamon_merged_requests,
		.elevator_dispatch_fn		= cinnamon_dispatch,
		.elevator_add_req_fn		= cinnamon_add_request,
		.elevator_former_req_fn		= cinnamon_former_request,
		.elevator_latter_req_fn		= cinnamon_latter_request,
		.elevator_init_fn			= cinnamon_init_queue,
		.elevator_exit_fn			= cinnamon_exit_queue,
		.elevator_merge_fn			= cinnamon_merge,
	},
	.elevator_name = "cinnamon",
	.elevator_owner = THIS_MODULE,
	.elevator_attrs = cinnamon_attrs,
};

static int __init cinnamon_init(void)
{
	return elv_register(&elevator_cinnamon);
}

static void __exit cinnamon_exit(void)
{
	elv_unregister(&elevator_cinnamon);
}

module_init(cinnamon_init);
module_exit(cinnamon_exit);

MODULE_AUTHOR("Noveris");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cinnamon I/O Scheduler");
