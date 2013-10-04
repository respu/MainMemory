/*
 * core.c - MainMemory core.
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

#include "core.h"

#include "alloc.h"
#include "chunk.h"
#include "future.h"
#include "hook.h"
#include "log.h"
#include "net.h"
#include "port.h"
#include "task.h"
#include "thread.h"
#include "timeq.h"
#include "timer.h"
#include "trace.h"

#include "dlmalloc/malloc.h"

#include <stdio.h>
#include <unistd.h>

#define MM_DEFAULT_CORES	1
#define MM_DEFAULT_WORKERS	256

#define MM_PRIO_MASTER		1
#define MM_PRIO_WORKER		MM_PRIO_DEFAULT
#define MM_PRIO_DEALER		MM_PRIO_IDLE

// Default dealer loop timeout - 1 second
#define MM_DEALER_TIMEOUT ((mm_timeout_t) 1000000)

#define MM_TIME_QUEUE_MAX_WIDTH	500
#define MM_TIME_QUEUE_MAX_COUNT	2000

// The core set.
static int mm_core_num;
static struct mm_core *mm_core_set;

// A core associated with the running thread.
__thread struct mm_core *mm_core;

#define MM_CORE_IS_PRIMARY(core)	(core == mm_core_set)

/**********************************************************************
 * Work queue.
 **********************************************************************/

/* A work item. */
struct mm_work
{
	/* A link in the work queue. */
	struct mm_list queue;

	/* The work is pinned to a specific core. */
	bool pinned;

	/* The work routine. */
	mm_routine_t routine;
	/* The work routine argument. */
	uintptr_t routine_arg;
};

static struct mm_work *
mm_core_create_work(struct mm_core *core,
		    mm_routine_t routine, uintptr_t routine_arg,
		    bool pinned)
{
	ENTER();

	struct mm_work *work;
	if (mm_list_empty(&core->work_cache)) {
		/* Create a new work item. */
		work = mm_alloc(sizeof(struct mm_core));
	} else {
		/* Reuse a cached work item. */
		struct mm_list *link = mm_list_delete_head(&core->work_cache);
		work = containerof(link, struct mm_work, queue);
	}

	work->pinned = pinned;
	work->routine = routine;
	work->routine_arg = routine_arg;

	LEAVE();
	return work;
}

static void
mm_core_destroy_work(struct mm_work *work)
{
	ENTER();

	mm_free(work);

	LEAVE();
}

static void
mm_core_add_work(struct mm_core *core, struct mm_work *work)
{
	ENTER();

	// Enqueue the item in the LIFO order.
	mm_list_insert(&core->work_queue, &work->queue);

	// If there is a task waiting for work then let it run now.
	mm_task_signal(&core->wait_queue);

	LEAVE();
}

void
mm_core_post(bool pinned, mm_routine_t routine, uintptr_t routine_arg)
{
	ENTER();

	// Create a work item.
	struct mm_work *work = mm_core_create_work(mm_core,
						   routine, routine_arg,
						   pinned);

	// Enqueue it.
	mm_core_add_work(mm_core, work);

	LEAVE();
}

void
mm_core_submit(struct mm_core *core, mm_routine_t routine, uintptr_t routine_arg)
{
	ENTER();
	ASSERT(mm_core != NULL);

	// Create a work item.
	struct mm_work *work = mm_core_create_work(core,
						   routine, routine_arg,
						   true);

	if (core == mm_core) {
		// Enqueue it directly if on the same core.
		mm_core_add_work(core, work);
	} else {
		// Put the item to the target core inbox.
		while (!mm_ring_core_put(&core->inbox, work)) {
			mm_task_yield();
		}

		// Wakeup the target core if it is asleep.
		if (MM_CORE_IS_PRIMARY(core)) {
			mm_event_notify();
		} else {
			mm_thread_signal(core->thread);
		}
	}

	LEAVE();
}

/**********************************************************************
 * Worker task.
 **********************************************************************/

static void
mm_core_worker_cleanup(uintptr_t arg __attribute__((unused)))
{
	// Wake up the master possibly waiting for worker availability.
	if (mm_core->nworkers == mm_core->nworkers_max) {
		mm_task_run(mm_core->master);
	}

	// Account for the exiting worker.
	mm_core->nworkers--;
}

