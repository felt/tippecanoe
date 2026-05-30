#ifdef MTRACE
#include <mcheck.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <pthread.h>
#include <vector>
#include <algorithm>
#include <set>
#include <map>
#include <string>
#include "jsonpull/jsonpull.h"
#include "pool.hpp"
#include "projection.hpp"
#include "memfile.hpp"
#include "main.hpp"
#include "mbtiles.hpp"
#include "geojson.hpp"
#include "geometry.hpp"
#include "options.hpp"
#include "serial.hpp"
#include "text.hpp"
#include "read_json.hpp"
#include "mvt.hpp"
#include "geojson-loop.hpp"
#include "milo/dtoa_milo.h"
#include "errors.hpp"

int serialize_geojson_feature(struct serialization_state *sst, json_object_ptr geometry, json_object_ptr properties, json_object_ptr id, int layer, json_object_ptr tippecanoe, json_object_ptr feature, std::string const &layername) {
	json_object_ptr geometry_type = json_hash_get(geometry, "type");
	if (geometry_type == nullptr) {
		static int warned = 0;
		if (!warned) {
			fprintf(stderr, "%s:%d: null geometry (additional not reported): ", sst->fname, sst->line);
			json_context(feature);
			warned = 1;
		}

		return 0;
	}

	if (geometry_type->type != JSON_STRING) {
		fprintf(stderr, "%s:%d: geometry type is not a string: ", sst->fname, sst->line);
		json_context(feature);
		return 0;
	}

	json_object_ptr coordinates = json_hash_get(geometry, "coordinates");
	if (coordinates == nullptr || coordinates->type != JSON_ARRAY) {
		fprintf(stderr, "%s:%d: feature without coordinates array: ", sst->fname, sst->line);
		json_context(feature);
		return 0;
	}

	int t;
	for (t = 0; t < GEOM_TYPES; t++) {
		if (geometry_type->string() == geometry_names[t]) {
			break;
		}
	}
	if (t >= GEOM_TYPES) {
		fprintf(stderr, "%s:%d: Can't handle geometry type %s: ", sst->fname, sst->line, geometry_type->string().c_str());
		json_context(feature);
		return 0;
	}

	int tippecanoe_minzoom = -1;
	int tippecanoe_maxzoom = -1;
	std::string tippecanoe_layername = layername;

	if (tippecanoe != nullptr) {
		json_object_ptr min = json_hash_get(tippecanoe, "minzoom");
		if (min != nullptr && (min->type == JSON_NUMBER)) {
			tippecanoe_minzoom = integer_zoom(sst->fname, milo::dtoa_milo(min->number()));
		}

		json_object_ptr max = json_hash_get(tippecanoe, "maxzoom");
		if (max != nullptr && (max->type == JSON_NUMBER)) {
			tippecanoe_maxzoom = integer_zoom(sst->fname, milo::dtoa_milo(max->number()));
		}

		json_object_ptr ln = json_hash_get(tippecanoe, "layer");
		if (ln != nullptr && (ln->type == JSON_STRING)) {
			tippecanoe_layername = ln->string();
		}
	}

	bool has_id = false;
	unsigned long long id_value = 0;
	if (id != nullptr) {
		if (id->type == JSON_NUMBER) {
			if (id->number() >= 0) {
				char *err = NULL;
				std::string id_number = milo::dtoa_milo(id->number());
				id_value = strtoull(id_number.c_str(), &err, 10);

				if (id->large_unsigned() != 0) {
					id_value = id->large_unsigned();
				}

				if (err != NULL && *err != '\0') {
					static bool warned_frac = false;

					if (!warned_frac) {
						fprintf(stderr, "Warning: Can't represent non-integer feature ID %s\n", milo::dtoa_milo(id->number()).c_str());
						warned_frac = true;
					}
				} else if (id->large_unsigned() == 0 && std::to_string(id_value) != milo::dtoa_milo(id->number())) {
					static bool warned = false;

					if (!warned) {
						fprintf(stderr, "Warning: Can't represent too-large feature ID %s\n", milo::dtoa_milo(id->number()).c_str());
						warned = true;
					}
				} else {
					has_id = true;
				}
			} else {
				static bool warned_neg = false;

				if (!warned_neg) {
					fprintf(stderr, "Warning: Can't represent negative feature ID %s\n", milo::dtoa_milo(id->number()).c_str());
					warned_neg = true;
				}
			}
		} else {
			bool converted = false;

			if (additional[A_CONVERT_NUMERIC_IDS] && id->type == JSON_STRING) {
				char *err = NULL;
				id_value = strtoull(id->string().c_str(), &err, 10);

				if (err != NULL && *err != '\0') {
					static bool warned_frac = false;

					if (!warned_frac) {
						fprintf(stderr, "Warning: Can't represent non-integer feature ID %s\n", id->string().c_str());
						warned_frac = true;
					}
				} else if (std::to_string(id_value) != id->string()) {
					static bool warned = false;

					if (!warned) {
						fprintf(stderr, "Warning: Can't represent too-large feature ID %s\n", id->string().c_str());
						warned = true;
					}
				} else {
					has_id = true;
					converted = true;
				}
			}

			if (!converted) {
				static bool warned_nan = false;

				if (!warned_nan) {
					fprintf(stderr, "Warning: Can't represent non-numeric feature ID %s\n", json_stringify(id).c_str());
					warned_nan = true;
				}
			}
		}
	}

	std::vector<std::shared_ptr<std::string>> full_keys;
	std::vector<serial_val> values;
	key_pool key_pool;

	if (properties != nullptr && properties->type == JSON_HASH) {
		const auto &entries = properties->entries();
		full_keys.reserve(entries.size());
		values.reserve(entries.size());

		for (const auto &e : entries) {
			if (e.key->type == JSON_STRING) {
				serial_val sv = stringify_value(e.value, sst->fname, sst->line, feature);

				full_keys.emplace_back(key_pool.pool(e.key->string().c_str()));
				values.push_back(std::move(sv));
			}
		}
	}

	drawvec dv;
	parse_coordinates(t, coordinates, dv, VT_MOVETO, sst->fname, sst->line, feature);

	serial_feature sf;
	sf.layer = layer;
	sf.segment = sst->segment;
	sf.t = mb_geometry[t];
	sf.has_id = has_id;
	sf.id = id_value;
	sf.tippecanoe_minzoom = tippecanoe_minzoom;
	sf.tippecanoe_maxzoom = tippecanoe_maxzoom;
	sf.geometry = dv;
	sf.feature_minzoom = 0;	 // Will be filled in during index merging
	sf.seq = *(sst->layer_seq);
	sf.full_keys = std::move(full_keys);
	sf.full_values = std::move(values);

	return serialize_feature(sst, sf, tippecanoe_layername);
}

