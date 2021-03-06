/*
 * base/combiner.h - MainMemory combining synchronization.
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

#ifndef BASE_COMBINER_H
#define BASE_COMBINER_H

#include "common.h"
#include "base/ring.h"

typedef void (*mm_combiner_routine_t)(uintptr_t data);

struct mm_combiner
{
	mm_combiner_routine_t routine;

	size_t handoff;

	struct mm_ring_mpmc ring;
};

struct mm_combiner * mm_combiner_create(mm_combiner_routine_t routine,
					size_t size, size_t handoff);

void mm_combiner_destroy(struct mm_combiner *combiner)
	__attribute__((nonnull(1)));

void mm_combiner_prepare(struct mm_combiner *combiner,
			 mm_combiner_routine_t routine,
			 size_t size, size_t handoff)
	__attribute__((nonnull(1)));

void mm_combiner_execute(struct mm_combiner *combiner, uintptr_t data)
	__attribute__((nonnull(1)));

#endif /* BASE_COMBINER_H */