static mm_result_t
mm_core_worker(uintptr_t arg)
{
	ENTER();

	// Ensure cleanup on exit.
	mm_task_cleanup_push(mm_core_worker_cleanup, 0);

	// Get initial work item.
	struct mm_work *work = (struct mm_work *) arg;

	// Cache thread-specific data. This gives a smallish speedup for
	// the code emitted for the loop below on platforms with emulated
	// thread specific data (that is on Darwin).
	struct mm_core *core = mm_core;

	for (;;) {
		// Save the work routine and recycle the work item.
		mm_routine_t routine = work->routine;
		uintptr_t routine_arg = work->routine_arg;
		mm_list_insert(&core->work_cache, &work->queue);

		// Execute the work routine.
		routine(routine_arg);

		// Check to see if there is more work available.
		if (mm_list_empty(&core->work_queue)) {
			// Wait for work standing at the front of the wait queue.
			mm_task_waitfirst(&core->wait_queue);
			if (mm_list_empty(&core->work_queue)) {
				break;
			}
		}

		// Take the first available work item.
		struct mm_list *link = mm_list_delete_head(&core->work_queue);
		work = containerof(link, struct mm_work, queue);
	}

	// Cleanup on return.
	mm_task_cleanup_pop(true);

	LEAVE();
	return 0;
}

static void
mm_core_worker_start(struct mm_work *work)
{
	ENTER();

	struct mm_task *task = mm_task_create("worker",
					      mm_core_worker,
					      (uintptr_t) work);
	task->priority = MM_PRIO_WORKER;
	mm_core->nworkers++;
	mm_task_run(task);

	LEAVE();
}

/**********************************************************************
 * Master task.
 **********************************************************************/

static mm_result_t
mm_core_master(uintptr_t arg)
{
	ENTER();

	struct mm_core *core = (struct mm_core *) arg;

	while (!mm_memory_load(core->master_stop)) {

		// Check to see if there are workers available.
		if (core->nworkers >= core->nworkers_max) {
			mm_task_block();
			continue;
		}

		// Check to see if there is some work available.
		if (mm_list_empty(&core->work_queue)) {
			// Wait for work at the back end of the wait queue.
			// So any idle worker would take work over the master.
			mm_task_wait(&core->wait_queue);
			continue;
		}

		// Take the first available work item.
		struct mm_list *link = mm_list_delete_head(&core->work_queue);
		struct mm_work *work = containerof(link, struct mm_work, queue);

		// Pass it to a new worker.
		mm_core_worker_start(work);
	}

	LEAVE();
	return 0;
}

/**********************************************************************
 * Dealer task.
 **********************************************************************/

#if ENABLE_SMP
static bool
mm_core_receive_work(struct mm_core *core)
{
	struct mm_work *work = mm_ring_get(&core->inbox);
	if (work == NULL)
		return false;

	do {
		mm_core_add_work(core, work);
		work = mm_ring_get(&core->inbox);
	} while (work != NULL);

	return true;
}
#else
# define mm_core_receive_work(core) ({(void) core; false})
#endif

static bool
mm_core_destroy_chunks(struct mm_core *core)
{
	struct mm_chunk *chunk = mm_ring_get(&core->chunks);
	if (chunk == NULL)
		return false;

	do {
		mm_chunk_destroy(chunk);
		chunk = mm_ring_get(&core->chunks);
	} while (chunk != NULL);

	return true;
}

static mm_result_t
mm_core_dealer(uintptr_t arg)
{
	ENTER();

	struct mm_core *core = (struct mm_core *) arg;

	while (!mm_memory_load(core->master_stop)) {
		if (!mm_core_receive_work(core)) {
			if (MM_CORE_IS_PRIMARY(core)) {
				mm_event_dispatch(MM_DEALER_TIMEOUT);
			} else {
				mm_thread_timedwait(MM_DEALER_TIMEOUT);
			}
		}
		mm_core_destroy_chunks(core);
		mm_timer_tick();
		mm_task_yield();
	}

	LEAVE();
	return 0;
}

/**********************************************************************
 * Core start and stop hooks.
 **********************************************************************/

static struct mm_hook mm_core_start_hook;
static struct mm_hook mm_core_param_start_hook;

static struct mm_hook mm_core_stop_hook;
static struct mm_hook mm_core_param_stop_hook;

static void
mm_core_free_hooks(void)
{
	ENTER();

	mm_hook_free(&mm_core_start_hook);
	mm_hook_free(&mm_core_param_start_hook);
	mm_hook_free(&mm_core_stop_hook);
	mm_hook_free(&mm_core_param_stop_hook);

	LEAVE();
}

