/*
 * memcache/table.c - MainMemory memcache entry table.
 *
 * Copyright (C) 2012-2014  Aleksey Demakov
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "memcache/table.h"
#include "memcache/action.h"
#include "memcache/entry.h"

#include "core/task.h"

#include "base/combiner.h"
#include "base/hash.h"
#include "base/log/error.h"
#include "base/log/plain.h"
#include "base/log/trace.h"

#include <sys/mman.h>

#if MM_WORD_32BIT
# define MC_TABLE_SIZE_MAX	((size_t) 64 * 1024 * 1024)
#else
# define MC_TABLE_SIZE_MAX	((size_t) 512 * 1024 * 1024)
#endif

#define MC_TABLE_VOLUME_RESERVE	(64 * 1024)

struct mc_table mc_table;

/**********************************************************************
 * Helper routines.
 **********************************************************************/

static inline size_t
mc_table_buckets_size(uint16_t nparts, uint32_t nbuckets)
{
	size_t space = nbuckets * sizeof(struct mm_link);
	return nparts * mm_round_up(space, MM_PAGE_SIZE);
}

static inline size_t
mc_table_entries_size(uint16_t nparts, uint32_t nentries)
{
	size_t space = nentries * sizeof(struct mc_entry);
	return nparts * mm_round_up(space, MM_PAGE_SIZE);
}

static inline bool
mc_table_check_size(struct mc_tpart *part)
{
	uint32_t nb = mm_memory_load(part->nbuckets);
	uint32_t ne = mm_memory_load(part->nentries);
	ne -= mm_memory_load(part->nentries_free);
	ne -= mm_memory_load(part->nentries_void);
	return ne > (nb * 2) && nb < mc_table.nbuckets_max;
}

static inline bool
mc_table_check_volume(struct mc_tpart *part, size_t reserve)
{
	uint32_t n = mm_memory_load(part->volume);
	return (n + reserve) > mc_table.volume_max;
}

/**********************************************************************
 * Table resize.
 **********************************************************************/

