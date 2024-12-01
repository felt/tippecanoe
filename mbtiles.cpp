// for vasprintf() on Linux
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <cmath>
#include <climits>
#include <vector>
#include <string>
#include <set>
#include <map>
#include <sys/stat.h>
#include "mvt.hpp"
#include "mbtiles.hpp"
#include "text.hpp"
#include "milo/dtoa_milo.h"
#include "write_json.hpp"
#include "version.hpp"
#include "errors.hpp"

size_t max_tilestats_attributes = 1000;
size_t max_tilestats_sample_values = 1000;
size_t max_tilestats_values = 100;

sqlite3 *mbtiles_open(char *dbname, char **argv, int forcetable) {
	sqlite3 *outdb;

	if (sqlite3_open(dbname, &outdb) != SQLITE_OK) {
		fprintf(stderr, "%s: %s: %s\n", argv[0], dbname, sqlite3_errmsg(outdb));
		exit(EXIT_OPEN);
	}

	char *err = NULL;
	if (sqlite3_exec(outdb, "PRAGMA synchronous=0", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: async: %s\n", argv[0], err);
		exit(EXIT_SQLITE);
	}
	if (sqlite3_exec(outdb, "PRAGMA locking_mode=EXCLUSIVE", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: async: %s\n", argv[0], err);
		exit(EXIT_SQLITE);
	}
	if (sqlite3_exec(outdb, "PRAGMA journal_mode=DELETE", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: async: %s\n", argv[0], err);
		exit(EXIT_SQLITE);
	}
	if (sqlite3_exec(outdb, "CREATE TABLE metadata (name text, value text);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: Tileset \"%s\" already exists. You can use --force if you want to delete the old tileset.\n", argv[0], dbname);
		fprintf(stderr, "%s: %s\n", argv[0], err);
		if (!forcetable) {
			exit(EXIT_EXISTS);
		}
	}
	if (sqlite3_exec(outdb, "create unique index name on metadata (name);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: index metadata: %s\n", argv[0], err);
		if (!forcetable) {
			exit(EXIT_EXISTS);
		}
	}

	// "map" maps z/x/y coordinates to a content hash
	if (sqlite3_exec(outdb, "CREATE TABLE map (zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, tile_id TEXT);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: create map table: %s\n", argv[0], err);
		if (!forcetable) {
			exit(EXIT_EXISTS);
		}
	}
	if (sqlite3_exec(outdb, "CREATE UNIQUE INDEX map_index ON map (zoom_level, tile_column, tile_row);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: create map index: %s\n", argv[0], err);
		if (!forcetable) {
			exit(EXIT_EXISTS);
		}
	}

	// "images" maps a content hash to tile contents, per zoom level
	if (sqlite3_exec(outdb, "CREATE TABLE images (zoom_level integer, tile_data blob, tile_id text);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: create images table: %s\n", argv[0], err);
		if (!forcetable) {
			exit(EXIT_EXISTS);
		}
	}
	if (sqlite3_exec(outdb, "CREATE UNIQUE INDEX images_id ON images (zoom_level, tile_id);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: create images index: %s\n", argv[0], err);
		if (!forcetable) {
			exit(EXIT_EXISTS);
		}
	}

	// "tiles" is a view that retrieves content from "images"
	// via the content hash looked up from "map".
	if (sqlite3_exec(outdb, "CREATE VIEW tiles AS SELECT map.zoom_level AS zoom_level, map.tile_column AS tile_column, map.tile_row AS tile_row, images.tile_data AS tile_data FROM map JOIN images ON images.tile_id = map.tile_id and images.zoom_level = map.zoom_level;", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: create tiles view: %s\n", argv[0], err);
		if (!forcetable) {
			exit(EXIT_EXISTS);
		}
	}

	return outdb;
}

void mbtiles_write_tile(sqlite3 *outdb, int z, int tx, int ty, const char *data, int size) {
	std::string hash = std::to_string(fnv1a(std::string(data, size)));

	// following https://github.com/mapbox/node-mbtiles/blob/master/lib/mbtiles.js

	sqlite3_stmt *stmt;
	const char *images = "replace into images (zoom_level, tile_id, tile_data) values (?, ?, ?)";
	if (sqlite3_prepare_v2(outdb, images, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 images prep failed\n");
		exit(EXIT_SQLITE);
	}

	sqlite3_bind_int(stmt, 1, z);
	sqlite3_bind_text(stmt, 2, hash.c_str(), hash.size(), NULL);
	sqlite3_bind_blob(stmt, 3, data, size, NULL);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		fprintf(stderr, "sqlite3 images insert failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}
	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 images finalize failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}

	const char *map = "insert into map (zoom_level, tile_column, tile_row, tile_id) values (?, ?, ?, ?)";
	if (sqlite3_prepare_v2(outdb, map, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 map prep failed\n");
		exit(EXIT_SQLITE);
	}

	sqlite3_bind_int(stmt, 1, z);
	sqlite3_bind_int(stmt, 2, tx);
	sqlite3_bind_int(stmt, 3, (1 << z) - 1 - ty);
	sqlite3_bind_text(stmt, 4, hash.c_str(), hash.size(), NULL);

	if (sqlite3_step(stmt) != SQLITE_DONE) {
		fprintf(stderr, "sqlite3 map insert failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}
	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 finalize failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}
}

void mbtiles_erase_zoom(sqlite3 *outdb, int z) {
	sqlite3_stmt *stmt;

	const char *query = "delete from map where zoom_level = ?";
	if (sqlite3_prepare_v2(outdb, query, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 delete map prep failed\n");
		exit(EXIT_SQLITE);
	}

	sqlite3_bind_int(stmt, 1, z);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		fprintf(stderr, "sqlite3 delete map failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}
	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 delete map finalize failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}

	query = "delete from images where zoom_level = ?";
	if (sqlite3_prepare_v2(outdb, query, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 delete images prep failed\n");
		exit(EXIT_SQLITE);
	}

	sqlite3_bind_int(stmt, 1, z);
	if (sqlite3_step(stmt) != SQLITE_DONE) {
		fprintf(stderr, "sqlite3 delete images failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}
	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 delete images finalize failed: %s\n", sqlite3_errmsg(outdb));
		exit(EXIT_SQLITE);
	}
}

bool serial_val::operator<(const serial_val &o) const {
	if (s < o.s) {
		return true;
	}
	if (s == o.s && type < o.type) {
		return true;
	}
	return false;
}

bool serial_val::operator!=(const serial_val &o) const {
	if (type != o.type) {
		return true;
	}
	if (s != o.s) {
		return true;
	}
	return false;
}

void tilestats(std::map<std::string, layermap_entry> const &layermap1, size_t elements, json_writer &state) {
	// Consolidate layers/attributes whose names are truncated
	std::vector<std::map<std::string, layermap_entry>> lmv;
	lmv.push_back(layermap1);
	std::map<std::string, layermap_entry> layermap = merge_layermaps(lmv, true);

	state.nospace = true;
	state.json_write_hash();

	state.nospace = true;
	state.json_write_string("layerCount");
	state.nospace = true;
	state.json_write_unsigned(layermap.size());

	state.nospace = true;
	state.json_write_string("layers");
	state.nospace = true;
	state.json_write_array();

	for (auto layer : layermap) {
		state.nospace = true;
		state.json_write_hash();

		state.nospace = true;
		state.json_write_string("layer");
		state.nospace = true;
		state.json_write_string(layer.first);

		state.nospace = true;
		state.json_write_string("count");
		state.nospace = true;
		state.json_write_unsigned(layer.second.points + layer.second.lines + layer.second.polygons);

		std::string geomtype = "Polygon";
		if (layer.second.points >= layer.second.lines && layer.second.points >= layer.second.polygons) {
			geomtype = "Point";
		} else if (layer.second.lines >= layer.second.polygons && layer.second.lines >= layer.second.points) {
			geomtype = "LineString";
		}

		state.nospace = true;
		state.json_write_string("geometry");
		state.nospace = true;
		state.json_write_string(geomtype);

		size_t attrib_count = layer.second.tilestats.size();
		if (attrib_count > max_tilestats_attributes) {
			attrib_count = max_tilestats_attributes;
		}

		state.nospace = true;
		state.json_write_string("attributeCount");
		state.nospace = true;
		state.json_write_unsigned(attrib_count);

		state.nospace = true;
		state.json_write_string("attributes");
		state.nospace = true;
		state.json_write_array();

		size_t attrs = 0;
		for (auto attribute : layer.second.tilestats) {
			if (attrs == elements) {
				break;
			}
			attrs++;

			state.nospace = true;
			state.json_write_hash();

			state.nospace = true;
			state.json_write_string("attribute");
			state.nospace = true;
			state.json_write_string(attribute.first);

			size_t val_count = attribute.second.sample_values.size();
			if (val_count > max_tilestats_sample_values) {
				val_count = max_tilestats_sample_values;
			}

			state.nospace = true;
			state.json_write_string("count");
			state.nospace = true;
			state.json_write_unsigned(val_count);

			int type = 0;
			for (auto s : attribute.second.sample_values) {
				type |= (1 << s.type);
			}

			std::string type_str;
			// No "null" because null attributes are dropped
			if (type == (1 << mvt_double)) {
				type_str = "number";
			} else if (type == (1 << mvt_bool)) {
				type_str = "boolean";
			} else if (type == (1 << mvt_string)) {
				type_str = "string";
			} else {
				type_str = "mixed";
			}

			state.nospace = true;
			state.json_write_string("type");
			state.nospace = true;
			state.json_write_string(type_str);

			state.nospace = true;
			state.json_write_string("values");
			state.nospace = true;
			state.json_write_array();

			size_t vals = 0;
			for (auto value : attribute.second.sample_values) {
				if (vals == elements) {
					break;
				}

				if (value.type == mvt_double || value.type == mvt_bool) {
					vals++;

					state.nospace = true;
					state.json_write_stringified(value.s);
				} else {
					std::string trunc = truncate16(value.s, 256);

					if (trunc.size() == value.s.size()) {
						vals++;

						state.nospace = true;
						state.json_write_string(value.s);
					}
				}
			}

			state.nospace = true;
			state.json_end_array();

			if ((type & (1 << mvt_double)) != 0) {
				state.nospace = true;
				state.json_write_string("min");
				state.nospace = true;
				state.json_write_number(attribute.second.min);

				state.nospace = true;
				state.json_write_string("max");
				state.nospace = true;
				state.json_write_number(attribute.second.max);
			}

			state.nospace = true;
			state.json_end_hash();
		}

		state.nospace = true;
		state.json_end_array();
		state.nospace = true;
		state.json_end_hash();
	}

	state.nospace = true;
	state.json_end_array();
	state.nospace = true;
	state.json_end_hash();
}

std::string stringify_strategies(std::vector<strategy> const &strategies) {
	std::string out;
	json_writer state(&out);
	bool any = false;

	state.nospace = true;
	state.json_write_array();
	for (size_t i = 0; i < strategies.size(); i++) {
		state.nospace = true;
		state.json_write_hash();

		if (strategies[i].dropped_by_rate > 0) {
			state.nospace = true;
			state.json_write_string("dropped_by_rate");
			state.nospace = true;
			state.json_write_number(strategies[i].dropped_by_rate);
			any = true;
		}

		if (strategies[i].dropped_by_gamma > 0) {
			state.nospace = true;
			state.json_write_string("dropped_by_gamma");
			state.nospace = true;
			state.json_write_number(strategies[i].dropped_by_gamma);
			any = true;
		}

		if (strategies[i].dropped_as_needed > 0) {
			state.nospace = true;
			state.json_write_string("dropped_as_needed");
			state.nospace = true;
			state.json_write_number(strategies[i].dropped_as_needed);
			any = true;
		}

		if (strategies[i].coalesced_as_needed > 0) {
			state.nospace = true;
			state.json_write_string("coalesced_as_needed");
			state.nospace = true;
			state.json_write_number(strategies[i].coalesced_as_needed);
			any = true;
		}

		if (strategies[i].detail_reduced > 0) {
			state.nospace = true;
			state.json_write_string("detail_reduced");
			state.nospace = true;
			state.json_write_number(strategies[i].detail_reduced);
			any = true;
		}

		if (strategies[i].tiny_polygons > 0) {
			state.nospace = true;
			state.json_write_string("tiny_polygons");
			state.nospace = true;
			state.json_write_number(strategies[i].tiny_polygons);
			any = true;
		}

		if (strategies[i].tile_size > 0) {
			state.nospace = true;
			state.json_write_string("tile_size_desired");
			state.nospace = true;
			state.json_write_number(strategies[i].tile_size);
			any = true;
		}

		if (strategies[i].feature_count > 0) {
			state.nospace = true;
			state.json_write_string("feature_count_desired");
			state.nospace = true;
			state.json_write_number(strategies[i].feature_count);
			any = true;
		}

		if (strategies[i].truncated_zooms > 0) {
			state.nospace = true;
			state.json_write_string("truncated_zooms");
			state.nospace = true;
			state.json_write_number(strategies[i].truncated_zooms);
			any = true;
		}

		state.nospace = true;
		state.json_end_hash();
	}
	state.nospace = true;
	state.json_end_array();

	if (any) {
		return out;
	} else {
		return "";
	}
}

void mbtiles_write_metadata(sqlite3 *db, const metadata &m, bool forcetable) {
	char *sql, *err;

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('name', %Q);", m.name.c_str());
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set name in metadata: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('description', %Q);", m.description.c_str());
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set description in metadata: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('version', %d);", m.version);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set version : %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('minzoom', %d);", m.minzoom);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set minzoom: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('maxzoom', %d);", m.maxzoom);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set maxzoom: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('center', '%f,%f,%d');", m.center_lon, m.center_lat, m.center_z);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set center: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('bounds', '%f,%f,%f,%f');", m.minlon, m.minlat, m.maxlon, m.maxlat);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set bounds: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('antimeridian_adjusted_bounds', '%f,%f,%f,%f');", m.minlon2, m.minlat2, m.maxlon2, m.maxlat2);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set bounds: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('type', %Q);", m.type.c_str());
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set type: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	if (m.attribution.size() > 0) {
		sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('attribution', %Q);", m.attribution.c_str());
		if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
			fprintf(stderr, "set attribution: %s\n", err);
			if (!forcetable) {
				exit(EXIT_SQLITE);
			}
		}
		sqlite3_free(sql);
	}

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('format', %Q);", m.format.c_str());
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set format: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('generator', %Q);", m.generator.c_str());
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set generator: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('generator_options', %Q);", m.generator_options.c_str());
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set commandline: %s\n", err);
		if (!forcetable) {
			exit(EXIT_SQLITE);
		}
	}
	sqlite3_free(sql);

	if (m.strategies_json.size() > 0) {
		sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('strategies', %Q);", m.strategies_json.c_str());
		if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
			fprintf(stderr, "set strategies: %s\n", err);
			if (!forcetable) {
				exit(EXIT_SQLITE);
			}
		}
		sqlite3_free(sql);
	}

	if (m.decisions_json.size() > 0) {
		sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('tippecanoe_decisions', %Q);", m.decisions_json.c_str());
		if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
			fprintf(stderr, "set decisions: %s\n", err);
			if (!forcetable) {
				exit(EXIT_SQLITE);
			}
		}
		sqlite3_free(sql);
	}

	if (m.vector_layers_json.size() > 0 || m.tilestats_json.size() > 0) {
		std::string json;
		json_writer state(&json);
		state.nospace = true;
		state.json_write_hash();

		if (m.vector_layers_json.size() > 0) {
			state.nospace = true;
			state.json_write_string("vector_layers");
			state.nospace = true;
			state.json_write_json(m.vector_layers_json);

			if (m.tilestats_json.size() > 0) {
				state.nospace = true;
				state.json_write_string("tilestats");
				state.nospace = true;
				state.json_write_json(m.tilestats_json);
			}
		} else {
			if (m.tilestats_json.size() > 0) {
				state.nospace = true;
				state.json_write_string("tilestats");
				state.nospace = true;
				state.json_write_json(m.tilestats_json);
			}
		}

		state.nospace = true;
		state.json_end_hash();

		sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('json', %Q);", json.c_str());
		if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
			fprintf(stderr, "set json: %s\n", err);
			if (!forcetable) {
				exit(EXIT_SQLITE);
			}
		}
		sqlite3_free(sql);
	}
}

static double sixdig(double val) {
	return std::round(val * 1e6) / 1e6;
}

#define str(x) #x
#define xstr(x) str(x)
std::string version_str() {
	std::string s = VERSION;
	std::string build_info = xstr(BUILD_INFO);
	if (build_info.size() > 0) {
		s += " " + build_info;
	}
	return s;
}

metadata make_metadata(const char *fname, int minzoom, int maxzoom, double minlat, double minlon, double maxlat, double maxlon, double minlat2, double minlon2, double maxlat2, double maxlon2, double midlat, double midlon, const char *attribution, std::map<std::string, layermap_entry> const &layermap, bool vector, const char *description, bool do_tilestats, std::map<std::string, std::string> const &attribute_descriptions, std::string const &program, std::string const &commandline, std::vector<strategy> const &strategies, int basezoom, double droprate, int retain_points_multiplier) {
	metadata m;

	m.name = fname;
	m.description = description != NULL ? description : fname;
	m.version = 2;
	m.type = "overlay";
	m.format = vector ? "pbf" : "png";

	m.minzoom = minzoom;
	m.maxzoom = maxzoom;

	m.minlat = sixdig(minlat);
	m.minlon = sixdig(minlon);
	m.maxlat = sixdig(maxlat);
	m.maxlon = sixdig(maxlon);

	m.minlat2 = sixdig(minlat2);
	m.minlon2 = sixdig(minlon2);
	m.maxlat2 = sixdig(maxlat2);
	m.maxlon2 = sixdig(maxlon2);

	m.center_lat = sixdig(midlat);
	m.center_lon = sixdig(midlon);
	m.center_z = maxzoom;

	if (attribution != NULL) {
		m.attribution = attribution;
	}

	m.generator = program + " " + version_str();
	m.generator_options = commandline;

	m.strategies_json = stringify_strategies(strategies);

	if (std::isinf(droprate)) {
		droprate = LLONG_MAX;
	}
	if (basezoom != maxzoom || droprate != 2.5 || retain_points_multiplier != 1) {
		m.decisions_json = std::string("{") +
				   "\"basezoom\":" + milo::dtoa_milo(basezoom) + "," +
				   "\"droprate\":" + milo::dtoa_milo(droprate) + "," +
				   "\"retain_points_multiplier\":" + std::to_string(retain_points_multiplier) +
				   std::string("}");
	}

	if (vector) {
		{
			json_writer state(&m.vector_layers_json);

			state.nospace = true;
			state.json_write_array();

			std::vector<std::string> lnames;
			for (auto ai = layermap.begin(); ai != layermap.end(); ++ai) {
				lnames.push_back(ai->first);
			}

			for (size_t i = 0; i < lnames.size(); i++) {
				auto ts = layermap.find(lnames[i]);
				state.nospace = true;
				state.json_write_hash();

				state.nospace = true;
				state.json_write_string("id");
				state.nospace = true;
				state.json_write_string(lnames[i]);

				state.nospace = true;
				state.json_write_string("description");
				state.nospace = true;
				state.json_write_string(ts->second.description);

				state.nospace = true;
				state.json_write_string("minzoom");
				state.nospace = true;
				state.json_write_signed(ts->second.minzoom);

				state.nospace = true;
				state.json_write_string("maxzoom");
				state.nospace = true;
				state.json_write_signed(ts->second.maxzoom);

				state.nospace = true;
				state.json_write_string("fields");
				state.nospace = true;
				state.json_write_hash();

				bool first = true;
				size_t attribute_count = 0;
				for (auto j = ts->second.tilestats.begin(); j != ts->second.tilestats.end(); ++j) {
					if (first) {
						first = false;
					}

					state.nospace = true;
					state.json_write_string(j->first);

					auto f = attribute_descriptions.find(j->first);
					if (f == attribute_descriptions.end()) {
						int type = 0;
						for (auto s : j->second.sample_values) {
							type |= (1 << s.type);
						}

						if (type == (1 << mvt_double)) {
							state.nospace = true;
							state.json_write_string("Number");
						} else if (type == (1 << mvt_bool)) {
							state.nospace = true;
							state.json_write_string("Boolean");
						} else if (type == (1 << mvt_string)) {
							state.nospace = true;
							state.json_write_string("String");
						} else {
							state.nospace = true;
							state.json_write_string("Mixed");
						}
					} else {
						state.nospace = true;
						state.json_write_string(f->second);
					}

					attribute_count++;
					if (attribute_count >= max_tilestats_attributes) {
						break;
					}
				}

				state.nospace = true;
				state.json_end_hash();
				state.nospace = true;
				state.json_end_hash();
			}

			state.nospace = true;
			state.json_end_array();
		}

		{
			size_t elements = max_tilestats_values;
			json_writer state(&m.tilestats_json);

			if (do_tilestats && elements > 0) {
				tilestats(layermap, elements, state);
			}
		}
	}

	return m;
}

void mbtiles_close(sqlite3 *outdb, const char *pgm) {
	char *err;

	if (sqlite3_exec(outdb, "ANALYZE;", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: ANALYZE failed: %s\n", pgm, err);
		exit(EXIT_SQLITE);
	}
	if (sqlite3_close(outdb) != SQLITE_OK) {
		fprintf(stderr, "%s: could not close database: %s\n", pgm, sqlite3_errmsg(outdb));
		exit(EXIT_CLOSE);
	}
}

std::map<std::string, layermap_entry> merge_layermaps(std::vector<std::map<std::string, layermap_entry>> const &maps) {
	return merge_layermaps(maps, false);
}

std::map<std::string, layermap_entry> merge_layermaps(std::vector<std::map<std::string, layermap_entry>> const &maps, bool trunc) {
	std::map<std::string, layermap_entry> out;

	for (size_t i = 0; i < maps.size(); i++) {
		for (auto map = maps[i].begin(); map != maps[i].end(); ++map) {
			if (map->second.points + map->second.lines + map->second.polygons + map->second.retain == 0) {
				continue;
			}

			std::string layername = map->first;
			if (trunc) {
				layername = truncate16(layername, 256);
			}

			if (out.count(layername) == 0) {
				out.insert(std::pair<std::string, layermap_entry>(layername, layermap_entry(out.size())));
				auto out_entry = out.find(layername);
				out_entry->second.minzoom = map->second.minzoom;
				out_entry->second.maxzoom = map->second.maxzoom;
				out_entry->second.description = map->second.description;
			}

			auto out_entry = out.find(layername);
			if (out_entry == out.end()) {
				fprintf(stderr, "Internal error merging layers\n");
				exit(EXIT_IMPOSSIBLE);
			}

			for (auto ts = map->second.tilestats.begin(); ts != map->second.tilestats.end(); ++ts) {
				std::string attribname = ts->first;
				if (trunc) {
					attribname = truncate16(attribname, 256);
				}

				auto ts2 = out_entry->second.tilestats.find(attribname);

				if (ts2 == out_entry->second.tilestats.end()) {
					out_entry->second.tilestats.insert(std::pair<std::string, tilestat>(attribname, ts->second));
				} else {
					for (auto val : ts->second.sample_values) {
						auto pt = std::lower_bound(ts2->second.sample_values.begin(), ts2->second.sample_values.end(), val);
						if (pt == ts2->second.sample_values.end() || *pt != val) {  // not found
							ts2->second.sample_values.insert(pt, val);

							if (ts2->second.sample_values.size() > max_tilestats_sample_values) {
								ts2->second.sample_values.pop_back();
							}
						}
					}

					ts2->second.type |= ts->second.type;

					if (ts->second.min < ts2->second.min) {
						ts2->second.min = ts->second.min;
					}
					if (ts->second.max > ts2->second.max) {
						ts2->second.max = ts->second.max;
					}
				}
			}

			if (map->second.minzoom < out_entry->second.minzoom) {
				out_entry->second.minzoom = map->second.minzoom;
			}
			if (map->second.maxzoom > out_entry->second.maxzoom) {
				out_entry->second.maxzoom = map->second.maxzoom;
			}

			out_entry->second.points += map->second.points;
			out_entry->second.lines += map->second.lines;
			out_entry->second.polygons += map->second.polygons;
		}
	}

	return out;
}

void add_to_tilestats(std::map<std::string, tilestat> &tilestats, std::string const &attrib, serial_val const &val) {
	if (val.type == mvt_null) {
		return;
	}

	auto tsa = tilestats.find(attrib);
	if (tsa == tilestats.end()) {
		tilestats.insert(std::pair<std::string, tilestat>(attrib, tilestat()));
		tsa = tilestats.find(attrib);
	}

	if (tsa == tilestats.end()) {
		fprintf(stderr, "Can't happen (tilestats)\n");
		exit(EXIT_IMPOSSIBLE);
	}

	if (val.type == mvt_double) {
		double d = atof(val.s.c_str());

		if (d < tsa->second.min) {
			tsa->second.min = d;
		}
		if (d > tsa->second.max) {
			tsa->second.max = d;
		}
	}

	auto pt = std::lower_bound(tsa->second.sample_values.begin(), tsa->second.sample_values.end(), val);
	if (pt == tsa->second.sample_values.end() || *pt != val) {  // not found
		if (tsa->second.sample_values.size() >= max_tilestats_sample_values) {
			if (pt == tsa->second.sample_values.end()) {
				// insertion point would be at the end,
				// and the list is already full, so do nothing
			} else {
				// bump the former last value, insert this one
				tsa->second.sample_values.insert(pt, val);
				tsa->second.sample_values.pop_back();
			}
		} else {
			tsa->second.sample_values.insert(pt, val);
		}
	}

	tsa->second.type |= (1 << val.type);
}
