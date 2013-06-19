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
#include "clock.h"
#include "future.h"
#include "hook.h"
#include "log.h"
#include "port.h"
#include "sched.h"
#include "task.h"
#include "thread.h"
#include "timeq.h"
#include "timer.h"
#include "work.h"
#include "trace.h"

#include "dlmalloc/malloc.h"

#include <stdio.h>
#include <unistd.h>

#if ENABLE_SMP
# define MM_DEFAULT_CORES	2
#else
# define MM_DEFAULT_CORES	1
#endif

#define MM_DEFAULT_WORKERS	256

#define MM_PRIO_MASTER		1
#define MM_PRIO_WORKER		MM_PRIO_DEFAULT

#define MM_TIME_QUEUE_MAX_WIDTH	500
#define MM_TIME_QUEUE_MAX_COUNT	2000

// The core set.
static int mm_core_num;
static struct mm_core *mm_core_set;

// A core associated with the running thread.
__thread struct mm_core *mm_core;

/**********************************************************************
 * Worker task.
 **********************************************************************/

static void
mm_core_worker_cleanup(uintptr_t arg __attribute__((unused)))
{
	mm_core->nworkers--;
	//mm_sched_run(mm_core->master);
}

static mm_result_t
mm_core_worker(uintptr_t arg)
{
	ENTER();

	// Ensure cleanup on exit.
	mm_task_cleanup_push(mm_core_worker_cleanup, 0);

	struct mm_work *work = (struct mm_work *) arg;
	for (;;) {
		mm_routine_t routine = work->routine;
		uintptr_t routine_arg = work->routine_arg;
		mm_work_recycle(work);

		// Execute the work routine.
		routine(routine_arg);

		while ((work = mm_work_get()) == NULL) {
			mm_task_waitfirst(&mm_core->wait_queue);
			mm_task_testcancel();
		}
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
	mm_sched_run(task);

	LEAVE();
}

/**********************************************************************
 * Master task.
 **********************************************************************/

static void
mm_core_receive_work(struct mm_core *core)
{
	mm_core_lock(&core->inbox_lock);
	if (mm_list_empty(&core->inbox)) {
		mm_core_unlock(&core->inbox_lock);

		// TODO: allow for cross-core wakeup
		mm_task_wait(&mm_core->wait_queue);
	} else {
		struct mm_list *head = mm_list_head(&core->inbox);
		struct mm_list *tail = mm_list_tail(&core->inbox);
		mm_list_cleave(head, tail);
		mm_core_unlock(&core->inbox_lock);
		mm_list_splice_next(&core->work_queue, head, tail);
	}
}

static void
mm_core_destroy_chunks(struct mm_core *core)
{
	mm_global_lock(&core->chunks_lock);
	if (mm_list_empty(&core->chunks)) {
		mm_global_unlock(&core->chunks_lock);
	} else {
		struct mm_list *head = mm_list_head(&core->chunks);
		struct mm_list *tail = mm_list_tail(&core->chunks);
		mm_list_cleave(head, tail);
		mm_global_unlock(&core->chunks_lock);
		mm_chunk_destroy_chain(head, tail);
	}
}

static mm_result_t
mm_core_master_loop(uintptr_t arg)
{
	ENTER();

	struct mm_core *core = (struct mm_core *) arg;

	while (!mm_memory_load(core->master_stop)) {

		mm_core_destroy_chunks(core);

		if (core->nworkers < core->nworkers_max) {
			struct mm_work *work = mm_work_get();
			if (work == NULL) {
				mm_core_receive_work(mm_core);
				continue;
			}
			mm_core_worker_start(work);
		}
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
	core->master = mm_task_create("master", mm_core_master_loop, (uintptr_t) core);
	core->master->priority = MM_PRIO_MASTER;
	mm_sched_run(core->master);
}

static void
mm_core_boot_term(struct mm_core *core)
{
	mm_timeq_destroy(core->time_queue);

	mm_future_term();
	mm_timer_term();
}

/* A per-core thread entry point. */
static mm_result_t
mm_core_boot(uintptr_t arg)
{
	ENTER();

	struct mm_core *core = (struct mm_core *) arg;
	bool is_primary_core = (core == mm_core_set);

	// Set the thread-local pointer to the core object.
	mm_core = core;
	mm_core->thread = mm_thread;

	// Set the thread-local pointer to the running task.
	mm_running_task = mm_core->boot;
	mm_running_task->state = MM_TASK_RUNNING;

	// Initialize per-core resources.
	mm_core_boot_init(core);

	// Call the start hooks on the first core.
	if (is_primary_core) {
		mm_hook_call_proc(&mm_core_start_hook, false);
		mm_hook_call_data_proc(&mm_core_param_start_hook, false);
	}

	// Run the other tasks while there are any.
	mm_sched_block();

	// Call the stop hooks on the first core.
	if (is_primary_core) {
		mm_hook_call_data_proc(&mm_core_param_stop_hook, false);
		mm_hook_call_proc(&mm_core_stop_hook, false);
	}

	// Destroy per-core resources.
	mm_core_boot_term(core);

	// Invalidate the boot task.
	mm_running_task->state = MM_TASK_INVALID;
	mm_running_task = NULL;

	// Abandon the core.
	mm_flush();
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

	mm_list_init(&core->log_chunks);

	core->inbox_lock = (mm_core_lock_t) MM_ATOMIC_LOCK_INIT;
	mm_list_init(&core->inbox);

	core->chunks_lock = (mm_global_lock_t) MM_ATOMIC_LOCK_INIT;
	mm_list_init(&core->chunks);

	LEAVE();
}

static void
mm_core_term_single(struct mm_core *core)
{
	ENTER();

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

void
mm_core_init(void)
{
	ENTER();
	ASSERT(mm_core_num == 0);

	mm_clock_init();
	mm_thread_init();

	mm_task_init();
	mm_port_init();

	// TODO: get the number of available CPU cores on the system.
	mm_core_num = MM_DEFAULT_CORES;

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

/**********************************************************************
 * Core time utilities.
 **********************************************************************/

void
mm_core_update_time(void)
{
	mm_core->time_value = mm_clock_gettime_monotonic();
	DEBUG("%lld", (long long) mm_core->time_value);
}

void
mm_core_update_real_time(void)
{
	mm_core->real_time_value = mm_clock_gettime_realtime();
	DEBUG("%lld", (long long) mm_core->real_time_value);
}
