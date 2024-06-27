#define COMMIT_BLOCK_SIZE (16 * 1024)

static Growable_Buffer way_buffer, node_buffer;
static Node *node_tree_root;

static jmp_buf out_of_memory;

#if defined(_WIN32)

/* On Windows use the VirtualAlloc interface to reserve an address range and to
 * commit memory to a buffer. */

static void *
reserve_memory(intptr_t size)
{
	return VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
}

static int
commit_memory(Growable_Buffer *buffer)
{
	int result = !!VirtualAlloc(buffer->start + buffer->commit_threshold_offset,
			COMMIT_BLOCK_SIZE, MEM_COMMIT, PAGE_READWRITE);

	if (result) {
		buffer->commit_threshold_offset += COMMIT_BLOCK_SIZE;
	}

	return result;
}

static char *
get_memory_error_message()
{
	static char result[512];

	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM,
			0, GetLastError(), 0,
			result, 512 / sizeof(TCHAR), 0);

	return result;
}

#else

/* On POSIX use mmap to reserve an address range and mprotect to commit memory
 * to a buffer. */

static void *
reserve_memory(intptr_t size)
{
	void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	return result == MAP_FAILED ? 0 : result;
}

static int
commit_memory(Growable_Buffer *buffer)
{
	int result = !mprotect(buffer->start + buffer->commit_threshold_offset,
			COMMIT_BLOCK_SIZE, PROT_READ | PROT_WRITE);

	if (result) {
		buffer->commit_threshold_offset += COMMIT_BLOCK_SIZE;
	}

	return result;
}

static char *
get_memory_error_message()
{
	return strerror(errno);
}

#endif

/* On error the various memory functions longjmp to out_of_memory, which is set
 * in the main function. */

static int
init_buffer(Growable_Buffer *buffer, intptr_t size)
{
	assert(size >= 0);

	void *start = reserve_memory(size);

	if (!start) {
		fprintf(stderr, "Unable to reserve address range for buffer: %s\n",
				get_memory_error_message());
		return 0;
	}

	buffer->start = start;
	buffer->size = size;
	buffer->first_in_offset = 0;
	buffer->next_in_offset = 0;
	buffer->commit_threshold_offset = 0;

	return 1;
}

/* Pushes to the end of the buffer, committing more memory if needed. */
static void *
buffer_push(Growable_Buffer *buffer, intptr_t size)
{
	assert(size >= 0);

	if (buffer->size - buffer->next_in_offset < size) {
		fprintf(stderr, "Ran out of buffer space.\n");
		longjmp(out_of_memory, 1);
	}

	char *result = buffer->start + buffer->next_in_offset;
	buffer->next_in_offset += size;

	if (buffer->next_in_offset > buffer->commit_threshold_offset
			&& !commit_memory(buffer)) {
		fprintf(stderr, "Unable to commit memory to buffer: %s\n",
				get_memory_error_message());
		longjmp(out_of_memory, 1);
	}

	return result;
}

/* Pops from the start of the buffer. */
static void *
buffer_pop(Growable_Buffer *buffer, intptr_t size)
{
	assert(size >= 0);
	assert(buffer->first_in_offset + size <= buffer->next_in_offset);
	assert(buffer->first_in_offset + size <= buffer->size);

	char *result = buffer->start + buffer->first_in_offset;
	buffer->first_in_offset += size;

	return result;
}

static int *
way_buffer_push_int(int value)
{
	int *result = buffer_push(&way_buffer, sizeof(int));
	*result = value;

	return result;
}

/* Strings are buffered as zero-terminated 8 bit character sequences padded to
 * the next 4 byte boundary. */
static void
way_buffer_push_string(const char *value)
{
	char chunk[4];

	do {
		for (int i = 0; i < 4; i++) {
			chunk[i] = *value;
			value += !!*value;
		}

		way_buffer_push_int(*(int *)chunk);
	} while (chunk[3]);
}

static int
way_buffer_pop_int()
{
	int *presult = buffer_pop(&way_buffer, sizeof(int));

	assert(presult);

	return *presult;
}

static char *
way_buffer_pop_string()
{
	char *result = buffer_pop(&way_buffer, 0);

	int buf;
	char *chunk = (char *)&buf;

	do {
		buf = way_buffer_pop_int();
	} while (chunk[3]);

	return result;
}

static Node *
alloc_node()
{
	return buffer_push(&node_buffer, sizeof(Node));
}

/* Buffers nodes (as coordinate pairs) into a quad tree. If a node with
 * identical coordinates has already been buffered, returns a pointer to the
 * previously seen node. Otherwise allocates a new node with its id field
 * initialized to 0 and returns a pointer to it. */
static Node *
node_upsert(int x, int y)
{
	Node *current = node_tree_root;

	while (1) {
		if (x == current->x && y == current->y) {
			return current;
		}

		int east = (x > current->x);
		int north = (y > current->y);
		int child_index = east | (north << 1);

		if (current->child_node_offsets[child_index] == 0) {
			Node *new = alloc_node();
			intptr_t offset = new - current;

			assert(offset > 0 && offset < INT_MAX);

			current->child_node_offsets[child_index] = (int)offset;
			new->x = x;
			new->y = y;

			return new;
		}

		current += current->child_node_offsets[child_index];
	}
}
