/* Bounded file-cache refault history. GPL-2.0 */
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/workingset.h>

#define HISTORY_BITS 12
#define IDENTITY_BITS 10
#define LOCK_BITS 6
#define HISTORY_WAYS 4
#define LOCK_COUNT (1U << LOCK_BITS)

struct refault_identity {
	struct address_space *mapping;
	u64 cookie;
};

struct identity_bucket {
	struct refault_identity entries[HISTORY_WAYS];
	unsigned int next;
};

struct refault_entry {
	u64 cookie;
	pgoff_t index;
	unsigned long age;
	struct zone *zone;
};

struct history_bucket {
	struct refault_entry entries[HISTORY_WAYS];
	unsigned int next;
};

static struct identity_bucket identities[1U << IDENTITY_BITS];
static struct history_bucket history[1U << HISTORY_BITS];
static spinlock_t identity_locks[LOCK_COUNT];
static spinlock_t history_locks[LOCK_COUNT];
static atomic_long_t inactive_age[MAX_NUMNODES][MAX_NR_ZONES];
static atomic64_t next_cookie = ATOMIC64_INIT(0);
static atomic_long_t evictions = ATOMIC_LONG_INIT(0);
static atomic_long_t refaults = ATOMIC_LONG_INIT(0);
static atomic_long_t activations = ATOMIC_LONG_INIT(0);
static atomic_long_t replacements = ATOMIC_LONG_INIT(0);
static bool ready;
static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "Protect recently evicted file pages on refault");

static atomic_long_t *zone_age(struct zone *zone)
{
	return &inactive_age[zone_to_nid(zone)][zone_idx(zone)];
}

static bool history_ready(void)
{
	if (!ACCESS_ONCE(ready))
		return false;
	smp_rmb();
	return true;
}

static unsigned int identity_hash(struct address_space *mapping)
{
	return hash_ptr(mapping, IDENTITY_BITS);
}

static unsigned int history_hash(u64 cookie, pgoff_t index)
{
	return hash_64(cookie ^ (u64)index, HISTORY_BITS);
}

/* Caller holds the identity lock; replaced identities only lose hints. */
static u64 identity_cookie(struct identity_bucket *bucket,
		struct address_space *mapping, bool create)
{
	struct refault_identity *entry, *empty = NULL;
	unsigned int i;
	u64 cookie;

	for (i = 0; i < HISTORY_WAYS; i++) {
		entry = &bucket->entries[i];
		if (entry->mapping == mapping)
			return entry->cookie;
		if (!entry->mapping && !empty)
			empty = entry;
	}
	if (!create)
		return 0;
	if (!empty)
		empty = &bucket->entries[bucket->next++ % HISTORY_WAYS];
	cookie = (u64)atomic64_inc_return(&next_cookie);
	if (!cookie)
		cookie = (u64)atomic64_inc_return(&next_cookie);
	empty->cookie = cookie;
	ACCESS_ONCE(empty->mapping) = mapping;
	return cookie;
}

void workingset_forget(struct address_space *mapping)
{
	unsigned int h, i;
	unsigned long flags;

	if (!history_ready())
		return;
	h = identity_hash(mapping);
	spin_lock_irqsave(&identity_locks[h % LOCK_COUNT], flags);
	for (i = 0; i < HISTORY_WAYS; i++)
		if (identities[h].entries[i].mapping == mapping)
			ACCESS_ONCE(identities[h].entries[i].mapping) = NULL;
	spin_unlock_irqrestore(&identity_locks[h % LOCK_COUNT], flags);
}

void workingset_eviction(struct address_space *mapping, struct page *page)
{
	struct history_bucket *bucket;
	struct refault_entry *entry = NULL;
	struct zone *zone = page_zone(page);
	unsigned long age, flags;
	unsigned int ih, hh, i;
	u64 cookie;

	if (!history_ready() || PageSwapBacked(page))
		return;
	age = (unsigned long)atomic_long_inc_return(zone_age(zone));
	if (!ACCESS_ONCE(enabled) || !mapping->host ||
	    !S_ISREG(mapping->host->i_mode))
		return;
	ih = identity_hash(mapping);
	/* Lock order: mapping tree -> identity -> history. */
	spin_lock_irqsave(&identity_locks[ih % LOCK_COUNT], flags);
	cookie = identity_cookie(&identities[ih], mapping, true);
	hh = history_hash(cookie, page->index);
	bucket = &history[hh];
	spin_lock(&history_locks[hh % LOCK_COUNT]);
	for (i = 0; i < HISTORY_WAYS; i++) {
		if (bucket->entries[i].cookie == cookie &&
		    bucket->entries[i].index == page->index) {
			entry = &bucket->entries[i];
			break;
		}
		if (!bucket->entries[i].cookie && !entry)
			entry = &bucket->entries[i];
	}
	if (!entry) {
		entry = &bucket->entries[bucket->next++ % HISTORY_WAYS];
		atomic_long_inc(&replacements);
	}
	entry->cookie = cookie;
	entry->index = page->index;
	entry->age = age;
	entry->zone = zone;
	spin_unlock(&history_locks[hh % LOCK_COUNT]);
	spin_unlock_irqrestore(&identity_locks[ih % LOCK_COUNT], flags);
	atomic_long_inc(&evictions);
}

