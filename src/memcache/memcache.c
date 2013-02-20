/*
 * memcache.c - MainMemory memcached protocol support.
 *
 * Copyright (C) 2012-2013  Aleksey Demakov
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

#include "memcache.h"

#include "../future.h"
#include "../list.h"
#include "../net.h"
#include "../pool.h"
#include "../util.h"
#include "../work.h"

#include <ctype.h>
#include <stdlib.h>
#include <sys/mman.h>

/**********************************************************************
 * Hash function.
 **********************************************************************/

/*
 * The Fowler/Noll/Vo (FNV) hash function, variant 1a.
 * 
 * http://www.isthe.com/chongo/tech/comp/fnv/index.html
 */

#define FNV1_32_INIT ((uint32_t) 0x811c9dc5)
#define FNV_32_PRIME ((uint32_t) 0x01000193)

static uint32_t
mc_hash(const void *data, size_t size)
{
	const unsigned char *p = (unsigned char *) data;
	const unsigned char *e = (unsigned char *) data + size;

	uint32_t h = FNV1_32_INIT;
	while (p < e) {
		h ^= (uint32_t) *p++;
		h *= FNV_32_PRIME;
	}

	return h;
}

/**********************************************************************
 * Memcache Table.
 **********************************************************************/

#define MC_ENTRY_ADD		1
#define MC_ENTRY_REPLACE	2

#define MC_TABLE_STRIDE		64

#define MC_TABLE_SIZE_MIN	((size_t) 4 * 1024)

#if 0
# define MC_TABLE_SIZE_MAX	((size_t) 64 * 1024 * 1024)
#else
# define MC_TABLE_SIZE_MAX	((size_t) 512 * 1024 * 1024)
#endif

struct mc_entry
{
	struct mc_entry *next;
	uint32_t data_len;
	uint8_t key_len;
	char data[];
};

struct mc_table
{
	uint32_t mask;
	uint32_t size;
	uint32_t used;

	bool striding;

	size_t nentries;

	struct mc_entry **table;
};

struct mc_table mc_table;

static inline size_t
mc_entry_nbytes(uint8_t key_len, size_t data_len)
{
	return sizeof(struct mc_entry) + key_len + data_len;
}

static inline size_t
mc_table_nbytes(size_t nbuckets)
{
	return nbuckets * sizeof (struct mc_entry *);
}

static uint32_t
mc_table_index(uint32_t h)
{
	uint32_t mask = mc_table.mask;
	uint32_t index = h & mask;
	if (index >= mc_table.used)
		index &= mask >> 1;
	return index;
}

static inline bool
mc_table_is_full(void)
{
	if (unlikely(mc_table.size == MC_TABLE_SIZE_MAX)
	    && unlikely(mc_table.used == mc_table.size))
		return false;
	return mc_table.nentries > (mc_table.size * 2);
}

