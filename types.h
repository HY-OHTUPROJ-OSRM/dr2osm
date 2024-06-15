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

typedef struct __attribute__((packed)) {
	uint8_t magic[2];
	uint8_t version;
	uint8_t flags;
	uint32_t srs_id;
	char envelope[];
} Geopackage_Binary_Header;

typedef struct {
	double x, y, z, m;
} Point_Zm;

typedef struct __attribute__((packed)) {
	uint8_t byte_order;
	uint32_t type;
	uint32_t num_points;
	Point_Zm points[];
} Wkb_Line_String_Zm;