void check_crs(json_object_ptr j, const char *reading) {
	json_object_ptr crs = json_hash_get(j, "crs");
	if (crs != nullptr) {
		json_object_ptr properties = json_hash_get(crs, "properties");
		if (properties != nullptr) {
			json_object_ptr name = json_hash_get(properties, "name");
			if (name != nullptr && name->type == JSON_STRING) {
				if (name->string() != projection->alias) {
					if (!quiet) {
						fprintf(stderr, "%s: Warning: GeoJSON specified projection \"%s\", not the expected \"%s\".\n", reading, name->string().c_str(), projection->alias);
						fprintf(stderr, "%s: If \"%s\" is not the expected projection, use -s to specify the right one.\n", reading, projection->alias);
					}
				}
			}
		}
	}
}

struct json_serialize_action : json_feature_action {
	serialization_state *sst;
	int layer;
	std::string layername;

	int add_feature(json_object_ptr geometry, bool geometrycollection, json_object_ptr properties, json_object_ptr id, json_object_ptr tippecanoe, json_object_ptr feature) {
		sst->line = geometry->parser->line;
		if (geometrycollection) {
			int ret = 1;
			for (size_t g = 0; g < geometry->array().size(); g++) {
				ret &= serialize_geojson_feature(sst, geometry->array()[g], properties, id, layer, tippecanoe, feature, layername);
			}
			return ret;
		} else {
			return serialize_geojson_feature(sst, geometry, properties, id, layer, tippecanoe, feature, layername);
		}
	}

	void check_crs(json_object_ptr j) {
		::check_crs(j, fname.c_str());
	}
};

void parse_json(struct serialization_state *sst, json_pull_ptr jp, int layer, std::string layername) {
	json_serialize_action jsa;
	jsa.fname = sst->fname;
	jsa.sst = sst;
	jsa.layer = layer;
	jsa.layername = layername;

	parse_json(&jsa, jp);
}

void *run_parse_json(void *v) {
	struct parse_json_args *pja = (struct parse_json_args *) v;

	parse_json(pja->sst, pja->jp, pja->layer, *pja->layername);

	return NULL;
}

struct jsonmap {
	char *map;
	unsigned long long off;
	unsigned long long end;
};

ssize_t json_map_read(struct json_pull *jp, char *buffer, size_t n) {
	struct jsonmap *jm = (struct jsonmap *) jp->source;

	if (jm->off + n >= jm->end) {
		n = jm->end - jm->off;
	}

	memcpy(buffer, jm->map + jm->off, n);
	jm->off += n;

	return n;
}

json_pull_ptr json_begin_map(char *map, long long len) {
	struct jsonmap *jm = new jsonmap;
	if (jm == NULL) {
		perror("Out of memory");
		exit(EXIT_MEMORY);
	}

	jm->map = map;
	jm->off = 0;
	jm->end = len;

	return json_begin(json_map_read, jm);
}

void json_end_map(json_pull_ptr jp) {
	delete (struct jsonmap *) jp->source;
	json_end(jp);
}