bool workingset_refault(struct address_space *mapping, struct page *page)
{
	struct identity_bucket *identity;
	struct history_bucket *bucket;
	struct zone *zone = NULL;
	unsigned long distance = 0, flags;
	unsigned int ih, hh, i;
	u64 cookie;

	if (!history_ready() || !ACCESS_ONCE(enabled) ||
	    PageSwapBacked(page) || !mapping->host ||
	    !S_ISREG(mapping->host->i_mode))
		return false;
	ih = identity_hash(mapping);
	identity = &identities[ih];
	/* Advisory miss check avoids locks for files without eviction history. */
	for (i = 0; i < HISTORY_WAYS; i++)
		if (ACCESS_ONCE(identity->entries[i].mapping) == mapping)
			break;
	if (i == HISTORY_WAYS)
		return false;
	spin_lock_irqsave(&identity_locks[ih % LOCK_COUNT], flags);
	cookie = identity_cookie(identity, mapping, false);
	if (!cookie)
		goto unlock_identity;
	hh = history_hash(cookie, page->index);
	bucket = &history[hh];
	spin_lock(&history_locks[hh % LOCK_COUNT]);
	for (i = 0; i < HISTORY_WAYS; i++) {
		struct refault_entry *entry = &bucket->entries[i];

		if (entry->cookie != cookie || entry->index != page->index)
			continue;
		zone = entry->zone;
		distance = (unsigned long)atomic_long_read(zone_age(zone)) -
			entry->age;
		entry->cookie = 0;
		break;
	}
	spin_unlock(&history_locks[hh % LOCK_COUNT]);
unlock_identity:
	spin_unlock_irqrestore(&identity_locks[ih % LOCK_COUNT], flags);
	if (!zone)
		return false;
	atomic_long_inc(&refaults);
	if (distance > zone_page_state(zone, NR_ACTIVE_FILE))
		return false;
	workingset_activation(page);
	atomic_long_inc(&activations);
	count_vm_event(PGACTIVATE);
	return true;
}

void workingset_activation(struct page *page)
{
	if (history_ready() && !PageSwapBacked(page))
		atomic_long_inc(zone_age(page_zone(page)));
}

static int refault_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "enabled %u\nmetadata_bytes %zu\nhistory_entries %u\n",
		ACCESS_ONCE(enabled), sizeof(identities) + sizeof(history) +
		sizeof(identity_locks) + sizeof(history_locks) +
		sizeof(inactive_age), (1U << HISTORY_BITS) * HISTORY_WAYS);
	seq_printf(m, "evictions %ld\nrefaults %ld\nactivations %ld\nreplacements %ld\n",
		atomic_long_read(&evictions), atomic_long_read(&refaults),
		atomic_long_read(&activations), atomic_long_read(&replacements));
	return 0;
}

static int refault_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, refault_stats_show, NULL);
}

static const struct file_operations refault_stats_fops = {
	.open = refault_stats_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init workingset_init(void)
{
	unsigned int i;

	for (i = 0; i < LOCK_COUNT; i++) {
		spin_lock_init(&identity_locks[i]);
		spin_lock_init(&history_locks[i]);
	}
	smp_wmb();
	ACCESS_ONCE(ready) = true;
	return 0;
}
core_initcall(workingset_init);

static int __init refault_stats_init(void)
{
	if (!proc_create("workingset_refault", 0444, NULL, &refault_stats_fops))
		pr_warn("workingset: statistics unavailable\n");
	return 0;
}
module_init(refault_stats_init);
MODULE_LICENSE("GPL v2");
