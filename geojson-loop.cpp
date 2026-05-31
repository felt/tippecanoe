#ifdef MTRACE
#include <mcheck.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include "geojson-loop.hpp"
#include "jsonpull/jsonpull.h"

// XXX duplicated
#define GEOM_TYPES 6
static const char *geometry_names[GEOM_TYPES] = {
	"Point",
	"MultiPoint",
	"LineString",
	"MultiLineString",
	"Polygon",
	"MultiPolygon",
};

// XXX duplicated
static void json_context(json_object *j) {
	std::string s = json_stringify(j);

	if (s.size() >= 500) {
		s.resize(497);
		s.append("...");
	}

	fprintf(stderr, "in JSON object %s\n", s.c_str());
}

void parse_json(json_feature_action *jfa, json_pull_ptr &jp) {
	long long found_hashes = 0;
	long long found_features = 0;
	long long found_geometries = 0;

	while (1) {
		json_object *j = json_read(jp);
		if (j == nullptr) {
			if (jp->error != nullptr) {
				fprintf(stderr, "%s:%d: %s: ", jfa->fname.c_str(), jp->line, jp->error);
				if (jp->root != nullptr) {
					json_context(jp->root.get());
				} else {
					fprintf(stderr, "\n");
				}
			}

			jp->root.reset();
			break;
		}

		if (j->type == JSON_HASH) {
			found_hashes++;

			if (found_hashes == 50 && found_features == 0 && found_geometries == 0) {
				fprintf(stderr, "%s:%d: Warning: not finding any GeoJSON features or geometries in input yet after 50 objects.\n", jfa->fname.c_str(), jp->line);
			}
		}

		json_object *type = json_hash_get(j, "type");
		if (type == nullptr || type->type != JSON_STRING) {
			continue;
		}

		if (found_features == 0) {
			int i;
			int is_geometry = 0;
			for (i = 0; i < GEOM_TYPES; i++) {
				if (type->string() == geometry_names[i]) {
					is_geometry = 1;
					break;
				}
			}

			if (is_geometry) {
				if (j->parent != nullptr) {
					if (j->parent->type == JSON_ARRAY && j->parent->parent != nullptr) {
						if (j->parent->parent->type == JSON_HASH) {
							json_object *geometries = json_hash_get(j->parent->parent, "geometries");
							if (geometries != nullptr) {
								// Parent of Parent must be a GeometryCollection
								is_geometry = 0;
							}
						}
					} else if (j->parent->type == JSON_HASH) {
						json_object *geometry = json_hash_get(j->parent, "geometry");
						if (geometry != nullptr) {
							// Parent must be a Feature
							is_geometry = 0;
						}
					}
				}
			}

			if (is_geometry) {
				json_object *jo = j;
				while (jo != nullptr) {
					if (jo->parent != nullptr && jo->parent->type == JSON_HASH) {
						if (json_hash_get(jo->parent, "properties") == jo) {
							// Ancestor is the value corresponding to a properties key
							is_geometry = 0;
							break;
						}
					}
					jo = jo->parent;
				}
			}

			if (is_geometry) {
				if (found_features != 0 && found_geometries == 0) {
					fprintf(stderr, "%s:%d: Warning: found a mixture of features and bare geometries\n", jfa->fname.c_str(), jp->line);
				}
				found_geometries++;

				jfa->add_feature(j, false, nullptr, nullptr, nullptr, j);
				json_free(j);
				continue;
			}
		}

		if (type->string() != "Feature") {
			if (type->string() == "FeatureCollection") {
				jfa->check_crs(j);
				json_free(j);
			}

			continue;
		}

		if (found_features == 0 && found_geometries != 0) {
			fprintf(stderr, "%s:%d: Warning: found a mixture of features and bare geometries\n", jfa->fname.c_str(), jp->line);
		}
		found_features++;

		json_object *geometry = json_hash_get(j, "geometry");
		if (geometry == nullptr) {
			fprintf(stderr, "%s:%d: feature with no geometry: ", jfa->fname.c_str(), jp->line);
			json_context(j);
			json_free(j);
			continue;
		}

		json_object *properties = json_hash_get(j, "properties");
		if (properties == nullptr || (properties->type != JSON_HASH && properties->type != JSON_NULL)) {
			fprintf(stderr, "%s:%d: feature without properties hash: ", jfa->fname.c_str(), jp->line);
			json_context(j);
			json_free(j);
			continue;
		}

		bool is_feature = true;
		{
			json_object *jo = j;
			while (jo != nullptr) {
				if (jo->parent != nullptr && jo->parent->type == JSON_HASH) {
					if (json_hash_get(jo->parent, "properties") == jo) {
						// Ancestor is the value corresponding to a properties key
						is_feature = false;
						break;
					}
				}
				jo = jo->parent;
			}
		}
		if (!is_feature) {
			continue;
		}

		json_object *tippecanoe = json_hash_get(j, "tippecanoe");
		json_object *id = json_hash_get(j, "id");

		json_object *geometries = json_hash_get(geometry, "geometries");
		if (geometries != nullptr && geometries->type == JSON_ARRAY) {
			jfa->add_feature(geometries, true, properties, id, tippecanoe, j);
		} else {
			jfa->add_feature(geometry, false, properties, id, tippecanoe, j);
		}

		json_free(j);

		/* XXX check for any non-features in the outer object */
	}
}
