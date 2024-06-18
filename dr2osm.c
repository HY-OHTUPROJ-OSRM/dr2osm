/* Standard library. */
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* System. */
#include <sys/mman.h>
#include <unistd.h>

/* Third-party libraries. */
#include <proj.h>
#include <sqlite3.h>

#ifdef RELEASE_BUILD
#define assert(P) 0
#else
#define assert(P) do { if (!(P)) __builtin_trap(); } while(0)
#endif

/* Project code. */
#include "types.h"
#include "buffer.c"

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

static char sql_query[] =
	"SELECT COALESCE(n.geom, l.geom) as geom,"
		"COALESCE(n.arvo, 0) AS speed_limit,"
		"COALESCE(l.toiminn_lk, 0) AS class,"
		"COALESCE(l.linkkityyp, 0) AS type,"
		"COALESCE(l.ajosuunta, 0) AS direction,"
		"COALESCE(l.tienimi_su, l.tienimi_ru, l.tienimi_sa) AS name\n"
	"FROM dr_linkki_k AS l LEFT OUTER JOIN dr_nopeusrajoitus_k AS n USING (segm_id);\n";
	//"WHERE l.kuntakoodi=91;";

static int last_id = 0;

static sqlite3_stmt *
prepare_statement(sqlite3 *db, char *sql)
{
	sqlite3_stmt *result = 0;

	int rc = sqlite3_prepare_v2(db, sql, -1, &result, 0);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "Unable to read data from input: %s\n", sqlite3_errstr(rc));
		fprintf(stderr, "%s\n", sqlite3_errmsg(db));
		return 0;
	}

	return result;
}

static int
get_num_ways(sqlite3 *db)
{
	sqlite3_stmt *statement = prepare_statement(db, "SELECT COUNT(*) FROM dr_linkki_k;");

	if (!statement) {
		return 0;
	}

	int result = 0;
	int rc;

	do {
		rc = sqlite3_step(statement);

		assert(rc != SQLITE_MISUSE);

		switch (rc) {
		case SQLITE_BUSY:
			break;

		case SQLITE_ROW:
			result = sqlite3_column_int(statement, 0);

			if (!result) {
				fprintf(stderr, "Input does not contain any ways.\n");
			}

			break;

		default:
			fprintf(stderr, "Unable to read data from input: %s\n", sqlite3_errstr(rc));
			fprintf(stderr, "%s\n", sqlite3_errmsg(db));
			break;
		}
	} while (rc == SQLITE_BUSY);

	sqlite3_finalize(statement);

	return result;
}

static int
generate_id()
{
	int result = ++last_id;
	assert(result > 0);
	return result;
}