static void
mc_table_resize(void *start, size_t old_size, size_t new_size)
{
	ASSERT(((intptr_t) start % MM_PAGE_SIZE) == 0);
	ASSERT((old_size % MM_PAGE_SIZE) == 0);
	ASSERT((new_size % MM_PAGE_SIZE) == 0);
	ASSERT(old_size != new_size);

	void *addr, *map_addr;
	if (old_size > new_size) {
		size_t diff = old_size - new_size;
		addr = (char *) start + new_size;
		map_addr = mmap(addr, diff, PROT_NONE,
				MAP_ANON | MAP_FIXED | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	} else {
		size_t diff = new_size - old_size;
		addr = (char *) start + old_size;
		map_addr = mmap(addr, diff, PROT_READ | PROT_WRITE,
				MAP_ANON | MAP_FIXED | MAP_PRIVATE, -1, 0);
	}

	if (map_addr == MAP_FAILED)
		mm_fatal(errno, "mmap");
	if (unlikely(map_addr != addr))
		mm_fatal(0, "mmap returned wrong address");
}

void
mc_table_buckets_resize(struct mc_tpart *part,
			uint32_t old_nbuckets,
			uint32_t new_nbuckets)
{
	ENTER();
	ASSERT(mm_is_pow2z(old_nbuckets));
	ASSERT(mm_is_pow2(new_nbuckets));

	size_t old_size = mc_table_buckets_size(1, old_nbuckets);
	size_t new_size = mc_table_buckets_size(1, new_nbuckets);
	if (likely(old_size != new_size)) {
		mm_brief("memcache enabled buckets for partition #%d: %u, %lu bytes",
			 (int) (part - mc_table.parts), new_nbuckets,
			 (unsigned long) new_size);
		mc_table_resize(part->buckets, old_size, new_size);
	}

	LEAVE();
}

void
mc_table_entries_resize(struct mc_tpart *part,
			uint32_t old_nentries,
			uint32_t new_nentries)
{
	ENTER();

	size_t old_size = mc_table_entries_size(1, old_nentries);
	size_t new_size = mc_table_entries_size(1, new_nentries);
	if (likely(old_size != new_size)) {
		mm_brief("memcache enabled entries for partition #%d: %u, %lu bytes",
			 (int) (part - mc_table.parts), new_nentries,
			 (unsigned long) new_size);
		mc_table_resize(part->entries, old_size, new_size);
	}

	LEAVE();
}

bool
mc_table_expand(struct mc_tpart *part, uint32_t n)
{
	ENTER();

	bool rc = false;

	uint32_t old_nentries = part->nentries;
	uint32_t new_nentries = old_nentries + n;
	if (unlikely(new_nentries < old_nentries))
		new_nentries = UINT32_MAX;
	if (unlikely(new_nentries > mc_table.nentries_max))
		new_nentries = mc_table.nentries_max;
	n = new_nentries - old_nentries;

	if (n) {
		mc_table_entries_resize(part, old_nentries, new_nentries);
		part->nentries_void += n;
		part->nentries += n;
		rc = true;
	}

	LEAVE();
	return rc;
}

static mm_value_t
mc_table_stride_routine(mm_value_t arg)
{
	ENTER();

	struct mc_tpart *part = (struct mc_tpart *) arg;
	ASSERT(part->striding);

	struct mc_action action;
	action.part = part;

	mc_action_stride(&action);

	part->striding = false;

	LEAVE();
	return 0;
}

static void
mc_table_start_striding(struct mc_tpart *part)
{
	ENTER();

#if ENABLE_MEMCACHE_DELEGATE
	mm_core_post(MM_CORE_SELF, mc_table_stride_routine, (mm_value_t) part);
#else
	mm_core_post(MM_CORE_NONE, mc_table_stride_routine, (mm_value_t) part);
#endif

	LEAVE();
}

/**********************************************************************
 * Entry eviction.
 **********************************************************************/

static mm_value_t
mc_table_evict_routine(mm_value_t arg)
{
	ENTER();

	struct mc_tpart *part = (struct mc_tpart *) arg;
	ASSERT(part->evicting);

	struct mc_action action;
	action.part = part;

	size_t reserve = MC_TABLE_VOLUME_RESERVE / mc_table.nparts;
	while (mc_table_check_volume(part, reserve)) {
		mc_action_evict(&action);
		mm_task_yield();
	}

	part->evicting = false;

	LEAVE();
	return 0;
}

static void
mc_table_start_evicting(struct mc_tpart *part)
{
	ENTER();

#if ENABLE_MEMCACHE_DELEGATE
	mm_core_post(MM_CORE_SELF, mc_table_evict_routine, (mm_value_t) part);
#else
	mm_core_post(MM_CORE_NONE, mc_table_evict_routine, (mm_value_t) part);
#endif

	LEAVE();
}

void
mc_table_reserve_volume(struct mc_tpart *part)
{
	if (!part->evicting && mc_table_check_volume(part, 0)) {
		part->evicting = true;
		mc_table_start_evicting(part);
	}
}

void
mc_table_reserve_entries(struct mc_tpart *part)
{
	if (!part->striding && mc_table_check_size(part)) {
		part->striding = true;
		mc_table_start_striding(part);
	}
}

/**********************************************************************
 * Table initialization and termination.
 **********************************************************************/

static void
mc_table_init_part(mm_core_t index, mm_core_t core)
{
	struct mc_tpart *part = &mc_table.parts[index];

	char *buckets = ((char *) mc_table.buckets_base)
			+ mc_table_buckets_size(index, mc_table.nbuckets_max);
	char *entries = ((char *) mc_table.entries_base)
			+ mc_table_entries_size(index, mc_table.nentries_max);

	part->buckets = (struct mm_link *) buckets;
	part->entries = (struct mc_entry *) entries;
	part->entries_end = part->entries;

	part->clock_hand = part->entries;

	mm_link_init(&part->free_list);

	part->nbuckets = 0;
	part->nbuckets = 0;
	part->nentries_free = 0;
	part->nentries_void = 0;

	part->volume = 0;

	mm_waitset_prepare(&part->waitset);
	mm_waitset_pin(&part->waitset, core);

#if ENABLE_MEMCACHE_COMBINER
	part->combiner = mm_combiner_create(mc_action_perform,
					    MC_COMBINER_SIZE,
					    MC_COMBINER_HANDOFF);
#elif ENABLE_MEMCACHE_DELEGATE
	mm_verbose("bind partition %d to core %d", index, core);
	part->core = core;
#elif ENABLE_MEMCACHE_LOCKING
	part->lookup_lock = (mm_task_lock_t) MM_TASK_LOCK_INIT;
	part->freelist_lock = (mm_task_lock_t) MM_TASK_LOCK_INIT;
#endif

	part->evicting = false;
	part->striding = false;

	part->stamp = index;

	// Allocate initial space for the table.
	mc_table_expand(part, mc_table.nentries_increment);
	uint32_t nbuckets = part->nentries / 2;
	mc_table_buckets_resize(part, 0, nbuckets);
	part->nbuckets = nbuckets;
}

void
mc_table_init(const struct mm_memcache_config *config)
{
	ENTER();

	// Round the number of table partitions to a power of 2.
	mm_core_t nparts;
#if ENABLE_MEMCACHE_DELEGATE
	nparts = mm_bitset_count(&config->affinity);
#else
	nparts = config->nparts;
#endif
	ASSERT(nparts > 0);
	uint16_t nbits = sizeof(int) * 8 - 1 - mm_clz(nparts);
	nparts = 1 << nbits;

	mm_brief("memcache partitions: %d", nparts);
	mm_brief("memcache partition bits: %d", nbits);

	// Determine the size constraints for table partitions.
	size_t volume = config->volume / nparts;
	if (volume < MM_PAGE_SIZE)
		volume = MM_PAGE_SIZE;
	// Make a very liberal estimate that for an average table entry
	// the combined size of key and data might be as small as 20 bytes.
	size_t nentries_max = volume / (sizeof(struct mc_entry) + 20);
	size_t nbuckets_max = 1 << (sizeof(int) * 8 - 1 - mm_clz(nentries_max));

	mm_brief("memcache maximum data volume per partition: %lu",
		 (unsigned long) volume);
	mm_brief("memcache maximum number of entries per partition: %lu",
		 (unsigned long) nentries_max);
	mm_brief("memcache maximum number of buckets per partition: %lu",
		 (unsigned long) nbuckets_max);
	if (nentries_max != (uint32_t) nentries_max)
		mm_fatal(0, "too many entries");
	if (nbuckets_max != (uint32_t) nbuckets_max)
		mm_fatal(0, "too many buckets");

	// Reserve address space for table entries.
	size_t entries_size = mc_table_entries_size(nparts, nentries_max);
	mm_brief("memcache reserved entries for table: %ld bytes",
		 (unsigned long) entries_size);
	void *entries_base = mmap(NULL, entries_size, PROT_NONE,
				  MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	if (entries_base == MAP_FAILED)
		mm_fatal(errno, "mmap");

	// Reserve address space for table buckets.
	size_t buckets_size = mc_table_buckets_size(nparts, nbuckets_max);
	mm_brief("memcache reserved buckets for table: %ld bytes",
		 (unsigned long) buckets_size);
	void *buckets_base = mmap(NULL, buckets_size, PROT_NONE,
				  MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	if (buckets_base == MAP_FAILED)
		mm_fatal(errno, "mmap");

	// Compute the number of entries added on expansion.
	uint32_t nentries_increment = 4 * 1024;
	if (nparts == 1)
		nentries_increment *= 4;
	else if (nparts == 2)
		nentries_increment *= 2;

	// Initialize the table.
	mc_table.parts = mm_shared_calloc(nparts, sizeof(struct mc_tpart));
	mc_table.nparts = nparts;
	mc_table.part_bits = nbits;
	mc_table.part_mask = nparts - 1;
	mc_table.volume_max = volume;
	mc_table.nbuckets_max = nbuckets_max;
	mc_table.nentries_max = nentries_max;
	mc_table.nentries_increment = nentries_increment;
	mc_table.buckets_base = buckets_base;
	mc_table.entries_base = entries_base;

	// Initialize the table partitions.
#if ENABLE_MEMCACHE_DELEGATE
	mm_core_t index = 0;
	ASSERT(nparts <= mm_core_getnum());
	for (mm_core_t core = 0; core < mm_core_getnum(); core++) {
		if (mm_bitset_test(&mc_config.affinity, core)) {
			mc_table_init_part(index++, core);
		}
	}
#else
	for (mm_core_t index = 0; index < nparts; index++) {
		mc_table_init_part(index, MM_CORE_NONE);
	}
#endif

	LEAVE();
}

void
mc_table_term(void)
{
	ENTER();

	// Free the table entries.
	for (mm_core_t p = 0; p < mc_table.nparts; p++) {
		struct mc_tpart *part = &mc_table.parts[p];
		for (uint32_t i = 0; i < part->nbuckets; i++) {
			struct mm_link *link = mm_link_head(&part->buckets[i]);
			while (link != NULL) {
				struct mc_entry *entry =
					containerof(link, struct mc_entry, link);
				link = link->next;

				mm_chunk_destroy_chain(mm_link_head(&entry->chunks));
			}
		}
	}

	// Free the table partitions.
	mm_shared_free(mc_table.parts);

	// Compute the reserved address space size.
	size_t buckets_size = mc_table_buckets_size(mc_table.nparts,
						    mc_table.nbuckets_max);
	size_t entries_size = mc_table_entries_size(mc_table.nparts,
						    mc_table.nentries_max);

	// Release the reserved address space.
	if (munmap(mc_table.buckets_base, buckets_size) < 0)
		mm_error(errno, "munmap");
	if (munmap(mc_table.entries_base, entries_size) < 0)
		mm_error(errno, "munmap");

	LEAVE();
}
