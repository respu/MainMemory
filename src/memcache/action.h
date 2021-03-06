/*
 * memcache/action.h - MainMemory memcache table actions.
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

#ifndef MEMCACHE_ACTION_H
#define	MEMCACHE_ACTION_H

#include "common.h"
#include "memcache/table.h"
#include "core/core.h"

#if ENABLE_MEMCACHE_COMBINER
# include "base/combiner.h"
#endif

#if ENABLE_MEMCACHE_COMBINER
typedef enum {

	MC_ACTION_DONE,

	/* Search for an entry. */
	MC_ACTION_LOOKUP,
	/* Finish using found entry. */
	MC_ACTION_FINISH,

	/* Delete existing entry if any. */
	MC_ACTION_DELETE,

	/* Create a fresh entry. */
	MC_ACTION_CREATE,
	/* Abandon a created entry. */
	MC_ACTION_CANCEL,
	/* Insert newly created entry. */
	MC_ACTION_INSERT,
	/* Replace existing entry if any. */
	MC_ACTION_UPDATE,
	/* Either insert new or replace existing entry. */
	MC_ACTION_UPSERT,

	MC_ACTION_STRIDE,

	MC_ACTION_EVICT,

	MC_ACTION_FLUSH,

} mc_action_t;
#endif

struct mc_action
{
	const char *key;
	uint32_t key_len;

	uint32_t hash;

	struct mc_tpart *part;
	struct mc_entry *new_entry;
	struct mc_entry *old_entry;

	uint64_t stamp;

#if ENABLE_MEMCACHE_COMBINER
	mc_action_t action;
#endif

	/* Input flag indicating if update should check entry stamp. */
	bool match_stamp;
	/* Input flags indicating if update should retain old and new
	   entry references after the action. */
	bool ref_old_on_failure;
	bool ref_new_on_success;
	/* Output flag indicating if the entry match succeeded. */
	bool entry_match;
};

void mc_action_lookup_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_finish_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_delete_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_create_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_cancel_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_insert_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_update_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_upsert_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_stride_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_evict_low(struct mc_action *action)
	__attribute__((nonnull(1)));

void mc_action_flush_low(struct mc_action *action)
	__attribute__((nonnull(1)));

static inline void
mc_action_wait(struct mc_action *action __attribute__((unused)))
{
#if ENABLE_MEMCACHE_COMBINER
	while (action->action != MC_ACTION_DONE)
		mm_spin_pause();
	mm_memory_load_fence();
#endif
}

static inline void
mc_action_lookup(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_LOOKUP;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_lookup_low(action);
#endif
}

static inline void
mc_action_finish(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_FINISH;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_finish_low(action);
#endif
}

static inline void
mc_action_delete(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_DELETE;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_delete_low(action);
#endif
}

static inline void
mc_action_create(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_CREATE;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_create_low(action);
#endif
}

static inline void
mc_action_cancel(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_CANCEL;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_cancel_low(action);
#endif
}

static inline void
mc_action_insert(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_INSERT;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_insert_low(action);
#endif
}

static inline void
mc_action_update(struct mc_action *action)
{
	action->match_stamp = false;
	action->ref_old_on_failure = false;
	action->ref_new_on_success = false;
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_UPDATE;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_update_low(action);
#endif
}

static inline void
mc_action_compare_and_update(struct mc_action *action,
			     bool ref_old_on_failure,
			     bool ref_new_on_success)
{
	action->match_stamp = true;
	action->ref_old_on_failure = ref_old_on_failure;
	action->ref_new_on_success = ref_new_on_success;
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_UPDATE;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_update_low(action);
#endif
}

static inline void
mc_action_upsert(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_UPSERT;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_upsert_low(action);
#endif
}

static inline void
mc_action_stride(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_STRIDE;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_stride_low(action);
#endif
}

static inline void
mc_action_evict(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_EVICT;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_evict_low(action);
#endif
}

static inline void
mc_action_flush(struct mc_action *action)
{
#if ENABLE_MEMCACHE_COMBINER
	action->action = MC_ACTION_FLUSH;
	mm_combiner_execute(action->part->combiner, (uintptr_t) action);
	mc_action_wait(action);
#else
	mc_action_flush_low(action);
#endif
}

#if ENABLE_MEMCACHE_COMBINER
void mc_action_perform(uintptr_t data);
#endif

#endif /* MEMCACHE_ACTION_H */
