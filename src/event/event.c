/*
 * event/event.c - MainMemory event loop.
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

#include "event/event.h"
#include "event/selfpipe.h"

#include "base/log/debug.h"
#include "base/log/trace.h"

/**********************************************************************
 * Event handlers.
 **********************************************************************/

/* Event handler table size. */
#define MM_EVENT_HANDLER_MAX	(255)

/* Event handler table. */
struct mm_event_hdesc mm_event_hdesc_table[MM_EVENT_HANDLER_MAX];

/* The number of registered event handlers. */
int mm_event_hdesc_table_size;

// A dummy event handler.
static void
mm_event_dummy(mm_event_t event __attribute__((unused)),
	       void *data __attribute__((unused)))
{
	DEBUG("hmm, dummy event handler invoked.");
}

// Initialize the event handler table.
static void
mm_event_init_handlers(void)
{
	ENTER();
	ASSERT(MM_EVENT_HANDLER_MAX < 256);

	// Register dummy handler with zero id.
	ASSERT(mm_event_hdesc_table_size == 0);
	(void) mm_event_register_handler(mm_event_dummy);
	ASSERT(mm_event_hdesc_table_size == 1);

	LEAVE();
}

/* Register an event handler in the table. */
mm_event_hid_t
mm_event_register_handler(mm_event_handler_t handler)
{
	ENTER();

	ASSERT(handler != NULL);
	ASSERT(mm_event_hdesc_table_size < MM_EVENT_HANDLER_MAX);

	mm_event_hid_t id = mm_event_hdesc_table_size++;
	mm_event_hdesc_table[id].handler = handler;

	DEBUG("registered event handler %d", id);

	LEAVE();
	return id;
}

/**********************************************************************
 * I/O events support.
 **********************************************************************/

bool
mm_event_prepare_fd(struct mm_event_fd *ev_fd, int fd, mm_core_t core,
		    mm_event_hid_t input_handler, bool input_oneshot,
		    mm_event_hid_t output_handler, bool output_oneshot,
		    mm_event_hid_t control_handler)
{
	ASSERT(fd >= 0);
	ASSERT(input_handler || output_handler || control_handler);
	ASSERT(input_handler < mm_event_hdesc_table_size);
	ASSERT(output_handler < mm_event_hdesc_table_size);
	ASSERT(control_handler < mm_event_hdesc_table_size);

	ev_fd->fd = fd;
	ev_fd->core = core;

	ev_fd->input_handler = input_handler;
	ev_fd->output_handler = output_handler;
	ev_fd->control_handler = control_handler;

	ev_fd->changed = false;
	ev_fd->oneshot_input = input_oneshot;
	ev_fd->oneshot_input_trigger = 0;
	ev_fd->oneshot_output = output_oneshot;
	ev_fd->oneshot_output_trigger = 0;

	return true;
}

/**********************************************************************
 * Event subsystem initialization.
 **********************************************************************/

void
mm_event_init(void)
{
	ENTER();

	// Initialize generic data.
	mm_event_init_handlers();
	mm_selfpipe_init();

	LEAVE();
}

/**********************************************************************
 * Event subsystem statistics.
 **********************************************************************/

void
mm_event_stats(void)
{
//	mm_selfpipe_stats();
}
