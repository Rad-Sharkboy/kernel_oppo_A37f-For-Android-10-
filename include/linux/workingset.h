#ifndef _LINUX_WORKINGSET_H
#define _LINUX_WORKINGSET_H

#include <linux/types.h>

struct address_space;
struct page;

#ifdef CONFIG_WORKINGSET_REFAULT
void workingset_forget(struct address_space *mapping);
void workingset_eviction(struct address_space *mapping, struct page *page);
bool workingset_refault(struct address_space *mapping, struct page *page);
void workingset_activation(struct page *page);
#else
static inline void workingset_forget(struct address_space *mapping) { }
static inline void workingset_eviction(struct address_space *mapping,
		struct page *page) { }
static inline bool workingset_refault(struct address_space *mapping,
		struct page *page) { return false; }
static inline void workingset_activation(struct page *page) { }
#endif
#endif
