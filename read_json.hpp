#define GEOM_POINT 0	       /* array of positions */
#define GEOM_MULTIPOINT 1      /* array of arrays of positions */
#define GEOM_LINESTRING 2      /* array of arrays of positions */
#define GEOM_MULTILINESTRING 3 /* array of arrays of arrays of positions */
#define GEOM_POLYGON 4	       /* array of arrays of arrays of positions */
#define GEOM_MULTIPOLYGON 5    /* array of arrays of arrays of arrays of positions */
#define GEOM_TYPES 6

extern const char *geometry_names[GEOM_TYPES];
extern int geometry_within[GEOM_TYPES];
extern int mb_geometry[GEOM_TYPES];

void json_context(json_object *j);
void parse_coordinates(int t, json_object *j, drawvec &out, int op, const char *fname, int line, json_object *feature);
std::pair<int, drawvec> parse_geometry(json_object *geometry, json_pull *jp, json_object *j,
				       int z, int x, int y, long long extent, bool fix_longitudes, bool mvt_style);
std::vector<mvt_layer> parse_layers(FILE *fp, int z, unsigned x, unsigned y, int extent, bool fix_longitudes);

serial_val stringify_value(json_object *value, const char *reading, int line, json_object *feature);
