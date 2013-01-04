/*
 * common.h - MainMemory common definitions.
 *
 * Copyright (C) 2012  Aleksey Demakov
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef COMMON_H
#define COMMON_H

/**********************************************************************
 * Common Headers.
 **********************************************************************/

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef __GNUC__
# error "Only GCC is currently supported."
#endif

/**********************************************************************
 * Compiler Helpers.
 **********************************************************************/

#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

/**********************************************************************
 * Bit Manipulation.
 **********************************************************************/

/* Count leading zeros (from MSB). Zero argument is not allowed. */
#define clz(x)		__builtin_clz(x)
/* Count trailing zeros (from LSB). Zero argument is not allowed. */
#define ctz(z)		__builtin_ctz(x)
/* For non-zero arguments just like ctz(x)+1 but for zero returns zero too. */
#define ffs(x)		__builtin_ffs(x)

/**********************************************************************
 * Common Macros.
 **********************************************************************/

#define min(a, b) ({			\
		typeof(a) _a = (a);	\
		typeof(b) _b = (b);	\
		_a < _b ? _a : _b;	\
	})

#define max(a, b) ({			\
		typeof(a) _a = (a);	\
		typeof(b) _b = (b);	\
		_a > _b ? _a : _b;	\
	})

#define containerof(field_ptr, type, field) \
	((type *) ((char *)(field_ptr) - offsetof(type, field)))

#endif /* COMMON_H */
