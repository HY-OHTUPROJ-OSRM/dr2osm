/* Standard library. */
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* System. */
#include <sys/mman.h>
#include <unistd.h>

/* Third-party libraries. */
#include <proj.h>
#include <sqlite3.h>

/* Project code. */
#include "types.h"

#ifdef RELEASE_BUILD
#define assert(P) 0
#else
#define assert(P) do { if (!(P)) __builtin_trap(); } while(0)
#endif

#define COMMIT_BLOCK_SIZE (16 * 1024)

#define HIGHWAY\
	X(HW_NONE, "")\
	X(HW_FOOTWAY, "footway")\
	X(HW_MOTORWAY, "motorway")\
	X(HW_TRUNK, "trunk")\
	X(HW_PRIMARY, "primary")\
	X(HW_SECONDARY, "secondary")\
	X(HW_TERTIARY, "tertiary")\
	X(HW_RESIDENTIAL, "residential")

#define ROUTE\
	X(RT_NONE, "")\
	X(RT_FERRY, "ferry")

#define ONEWAY\
	X(OW_NONE, "")\
	X(OW_NO, "no")\
	X(OW_YES, "yes")

enum {
#define X(IDENT, STRING) IDENT,
	HIGHWAY
	ROUTE
	ONEWAY
#undef X
	STRING_COUNT
};

static char *osm_strings[STRING_COUNT] = {
#define X(IDENT, STRING) STRING,
	HIGHWAY
	ROUTE
	ONEWAY
#undef X
};

char sql_query[] =
	"SELECT COALESCE(n.geom, l.geom) as geom,"
		"COALESCE(n.arvo, 0) AS speed_limit,"
		"COALESCE(l.toiminn_lk, 0) AS class,"
		"COALESCE(l.linkkityyp, 0) AS type,"
		"COALESCE(l.ajosuunta, 0) AS direction,"
		"COALESCE(l.tienimi_su, l.tienimi_ru, l.tienimi_sa) AS name\n"
	"FROM dr_linkki_k AS l LEFT OUTER JOIN dr_nopeusrajoitus_k AS n USING (segm_id);\n";
	//"WHERE l.kuntakoodi=91;";

int *way_buffer;
intptr_t way_buffer_offset;
intptr_t way_buffer_size;
char *way_buffer_commit_threshold;

Node *node_buffer;
intptr_t node_buffer_offset;
intptr_t node_buffer_size;
char *node_buffer_commit_threshold;

int last_id = 0;

static int *
way_buffer_push_int(int value)
{
	assert(((char *)way_buffer + way_buffer_size)
			- (char*)(way_buffer + way_buffer_offset)
			> sizeof(int));

	int *result = &way_buffer[way_buffer_offset++];

	if ((char *)(way_buffer + way_buffer_offset) > way_buffer_commit_threshold) {
		mprotect(way_buffer_commit_threshold, COMMIT_BLOCK_SIZE, PROT_READ | PROT_WRITE);
		way_buffer_commit_threshold += COMMIT_BLOCK_SIZE;
	}

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
	assert(way_buffer_offset > 0);
	way_buffer_offset--;
	return *(way_buffer++);
}

static char *
way_buffer_pop_string()
{
	char *result = (char *)way_buffer;

	int buf;
	char *chunk = (char *)&buf;

	do {
		buf = way_buffer_pop_int();
	} while (chunk[0] && chunk[1] && chunk[2] && chunk[3]);

	return result;
}

static Node *
alloc_node()
{
	assert(((char *)node_buffer + node_buffer_size)
			- (char *)(node_buffer + node_buffer_offset)
			> sizeof(Node));

	Node *result = &node_buffer[node_buffer_offset++];

	if ((char *)(node_buffer + node_buffer_offset) > node_buffer_commit_threshold) {
		mprotect(node_buffer_commit_threshold, COMMIT_BLOCK_SIZE, PROT_READ | PROT_WRITE);
		node_buffer_commit_threshold += COMMIT_BLOCK_SIZE;
	}

	return result;
}

