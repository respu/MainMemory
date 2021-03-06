/*
 * base/sys/clock.c - MainMemory time routines.
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

#include "base/sys/clock.h"
#include "base/log/debug.h"
#include "base/log/error.h"
#include "base/log/log.h"

#ifdef HAVE_MACH_MACH_TIME_H
# include <mach/mach_time.h>
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_TIME_H
# include <time.h>
#endif

#if defined(CLOCK_REALTIME) && defined(CLOCK_MONOTONIC)

void
mm_clock_init(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
		mm_fatal(0, "clock_gettime(CLOCK_REALTIME, ...) does not seem to work");
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		mm_fatal(0, "clock_gettime(CLOCK_MONOTONIC, ...) does not seem to work");
}

mm_timeval_t
mm_clock_gettime_realtime(void)
{
	struct timespec ts;
	(void) clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

mm_timeval_t
mm_clock_gettime_monotonic(void)
{
	struct timespec ts;
	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#elif defined(HAVE_MACH_MACH_TIME_H)

static mm_timeval_t mm_abstime_numer;
static mm_timeval_t mm_abstime_denom;

void
mm_clock_init(void)
{
	mach_timebase_info_data_t timebase_info;
	(void) mach_timebase_info(&timebase_info);
	ASSERT(timebase_info.denom != 0);

	mm_abstime_numer = timebase_info.numer;
	mm_abstime_denom = timebase_info.denom * 1000LL;
}

mm_timeval_t
mm_clock_gettime_realtime(void)
{
	struct timeval tv;
	(void) gettimeofday(&tv, 0);
	return tv.tv_sec * 1000000LL + tv.tv_usec;
}

mm_timeval_t
mm_clock_gettime_monotonic(void)
{
	uint64_t at = mach_absolute_time();
	return at * mm_abstime_numer / mm_abstime_denom;
}

#else

#error "Unsupported platform"

#endif

mm_timeval_t
mm_clock_gettime(mm_clock_t clock)
{
	if (clock == MM_CLOCK_REALTIME)
		return mm_clock_gettime_realtime();
	else
		return mm_clock_gettime_monotonic();
}
