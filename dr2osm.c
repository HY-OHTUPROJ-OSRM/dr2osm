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

#define ICE_ROAD_SPEED_LIMIT 25

#define HIGHWAY\
	X(HW_NONE, "")\
	X(HW_FOOTWAY, "footway")\
	X(HW_MOTORWAY, "motorway")\
	X(HW_TRUNK, "trunk")\
	X(HW_PRIMARY, "primary")\
	X(HW_SECONDARY, "secondary")\
	X(HW_TERTIARY, "tertiary")\
	X(HW_RESIDENTIAL, "residential")\
	X(HW_UNCLASSIFIED, "unclassified")

#define ROUTE\
	X(RT_NONE, "")\
	X(RT_FERRY, "ferry")

#define ONEWAY\
	X(OW_NONE, "")\
	X(OW_NO, "no")\
	X(OW_YES, "yes")

#define ADDITIONAL_TAGS\
	X(AT_ICE_ROAD, "ice_road")

enum {
#define X(IDENT, STRING) IDENT,
	HIGHWAY
	ROUTE
	ONEWAY
	ADDITIONAL_TAGS
#undef X
	STRING_COUNT
};

static char *osm_strings[STRING_COUNT] = {
#define X(IDENT, STRING) STRING,
	HIGHWAY
	ROUTE
	ONEWAY
	ADDITIONAL_TAGS
#undef X
};

static char input_sql_query[] =
	"SELECT COALESCE(n.geom, l.geom) as geom,"
		"COALESCE(n.arvo, 0) AS speed_limit,"
		"COALESCE(l.toiminn_lk, 0) AS class,"
		"COALESCE(l.linkkityyp, 0) AS type,"
		"COALESCE(l.ajosuunta, 0) AS direction,"
		"COALESCE(l.tienimi_su, l.tienimi_ru, l.tienimi_sa, '') AS name\n"
	"FROM dr_linkki_k AS l LEFT OUTER JOIN dr_nopeusrajoitus_k AS n USING (segm_id);\n";
	//"WHERE l.kuntakoodi=91;";

static char mml_iceroads_sql_query[] =
	"SELECT geom,"
		"COALESCE(yksisuuntaisuus, -1) AS direction,"
		"COALESCE("
			"nimi_suomi, nimi_ruotsi, nimi_inarinsaame, nimi_koltansaame, nimi_pohjoissaame, ''"
		") AS name\n"
	"FROM iceroads;";

static int last_id = 0;

static int
parse_commandline_arguments(Program_Configuration *config, int argc, char **argv)
{
	argc--;
	argv++;

	while (argc > 0 && argv[0][0] == '-') {
		char *argument = argv[0];
		argc--;
		argv++;

		if (!strcmp(argument, "--mml-iceroads")) {
			if (argc < 1) {
				return 0;
			}

			config->mml_iceroads_path = argv[0];
			argc--;
			argv++;
		}
	}

	if (argc != 2) {
		return 0;
	}

	config->input_path = argv[0];
	config->output_path = argv[1];

	return 1;
}

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
buffer_ids(const Geopackage_Binary_Header *geom_header, int reverse_node_order, Query_Context *context) {
	int envelope_indicator = (geom_header->flags >> 1) & 7;

	if (envelope_indicator > 4) {
		return 0;
	}

	static int envelope_sizes[] = { 0, 32, 48, 48, 64 };

	int envelope_size = envelope_sizes[envelope_indicator];
	Wkb_Line_String_Any *line_string = (Wkb_Line_String_Any *)(geom_header->envelope + envelope_size);

	if (line_string->byte_order != 1) {
		return 0;
	}

	int point_stride;

	switch (line_string->type) {
	case 2: /* wkbLineString */
		point_stride = 2;
		break;

	case 1002: /* wkbLineStringZ */
	case 2002: /* wkbLineStringM */
		point_stride = 3;
		break;

	case 3002: /* wkbLineStringZM */
		point_stride = 4;
		break;

	default:
		return 0;
	}

	int *way_id = way_buffer_push_int(0);

	int prev_x = INT_MIN;
	int prev_y = INT_MIN;

	double *p = line_string->points;

	if (reverse_node_order) {
		p += (line_string->num_points - 1) * point_stride;
		point_stride = -point_stride;
	}

	for (int i = 0; i < line_string->num_points; i++) {
		int x = (int)(p[0] + 0.5);
		int y = (int)(p[1] + 0.5);

		p += point_stride;

		if (x == prev_x && y == prev_y) {
			continue;
		}

		prev_x = x;
		prev_y = y;

		Node *node = node_upsert(x, y);

		if (!node->id) {
			node->id = generate_id();

			PJ_COORD fin = proj_coord((double)x, (double)y, 0, 0);
			PJ_COORD wgs = proj_trans(context->projection, PJ_FWD, fin);

			fprintf(context->output,
					"<node visible=\"true\" id=\"%d\" lat=\"%.9f\" lon=\"%.9f\"/>\n",
					node->id, wgs.xy.x, wgs.xy.y);
		}

		way_buffer_push_int(node->id);
	}

	way_buffer_push_int(0);
	*way_id = generate_id();

	return 1;
}

