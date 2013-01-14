/*
 * pool.h - MainMemory memory pools.
 *
 * Copyright (C) 2012  Aleksey Demakov
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

#ifndef POOL_H
#define POOL_H

#include "common.h"

struct mm_pool
{
	uint32_t item_count;
	uint32_t free_index;
	uint32_t pool_size;
	uint32_t item_size;
	void *pool_data;
	char *pool_name;
};

void mm_pool_init(struct mm_pool *pool, const char *name, size_t item_size)
	__attribute__((nonnull(1, 2)));

void mm_pool_discard(struct mm_pool *pool)
	__attribute__((nonnull(1)));

void * mm_pool_idx2ptr(struct mm_pool *pool, uint32_t index)
	__attribute__((nonnull(1)));

uint32_t mm_pool_ptr2idx(struct mm_pool *pool, void *item)
	__attribute__((nonnull(1, 2)));

void * mm_pool_alloc(struct mm_pool *pool)
	__attribute__((nonnull(1)));

void mm_pool_free(struct mm_pool *pool, void *item)
	__attribute__((nonnull(1, 2)));

#endif /* POOL_H */
