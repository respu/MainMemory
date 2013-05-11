/*
 * thread.h - MainMemory threads.
 *
 * Copyright (C) 2013  Aleksey Demakov
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

#ifndef THREAD_H
#define THREAD_H

#include "common.h"

/* Maximal thread name length (including terminating zero). */
#define MM_THREAD_NAME_SIZE	40

/* Declare opaque thread type. */
struct mm_thread;

struct mm_thread_attr
{
	/* CPU affinity tag. */
	uint32_t cpu_tag;

	/* The thread stack. */
	uint32_t stack_size;
	void *stack_base;

	/* The thread name. */
	char name[MM_THREAD_NAME_SIZE];
};

extern __thread struct mm_thread *mm_thread;

void mm_thread_init();
void mm_thread_term();

void mm_thread_attr_init(struct mm_thread_attr *attr);
void mm_thread_attr_setcputag(struct mm_thread_attr *attr, uint32_t cpu_tag);
void mm_thread_attr_setstack(struct mm_thread_attr *attr,
			     void *stack_base, uint32_t stack_size);
void mm_thread_attr_setname(struct mm_thread_attr *attr, const char *name);

struct mm_thread * mm_thread_create(struct mm_thread_attr *attr,
				    mm_routine_t start, uintptr_t start_arg);

void mm_thread_destroy(struct mm_thread *thread);

void mm_thread_cancel(struct mm_thread *thread);

void mm_thread_join(struct mm_thread *thread);

#endif /* THREAD_H */