static void
mc_table_expand(size_t size)
{
	ENTER();
	ASSERT(size > mc_table.size);
	/* Assert the size is a power of 2. */
	ASSERT((size & (size - 1)) == 0);

	size_t old_nbytes = mc_table_nbytes(mc_table.size);
	size_t new_nbytes = mc_table_nbytes(size);

	void *address = (char *) mc_table.table + old_nbytes;
	size_t nbytes = new_nbytes - old_nbytes;

	void *area = mmap(address, nbytes, PROT_READ | PROT_WRITE,
			  MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if (area == MAP_FAILED)
		mm_fatal(errno, "mmap");
	if (area != address)
		mm_fatal(0, "mmap returned wrong address");

	mc_table.size = size;
	mc_table.mask = size - 1;

	LEAVE();
}

static void
mc_table_stride(void)
{
	ENTER();
	ASSERT(mc_table.used < mc_table.size);

	uint32_t mask = mc_table.mask;
	uint32_t target = mc_table.used;
	uint32_t source = target - mc_table.size / 2;

	for (uint32_t count = 0; count < MC_TABLE_STRIDE; count++) {
		struct mc_entry *entry = mc_table.table[source];

		struct mc_entry *s_entries = NULL;
		struct mc_entry *t_entries = NULL;
		while (entry != NULL) {
			struct mc_entry *next = entry->next;

			uint32_t h = mc_hash(entry->data, entry->key_len);
			uint32_t index = h & mask;
			if (index == source) {
				entry->next = s_entries;
				s_entries = entry;
			} else {
				ASSERT(index == target);
				entry->next = t_entries;
				t_entries = entry;
			}

			entry = next;
		}

		mc_table.table[source++] = s_entries;
		mc_table.table[target++] = t_entries;
	}

	mc_table.used += MC_TABLE_STRIDE;

	LEAVE();
}

static void
mc_table_init(void)
{
	ENTER();

	/* Compute the maximal size of the table in bytes. */
	size_t nbytes = mc_table_nbytes(MC_TABLE_SIZE_MAX);

	/* Reserve the address space for the table. */
	mm_print("Reserve %ld bytes of the address apace for the memcache table.", (unsigned long) nbytes);
	void *area = mmap(NULL, nbytes, PROT_NONE,
			  MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	if (area == MAP_FAILED)
		mm_fatal(errno, "mmap");

	/* Initialize the table. */
	mc_table.size = 0;
	mc_table.mask = 0;
	mc_table.striding = false;
	mc_table.nentries = 0;
	mc_table.table = area;

	/* Allocate initial space for the table. */
	mc_table_expand(MC_TABLE_SIZE_MIN);
	mc_table.used = MC_TABLE_SIZE_MIN;

	LEAVE();
}

static void
mc_table_term(void)
{
	ENTER();

	for (uint32_t index = 0; index < mc_table.used; index++) {
		struct mc_entry *entry = mc_table.table[index];
		while (entry != NULL) {
			struct mc_entry *next = entry->next;
			mm_free(entry);
			entry = next;
		}
	}

	munmap(mc_table.table, mc_table_nbytes(mc_table.size));

	LEAVE();
}

static void mc_table_start_striding(void);

static void
mc_table_stride_routine(uintptr_t arg __attribute__((unused)))
{
	ENTER();
	ASSERT(mc_table.striding);

	if (unlikely(mc_table.used == mc_table.used))
		mc_table_expand(mc_table.size * 2);

	mc_table_stride();

	if (mc_table_is_full())
		mc_table_start_striding();
	else
		mc_table.striding = false;

	LEAVE();
}

static void
mc_table_start_striding(void)
{
	ENTER();

	mm_work_add(0, mc_table_stride_routine, 0);

	LEAVE();
}

static struct mc_entry *
mc_table_create_entry(const char *key, uint8_t key_len,
		      const char *data, size_t data_len)
{
	ENTER();

	size_t nbytes = mc_entry_nbytes(key_len, data_len);
	struct mc_entry *entry = mm_alloc(nbytes);
	entry->key_len = key_len;
	entry->data_len = data_len;

	char *entry_key = entry->data;
	memcpy(entry_key, key, key_len);

	char *entry_data = entry_key + entry->key_len;
	memcpy(entry_data, data, data_len);

	++mc_table.nentries;
	if (!mc_table.striding && mc_table_is_full()) {
		mc_table.striding = true;
		mc_table_start_striding();
	}

	LEAVE();
	return entry;
}

static void
mc_table_destroy_entry(struct mc_entry *entry)
{
	ENTER();

	--mc_table.nentries;
	mm_free(entry);

	LEAVE();
}

static struct mc_entry *
mc_table_lookup_entry(const char *key, uint8_t key_len)
{
	ENTER();

	uint32_t h = mc_hash(key, key_len);
	uint32_t index = mc_table_index(h);

	struct mc_entry *entry = mc_table.table[index];
	for (;;) {
		if (entry == NULL)
			break;
		if (key_len == entry->key_len && !memcmp(key, entry->data, key_len))
			break;
		entry = entry->next;
	}

	LEAVE();
	return entry;
}

static int
mc_table_insert_entry(const char *key, uint8_t key_len,
		      const char *data, size_t data_len,
		      int flags)
{
	ENTER();

	int rc = 0;

	uint32_t h = mc_hash(key, key_len);
	uint32_t index = mc_table_index(h);

	struct mc_entry **entry_ref = &mc_table.table[index];
	for (;;) {
		struct mc_entry *entry = *entry_ref;
		if (entry == NULL) {
			if ((flags & MC_ENTRY_REPLACE) != 0) {
				rc = -1;
				break;
			}

			entry = mc_table_create_entry(key, key_len, data, data_len);
			entry->next = NULL;
			*entry_ref = entry;
			break;
		}
		if (key_len == entry->key_len && !memcmp(key, entry->data, key_len)) {
			if ((flags & MC_ENTRY_ADD) != 0) {
				rc = -1;
				break;
			}

			*entry_ref = entry->next;
			mm_free(entry);

			entry = mc_table_create_entry(key, key_len, data, data_len);
			entry->next = *entry_ref;
			*entry_ref = entry;
			break;
		}
		entry_ref = &entry->next;
	}

	LEAVE();
	return rc;
}

static bool
mc_table_delete_entry(const char *key, uint8_t key_len)
{
	ENTER();

	bool found = false;
	uint32_t h = mc_hash(key, key_len);
	uint32_t index = mc_table_index(h);

	struct mc_entry **entry_ref = &mc_table.table[index];
	for (;;) {
		struct mc_entry *entry = *entry_ref;
		if (entry == NULL)
			break;
		if (key_len == entry->key_len && !memcmp(key, entry->data, key_len)) {
			*entry_ref = entry->next;
			mc_table_destroy_entry(entry);
			found = true;
			break;
		}
		entry_ref = &entry->next;
	}

	LEAVE();
	return found;
}

/**********************************************************************
 * Protocol I/O Buffer.
 **********************************************************************/

#define MC_DEFAULT_BUFFER_SIZE	4000

struct mc_buffer
{
	struct mc_buffer *next;
	size_t size;
	size_t used;
	char data[];
};

static struct mc_buffer *
mc_buffer_create(size_t size)
{
	ENTER();

	size_t total_size = sizeof(struct mc_buffer) + size;
	struct mc_buffer *buffer = mm_alloc(total_size);
	buffer->next = NULL;
	buffer->size = size;
	buffer->used = 0;

	LEAVE();
	return buffer;
}

static void
mc_buffer_destroy(struct mc_buffer *buffer)
{
	ENTER();

	mm_free(buffer);

	LEAVE();
}

static inline bool
mc_buffer_contains(struct mc_buffer *buffer, const char *ptr)
{
	return ptr >= buffer->data && ptr < (buffer->data + buffer->size);
}

static inline bool
mc_buffer_finished(struct mc_buffer *buffer, const char *ptr)
{
	return ptr == (buffer->data + buffer->size);
}

/**********************************************************************
 * Command Data.
 **********************************************************************/

typedef enum
{
	MC_RESULT_NONE,
	MC_RESULT_REPLY,
	MC_RESULT_ENTRY,
	MC_RESULT_BLANK,
} mc_result_t;

struct mc_string
{
	size_t len;
	const char *str;
};

struct mc_get_params
{
	struct mc_string *keys;
	uint32_t nkeys;
};

struct mc_set_params
{
	struct mc_string key;
	uint32_t flags;
	uint32_t exptime;
	uint32_t bytes;
	bool noreply;
};

struct mc_cas_params
{
	struct mc_string key;
	uint32_t flags;
	uint32_t exptime;
	uint32_t bytes;
	bool noreply;
	uint64_t cas;
};

struct mc_inc_params
{
	struct mc_string key;
	uint64_t value;
	bool noreply;
};

struct mc_del_params
{
	struct mc_string key;
	bool noreply;
};

struct mc_touch_params
{
	struct mc_string key;
	uint32_t exptime;
	bool noreply;
};

struct mc_flush_params
{
	uint32_t exptime;
	bool noreply;
};

union mc_params
{
	struct mc_set_params set;
	struct mc_get_params get;
	struct mc_cas_params cas;
	struct mc_inc_params inc;
	struct mc_del_params del;
	struct mc_touch_params touch;
	struct mc_flush_params flush;
};

struct mc_command
{
	struct mc_command *next;

	struct mc_command_desc *desc;
	union mc_params params;
	char *end_ptr;

	mc_result_t result_type;
	struct mc_string reply;
	struct mc_entry *entry;

	struct mc_future *future;
};


static struct mm_pool mc_command_pool;


static void
mc_command_init(void)
{
	ENTER();

	mm_pool_init(&mc_command_pool, "memcache command", sizeof(struct mc_command));

	LEAVE();
}

static void
mc_command_term()
{
	ENTER();

	mm_pool_discard(&mc_command_pool);

	LEAVE();
}

static struct mc_command *
mc_command_create(void)
{
	ENTER();

	struct mc_command *command = mm_pool_alloc(&mc_command_pool);
	command->next = NULL;
	command->desc = NULL;
	command->end_ptr = NULL;
	command->result_type = MC_RESULT_NONE;
	command->future = NULL;

	LEAVE();
	return command;
}

static void
mc_command_destroy(struct mc_command *command)
{
	ENTER();

	mm_pool_free(&mc_command_pool, command);

	LEAVE();
}

/**********************************************************************
 * Aggregate Connection State.
 **********************************************************************/

struct mc_state
{
	char *start_ptr;

	/* Input buffer queue. */
	struct mc_buffer *read_head;
	struct mc_buffer *read_tail;

	/* Cached command. */
	struct mc_command *command;

	/* Command processing queue. */
	struct mc_command *command_head;
	struct mc_command *command_tail;

	bool quit, quit_fast;
};


static struct mc_state *
mc_create(void)
{
	ENTER();

	struct mc_state *state = mm_alloc(sizeof(struct mc_state));
	state->start_ptr = NULL;

	state->read_head = NULL;
	state->read_tail = NULL;

	state->command = NULL;

	state->command_head = NULL;
	state->command_tail = NULL;

	state->quit = state->quit_fast = false;

	LEAVE();
	return state;
}

static void
mc_destroy(struct mc_state *state)
{
	ENTER();

	while (state->read_head != NULL) {
		struct mc_buffer *buffer = state->read_head;
		state->read_head = buffer->next;
		mc_buffer_destroy(buffer);
	}

	while (state->command_head != NULL) {
		struct mc_command *command = state->command_head;
		state->command_head = command->next;
		mc_command_destroy(command);
	}

	if (state->command != NULL) {
		mc_command_destroy(state->command);
	}

	mm_free(state);

	LEAVE();
}

static void
mc_quit(struct mc_state *state, bool fast)
{
	ENTER();

	state->quit = true;
	state->quit_fast = fast;

	LEAVE();
}

static struct mc_buffer *
mc_add_read_buffer(struct mc_state *state, size_t size)
{
	ENTER();

	struct mc_buffer *buffer = mc_buffer_create(size);
	if (state->read_head == NULL) {
		state->read_head = buffer;
		state->start_ptr = buffer->data;
	} else {
		state->read_tail->next = buffer;
	}
	state->read_tail = buffer;

	LEAVE();
	return buffer;
}

static struct mc_command *
mc_cache_command(struct mc_state *state)
{
	ENTER();

	struct mc_command *command = state->command;
	if (command == NULL) {
		command = state->command = mc_command_create();
	}

	LEAVE();
	return command;
}

static void
mc_queue_command(struct mc_state *state)
{
	ENTER();

	struct mc_command *command = state->command;
	ASSERT(command != NULL);
	state->command = NULL;

	if (state->command_head == NULL)
		state->command_head = command;
	else
		state->command_tail->next = command;
	state->command_tail = command;

	LEAVE();
}

static void
mc_release_buffers(struct mc_state *state, const char *ptr)
{
	ENTER();

	while (state->read_head != NULL) {
		struct mc_buffer *buffer = state->read_head;
		if (mc_buffer_contains(buffer, ptr)) {
			/* The buffer is (might be) still in use. */
			break;
		}

		ASSERT(!mc_buffer_contains(buffer, state->start_ptr));

		if (mc_buffer_finished(buffer, ptr)) {
			/* The buffer has been used up to its end. */
			if (buffer->next == NULL) {
				state->read_head = state->read_tail = NULL;
				state->start_ptr = NULL;
			} else {
				state->read_head = buffer->next;
				if (state->start_ptr == ptr) {
					state->start_ptr = state->read_head->data;
				}
			}
			mc_buffer_destroy(buffer);
			break;
		}

		/* The buffer use is long past. */
		state->read_head = buffer->next;
		mc_buffer_destroy(buffer);
	}

	LEAVE();
}

/**********************************************************************
 * Command Descriptors.
 **********************************************************************/

/* Forward declaration. */
struct mc_parser;

/* Command parsing routine. */
typedef bool (*mc_parse_routine)(struct mc_parser *parser);

/* Command parsing and processing info. */
struct mc_command_desc
{
	const char *name;
	mc_parse_routine parse;
	mm_routine process;
};

#define MC_COMMAND_DESC(cmd, parse_name, process_name)		\
	static bool mc_parse_##parse_name(struct mc_parser *);	\
	static void mc_process_##process_name(uintptr_t);	\
	static struct mc_command_desc mc_desc_##cmd = {		\
		.name = #cmd,					\
		.parse = mc_parse_##parse_name,			\
		.process = mc_process_##process_name,		\
	}

MC_COMMAND_DESC(get, get, get);
MC_COMMAND_DESC(gets, get, gets);
MC_COMMAND_DESC(set, set, set);
MC_COMMAND_DESC(add, set, add);
MC_COMMAND_DESC(replace, set, replace);
MC_COMMAND_DESC(append, set, append);
MC_COMMAND_DESC(prepend, set, prepend);
MC_COMMAND_DESC(cas, cas, cas);
MC_COMMAND_DESC(incr, incr, incr);
MC_COMMAND_DESC(decr, incr, decr);
MC_COMMAND_DESC(delete, delete, delete);
MC_COMMAND_DESC(touch, touch, touch);
MC_COMMAND_DESC(slabs, slabs, slabs);
MC_COMMAND_DESC(stats, stats, stats);
MC_COMMAND_DESC(flush_all, flush_all, flush_all);
MC_COMMAND_DESC(version, eol, version);
MC_COMMAND_DESC(verbosity, verbosity, verbosity);
MC_COMMAND_DESC(quit, eol, quit);

/**********************************************************************
 * Command Processing.
 **********************************************************************/

static void
mc_reply(struct mc_command *command, const char *str)
{
	ENTER();

	command->reply.str = str;
	command->reply.len = strlen(str);
	command->result_type = MC_RESULT_REPLY;

	LEAVE();
}

static void
mc_blank(struct mc_command *command)
{
	ENTER();

	command->result_type = MC_RESULT_BLANK;

	LEAVE();
}

static void
mc_process_get(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_gets(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_set(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_add(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_replace(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_append(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_prepend(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_cas(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_incr(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_decr(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_delete(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;

	bool rc = mc_table_delete_entry(command->params.del.key.str,
					command->params.del.key.len);

	if (command->params.del.noreply)
		mc_blank(command);
	else if (rc)
		mc_reply(command, "DELETED\r\n");
	else
		mc_reply(command, "NOT_FOUND\r\n");

	LEAVE();
}

static void
mc_process_touch(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_slabs(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_stats(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_flush_all(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_version(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_verbosity(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_quit(uintptr_t arg)
{
	ENTER();

	struct mc_command *command = (struct mc_command *) arg;
	mc_reply(command, "SERVER_ERROR not implemented\r\n");

	LEAVE();
}

static void
mc_process_command(struct mc_state *state)
{
	ENTER();

	struct mc_command *command = state->command;
	command->end_ptr = state->start_ptr;

	if (command->result_type == MC_RESULT_NONE)
		state->command->desc->process((intptr_t) command);

	mc_queue_command(state);

	LEAVE();
}

/**********************************************************************
 * Command Parsing.
 **********************************************************************/

#define MC_KEY_LEN_MAX		250

#define MC_BINARY_REQ		0x80
#define MC_BINARY_RES		0x81

struct mc_parser
{
	struct mc_state *state;
	struct mc_command *command;

	struct mc_buffer *buffer;
	char *start_ptr;
	char *end_ptr;

	bool error;
};

/*
 * Prepare for parsing a command.
 */
static void
mc_start_input(struct mc_parser *parser, struct mc_state *state)
{
	ENTER();

	parser->state = state;
	parser->command = mc_cache_command(state);

	struct mc_buffer *buffer = state->read_head;
	while (!mc_buffer_contains(buffer, state->start_ptr)) {
		buffer = buffer->next;
	}

	parser->buffer = buffer;
	parser->start_ptr = state->start_ptr;
	parser->end_ptr = buffer->data + buffer->used;

	parser->error = 0;

	LEAVE();
}

/*
 * Move to the next input buffer.
 */
static bool
mc_shift_input(struct mc_parser *parser)
{
	ENTER();

	struct mc_buffer *buffer = parser->buffer->next;
	bool rc = (buffer != NULL);
	if (rc) {
		parser->buffer = buffer;
		parser->start_ptr = buffer->data;
		parser->end_ptr = buffer->data + buffer->used;
		rc = (buffer->used > 0);
	}

	LEAVE();
	return rc;
}

/* 
 * Carry the param over to the next input buffer. Create one if there is
 * no already.
 */
static void
mc_carry_param(struct mc_parser *parser, int count)
{
	struct mc_buffer *buffer = parser->buffer->next;
	if (buffer == NULL) {
		buffer = mc_add_read_buffer(parser->state, MC_DEFAULT_BUFFER_SIZE);
	} else if (buffer->used > 0) {
		ASSERT((buffer->size - buffer->used) >= MC_KEY_LEN_MAX);
		memmove(buffer->data + count, buffer->data, buffer->used);
	}

	memcpy(buffer->data, parser->start_ptr, count);
	memset(parser->start_ptr, ' ', count);
	buffer->used += count;

	parser->buffer = buffer;
	parser->start_ptr = buffer->data + count;
	parser->end_ptr = buffer->data + buffer->used;
}

/*
 * Ask for the next input buffer with additional sanity checking.
 */
static bool
mc_claim_input(struct mc_parser *parser, int count)
{
	if (count > 1024) {
		/* The client looks insane. Quit fast. */
		mc_quit(parser->state, true);
		return false;
	}
	return mc_shift_input(parser);
}

static int
mc_peek_input(struct mc_parser *parser, char *s, char *e)
{
	ASSERT(mc_buffer_contains(parser->buffer, s));
	ASSERT(e == (parser->buffer->data + parser->buffer->used));

	if ((s + 1) < e)
		return s[1];
	else if (parser->buffer->next && parser->buffer->next->used > 0)
		return parser->buffer->next->data[0];
	else
		return 256; /* a non-char */
}

static bool
mc_parse_space(struct mc_parser *parser)
{
	ENTER();

	bool rc = true;

	/* The count of scanned chars. Used to check if the client sends
	   too much junk data. */
	int count = 0;

	for (;;) {
		char *s = parser->start_ptr;
		char *e = parser->end_ptr;

		for (; s < e; s++) {
			if (*s != ' ') {
				parser->start_ptr = s;
				goto done;
			}
		}

		count += parser->end_ptr - parser->start_ptr;
		if (!mc_claim_input(parser, count)) {
			rc = false;
			goto done;
		}
	}

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_error(struct mc_parser *parser, const char *error_string)
{
	ENTER();

	/* Initialize the result. */
	bool rc = true;
	parser->error = true;

	/* The count of scanned chars. Used to check if the client sends
	   too much junk data. */
	int count = 0;

	for (;;) {
		char *s = parser->start_ptr;
		char *e = parser->end_ptr;

		/* Scan input for a newline. */
		char *p = memchr(s, '\n', e - s);
		if (p != NULL) {
			/* Go past the newline ready for the next command. */
			parser->start_ptr = p + 1;
			parser->state->start_ptr = parser->start_ptr;
			/* Report the error. */
			mc_reply(parser->command, error_string);
			break;
		}

		count += parser->end_ptr - parser->start_ptr;
		if (!mc_claim_input(parser, count)) {
			/* The line is not complete, wait for completion
			   before reporting error. */
			rc = false;
			break;
		}
	}

	LEAVE();
	return rc;
}

static bool
mc_parse_eol(struct mc_parser *parser)
{
	ENTER();

	/* Skip spaces. */
	bool rc = mc_parse_space(parser);
	if (!rc) {
		goto done;
	}

	char *s = parser->start_ptr;
	char *e = parser->end_ptr;

	int c = *s;

	/* Check for optional CR char and required LF. */
	if ((c == '\r' && mc_peek_input(parser, s, e) == '\n') || c == '\n') {
		/* All right, got the line end. */
		if (c == '\r' && ++s == e) {
			struct mc_buffer *buffer = parser->buffer->next;
			parser->buffer = buffer;
			parser->start_ptr = buffer->data + 1;
			parser->end_ptr = buffer->data + buffer->used;
		} else {
			parser->start_ptr = s + 1;
		}
	} else {
		/* Oops, unexpected char. */
		rc = mc_parse_error(parser, "CLIENT_ERROR unexpected parameter\r\n");
	}

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_param(struct mc_parser *parser, struct mc_string *value)
{
	ENTER();

	bool rc = mc_parse_space(parser);
	if (!rc) {
		goto done;
	}

	char *s, *e;

retry:
	s = parser->start_ptr;
	e = parser->end_ptr;
	for (; s < e; s++) {
		int c = *s;
		if (c == ' ' || (c == '\r' && mc_peek_input(parser, s, e) == '\n') || c == '\n') {
			int count = s - parser->start_ptr;
			if (count == 0) {
				mc_parse_error(parser, "CLIENT_ERROR missing parameter\r\n");
			} else if (count > MC_KEY_LEN_MAX) {
				mc_parse_error(parser, "CLIENT_ERROR parameter is too long\r\n");
			} else {
				value->len = count;
				value->str = parser->start_ptr;
				parser->start_ptr = s;
			}
			goto done;
		}
	}

	int count = e - parser->start_ptr;
	if (count > MC_KEY_LEN_MAX) {
		mc_parse_error(parser, "CLIENT_ERROR parameter is too long\r\n");
	} else if (parser->buffer->used < parser->buffer->size) {
		rc = false;
	} else {
		mc_carry_param(parser, count);
		goto retry;
	}

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_u32(struct mc_parser *parser, uint32_t *value)
{
	ENTER();

	struct mc_string param;
	bool rc = mc_parse_param(parser, &param);
	if (rc && !parser->error) {
		char *endp;
		unsigned long v = strtoul(param.str, &endp, 10);
		if (endp < param.str + param.len) {
			mc_parse_error(parser, "CLIENT_ERROR invalid number parameter\r\n");
		} else {
			*value = v;
		}
	}

	LEAVE();
	return rc;
}

static bool
mc_parse_u64(struct mc_parser *parser, uint64_t *value)
{
	ENTER();

	struct mc_string param;
	bool rc = mc_parse_param(parser, &param);
	if (rc && !parser->error) {
		char *endp;
		unsigned long long v = strtoull(param.str, &endp, 10);
		if (endp < param.str + param.len) {
			mc_parse_error(parser, "CLIENT_ERROR invalid number parameter\r\n");
		} else {
			*value = v;
		}
	}

	LEAVE();
	return rc;
}

static bool
mc_parse_noreply(struct mc_parser *parser, bool *value)
{
	ENTER();

	bool rc = mc_parse_space(parser);
	if (!rc) {
		goto done;
	}

	char *s, *e;
	s = parser->start_ptr;
	e = parser->end_ptr;

	char *t = "noreply";

	int n = e - s;
	if (n > 7) {
		n = 7;
	} else if (n < 7) {
		if (memcmp(s, t, n) != 0) {
			*value = false;
			goto done;
		}

		if (!mc_shift_input(parser)) {
			rc = false;
			goto done;
		}
		s = parser->start_ptr;
		e = parser->end_ptr;

		t = t + n;
		n = 7 - n;

		if ((e - s) < n) {
			rc = false;
			goto done;
		}
	}

	if (memcmp(s, t, n) != 0) {
		*value = false;
		goto done;
	}

	*value = true;
	parser->start_ptr += n;

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_command(struct mc_parser *parser)
{
	ENTER();

	enum scan {
		SCAN_START,
		SCAN_CMD,
		SCAN_CMD_GE,
		SCAN_CMD_GET,
		SCAN_CMD_DE,
		SCAN_CMD_VE,
		SCAN_CMD_VER,
		SCAN_CMD_REST,
	};

	/* Initialize the result. */
	bool rc = mc_parse_space(parser);
	if (!rc) {
		goto done;
	}

	/* Get current position in the input buffer. */
	char *s = parser->start_ptr;
	char *e = parser->end_ptr;

	/* Must have some ready input at this point. */
	ASSERT(s < e);

	/* Initialize the scanner state. */
	enum scan scan = SCAN_START;
	int cmd_first = -1;
	char *cmd_rest = "";

	/* The count of scanned chars. Used to check if the client sends
	   too much junk data. */
	int count = 0;

	for (;;) {
		int c = *s;
		switch (scan) {
		case SCAN_START:
			if (likely(c != '\n')) {
				/* Got the first command char. */
				scan = SCAN_CMD;
				cmd_first = c;
				goto next;
			} else {
				/* Unexpected line end. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}

		case SCAN_CMD:
#define C(a, b) (((a) << 8) | (b))
			switch (C(cmd_first, c)) {
			case C('g', 'e'):
				scan = SCAN_CMD_GE;
				goto next;
			case C('s', 'e'):
				parser->command->desc = &mc_desc_set;
				scan = SCAN_CMD_REST;
				cmd_rest = "t";
				goto next;
			case C('a', 'd'):
				parser->command->desc = &mc_desc_add;
				scan = SCAN_CMD_REST;
				cmd_rest = "d";
				goto next;
			case C('r', 'e'):
				parser->command->desc = &mc_desc_replace;
				scan = SCAN_CMD_REST;
				cmd_rest = "place";
				goto next;
			case C('a', 'p'):
				parser->command->desc = &mc_desc_append;
				scan = SCAN_CMD_REST;
				cmd_rest = "pend";
				goto next;
			case C('p', 'r'):
				parser->command->desc = &mc_desc_prepend;
				scan = SCAN_CMD_REST;
				cmd_rest = "epend";
				goto next;
			case C('c', 'a'):
				parser->command->desc = &mc_desc_cas;
				scan = SCAN_CMD_REST;
				cmd_rest = "s";
				goto next;
			case C('i', 'n'):
				parser->command->desc = &mc_desc_incr;
				scan = SCAN_CMD_REST;
				cmd_rest = "cr";
				goto next;
			case C('d', 'e'):
				scan = SCAN_CMD_DE;
				goto next;
			case C('t', 'o'):
				parser->command->desc = &mc_desc_touch;
				scan = SCAN_CMD_REST;
				cmd_rest = "uch";
				goto next;
			case C('s', 'l'):
				parser->command->desc = &mc_desc_slabs;
				scan = SCAN_CMD_REST;
				cmd_rest = "abs";
				goto next;
			case C('s', 't'):
				parser->command->desc = &mc_desc_stats;
				scan = SCAN_CMD_REST;
				cmd_rest = "ats";
				goto next;
			case C('f', 'l'):
				parser->command->desc = &mc_desc_flush_all;
				scan = SCAN_CMD_REST;
				cmd_rest = "ush_all";
				goto next;
			case C('v', 'e'):
				scan = SCAN_CMD_VE;
				goto next;
			case C('q', 'u'):
				parser->command->desc = &mc_desc_quit;
				scan = SCAN_CMD_REST;
				cmd_rest = "it";
				goto next;
			default:
				/* Unexpected char. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}
#undef C

		case SCAN_CMD_GE:
			if (likely(c == 't')) {
				scan = SCAN_CMD_GET;
				goto next;
			} else {
				/* Unexpected char. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}

		case SCAN_CMD_GET:
			if (c == ' ') {
				parser->command->desc = &mc_desc_get;
				parser->start_ptr = s;
				goto done;
			} else if (c == 's') {
				parser->command->desc = &mc_desc_gets;
				/* Scan one char more with empty "rest" string
				   to verify that the command name ends here. */
				scan = SCAN_CMD_REST;
				goto next;
			} else if (c == '\r' || c == '\n') {
				/* Well, this turns out to be a get command
				   without arguments, albeit pointless this
				   is actually legal. */
				parser->command->desc = &mc_desc_get;
				goto done;
			} else {
				/* Unexpected char. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}

		case SCAN_CMD_DE:
			if (likely(c == 'c')) {
				parser->command->desc = &mc_desc_decr;
				scan = SCAN_CMD_REST;
				cmd_rest = "r";
				goto next;
			} else if (likely(c == 'l')) {
				parser->command->desc = &mc_desc_delete;
				scan = SCAN_CMD_REST;
				cmd_rest = "ete";
				goto next;
			} else {
				/* Unexpected char. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}

		case SCAN_CMD_VE:
			if (c == 'r') {
				scan = SCAN_CMD_VER;
				goto next;
			} else {
				/* Unexpected char. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}

		case SCAN_CMD_VER:
			if (c == 's') {
				parser->command->desc = &mc_desc_version;
				scan = SCAN_CMD_REST;
				cmd_rest = "ion";
				goto next;
			} else if (c == 'b') {
				parser->command->desc = &mc_desc_verbosity;
				scan = SCAN_CMD_REST;
				cmd_rest = "osity";
				goto next;
			} else {
				/* Unexpected char. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}

		case SCAN_CMD_REST:
			if (c == *cmd_rest) {
				if (unlikely(c == 0)) {
					/* Hmm, zero byte in the input. */
					parser->start_ptr = s;
					mc_parse_error(parser, "ERROR\r\n");
					goto done;
				}
				/* So far so good. */
				cmd_rest++;
				goto next;
			} else if (*cmd_rest != 0) {
				/* Unexpected char in the command name. */
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			} else if (c == ' ') {
				/* Success. */
				parser->start_ptr = s;
				goto done;
			} else if (c == '\r' || c == '\n') {
				/* Success. */
				parser->start_ptr = s;
				goto done;
			} else {
				/* Unexpected char after the command name. */
				parser->start_ptr = s;
				mc_parse_error(parser, "ERROR\r\n");
				goto done;
			}
		}

	next:
		if (++s == e) {
			count += parser->end_ptr - parser->start_ptr;
			if (!mc_claim_input(parser, count)) {
				rc = false;
				goto done;
			}

			s = parser->start_ptr;
			e = parser->end_ptr;
		}
	}

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_get(struct mc_parser *parser)
{
	ENTER();

	bool rc;
	int nkeys, nkeys_max;
	struct mc_string *keys;

	nkeys = 0;
	nkeys_max = 8;
	keys = mm_alloc(nkeys_max * sizeof(struct mc_string));

	for (;;) {
		rc = mc_parse_param(parser, &keys[nkeys]);
		if (!rc || parser->error)
			goto done;

		if (keys[nkeys].len == 0) {
			parser->command->params.get.keys = keys;
			parser->command->params.get.nkeys = nkeys;
			goto done;
		}

		if (++nkeys == nkeys_max) {
			nkeys_max += 8;
			keys = mm_realloc(keys, nkeys_max * sizeof(struct mc_string));
		}
	}

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_set(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_param(parser, &parser->command->params.set.key);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u32(parser, &parser->command->params.set.flags);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u32(parser, &parser->command->params.set.exptime);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u32(parser, &parser->command->params.set.bytes);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_noreply(parser, &parser->command->params.set.noreply);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_eol(parser);

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_cas(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_param(parser, &parser->command->params.cas.key);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u32(parser, &parser->command->params.cas.flags);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u32(parser, &parser->command->params.cas.exptime);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u32(parser, &parser->command->params.cas.bytes);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u64(parser, &parser->command->params.cas.cas);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_noreply(parser, &parser->command->params.cas.noreply);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_eol(parser);

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_incr(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_param(parser, &parser->command->params.inc.key);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u64(parser, &parser->command->params.inc.value);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_noreply(parser, &parser->command->params.inc.noreply);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_eol(parser);

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_delete(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_param(parser, &parser->command->params.del.key);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_noreply(parser, &parser->command->params.del.noreply);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_eol(parser);

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_touch(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_param(parser, &parser->command->params.touch.key);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_u32(parser, &parser->command->params.touch.exptime);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_noreply(parser, &parser->command->params.touch.noreply);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_eol(parser);

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_slabs(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_error(parser, "CLIENT_ERROR not implemented\r\n");

	LEAVE();
	return rc;
}

static bool
mc_parse_stats(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_error(parser, "CLIENT_ERROR not implemented\r\n");

	LEAVE();
	return rc;
}

static bool
mc_parse_flush_all(struct mc_parser *parser)
{
	ENTER();

	bool rc = mc_parse_u32(parser, &parser->command->params.flush.exptime);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_noreply(parser, &parser->command->params.flush.noreply);
	if (!rc || parser->error)
		goto done;
	rc = mc_parse_eol(parser);

done:
	LEAVE();
	return rc;
}

static bool
mc_parse_verbosity(struct mc_parser *parser)
{
	ENTER();
	
	bool rc = mc_parse_error(parser, "CLIENT_ERROR not implemented\r\n");

	LEAVE();
	return rc;
}

static bool
mc_parse(struct mc_state *state)
{
	ENTER();

	/* Initialize the parser. */
	struct mc_parser parser;
	mc_start_input(&parser, state);

	/* Parse the command name. */
	bool rc = mc_parse_command(&parser);
	if (!rc || parser.error)
		goto done;

	/* Parse the rest of the command. */
	rc = parser.command->desc->parse(&parser);
	if (!rc || parser.error)
		goto done;

	/* The command has been successfully parsed, go past it ready for. */
	state->start_ptr = parser.start_ptr;

done:
	LEAVE();
	return rc;
}

/**********************************************************************
 * I/O routines.
 **********************************************************************/

#define MC_READ_TIMEOUT		500

static ssize_t
mc_read(struct mm_net_socket *sock, struct mc_state *state)
{
	ENTER();

	struct mc_buffer *buffer = state->read_tail;
	if (buffer == NULL || buffer->used == buffer->size)
		buffer = mc_add_read_buffer(state, MC_DEFAULT_BUFFER_SIZE);

	size_t nbytes = buffer->size - buffer->used;
	ssize_t n = mm_net_read(sock, buffer->data + buffer->used, nbytes);
	if (n > 0) {
		buffer->used += n;
	}

	LEAVE();
	return n;
}

static ssize_t
mc_write(struct mm_net_socket *sock, const char *data, size_t size)
{
	ENTER();

	ssize_t rc = size;
	while (size > 0) {
		ssize_t n = mm_net_write(sock, data, size);
		if (n < 0) {
			rc = n;
			break;
		}

		size -= n;
		data += n;
	}

	LEAVE();
	return rc;
}

static bool
mc_receive_command(struct mm_net_socket *sock,
		   struct mc_state *state)
{
	ENTER();

	bool rc = false;

	mm_net_set_nonblock(sock);
	ssize_t n = mc_read(sock, state);
	mm_net_clear_nonblock(sock);

	while (n > 0 && !(rc = mc_parse(state))) {
		mm_net_set_timeout(sock, MC_READ_TIMEOUT);
		n = mc_read(sock, state);
		mm_net_set_timeout(sock, MM_TIMEOUT_INFINITE);
	}

	LEAVE();
	return rc;
}

static bool
mc_transmit_result(struct mm_net_socket *sock,
		   struct mc_state *state __attribute__((unused)),
		   struct mc_command *command)
{
	ENTER();

	bool rc;

	switch (command->result_type) {
	case MC_RESULT_NONE:
		rc = false;
		break;
	case MC_RESULT_REPLY:
		if (mc_write(sock, command->reply.str, command->reply.len) < 0)
			rc = false;
		else
			rc = true;
		break;
	case MC_RESULT_ENTRY:
		rc = true;
		break;
	case MC_RESULT_BLANK:
		rc = true;
		break;
	default:
		ABORT();
	}

	LEAVE();
	return rc;
}

/**********************************************************************
 * Protocol Handlers.
 **********************************************************************/

static void
mc_prepare(struct mm_net_socket *sock)
{
	ENTER();

	sock->proto_data = 0;

	LEAVE();
}

static void
mc_cleanup(struct mm_net_socket *sock)
{
	ENTER();

	if (sock->proto_data) {
		mc_destroy((struct mc_state *) sock->proto_data);
		sock->proto_data = 0;
	}

	LEAVE();
}

static void
mc_reader_routine(struct mm_net_socket *sock)
{
	ENTER();

	/* Get the protocol data. */
	if (!sock->proto_data)
		sock->proto_data = (intptr_t) mc_create();
	struct mc_state *state = (struct mc_state *) sock->proto_data;

	if (mc_receive_command(sock, state)) {
		mc_process_command(state);
		mm_net_spawn_writer(sock);
	}

	LEAVE();
}

static void
mc_writer_routine(struct mm_net_socket *sock)
{
	ENTER();

	/* Get the protocol data if any. */
	if (!sock->proto_data)
		goto done;
	struct mc_state *state = (struct mc_state *) sock->proto_data;

	/* Get the first queued command and try to transmit its result. */
	struct mc_command *command = state->command_head;
	if (command == NULL)
		goto done;
	if (!mc_transmit_result(sock, state, command))
		goto done;

	/* Try to transmit more command results. */
	for (;;) {
		struct mc_command *next = command->next;
		if (next == NULL) {
			state->command_head = state->command_tail = NULL;
			break;
		}
		if (!mc_transmit_result(sock, state, next)) {
			state->command_head = next;
			break;
		}

		/* Free no longer needed command. */
		mc_command_destroy(command);
		command = next;
	}

	/* Free pertinent input buffers along with the last command. */
	mc_release_buffers(state, command->end_ptr);
	mc_command_destroy(command);

done:
	LEAVE();
}

/**********************************************************************
 * Module Entry Points.
 **********************************************************************/

/* TCP memcache server. */
static struct mm_net_server *mc_tcp_server;

void
mm_memcache_init(void)
{
	ENTER();

	mc_table_init();
	mc_command_init();

	static struct mm_net_proto proto = {
		.flags = MM_NET_INBOUND,
		.prepare = mc_prepare,
		.cleanup = mc_cleanup,
		.reader_routine = mc_reader_routine,
		.writer_routine = mc_writer_routine,
	};

	mc_tcp_server = mm_net_create_inet_server("memcache", "127.0.0.1", 11211);
	mm_net_start_server(mc_tcp_server, &proto);

	LEAVE();
}

void
mm_memcache_term(void)
{
	ENTER();

	mm_net_stop_server(mc_tcp_server);

	mc_command_term();
	mc_table_term();

	LEAVE();
}