static int
digiroad_row(sqlite3_stmt *statement, Query_Context *context)
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
	 * int... additional_tags
	 * int additional_tags_terminator = 0
	 *
	 * way_id corresponds to the <way> tag's id attribute. Each element
	 * of node_ids corresponds to the ref attribute of a distinct <nd> tag
	 * within the way element. Each of highway, route and oneway is an index
	 * into the osm_strings array defined at the top of this file, the
	 * corresponding element of which corresponds to the v attribute of a
	 * <tag> tag in the way element, with "highway", "route" or "oneway"
	 * respectively as its k attribute. name corresponds to the v attribute
	 * of a <tag> tag in the way element, with "name" as its k attribute. */

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

	const Geopackage_Binary_Header *geom_header = sqlite3_column_blob(statement, 0);
	int speed_limit = sqlite3_column_int(statement, 1);
	int class = sqlite3_column_int(statement, 2);
	int type = sqlite3_column_int(statement, 3);
	int direction = sqlite3_column_int(statement, 4);
	const char *name = sqlite3_column_text(statement, 5);

	int reverse_node_order = (direction == 3);

	if (!buffer_ids(geom_header, reverse_node_order, context)) {
		return 0;
	}

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

	int route = RT_NONE;

	if (type == 21 && class != 8) {
		route = RT_FERRY;
	}

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

	way_buffer_push_int(highway);
	way_buffer_push_int(route);
	way_buffer_push_int(oneway);
	way_buffer_push_int(speed_limit);
	way_buffer_push_string(name);
	way_buffer_push_int(0); /* No additional tags. */

	return 1;
}

static int
mml_iceroads_row(sqlite3_stmt* statement, Query_Context *context)
{
	assert(!strcmp(sqlite3_column_name(statement, 0), "geom"));
	assert(!strcmp(sqlite3_column_name(statement, 1), "direction"));
	assert(!strcmp(sqlite3_column_name(statement, 2), "name"));

	assert(sqlite3_column_type(statement, 0) == SQLITE_BLOB);
	assert(sqlite3_column_type(statement, 1) == SQLITE_INTEGER);
	assert(sqlite3_column_type(statement, 2) == SQLITE_TEXT);

	const Geopackage_Binary_Header *geom_header = sqlite3_column_blob(statement, 0);
	int direction = sqlite3_column_int(statement, 1);
	const char *name = sqlite3_column_text(statement, 2);

	int reverse_node_order = (direction == 2);

	if (!buffer_ids(geom_header, reverse_node_order, context)) {
		return 0;
	}

	int oneway = OW_NONE;

	switch (direction) {
	case 0:
		oneway = OW_NO;
		break;

	case 1:
	case 2:
		oneway = OW_YES;
		break;

	default:
		break;
	}

	way_buffer_push_int(HW_UNCLASSIFIED);
	way_buffer_push_int(RT_NONE);
	way_buffer_push_int(oneway);
	way_buffer_push_int(ICE_ROAD_SPEED_LIMIT);
	way_buffer_push_string(name);
	way_buffer_push_int(AT_ICE_ROAD);
	way_buffer_push_int(0);

	return 1;
}

