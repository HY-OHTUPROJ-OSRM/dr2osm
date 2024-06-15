#define COMMIT_BLOCK_SIZE (16 * 1024)

static Growable_Buffer way_buffer, node_buffer;
static Node *node_tree_root;

jmp_buf out_of_memory;

static int
init_buffer(Growable_Buffer *buffer, intptr_t size)
{
	assert(size >= 0);

	void *start = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (start == MAP_FAILED) {
		fprintf(stderr, "Unable to reserve address range for buffer: %s\n", strerror(errno));
		return 0;
	}

	buffer->start = start;
	buffer->size = size;
	buffer->first_in_offset = 0;
	buffer->next_in_offset = 0;
	buffer->commit_threshold_offset = 0;

	return 1;
}

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

	if (buffer->next_in_offset > buffer->commit_threshold_offset) {
		int mp = mprotect(buffer->start + buffer->commit_threshold_offset,
				COMMIT_BLOCK_SIZE, PROT_READ | PROT_WRITE);

		if (mp == -1) {
			fprintf(stderr, "Unable to commit memory to buffer: %s\n", strerror(errno));
			longjmp(out_of_memory, 1);
		}

		buffer->commit_threshold_offset += COMMIT_BLOCK_SIZE;
	}

	return result;
}

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