/*
 * base/thr/domain.h - MainMemory thread domain.
 *
 * Copyright (C) 2014  Aleksey Demakov
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

#ifndef BASE_THR_DOMAIN_H
#define	BASE_THR_DOMAIN_H

#include "common.h"
#include "base/list.h"
#include "base/lock.h"
#include "base/barrier.h"
#include "base/bitset.h"
#include "base/thr/thread.h"

/* Maximum domain name length (including terminating zero). */
#define MM_DOMAIN_NAME_SIZE	32

struct mm_domain_thread
{
	struct mm_thread *thread;
	struct mm_thread_attr thread_attr;
};

struct mm_domain
{
	/* Domain threads. */
	mm_core_t nthreads;
	struct mm_domain_thread *threads;

	/* Per-thread data. */
	struct mm_queue per_thread_chunk_list;
	struct mm_queue per_thread_entry_list;
	mm_lock_t per_thread_lock;

	/* Thread start barrier. */
	struct mm_barrier barrier;

	/* Domain name. */
	char name[MM_DOMAIN_NAME_SIZE];
};

extern __thread struct mm_domain *__mm_domain_self;

static inline struct mm_domain *
mm_domain_self(void)
{
	return __mm_domain_self;
}

void mm_domain_prepare(struct mm_domain *domain, const char *name, mm_core_t nthreads)
	__attribute__((nonnull(1)));

void mm_domain_cleanup(struct mm_domain *domain)
	__attribute__((nonnull(1)));

void mm_domain_setcputag(struct mm_domain *domain, mm_core_t n, uint32_t cpu_tag)
	__attribute__((nonnull(1)));

void mm_domain_setstack(struct mm_domain *domain, mm_core_t n, void *stack_base, uint32_t stack_size)
	__attribute__((nonnull(1)));

void mm_domain_start(struct mm_domain *domain, mm_routine_t start)
	__attribute__((nonnull(1)));

void mm_domain_join(struct mm_domain *domain)
	__attribute__((nonnull(1)));

#endif /* BASE_THR_DOMAIN_H */
