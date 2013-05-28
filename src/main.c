/*
 * daemon.c - MainMemory main routine.
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

#include "common.h"
#include "core.h"
#include "event.h"
#include "log.h"
#include "net.h"
#include "trace.h"

#include "memcache/memcache.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

static struct mm_net_server *mm_ucmd_server;
static struct mm_net_server *mm_icmd_server;

static void
mm_term_handler(int signo __attribute__((unused)))
{
	ENTER();

	mm_core_stop();
	mm_exit_set();

	LEAVE();
}

static void
mm_signal(int signo, void (*handler)(int))
{
	ENTER();

	struct sigaction sa;
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(signo, &sa, NULL) == -1) {
		mm_fatal(errno, "Failed sigaction() call");
	}

	LEAVE();
}

static void
mm_signal_init(void)
{
	ENTER();

	mm_signal(SIGINT, mm_term_handler);
	mm_signal(SIGTERM, mm_term_handler);

	LEAVE();
}

static void
mm_init(void)
{
	ENTER();

	// Set signal handlers.
	mm_signal_init();

	// Initialize subsystems.
	mm_core_init();
	mm_event_init();
	mm_net_init();

	LEAVE();
}

static void
mm_term(void)
{
	ENTER();

	// Terminate subsystems.
	mm_net_term();
	mm_event_term();
	mm_core_term();

	LEAVE();
}

static void
mm_cmd_reader(struct mm_net_socket *sock)
{
	ENTER();

	char buf[1026];
	int n = mm_net_read(sock, buf, sizeof(buf));
	if (n <= 0) {
		if (n < 0)
			mm_error(errno, "read()");
		mm_net_close(sock);
	}

	mm_net_write(sock, "test\n", 5);
	mm_net_close(sock);

	LEAVE();
}

static void
mm_server_open(void)
{
	ENTER();

	static struct mm_net_proto proto = {
		.flags = MM_NET_INBOUND,
		.prepare = NULL,
		.cleanup = NULL,
		.reader_routine = mm_cmd_reader,
		.writer_routine = NULL,
	};

	mm_ucmd_server = mm_net_create_unix_server("test", "mm_cmd.sock");
	mm_icmd_server = mm_net_create_inet_server("test", "127.0.0.1", 8000);

	//mm_net_start_server(mm_ucmd_server, &proto);
	mm_net_start_server(mm_icmd_server, &proto);

	mm_memcache_init();

	LEAVE();
}

static void
mm_server_close(void)
{
	ENTER();

	mm_memcache_term();

	LEAVE();
}

int
main(/*int ac, char *av[]*/)
{
	ENTER();

	/* Initialize. */
	mm_init();

	/* Start server. */
	mm_server_open();

	/* Execute main loop. */
	mm_core_start();

	/* Shutdown server. */
	mm_server_close(); 

	/* Terminate. */
	mm_term();

	LEAVE();
	return EXIT_SUCCESS;
}