static Node *
node_upsert(int x, int y)
{
	Node *current = node_buffer;

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

static int
generate_id()
{
	int result = ++last_id;
	assert(result > 0);
	return result;
}

static void
handle_row(FILE *output, PJ *projection,
		const Geopackage_Binary_Header *geom_header,
		int speed_limit, int class, int type, int direction,
		const char *name)
{
	int *way_id = way_buffer_push_int(0);

	int envelope_indicator = (geom_header->flags >> 1) & 7;

	assert(envelope_indicator >= 0 && envelope_indicator <= 4);

	static int envelope_sizes[] = { 0, 32, 48, 48, 64 };

	int envelope_size = envelope_sizes[envelope_indicator];
	Wkb_Line_String_Zm *line_string = (Wkb_Line_String_Zm *)(geom_header->envelope + envelope_size);

	assert(line_string->byte_order = 1); /* Little endian. */
	assert(line_string->type = 3002); /* wkbLineStringZM */

	int prev_x = INT_MIN;
	int prev_y = INT_MIN;

	for (int i = 0; i < line_string->num_points; i++) {
		Point_Zm *point = &line_string->points[i];

		int x = (int)(point->x + 0.5);
		int y = (int)(point->y + 0.5);

		if (x == prev_x && y == prev_y) {
			continue;
		}

		prev_x = x;
		prev_y = y;

		Node *node = node_upsert(x, y);

		if (!node->id) {
			node->id = generate_id();

			PJ_COORD fin = proj_coord((double)x, (double)y, 0, 0);
			PJ_COORD wgs = proj_trans(projection, PJ_FWD, fin);

			fprintf(output, "<node visible=\"true\" id=\"%d\" lat=\"%.9f\" lon=\"%.9f\"/>\n",
					node->id, wgs.xy.x, wgs.xy.y);
		}

		way_buffer_push_int(node->id);
	}

	way_buffer_push_int(0);
	*way_id = generate_id();

	int highway = HW_NONE;

	if (type == 8 || type == 9 || class == 8) {
		highway = HW_FOOTWAY;
	} else if (type != 21) {
		switch (class) {
		case 1:
			highway = HW_MOTORWAY;
			break;

		case 2:
			highway = HW_TRUNK;
			break;

		case 3:
			highway = HW_PRIMARY;
			break;

		case 4:
			highway = HW_SECONDARY;
			break;

		case 5:
			highway = HW_TERTIARY;
			break;

		default:
			highway = HW_RESIDENTIAL;
			break;
		}
	}

	way_buffer_push_int(highway);

	int route = RT_NONE;

	if (type == 21 && class != 8) {
		route = RT_FERRY;
	}

	way_buffer_push_int(route);

	int oneway = OW_NONE;

	switch (direction) {
	case 2:
		oneway = OW_NO;
		break;

	case 3:
	case 4:
		oneway = OW_YES;
		break;

	default:
		break;
	}

	way_buffer_push_int(oneway);
	way_buffer_push_int(speed_limit);
	way_buffer_push_string(name);
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		return 1;
	}

	char *argv0 = argv[0];
	char *input_path = argv[1];
	char *output_path = argv[2];

	PJ *projection = proj_create_crs_to_crs(0,
			"EPSG:3067", "EPSG:4326", 0);

	if (!projection) {
		return 1;
	}

	sqlite3 *db;
	int rc;

	rc = sqlite3_open_v2(input_path, &db, SQLITE_OPEN_READONLY, 0);

	if (rc != SQLITE_OK) {
		return 1;
	}

	sqlite3_stmt *statement;

	rc = sqlite3_prepare_v2(db, sql_query, sizeof(sql_query), &statement, 0);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "sqlite3_prepare_v2: %s\n", sqlite3_errstr(rc));
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
	}

	FILE *output = fopen(output_path, "w");

	way_buffer_size = (intptr_t)40 * 1024 * 1024 * 1024;
	way_buffer = mmap(0, way_buffer_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	way_buffer_commit_threshold = (char *)way_buffer;
	node_buffer_size = sizeof(Node) << 31;
	node_buffer = mmap(0, node_buffer_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	node_buffer_commit_threshold = (char *)node_buffer;

	*alloc_node() = (Node) {.x = 1018199, .y = 7248352};

	fprintf(output, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	fprintf(output, "<osm version=\"0.6\" generator=\"dr2osm\">\n");

	int num_ways = 0;

	fprintf(stderr, "\n\n\n");

	do {
		rc = sqlite3_step(statement);

		assert(rc != SQLITE_MISUSE);

		switch (rc) {
		case SQLITE_DONE:
		case SQLITE_BUSY:
			break;

		case SQLITE_ROW:
			assert(!strcmp(sqlite3_column_name(statement, 0), "geom"));
			assert(!strcmp(sqlite3_column_name(statement, 1), "speed_limit"));
			assert(!strcmp(sqlite3_column_name(statement, 2), "class"));
			assert(!strcmp(sqlite3_column_name(statement, 3), "type"));
			assert(!strcmp(sqlite3_column_name(statement, 4), "direction"));
			assert(!strcmp(sqlite3_column_name(statement, 5), "name"));

			assert(sqlite3_column_type(statement, 0) == SQLITE_BLOB);
			assert(sqlite3_column_type(statement, 1) == SQLITE_INTEGER);
			assert(sqlite3_column_type(statement, 2) == SQLITE_INTEGER);
			assert(sqlite3_column_type(statement, 3) == SQLITE_INTEGER);
			assert(sqlite3_column_type(statement, 4) == SQLITE_INTEGER);
			assert(sqlite3_column_type(statement, 5) == SQLITE_TEXT);

			num_ways++;

			assert(num_ways > 0);

			handle_row(output, projection,
					sqlite3_column_blob(statement, 0),
					sqlite3_column_int(statement, 1),
					sqlite3_column_int(statement, 2),
					sqlite3_column_int(statement, 3),
					sqlite3_column_int(statement, 4),
					sqlite3_column_text(statement, 5));
			break;

		default:
			fprintf(output, "sqlite3_step: %s\n%s\n%s\n",
					sqlite3_errstr(rc),
					sqlite3_sql(statement),
					sqlite3_errmsg(db));
			return 1;
		}

		if (!((num_ways - 1) & 0xFFF)) {
			intptr_t buffer_committed =
				(way_buffer_commit_threshold - (char *)way_buffer) +
				(node_buffer_commit_threshold - (char *)node_buffer);

			fprintf(stderr, "\33[A\33[2K\33[A\33[2K\33[A\33[2K\r");
			fprintf(stderr, "Buffer committed: %10ld kilobytes\n", buffer_committed >> 10);
			fprintf(stderr, "Nodes written out: %9ld\n", node_buffer_offset - 1);
			fprintf(stderr, "Ways buffered: %13d\n", num_ways);
		}
	} while (rc != SQLITE_DONE);

	intptr_t node_buffer_size = node_buffer_commit_threshold - (char *)node_buffer;
	intptr_t way_buffer_size = way_buffer_commit_threshold - (char *)way_buffer;

	fprintf(stderr, "\33[A\33[2K\33[A\33[2K\33[A\33[2K\r");
	fprintf(stderr, "Node buffer size: %10ld kilobytes\n", node_buffer_size >> 10);
	fprintf(stderr, "Way buffer size: %11ld kilobytes\n", way_buffer_size >> 10);
	fprintf(stderr, "Nodes written out: %1$9ld/%1$ld\n", node_buffer_offset - 1);
	fprintf(stderr, "Ways buffered: %1$13d/%1$d\n", num_ways);
	fprintf(stderr, "\n");

	for (int i = 0; i < num_ways; i++) {
		int way_id = way_buffer_pop_int();

		fprintf(output, "<way visible=\"true\" id=\"%d\">", way_id);

		for (int node_id; node_id = way_buffer_pop_int();) {
			fprintf(output, "<nd ref=\"%d\"/>", node_id);
		}

		int highway = way_buffer_pop_int();
		int route = way_buffer_pop_int();
		int oneway = way_buffer_pop_int();
		int maxspeed = way_buffer_pop_int();
		char *name = way_buffer_pop_string();

		fprintf(output, "<tag k=\"highway\" v=\"%s\"/>", osm_strings[highway]);
		fprintf(output, "<tag k=\"route\" v=\"%s\"/>", osm_strings[route]);
		fprintf(output, "<tag k=\"oneway\" v=\"%s\"/>", osm_strings[oneway]);
		fprintf(output, "<tag k=\"maxspeed\" v=\"%d\"/>", maxspeed);
		fprintf(output, "<tag k=\"name\" v=\"%s\"/>", name);
		fprintf(output, "</way>\n");

		if (!(i & 0xFFFF)) {
			fprintf(stderr, "\33[A\33[2K\rWays written out: %10d/%d\n", i + 1, num_ways);
		}
	}

	fprintf(stderr, "\33[A\33[2K\rWays written out: %1$10d/%1$d\n", num_ways);

	fprintf(output, "</osm>\n");
}