void
mm_core_hook_start(void (*proc)(void))
{
	ENTER();

	mm_hook_tail_proc(&mm_core_start_hook, proc);

	LEAVE();
}

void
mm_core_hook_param_start(void (*proc)(void *), void *data)
{
	ENTER();

	mm_hook_tail_data_proc(&mm_core_param_start_hook, proc, data);

	LEAVE();
}

void
mm_core_hook_stop(void (*proc)(void))
{
	ENTER();

	mm_hook_tail_proc(&mm_core_stop_hook, proc);

	LEAVE();
}

void
mm_core_hook_param_stop(void (*proc)(void *), void *data)
{
	ENTER();

	mm_hook_tail_data_proc(&mm_core_param_stop_hook, proc, data);

	LEAVE();
}

/**********************************************************************
 * Core initialization and termination.
 **********************************************************************/

static void
mm_core_boot_init(struct mm_core *core)
{
	mm_timer_init();
	mm_future_init();

	// Update the time.
	mm_core_update_time();
	mm_core_update_real_time();

	// Create the time queue.
	core->time_queue = mm_timeq_create();
	mm_timeq_set_max_bucket_width(core->time_queue, MM_TIME_QUEUE_MAX_WIDTH);
	mm_timeq_set_max_bucket_count(core->time_queue, MM_TIME_QUEUE_MAX_COUNT);

	// Create the master task for this core and schedule it for execution.
	core->master = mm_task_create("master", mm_core_master, (uintptr_t) core);
	core->master->priority = MM_PRIO_MASTER;
	mm_task_run(core->master);

	// Create the dealer task for this core and schedule it for execution.
	core->dealer = mm_task_create("dealer", mm_core_dealer, (uintptr_t) core);
	core->dealer->priority = MM_PRIO_DEALER;
	mm_task_run(core->dealer);

	// Call the start hooks on the first core.
	if (MM_CORE_IS_PRIMARY(core)) {
		mm_hook_call_proc(&mm_core_start_hook, false);
		mm_hook_call_data_proc(&mm_core_param_start_hook, false);
	}
}

static void
mm_core_boot_term(struct mm_core *core)
{
	// Call the stop hooks on the first core.
	if (MM_CORE_IS_PRIMARY(core)) {
		mm_hook_call_data_proc(&mm_core_param_stop_hook, false);
		mm_hook_call_proc(&mm_core_stop_hook, false);
	}

	mm_timeq_destroy(core->time_queue);

	mm_future_term();
	mm_timer_term();

	// TODO:
	//mm_task_destroy(core->master);
	//mm_task_destroy(core->dealer);
}

/* A per-core thread entry point. */
static mm_result_t
mm_core_boot(uintptr_t arg)
{
	ENTER();

	struct mm_core *core = (struct mm_core *) arg;

	// Set the thread-local pointer to the core object.
	mm_core = core;
	mm_core->thread = mm_thread;

	// Set the thread-local pointer to the running task.
	mm_running_task = mm_core->boot;
	mm_running_task->state = MM_TASK_RUNNING;

	// Initialize per-core resources.
	mm_core_boot_init(core);

	// Run the other tasks while there are any.
	mm_task_yield();

	// Destroy per-core resources.
	mm_core_boot_term(core);

	// Invalidate the boot task.
	mm_running_task->state = MM_TASK_INVALID;
	mm_running_task = NULL;

	// Abandon the core.
	mm_core = NULL;

	LEAVE();
	return 0;
}

static void
mm_core_init_single(struct mm_core *core, uint32_t nworkers_max)
{
	ENTER();

	mm_runq_init(&core->run_queue);
	mm_list_init(&core->work_queue);
	mm_list_init(&core->work_cache);
	mm_list_init(&core->wait_queue);

	core->arena = create_mspace(0, 0);

	core->time_queue = NULL;
	core->time_value = 0;
	core->real_time_value = 0;

	core->nworkers = 0;
	core->nworkers_max = nworkers_max;
	mm_list_init(&core->dead_list);

	core->master_stop = false;
	core->master = NULL;
	core->boot = mm_task_create_boot();
	core->thread = NULL;

	core->log_head = NULL;
	core->log_tail = NULL;

	mm_ring_prepare(&core->sched, MM_CORE_SCHED_RING_SIZE);
	mm_ring_prepare(&core->inbox, MM_CORE_INBOX_RING_SIZE);
	mm_ring_prepare(&core->chunks, MM_CORE_CHUNK_RING_SIZE);

	LEAVE();
}

