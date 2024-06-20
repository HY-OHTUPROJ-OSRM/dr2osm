#if defined(_MSC_VER)
#define PACK_BEGIN __pragma(pack(push, 1)) struct
#define PACK_END __pragma(pack(pop))
#else
#define PACK_BEGIN struct __attribute__((packed))
#define PACK_END
#endif

typedef struct {
	char *input_path;
	char *output_path;
	char *mml_iceroads_path;
	int default_speed_limits;
} Program_Configuration;

typedef struct {
	char *start;
	intptr_t size;
	intptr_t first_in_offset;
	intptr_t next_in_offset;
	intptr_t commit_threshold_offset;
} Growable_Buffer;

typedef struct {
	int x, y;
	int id;
	int child_node_offsets[4];
} Node;

typedef struct {
	FILE *output;
	PJ *projection;
	int num_valid, num_invalid, num_total;
	int default_speed_limits;
} Query_Context;

typedef PACK_BEGIN {
	uint8_t magic[2];
	uint8_t version;
	uint8_t flags;
	uint32_t srs_id;
	char envelope[];
} PACK_END Geopackage_Binary_Header;

typedef PACK_BEGIN {
	uint8_t byte_order;
	uint32_t type;
	uint32_t num_points;
	double points[];
} PACK_END Wkb_Line_String_Any;

typedef int Row_Function(sqlite3_stmt *, Query_Context *);