static int
run_query(sqlite3_stmt *statement, Row_Function *callback, Query_Context *context)
{
	int rc;

	do {
		rc = sqlite3_step(statement);

		assert(rc != SQLITE_MISUSE);

		switch (rc) {
		case SQLITE_DONE:
		case SQLITE_BUSY:
			break;

		case SQLITE_ROW:
			if (callback(statement, context)) {
				context->num_valid++;
			} else {
				context->num_invalid++;
			}

			break;

		default:
			fprintf(stderr, "sqlite3_step: %s\n%s\n%s\n",
					sqlite3_errstr(rc),
					sqlite3_sql(statement),
					sqlite3_errmsg(sqlite3_db_handle(statement)));
			return 0;
		}
	} while (rc != SQLITE_DONE);
}

int
main(int argc, char **argv)
{
	/* Initialization. */

	int result = 1;
	Program_Configuration config = {0};

	if (!parse_commandline_arguments(&config, argc, argv)) {
		fprintf(stderr, "Usage: %s [--mml-iceroads <ice-roads-path>] <input-path> <output-path>\n",
				argv[0]);
		return 1;
	}

	PJ *projection = proj_create_crs_to_crs(0,
			"EPSG:3067", "EPSG:4326", 0);

	if (!projection) {
		fprintf(stderr, "proj_create_crs_to_crs: %s\n",
				proj_errno_string(proj_errno(0)));
		return 1;
	}

	FILE *output;

	if (!strcmp(config.output_path, "-")) {
		output = stdout;
	} else {
		output = fopen(config.output_path, "w");
	}

	if (!output) {
		fprintf(stderr, "Unable to open \"%s\" for writing: %s\n",
				config.output_path, strerror(errno));
		return 1;
	}

	sqlite3 *db;
	int rc;

	rc = sqlite3_open_v2(config.input_path, &db, SQLITE_OPEN_READONLY, 0);

	if (rc != SQLITE_OK) {
		fprintf(stderr, "Unable to open \"%s\" for reading: %s\n",
				config.input_path, sqlite3_errmsg(db));
		goto cleanup_output;
	}

	sqlite3_stmt *statement = prepare_statement(db, input_sql_query);

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
	Query_Context context = {0};
	context.output = output;
	context.projection = projection;

	if (!run_query(statement, digiroad_row, &context)) {
		goto cleanup;
	}

	num_ways_processed += context.num_valid;
	num_invalid_ways += context.num_invalid;

	context.num_valid = context.num_invalid = 0;

	if (config.mml_iceroads_path) {
		sqlite3_finalize(statement);
		sqlite3_close(db);

		rc = sqlite3_open_v2(config.mml_iceroads_path, &db, SQLITE_OPEN_READONLY, 0);

		if (rc != SQLITE_OK) {
			goto cleanup_output;
		}

		statement = prepare_statement(db, mml_iceroads_sql_query);

		if (!statement) {
			goto cleanup_output;
		}

		if(!run_query(statement, mml_iceroads_row, &context)) {
			goto cleanup;
		}

		num_ways_processed += context.num_valid;
		num_invalid_ways += context.num_invalid;
	}

	/* Write buffered ways. */

	for (int i = 0; i < num_ways_processed; i++) {
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

		for (int additional_tag; additional_tag = way_buffer_pop_int();) {
			fprintf(output, "<tag k=\"%s\" v=\"yes\"/>",
					osm_strings[additional_tag]);
		}

		fprintf(output, "</way>\n");
	}

	if (context.num_invalid > 0) {
		fprintf(stderr, "Input contained %d ways with geometries that"
				"could not be parsed and were skipped.\n",
				context.num_invalid);
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