static void
mm_core_term_work_queue(struct mm_list *queue)
{
	while (!mm_list_empty(queue)) {
		struct mm_list *link = mm_list_delete_head(queue);
		struct mm_work *work = containerof(link, struct mm_work, queue);
		mm_core_destroy_work(work);
	}
}

static void
mm_core_term_inbox(struct mm_core *core)
{
	struct mm_work *work = mm_ring_get(&core->inbox);
	while (work != NULL) {
		mm_core_destroy_work(work);
		work = mm_ring_get(&core->inbox);
	}
}

static void
mm_core_term_single(struct mm_core *core)
{
	ENTER();

	mm_core_term_work_queue(&core->work_queue);
	mm_core_term_work_queue(&core->work_cache);
	mm_core_term_inbox(core);

	mm_thread_destroy(core->thread);
	mm_task_destroy(core->boot);

	destroy_mspace(core->arena);

	LEAVE();
}

static void
mm_core_start_single(struct mm_core *core, int core_tag)
{
	ENTER();

	// Concoct a thread name.
	char name[MM_THREAD_NAME_SIZE];
	sprintf(name, "core %d", core_tag);

	// Set thread attributes.
	struct mm_thread_attr attr;
	mm_thread_attr_init(&attr);
	mm_thread_attr_setname(&attr, name);
	mm_thread_attr_setstack(&attr,
				core->boot->stack_base,
				core->boot->stack_size);
	mm_thread_attr_setcputag(&attr, core_tag);

	// Create a core thread.
	core->thread = mm_thread_create(&attr, &mm_core_boot, (uintptr_t) core);

	LEAVE();
}

static int
mm_core_get_num(void)
{
#if ENABLE_SMP
# if defined(_SC_NPROCESSORS_ONLN)
	int nproc_onln = sysconf(_SC_NPROCESSORS_ONLN);
	if (nproc_onln < 0)
		mm_fatal(errno, "Failed to count cores.");
	return nproc_onln;
# else
#  error "Unsupported SMP architecture."
# endif
#endif
	return MM_DEFAULT_CORES;
}

void
mm_core_init(void)
{
	ENTER();
	ASSERT(mm_core_num == 0);

	dlmallopt(M_GRANULARITY, 16 * MM_PAGE_SIZE);

	mm_core_num = mm_core_get_num();
	ASSERT(mm_core_num > 0);
	if (mm_core_num == 1)
		mm_brief("Running on 1 core.");
	else
		mm_brief("Running on %d cores.", mm_core_num);

	mm_clock_init();
	mm_thread_init();

	mm_task_init();
	mm_port_init();

	mm_core_set = mm_alloc_aligned(MM_CACHELINE, mm_core_num * sizeof(struct mm_core));
	for (int i = 0; i < mm_core_num; i++)
		mm_core_init_single(&mm_core_set[i], MM_DEFAULT_WORKERS);

	LEAVE();
}

void
mm_core_term(void)
{
	ENTER();
	ASSERT(mm_core_num > 0);

	for (int i = 0; i < mm_core_num; i++)
		mm_core_term_single(&mm_core_set[i]);
	mm_free(mm_core_set);

	mm_core_free_hooks();

	mm_task_term();
	mm_port_term();

	mm_thread_term();

	LEAVE();
}

void
mm_core_register_server(struct mm_net_server *srv)
{
	ENTER();

	// Register the server start hook.
	mm_core_hook_param_start((mm_hook_rtn1) mm_net_start_server, srv);

	// Register the server stop hook.
	mm_core_hook_param_stop((mm_hook_rtn1) mm_net_stop_server, srv);

	LEAVE();
}

void
mm_core_start(void)
{
	ENTER();
	ASSERT(mm_core_num > 0);

	// Start core threads.
	for (int i = 0; i < mm_core_num; i++)
		mm_core_start_single(&mm_core_set[i], i);

	// Loop until stopped.
	while (!mm_exit_test()) {
		size_t logged = mm_log_write();
		usleep(logged ? 10000 : 1000000);
		DEBUG("cycle");
	}

	// Wait for core threads completion.
	for (int i = 0; i < mm_core_num; i++)
		mm_thread_join(mm_core_set[i].thread);

 	LEAVE();
}

void
mm_core_stop(void)
{
	ENTER();
	ASSERT(mm_core_num > 0);

	// Set stop flag for core threads.
	for (int i = 0; i < mm_core_num; i++)
		mm_memory_store(mm_core_set[i].master_stop, true);

	LEAVE();
}