static int
handle_row(FILE *output, PJ *projection,
		const Geopackage_Binary_Header *geom_header,
		int speed_limit, int class, int type, int direction,
		const char *name)
{
	/* The format of a single way in the way buffer:
	 *
	 * int way_id
	 * int... node_ids
	 * int node_ids_terminator = 0
	 * int highway
	 * int route
	 * int oneway
	 * int maxspeed
	 * string name
	 *
	 * way_id corresponds to the <way> tag's id attribute. Each element
	 * of node_ids corresponds to the ref attribute of a distinct <nd> tag
	 * within the way element. Each of highway, route and oneway is an index
	 * into the osm_strings array defined at the top of this file, the
	 * corresponding element of which corresponds to the v attribute of a
	 * <tag> tag in the way element, with "highway", "route" or "oneway"
	 * respectively as its k attribute. name corresponds to the v attribute
	 * of a <tag> tag in the way element, with "name" as its k attribute. */

	int envelope_indicator = (geom_header->flags >> 1) & 7;

	if (envelope_indicator > 4) {
		return 0;
	}

	static int envelope_sizes[] = { 0, 32, 48, 48, 64 };

	int envelope_size = envelope_sizes[envelope_indicator];
	Wkb_Line_String_Zm *line_string = (Wkb_Line_String_Zm *)(geom_header->envelope + envelope_size);

	int can_parse_geometry =
		line_string->byte_order == 1 /* Little endian. */
		&& line_string->type == 3002; /* wkbLineStringZM */

	if (!can_parse_geometry) {
		return 0;
	}

	int *way_id = way_buffer_push_int(0);

	int prev_x = INT_MIN;
	int prev_y = INT_MIN;

	for (int i = 0; i < line_string->num_points; i++) {
		Point_Zm *point;

		if (direction == 3) {
			point = &line_string->points[line_string->num_points - 1 - i];
		} else {
			point = &line_string->points[i];
		}

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

	return 1;
}

int
main(int argc, char **argv)
{
	/* Initialization. */

	int result = 1;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s <input-path> <output-path>\n", argv[0]);
		return 1;
	}

	char *input_path = argv[1];
	char *output_path = argv[2];

	PJ *projection = proj_create_crs_to_crs(0,
			"EPSG:3067", "EPSG:4326", 0);

	if (!projection) {
		fprintf(stderr, "proj_create_crs_to_crs: %s\n",
				proj_errno_string(proj_errno(0)));
		return 1;
	}

	FILE *output;

	if (!strcmp(output_path, "-")) {
		output = stdout;
	} else {
		output = fopen(output_path, "w");
	}

	if (!output) {
		fprintf(stderr, "Unable to open \"%s\" for writing: %s\n",
				output_path, strerror(errno));
		return 1;
	}

	sqlite3 *db;
	int rc;

	rc = sqlite3_open_v2(input_path, &db, SQLITE_OPEN_READONLY, 0);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "Unable to open \"%s\" for reading: %s\n",
				input_path, sqlite3_errmsg(db));
		goto cleanup_output;
	}

	int num_ways_total = get_num_ways(db);

	if (!num_ways_total) {
		goto cleanup_output;
	}

	sqlite3_stmt *statement = prepare_statement(db, sql_query);

	if (!statement) {
		goto cleanup_output;
	}

	if (!init_buffer(&way_buffer, (intptr_t)40 * 1024 * 1024 * 1024)) {
		goto cleanup;
	}

	if (!init_buffer(&node_buffer, sizeof(Node) << 31)) {
		goto cleanup;
	}

	if (setjmp(out_of_memory)) {
		goto cleanup;
	}

	node_tree_root = alloc_node();
	node_tree_root->x = 1018199;
	node_tree_root->y = 7248352;

	/* Write OSM header. */

	fprintf(output, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	fprintf(output, "<osm version=\"0.6\" generator=\"dr2osm\">\n");

	/* Process ways and nodes, and write nodes. */

	int num_ways_processed = 0;
	int num_invalid_ways = 0;

	fprintf(stderr, "(1/2) Processing ways.\t\t  0%%\n");

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

			int handled = handle_row(output, projection,
					sqlite3_column_blob(statement, 0),
					sqlite3_column_int(statement, 1),
					sqlite3_column_int(statement, 2),
					sqlite3_column_int(statement, 3),
					sqlite3_column_int(statement, 4),
					sqlite3_column_text(statement, 5));

			if (handled) {
				num_ways_processed++;
			} else {
				num_invalid_ways++;
			}

			if (!(num_ways_processed & 0x3FFF)) {
				fprintf(stderr, "\33[A\33[2K\r(1/2) Processing ways.\t\t%3d%%\n",
						(num_ways_processed + num_invalid_ways) * 100 / num_ways_total);
			}

			break;

		default:
			fprintf(output, "sqlite3_step: %s\n%s\n%s\n",
					sqlite3_errstr(rc),
					sqlite3_sql(statement),
					sqlite3_errmsg(db));
			goto cleanup;
		}
	} while (rc != SQLITE_DONE);

	fprintf(stderr, "\33[A\33[2K\r(1/2) Processing ways.\t\t100%%\n");

	/* Write buffered ways. */

	fprintf(stderr, "\n");

	for (int i = 0; i < num_ways_processed; i++) {
		if (!(i & 0x3FFF)) {
			fprintf(stderr, "\33[A\33[\r2K(2/2) Writing ways to file.\t%3d%%\n",
					i * 100 / num_ways_processed);
		}

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
	}

	fprintf(stderr, "\33[A\33[2K\r(2/2) Writing ways to file.\t100%%\n");

	if (num_invalid_ways > 0) {
		fprintf(stderr, "Input contained %d ways with geometries that"
				"could not be parsed and were skipped.\n",
				num_invalid_ways);
	}

	fprintf(output, "</osm>\n");

	result = 0;

cleanup:
	sqlite3_finalize(statement);

cleanup_input:
	sqlite3_close(db);

cleanup_output:
	fclose(output);

	return result;
}
