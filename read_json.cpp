#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>
#include <map>
#include "jsonpull/jsonpull.h"
#include "geometry.hpp"
#include "projection.hpp"
#include "read_json.hpp"
#include "text.hpp"
#include "mvt.hpp"
#include "milo/dtoa_milo.h"
#include "errors.hpp"

const char *geometry_names[GEOM_TYPES] = {
	"Point",
	"MultiPoint",
	"LineString",
	"MultiLineString",
	"Polygon",
	"MultiPolygon",
};

int geometry_within[GEOM_TYPES] = {
	-1,		 /* point */
	GEOM_POINT,	 /* multipoint */
	GEOM_POINT,	 /* linestring */
	GEOM_LINESTRING, /* multilinestring */
	GEOM_LINESTRING, /* polygon */
	GEOM_POLYGON,	 /* multipolygon */
};

int mb_geometry[GEOM_TYPES] = {
	VT_POINT,
	VT_POINT,
	VT_LINE,
	VT_LINE,
	VT_POLYGON,
	VT_POLYGON,
};

void json_context(json_object *j) {
	char *s = json_stringify(j);

	if (strlen(s) >= 500) {
		snprintf(s + 497, strlen(s) + 1 - 497, "...");
	}

	fprintf(stderr, "in JSON object %s\n", s);
	free(s);  // stringify
}

void parse_geometry(int t, json_object *j, drawvec &out, int op, const char *fname, int line, json_object *feature) {
	if (j == NULL || j->type != JSON_ARRAY) {
		fprintf(stderr, "%s:%d: expected array for geometry type %d: ", fname, line, t);
		json_context(feature);
		return;
	}

	int within = geometry_within[t];
	if (within >= 0) {
		size_t i;
		for (i = 0; i < j->value.array.length; i++) {
			if (within == GEOM_POINT) {
				if (i == 0 || mb_geometry[t] == VT_POINT) {
					op = VT_MOVETO;
				} else {
					op = VT_LINETO;
				}
			}

			parse_geometry(within, j->value.array.array[i], out, op, fname, line, feature);
		}
	} else {
		if (j->value.array.length >= 2 && j->value.array.array[0]->type == JSON_NUMBER && j->value.array.array[1]->type == JSON_NUMBER) {
			long long x, y;
			double lon = j->value.array.array[0]->value.number.number;
			double lat = j->value.array.array[1]->value.number.number;
			projection->project(lon, lat, 32, &x, &y);

			if (j->value.array.length > 2) {
				static int warned = 0;

				if (!warned) {
					fprintf(stderr, "%s:%d: ignoring dimensions beyond two: ", fname, line);
					json_context(j);
					fprintf(stderr, "%s:%d: ignoring dimensions beyond two: ", fname, line);
					json_context(feature);
					warned = 1;
				}
			}

			out.push_back(draw(op, x, y));
		} else {
			fprintf(stderr, "%s:%d: malformed point: ", fname, line);
			json_context(j);
			fprintf(stderr, "%s:%d: malformed point: ", fname, line);
			json_context(feature);
			exit(EXIT_JSON);
		}
	}

	if (t == GEOM_POLYGON) {
		// Note that this is not using the correct meaning of closepath.
		//
		// We are using it here to close an entire Polygon, to distinguish
		// the Polygons within a MultiPolygon from each other.
		//
		// This will be undone in fix_polygon(), which needs to know which
		// rings come from which Polygons so that it can make the winding order
		// of the outer ring be the opposite of the order of the inner rings.

		out.push_back(draw(VT_CLOSEPATH, 0, 0));
	}
}

void stringify_value(json_object *value, int &type, std::string &stringified, const char *reading, int line, json_object *feature) {
	if (value != NULL) {
		int vt = value->type;
		std::string val;

		if (vt == JSON_STRING) {
			val = value->value.string.string;
		} else if (vt == JSON_NUMBER) {
			if (value->value.number.large_unsigned != 0) {
				val = std::to_string(value->value.number.large_unsigned);
			} else if (value->value.number.large_signed != 0) {
				val = std::to_string(value->value.number.large_signed);
			} else {
				val = milo::dtoa_milo(value->value.number.number);
			}
		} else if (vt == JSON_TRUE) {
			val = "true";
		} else if (vt == JSON_FALSE) {
			val = "false";
		} else if (vt == JSON_NULL) {
			val = "null";
		} else {
			const char *v = json_stringify(value);
			val = std::string(v);
			free((void *) v);  // stringify
		}

		if (vt == JSON_STRING) {
			type = mvt_string;
			stringified = val;
			std::string err = check_utf8(val);
			if (err != "") {
				fprintf(stderr, "%s:%d: %s: ", reading, line, err.c_str());
				json_context(feature);
				exit(EXIT_UTF8);
			}
		} else if (vt == JSON_NUMBER) {
			type = mvt_double;
			stringified = val;
		} else if (vt == JSON_TRUE || vt == JSON_FALSE) {
			type = mvt_bool;
			stringified = val;
		} else if (vt == JSON_NULL) {
			type = mvt_null;
			stringified = "null";
		} else {
			type = mvt_string;
			stringified = val;
		}
	}
}
