/*
 * memcache/state.h - MainMemory memcache connection state.
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

#ifndef MEMCACHE_STATE_H
#define MEMCACHE_STATE_H

#include "memcache/command.h"

#include "base/log/trace.h"
#include "net/netbuf.h"

struct mc_state
{
	// The client socket,
	struct mm_netbuf_socket sock;

	// Current parse position.
	char *start_ptr;
	// Last processed position.
	char *end_ptr;

	// Command processing queue.
	struct mc_command *command_head;
	struct mc_command *command_tail;

	// Flags.
	bool error;
	bool trash;
};

/* Net-proto routines. */
struct mm_net_socket *mc_state_alloc(void);
void mc_state_free(struct mm_net_socket *sock);
void mc_state_prepare(struct mm_net_socket *sock);
void mc_state_cleanup(struct mm_net_socket *sock);

static inline void
mc_queue_command(struct mc_state *state,
		 struct mc_command *first,
		 struct mc_command *last)
{
	ENTER();
	ASSERT(first != NULL);
	ASSERT(last != NULL);

	if (state->command_head == NULL) {
		state->command_head = first;
	} else {
		state->command_tail->next = first;
	}
	state->command_tail = last;

	LEAVE();
}

#endif /* MEMCACHE_STATE_H */
