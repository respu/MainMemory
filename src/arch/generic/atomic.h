/*
 * arch/generic/atomic.h - MainMemory arch-specific atomic ops.
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

#ifndef ARCH_GENERIC_ATOMIC_H
#define ARCH_GENERIC_ATOMIC_H

/**********************************************************************
 * Atomic types.
 **********************************************************************/

#define mm_atomic_type(base_type) \
	struct { base_type value __align(sizeof(base_type)); }

typedef mm_atomic_type(uint8_t) mm_atomic_8_t;
typedef mm_atomic_type(uint16_t) mm_atomic_16_t;
typedef mm_atomic_type(uint32_t) mm_atomic_32_t;

/**********************************************************************
 * Atomic arithmetics.
 **********************************************************************/

#define mm_atomic_unary(bits, name, func)			\
	static inline void					\
	mm_atomic_##bits##_##name(mm_atomic_##bits##_t *p)	\
	{							\
		func(&p->value, 1);				\
	}

mm_atomic_unary(8, inc, __sync_fetch_and_add)
mm_atomic_unary(16, inc, __sync_fetch_and_add)
mm_atomic_unary(32, inc, __sync_fetch_and_add)
mm_atomic_unary(8, dec, __sync_fetch_and_sub)
mm_atomic_unary(16, dec, __sync_fetch_and_sub)
mm_atomic_unary(32, dec, __sync_fetch_and_sub)

/**********************************************************************
 * Atomic operations for spin-locks.
 **********************************************************************/

/*
 * mm_atomic_lock_acquire() is a test-and-set atomic operation along with
 * acquire fence.
 * 
 * mm_atomic_lock_release() is a simple clear operation along with release
 * fence.
 * 
 * mm_atomic_lock_pause() is a special instruction to be used in spin-lock
 * loops to make hyper-threading CPUs happy.
 */

#define MM_ATOMIC_LOCK_INIT	{0}

typedef struct { char locked; } mm_atomic_lock_t;

static inline int
mm_atomic_lock_acquire(mm_atomic_lock_t *lock)
{
	return __sync_lock_test_and_set(&lock->locked, 1);
}

static inline void
mm_atomic_lock_release(mm_atomic_lock_t *lock)
{
	__sync_lock_release(&lock->locked);
}

static inline void
mm_atomic_lock_pause(void)
{
}

#endif /* ARCH_GENERIC_ATOMIC_H */
