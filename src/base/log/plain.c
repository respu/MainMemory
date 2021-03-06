/*
 * base/log/plain.c - MainMemory plain message logging.
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

#include "base/log/plain.h"
#include "base/log/log.h"
#include "base/log/trace.h"

#include <stdarg.h>

static bool mm_verbose_enabled = false;

void
mm_enable_verbose(bool value)
{
	mm_verbose_enabled = value;
}

void
mm_verbose(const char *restrict msg, ...)
{
	if (!mm_verbose_enabled)
		return;

	mm_trace_prefix();

	va_list va;
	va_start(va, msg);
	mm_log_vfmt(msg, va);
	va_end(va);

	mm_log_str("\n");
}

void
mm_brief(const char *restrict msg, ...)
{
	mm_trace_prefix();

	va_list va;
	va_start(va, msg);
	mm_log_vfmt(msg, va);
	va_end(va);

	mm_log_str("\n");
}
