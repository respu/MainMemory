/*
 * event/epoll.h - MainMemory epoll support.
 *
 * Copyright (C) 2012-2015  Aleksey Demakov
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

#ifndef EVENT_EPOLL_H
#define EVENT_EPOLL_H

#include "common.h"

#if HAVE_SYS_EPOLL_H

#include <sys/epoll.h>

#define MM_EVENT_EPOLL_NEVENTS	(512)

/* Forward declaration. */
struct mm_event_batch;

/* Data for epoll support. */
struct mm_event_epoll
{
	/* The epoll file descriptor. */
	int event_fd;

	/* The epoll list size. */
	int nevents;

	/* The epoll list. */
	struct epoll_event events[MM_EVENT_EPOLL_NEVENTS];
};

void __attribute__((nonnull(1)))
mm_event_epoll_prepare(struct mm_event_epoll *event_backend);

void __attribute__((nonnull(1)))
mm_event_epoll_cleanup(struct mm_event_epoll *event_backend);

void __attribute__((nonnull(1, 2, 3)))
mm_event_epoll_listen(struct mm_event_epoll *event_backend,
		      struct mm_event_batch *change_events,
		      struct mm_event_batch *return_events,
		      mm_timeout_t timeout);

#endif /* HAVE_SYS_EPOLL_H */
#endif /* EVENT_EPOLL_H */
