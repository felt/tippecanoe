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
#include "serial.hpp"

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

void parse_coordinates(int t, json_object *j, drawvec &out, int op, const char *fname, int line, json_object *feature) {
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

			parse_coordinates(within, j->value.array.array[i], out, op, fname, line, feature);
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

// This is used to convert a JSON attribute value into a serial_val-style
// type and stringified value. All numeric values, even if they are integers,
// even integers that are too large to fit in a double but will still be
// stringified with their original precision, are recorded here as mvt_double.
serial_val stringify_value(json_object *value, const char *reading, int line, json_object *feature) {
	serial_val sv;

	if (value != NULL) {
		int vt = value->type;

		if (vt == JSON_STRING) {
			sv.type = mvt_string;
			sv.s = value->value.string.string;

			std::string err = check_utf8(sv.s);
			if (err.size() > 0) {
				fprintf(stderr, "%s:%d: %s: ", reading, line, err.c_str());
				json_context(feature);
				exit(EXIT_UTF8);
			}
		} else if (vt == JSON_NUMBER) {
			sv.type = mvt_double;

			if (value->value.number.large_unsigned != 0) {
				sv.s = std::to_string(value->value.number.large_unsigned);
			} else if (value->value.number.large_signed != 0) {
				sv.s = std::to_string(value->value.number.large_signed);
			} else {
				sv.s = milo::dtoa_milo(value->value.number.number);
			}
		} else if (vt == JSON_TRUE) {
			sv.type = mvt_bool;
			sv.s = "true";
		} else if (vt == JSON_FALSE) {
			sv.type = mvt_bool;
			sv.s = "false";
		} else if (vt == JSON_NULL) {
			sv.type = mvt_null;
			sv.s = "null";
		} else {
			sv.type = mvt_string;
			const char *v = json_stringify(value);
			sv.s = std::string(v);
			free((void *) v);  // stringify
		}
	}

	return sv;
}

// XXX deduplicate
static std::vector<mvt_geometry> to_feature(drawvec &geom) {
	std::vector<mvt_geometry> out;

	for (size_t i = 0; i < geom.size(); i++) {
		out.push_back(mvt_geometry(geom[i].op, geom[i].x, geom[i].y));
	}

	return out;
}

std::pair<int, drawvec> parse_geometry(json_object *geometry, json_pull *jp, json_object *j,
				       int z, int x, int y, long long extent, bool fix_longitudes, bool mvt_style) {
	json_object *geometry_type = json_hash_get(geometry, "type");
	if (geometry_type == NULL) {
		fprintf(stderr, "Filter output:%d: null geometry (additional not reported): ", jp->line);
		json_context(j);
		exit(EXIT_JSON);
	}

	if (geometry_type->type != JSON_STRING) {
		fprintf(stderr, "Filter output:%d: geometry type is not a string: ", jp->line);
		json_context(j);
		exit(EXIT_JSON);
	}

	json_object *coordinates = json_hash_get(geometry, "coordinates");
	if (coordinates == NULL || coordinates->type != JSON_ARRAY) {
		fprintf(stderr, "Filter output:%d: geometry without coordinates array: ", jp->line);
		json_context(j);
		exit(EXIT_JSON);
	}

	int t;
	for (t = 0; t < GEOM_TYPES; t++) {
		if (strcmp(geometry_type->value.string.string, geometry_names[t]) == 0) {
			break;
		}
	}
	if (t >= GEOM_TYPES) {
		fprintf(stderr, "Filter output:%d: Can't handle geometry type %s: ", jp->line, geometry_type->value.string.string);
		json_context(j);
		exit(EXIT_JSON);
	}

	drawvec dv;
	parse_coordinates(t, coordinates, dv, VT_MOVETO, "Filter output", jp->line, j);

	// handle longitude wraparound
	//
	// this is supposed to be data for a single tile,
	// so any jump from the left hand side edge of the world
	// to the right edge, or vice versa, is unexpected,
	// so move it to the other side.

	if (fix_longitudes && mb_geometry[t] == VT_POLYGON) {
		const long long quarter_world = 1LL << 30;
		const long long world = 1LL << 32;

		bool copy_to_left = false;
		bool copy_to_right = false;

		for (size_t i = 0; i < dv.size(); i++) {
			// is this vertex on a different side of the world
			// than the first vertex? then shift this one to match
			if (i > 0) {
				if ((dv[0].x < quarter_world) && (dv[i].x > 3 * quarter_world)) {
					dv[i].x -= world;
				}
				if ((dv[0].x > 3 * quarter_world) && (dv[i].x < quarter_world)) {
					dv[i].x += world;
				}
			}

			// does it stick off the edge of the world?
			// then we need another copy on the other side of the world
			if (dv[i].x < 0) {
				copy_to_right = true;
			}
			if (dv[i].x > world) {
				copy_to_left = true;
			}
		}

		if (copy_to_left) {
			size_t n = dv.size();
			for (size_t i = 0; i < n; i++) {
				dv.emplace_back(dv[i].op, dv[i].x - world, (long long) dv[i].y);
			}
		}
		if (copy_to_right) {
			size_t n = dv.size();
			for (size_t i = 0; i < n; i++) {
				dv.emplace_back(dv[i].op, dv[i].x + world, (long long) dv[i].y);
			}
		}
	}

	if (mb_geometry[t] == VT_POLYGON) {
		dv = fix_polygon(dv, false, false);
	}

	// Offset and scale geometry from global to tile
	for (size_t i = 0; i < dv.size(); i++) {
		long long scale = 1LL << (32 - z);

		// offset to tile
		dv[i].x -= scale * x;
		dv[i].y -= scale * y;

		// scale to tile
		dv[i].x = std::round(dv[i].x * (extent / (double) scale));
		dv[i].y = std::round(dv[i].y * (extent / (double) scale));
	}

	if (mb_geometry[t] == VT_POLYGON) {
		// don't try scaling up because we may have coordinates
		// on the other side of the world
		dv = clean_or_clip_poly(dv, z, 256, true, false);
		if (dv.size() < 3) {
			dv.clear();
		}
	}
	dv = remove_noop(dv, mb_geometry[t], 0);

	if (mvt_style) {
		if (mb_geometry[t] == VT_POLYGON) {
			dv = close_poly(dv);
		}
	}

	return std::pair<int, drawvec>(t, dv);
}

std::vector<mvt_layer> parse_layers(FILE *fp, int z, unsigned x, unsigned y, int extent, bool fix_longitudes) {
	std::map<std::string, mvt_layer> ret;
	std::shared_ptr<std::string> tile_stringpool = std::make_shared<std::string>();

	json_pull *jp = json_begin_file(fp);
	while (1) {
		json_object *j = json_read(jp);
		if (j == NULL) {
			if (jp->error != NULL) {
				fprintf(stderr, "Filter output:%d: %s: ", jp->line, jp->error);
				if (jp->root != NULL) {
					json_context(jp->root);
				} else {
					fprintf(stderr, "\n");
				}
				exit(EXIT_JSON);
			}

			json_free(jp->root);
			break;
		}

		json_object *type = json_hash_get(j, "type");
		if (type == NULL || type->type != JSON_STRING) {
			continue;
		}
		if (strcmp(type->value.string.string, "Feature") != 0) {
			continue;
		}

		json_object *properties = json_hash_get(j, "properties");
		if (properties == NULL || (properties->type != JSON_HASH && properties->type != JSON_NULL)) {
			fprintf(stderr, "Filter output:%d: feature without properties hash: ", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_JSON);
		}

		std::string layername = "unknown";
		json_object *tippecanoe = json_hash_get(j, "tippecanoe");
		json_object *layer = NULL;
		if (tippecanoe != NULL) {
			layer = json_hash_get(tippecanoe, "layer");
			if (layer != NULL && layer->type == JSON_STRING) {
				layername = std::string(layer->value.string.string);
			}
		}

		if (ret.count(layername) == 0) {
			mvt_layer l;
			l.name = layername;
			l.version = 2;
			l.extent = extent;

			ret.insert(std::pair<std::string, mvt_layer>(layername, l));
		}
		auto l = ret.find(layername);

		json_object *geometry = json_hash_get(j, "geometry");
		if (geometry == NULL) {
			fprintf(stderr, "Filter output:%d: filtered feature with no geometry: ", jp->line);
			json_context(j);
			json_free(j);
			exit(EXIT_JSON);
		}

		std::pair<int, drawvec> parsed_geometry = parse_geometry(geometry, jp, j, z, x, y, extent, fix_longitudes, true);

		int t = parsed_geometry.first;
		drawvec &dv = parsed_geometry.second;

		if (dv.size() > 0) {
			mvt_feature feature;
			feature.type = mb_geometry[t];
			feature.geometry = to_feature(dv);

			json_object *id = json_hash_get(j, "id");
			if (id != NULL && id->type == JSON_NUMBER) {
				feature.id = id->value.number.number;
				if (id->value.number.large_unsigned > 0) {
					feature.id = id->value.number.large_unsigned;
				}
				feature.has_id = true;
			}

			for (size_t i = 0; i < properties->value.object.length; i++) {
				serial_val sv = stringify_value(properties->value.object.values[i], "Filter output", jp->line, j);

				// Nulls can be excluded here because this is the postfilter
				// and it is nearly time to create the vector representation

				if (sv.type != mvt_null) {
					mvt_value v = stringified_to_mvt_value(sv.type, sv.s.c_str(), tile_stringpool);
					l->second.tag(feature, std::string(properties->value.object.keys[i]->value.string.string), v);
				}
			}

			l->second.features.push_back(feature);
		}

		json_free(j);
	}

	json_end(jp);

	std::vector<mvt_layer> final;
	for (auto a : ret) {
		final.push_back(a.second);
	}

	return final;
}
