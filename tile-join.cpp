// for vasprintf() on Linux
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define _DEFAULT_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>
#include <limits.h>
#include <getopt.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <zlib.h>
#include <math.h>
#include <pthread.h>
#include "mvt.hpp"
#include "projection.hpp"
#include "mbtiles.hpp"
#include "geometry.hpp"
#include "dirtiles.hpp"
#include "pmtiles_file.hpp"
#include "evaluator.hpp"
#include "csv.hpp"
#include "text.hpp"
#include "tile.hpp"
#include "tile-cache.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include "jsonpull/jsonpull.h"
#include "milo/dtoa_milo.h"
#include "errors.hpp"
#include "geometry.hpp"
#include "thread.hpp"
#include "platform.hpp"

int pk = false;
int pC = false;
int pg = false;
int pe = false;
size_t CPUS;
int quiet = false;
int maxzoom = 32;
int minzoom = 0;
std::map<std::string, std::string> renames;
bool exclude_all = false;
bool exclude_all_tile_attributes = false;
std::vector<std::string> unidecode_data;
std::string join_tile_attribute;
std::string join_table_expression;
std::string join_table;
size_t join_count_limit = 1;
std::string attribute_for_id;

bool want_overzoom = false;
int buffer = 5;

bool progress_time() {
	return false;
}

struct stats {
	int minzoom = 0;
	int maxzoom = 0;
	double midlat = 0, midlon = 0;
	double minlat = 0, minlon = 0, maxlat = 0, maxlon = 0;
	double minlat2 = 0, minlon2 = 0, maxlat2 = 0, maxlon2 = 0;
	std::vector<struct strategy> strategies{};
};

// list, per feature in the tile,
// of lists of features in the sqlite response,
// each of which is a mapping from keys to values
std::vector<std::vector<std::map<std::string, mvt_value>>> get_joined_rows(sqlite3 *db, const std::vector<mvt_value> &join_keys) {
	std::vector<std::vector<std::map<std::string, mvt_value>>> ret;
	ret.resize(join_keys.size());

	// double quotes for table and column identifiers
	const char *s = sqlite3_mprintf("select %s, * from \"%w\" where %s in (",
					join_table_expression.c_str(), join_table.c_str(), join_table_expression.c_str());
	std::string query = s;
	sqlite3_free((void *) s);

	std::multimap<std::string, size_t> key_to_row;
	for (size_t i = 0; i < join_keys.size(); i++) {
		const mvt_value &v = join_keys[i];

		// single quotes for literals
		if (v.type == mvt_string) {
			s = sqlite3_mprintf("'%q'", v.c_str());
			query += s;
			sqlite3_free((void *) s);
			key_to_row.emplace(v.get_string_value(), i);
		} else {
			std::string stringified = v.toString();
			key_to_row.emplace(stringified, i);
			query += stringified;
		}

		if (i + 1 < join_keys.size()) {
			query += ", ";
		}
	}

	// this doesn't add a LIMIT to the query because our limit
	// is per tiled feature, not a limit on the entire query response.
	query += ");";

	sqlite3_stmt *stmt;
	if (sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 query %s failed: %s\n", query.c_str(), sqlite3_errmsg(db));
		exit(EXIT_SQLITE);
	}
	while (sqlite3_step(stmt) == SQLITE_ROW) {
		int count = sqlite3_column_count(stmt);
		std::map<std::string, mvt_value> row;

		if (count > 0) {
			// join key is 0th column of query
			std::string key = (const char *) sqlite3_column_text(stmt, 0);
			auto f = key_to_row.equal_range(key);
			if (f.first == f.second) {
				fprintf(stderr, "Unexpected join key: %s\n", key.c_str());
				continue;
			}

			for (auto ff = f.first; ff != f.second; ++ff) {
				if (ret[ff->second].size() < join_count_limit) {
					for (int i = 1; i < count; i++) {
						int type = sqlite3_column_type(stmt, i);
						mvt_value v;
						v.type = mvt_null;

						if (type == SQLITE_INTEGER || type == SQLITE_FLOAT) {
							v = mvt_value(sqlite3_column_double(stmt, i));
						} else if (type == SQLITE_TEXT || type == SQLITE_BLOB) {
							v.set_string_value((const char *) sqlite3_column_text(stmt, i));
						}

						const char *name = sqlite3_column_name(stmt, i);
						row.emplace(name, v);
					}

					ret[ff->second].push_back(row);
				}
			}
		}
	}
	if (sqlite3_finalize(stmt) != SQLITE_OK) {
		fprintf(stderr, "sqlite3 finalize failed: %s\n", sqlite3_errmsg(db));
		exit(EXIT_SQLITE);
	}

	return ret;
}

struct arg {
	std::map<zxy, std::vector<std::string>> inputs{};
	std::map<zxy, std::string> outputs{};

	std::map<std::string, layermap_entry> *layermap = NULL;

	std::vector<std::string> *header = NULL;
	std::map<std::string, std::vector<std::string>> *mapping = NULL;
	sqlite3 *db = NULL;
	std::set<std::string> *exclude = NULL;
	std::set<std::string> *include = NULL;
	std::set<std::string> *keep_layers = NULL;
	std::set<std::string> *remove_layers = NULL;
	int ifmatched = 0;
	json_object *filter = NULL;
	struct tileset_reader *readers = NULL;

	double minlat, minlon;
	double maxlat, maxlon;
	double minlon2, maxlon2;
};

void append_tile(std::string message, int z, unsigned x, unsigned y, std::map<std::string, layermap_entry> &layermap, std::vector<std::string> &header, std::map<std::string, std::vector<std::string>> &mapping, sqlite3 *db, std::set<std::string> &exclude, std::set<std::string> &include, std::set<std::string> &keep_layers, std::set<std::string> &remove_layers, int ifmatched, mvt_tile &outtile, json_object *filter, struct arg *a) {
	mvt_tile tile;
	int features_added = 0;
	bool was_compressed;

	try {
		if (!tile.decode(message, was_compressed)) {
			fprintf(stderr, "Couldn't decompress tile %d/%u/%u\n", z, x, y);
			exit(EXIT_MVT);
		}
	} catch (std::exception const &e) {
		fprintf(stderr, "PBF decoding error in tile %d/%u/%u\n", z, x, y);
		exit(EXIT_MVT);
	}

	std::shared_ptr<std::string> tile_stringpool = std::make_shared<std::string>();

	for (size_t l = 0; l < tile.layers.size(); l++) {
		mvt_layer &layer = tile.layers[l];

		auto found = renames.find(layer.name);
		if (found != renames.end()) {
			layer.name = found->second;
		}

		if (keep_layers.size() > 0 && keep_layers.count(layer.name) == 0) {
			continue;
		}
		if (remove_layers.count(layer.name) != 0) {
			continue;
		}

		size_t ol;
		for (ol = 0; ol < outtile.layers.size(); ol++) {
			if (tile.layers[l].name == outtile.layers[ol].name) {
				break;
			}
		}

		if (ol == outtile.layers.size()) {
			outtile.layers.push_back(mvt_layer());

			outtile.layers[ol].name = layer.name;
			outtile.layers[ol].version = layer.version;
			outtile.layers[ol].extent = layer.extent;
		}

		mvt_layer &outlayer = outtile.layers[ol];

		if (layer.extent != outlayer.extent) {
			if (layer.extent > outlayer.extent) {
				// this always scales up the existing layer instead of scaling down
				// the layer that is being added, because the assumption is that
				// scaling up is safe while scaling down requires geometry cleaning.

				for (size_t i = 0; i < outlayer.features.size(); i++) {
					for (size_t j = 0; j < outlayer.features[i].geometry.size(); j++) {
						outlayer.features[i].geometry[j].x = outlayer.features[i].geometry[j].x * layer.extent / outlayer.extent;
						outlayer.features[i].geometry[j].y = outlayer.features[i].geometry[j].y * layer.extent / outlayer.extent;
					}
				}

				outlayer.extent = layer.extent;
			}
		}

		std::vector<std::vector<std::map<std::string, mvt_value>>> joined;
		if (db != NULL) {
			// collect join keys for sql query

			std::vector<mvt_value> join_keys;
			join_keys.resize(layer.features.size());

			for (size_t f = 0; f < layer.features.size(); f++) {
				mvt_feature &feat = layer.features[f];
				join_keys[f].type = mvt_no_such_key;

				for (size_t t = 0; t + 1 < feat.tags.size(); t += 2) {
					const std::string &key = layer.keys[feat.tags[t]];
					if (key == join_tile_attribute) {
						const mvt_value &val = layer.values[feat.tags[t + 1]];
						join_keys[f] = val;
						break;
					}
				}
			}

			joined = get_joined_rows(db, join_keys);
		}

		auto tilestats = layermap.find(layer.name);

		long long minx = LLONG_MAX;
		long long miny = LLONG_MAX;
		long long maxx = LLONG_MIN;
		long long maxy = LLONG_MIN;

		long long minx2 = LLONG_MAX;
		long long maxx2 = LLONG_MIN;
		bool features_added_to_layer = false;

		for (size_t f = 0; f < layer.features.size(); f++) {
			mvt_feature &feat = layer.features[f];

			std::set<std::string> exclude_attributes;
			if (filter != NULL && !evaluate(feat, layer, filter, exclude_attributes, z, unidecode_data)) {
				continue;
			}

			struct match {
				bool has_id = false;
				unsigned long long id;
				std::map<std::string, std::pair<mvt_value, serial_val>> attributes;
				std::vector<std::string> key_order;
			};

			std::vector<match> matches;
			bool matched = false;

			// start filling out sql matches

			if (f < joined.size()) {
				if (joined[f].size() > 0) {
					matched = true;
				}

				for (auto const &joined_feature : joined[f]) {
					match m;
					m.has_id = feat.has_id;
					m.id = feat.id;

					if (!exclude_all_tile_attributes) {
						for (size_t t = 0; t + 1 < feat.tags.size(); t += 2) {
							const std::string &key = layer.keys[feat.tags[t]];
							mvt_value &val = layer.values[feat.tags[t + 1]];
							serial_val sv = mvt_value_to_serial_val(val);

							if (include.count(key) || (!exclude_all && exclude.count(key) == 0 && exclude_attributes.count(key) == 0)) {
								m.attributes.insert(std::pair<std::string, std::pair<mvt_value, serial_val>>(key, std::pair<mvt_value, serial_val>(val, sv)));
								m.key_order.push_back(key);
							}
						}
					}

					for (auto const &kv : joined_feature) {
						if (kv.first == attribute_for_id) {
							m.has_id = true;
							m.id = mvt_value_to_long_long(kv.second);
						} else if (include.count(kv.first) || (!exclude_all && exclude.count(kv.first) == 0 && exclude_attributes.count(kv.first) == 0)) {
							if (kv.second.type != mvt_null) {
								m.attributes.insert(std::pair<std::string, std::pair<mvt_value, serial_val>>(kv.first, std::pair<mvt_value, serial_val>(kv.second, mvt_value_to_serial_val(kv.second))));
								m.key_order.push_back(kv.first);
							}
						}
					}

					matches.push_back(m);
				}
			}

			// look for csv matches and start filling them out

			if (!matched) {
				match m;
				m.id = feat.id;
				m.has_id = feat.has_id;
				// populate attributes and key_order as we look for matches,
				// because apparently at some point i thought it was important
				// to insert the joined attributes at the point in the sequence
				// where the join key had been

				for (size_t t = 0; t + 1 < feat.tags.size(); t += 2) {
					const std::string &key = layer.keys[feat.tags[t]];
					mvt_value &val = layer.values[feat.tags[t + 1]];
					serial_val sv = mvt_value_to_serial_val(val);

					if (val.type == mvt_null) {
						continue;
					}

					if (!exclude_all_tile_attributes) {
						if (include.count(std::string(key)) || (!exclude_all && exclude.count(std::string(key)) == 0 && exclude_attributes.count(std::string(key)) == 0)) {
							m.attributes.insert(std::pair<std::string, std::pair<mvt_value, serial_val>>(key, std::pair<mvt_value, serial_val>(val, sv)));
							m.key_order.push_back(key);
						}
					}

					if (header.size() > 0 && key == header[0]) {
						std::map<std::string, std::vector<std::string>>::iterator ii = mapping.find(sv.s);

						if (ii != mapping.end()) {
							std::vector<std::string> fields = ii->second;
							matched = true;

							for (size_t i = 1; i < fields.size(); i++) {
								std::string joinkey = header[i];
								std::string joinval = fields[i];
								int attr_type = mvt_string;

								if (joinval.size() > 0) {
									if (joinval[0] == '"') {
										joinval = csv_dequote(joinval);
									} else if (is_number(joinval)) {
										attr_type = mvt_double;
									}
								} else if (pe) {
									attr_type = mvt_null;
								}

								const char *sjoinkey = joinkey.c_str();

								if (include.count(joinkey) || (!exclude_all && exclude.count(joinkey) == 0 && exclude_attributes.count(joinkey) == 0 && attr_type != mvt_null)) {
									mvt_value outval;
									if (attr_type == mvt_string) {
										outval.type = mvt_string;
										outval.set_string_value(joinval);
									} else {
										outval.type = mvt_double;
										outval.numeric_value.double_value = atof(joinval.c_str());
									}

									auto fa = m.attributes.find(sjoinkey);
									if (fa != m.attributes.end()) {
										m.attributes.erase(fa);
									}

									serial_val outsv;
									outsv.type = outval.type;
									outsv.s = joinval;

									outval = stringified_to_mvt_value(outval.type, joinval.c_str(), tile_stringpool);

									m.attributes.insert(std::pair<std::string, std::pair<mvt_value, serial_val>>(joinkey, std::pair<mvt_value, serial_val>(outval, outsv)));
									m.key_order.push_back(joinkey);
								}
							}
						}
					}
				}

				if (matched) {
					matches.push_back(m);
				}
			}

			if (!matched && !ifmatched) {
				// no matches, but they said to keep even unmatched tile features,
				// so make one that is just the original feature

				match m;
				m.id = feat.id;
				m.has_id = feat.has_id;

				if (!exclude_all_tile_attributes) {
					for (size_t t = 0; t + 1 < feat.tags.size(); t += 2) {
						const std::string &key = layer.keys[feat.tags[t]];
						mvt_value &val = layer.values[feat.tags[t + 1]];
						serial_val sv = mvt_value_to_serial_val(val);

						if (include.count(key) || (!exclude_all && exclude.count(key) == 0 && exclude_attributes.count(key) == 0)) {
							m.attributes.insert(std::pair<std::string, std::pair<mvt_value, serial_val>>(key, std::pair<mvt_value, serial_val>(val, sv)));
							m.key_order.push_back(key);
						}
					}
				}

				matches.push_back(m);
			}

			for (auto &m : matches) {
				if (tilestats == layermap.end()) {
					layermap.insert(std::pair<std::string, layermap_entry>(layer.name, layermap_entry(layermap.size())));
					tilestats = layermap.find(layer.name);
					tilestats->second.minzoom = z;
					tilestats->second.maxzoom = z;
				}

				mvt_feature outfeature;
				outfeature.id = m.id;
				outfeature.has_id = m.has_id;

				// To keep attributes in their original order instead of alphabetical
				for (auto k : m.key_order) {
					auto fa = m.attributes.find(k);

					if (fa != m.attributes.end()) {
						outlayer.tag(outfeature, k, fa->second.first);
						add_to_tilestats(tilestats->second.tilestats, k, fa->second.second);
						m.attributes.erase(fa);
					}
				}

				outfeature.type = feat.type;
				outfeature.geometry = feat.geometry;

				if (layer.extent != outlayer.extent) {
					for (size_t i = 0; i < outfeature.geometry.size(); i++) {
						outfeature.geometry[i].x = outfeature.geometry[i].x * outlayer.extent / layer.extent;
						outfeature.geometry[i].y = outfeature.geometry[i].y * outlayer.extent / layer.extent;
					}
				}

				for (auto const &g : outfeature.geometry) {
					if (g.op == mvt_moveto || g.op == mvt_lineto) {
						// pin to the tile extent, since we don't want bounds bigger than the earth
						long long gx = std::min((long long) outlayer.extent, std::max(0LL, g.x));
						long long gy = std::min((long long) outlayer.extent, std::max(0LL, g.y));

						// to world scale
						gx = gx * (1LL << (32 - z)) / outlayer.extent;
						gy = gy * (1LL << (32 - z)) / outlayer.extent;

						// to world offset
						gx += (1LL << (32 - z)) * x;
						gy += (1LL << (32 - z)) * y;

						minx = std::min(minx, gx);
						miny = std::min(miny, gy);
						maxx = std::max(maxx, gx);
						maxy = std::max(maxy, gy);

						// if in the western hemisphere, try shifting to east
						if (gx < (1LL << 31)) {
							gx += 1LL << 32;
						}

						minx2 = std::min(minx2, gx);
						maxx2 = std::max(maxx2, gx);
					}
				}

				features_added++;
				features_added_to_layer = true;
				outlayer.features.push_back(outfeature);

				if (z < tilestats->second.minzoom) {
					tilestats->second.minzoom = z;
				}
				if (z > tilestats->second.maxzoom) {
					tilestats->second.maxzoom = z;
				}

				if (feat.type == mvt_point) {
					tilestats->second.points++;
				} else if (feat.type == mvt_linestring) {
					tilestats->second.lines++;
				} else if (feat.type == mvt_polygon) {
					tilestats->second.polygons++;
				}
			}
		}

		if (features_added_to_layer) {
			double lat1, lon1;
			double lat2, lon2;
			tile2lonlat(minx, maxy, 32, &lon1, &lat1);
			tile2lonlat(maxx, miny, 32, &lon2, &lat2);

			a->minlat = std::min(a->minlat, std::min(lat1, lat2));
			a->minlon = std::min(a->minlon, std::min(lon1, lon2));
			a->maxlat = std::max(a->maxlat, std::max(lat1, lat2));
			a->maxlon = std::max(a->maxlon, std::max(lon1, lon2));

			tile2lonlat(minx2, maxy, 32, &lon1, &lat1);
			tile2lonlat(maxx2, miny, 32, &lon2, &lat2);

			a->minlon2 = std::min(a->minlon2, std::min(lon1, lon2));
			a->maxlon2 = std::max(a->maxlon2, std::max(lon1, lon2));
		}
	}

	if (features_added == 0) {
		return;
	}
}

struct tilecmp {
	bool operator()(std::pair<unsigned, unsigned> const &a, std::pair<unsigned, unsigned> const &b) {
		// must match behavior of tileset_reader::operator<()
		// except backwards, since we are pulling from the end of the list

		if (b.first < a.first) {
			return true;
		}
		if (b.first == a.first) {
			// Y sorts backwards, in TMS order
			if (b.second > a.second) {
				return true;
			}
		}

		return false;
	}
} tilecmp;

// The `tileset_reader` is an iterator through the tiles of a tileset,
// in z/x/tms_y order.
//
// The basic idea is that it is used like this:
//
//  void blah(const char *fname) {
//      tileset_reader r(fname);
//
//      for (; !r.all_done(); r.advance()) {
//          std::pair<zxy, std::string> tile = r.current();
//          whatever(tile);
//      }
//
//      r.close();
//  }
//
// The complication is that you can actually keep calling current()
// and advance() after the tileset_reader claims to be done, in which case
// it will produce overzoomed tiles generated from the tiles in
// the maxzoom tileset. The parent tiles for those overzoomed tiles
// are retrieved internally using get_tile() rather than through the
// main iteration query.

struct tileset_reader {
	// z/x/y and data of the current tile
	long long zoom = 0;
	long long x = 0;
	long long y = 0;
	std::string data = "";
	bool current_tile_is_overzoomed = false;

	// "done" means we have read all of the real tiles from the source.
	// The iterator will continue to produce overzoomed tiles after it is "done."
	bool done = false;

	// for overzooming
	int maxzoom_so_far = -1;
	std::vector<std::pair<unsigned, unsigned>> tiles_at_maxzoom_so_far;
	std::vector<std::pair<unsigned, unsigned>> overzoomed_tiles;	   // tiles at `zoom`
	std::vector<std::pair<unsigned, unsigned>> next_overzoomed_tiles;  // tiles at `zoom + 1`
	bool overzoom_consumed_at_this_zoom = false;

	// parent tile cache
	tile_cache cache;

	// for iterating mbtiles
	sqlite3 *db = NULL;
	sqlite3_stmt *stmt = NULL;
	struct tileset_reader *next = NULL;

	// for iterating dirtiles
	std::vector<zxy> dirtiles;
	std::string dirbase;
	std::string name;

	// for iterating pmtiles
	char *pmtiles_map = NULL;
	std::vector<pmtiles::entry_zxy> pmtiles_entries;

	tileset_reader(const char *fname) {
		name = fname;
		struct stat st;
		if (stat(fname, &st) == 0 && (st.st_mode & S_IFDIR) != 0) {
			db = NULL;
			stmt = NULL;
			next = NULL;

			dirtiles = enumerate_dirtiles(fname, minzoom, maxzoom);
			dirbase = fname;
		} else if (pmtiles_has_suffix(fname)) {
			int pmtiles_fd = open(fname, O_RDONLY | O_CLOEXEC);
			pmtiles_map = (char *) mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, pmtiles_fd, 0);

			if (pmtiles_map == MAP_FAILED) {
				perror("mmap in decode");
				exit(EXIT_MEMORY);
			}
			if (::close(pmtiles_fd) != 0) {
				perror("close");
				exit(EXIT_CLOSE);
			}

			pmtiles_entries = pmtiles_entries_tms(pmtiles_map, minzoom, maxzoom);
			std::reverse(pmtiles_entries.begin(), pmtiles_entries.end());
		} else {
			if (sqlite3_open(fname, &db) != SQLITE_OK) {
				fprintf(stderr, "%s: %s\n", fname, sqlite3_errmsg(db));
				exit(EXIT_SQLITE);
			}

			char *err = NULL;
			if (sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, &err) != SQLITE_OK) {
				fprintf(stderr, "%s: integrity_check: %s\n", fname, err);
				exit(EXIT_SQLITE);
			}

			const char *sql = "SELECT zoom_level, tile_column, tile_row, tile_data from tiles order by zoom_level, tile_column, tile_row;";
			sqlite3_stmt *query;

			if (sqlite3_prepare_v2(db, sql, -1, &query, NULL) != SQLITE_OK) {
				fprintf(stderr, "%s: select failed: %s\n", fname, sqlite3_errmsg(db));
				exit(EXIT_SQLITE);
			}

			stmt = query;
			next = NULL;
		}
	}

	// Checks the done status not only of this tileset_reader but also
	// the others chained to it in the queue.
	//
	// Also claims not to be done if at least one overzoomed tile
	// has been consumed at this zoom level, in which case they should
	// all allowed to be consumed before stopping.
	bool all_done() {
		if (!done) {
			return false;
		}
		if (overzoom_consumed_at_this_zoom) {
			return false;
		}

		for (struct tileset_reader *r = next; r != NULL; r = r->next) {
			if (!r->done) {
				return false;
			}
			if (r->overzoom_consumed_at_this_zoom) {
				return false;
			}
		}
		return true;
	}

	std::pair<zxy, std::string> current() {
		if (current_tile_is_overzoomed) {
			overzoom_consumed_at_this_zoom = true;
		}

		return std::pair<zxy, std::string>(zxy(zoom, x, y), data);
	}

	void advance() {
		if (done) {
			if (!want_overzoom) {
				fprintf(stderr, "overzoom advance called without -O\n");
				exit(EXIT_IMPOSSIBLE);
			}

			if (overzoomed_tiles.size() == 0) {
				next_overzoom();
				overzoom_consumed_at_this_zoom = false;
			}

			if (overzoomed_tiles.size() == 0) {
				// we have nothing to overzoom; give up
				current_tile_is_overzoomed = false;
				zoom = 32;
				return;
			}

			auto xy = overzoomed_tiles.back();
			overzoomed_tiles.erase(overzoomed_tiles.begin() + overzoomed_tiles.size() - 1);

			x = xy.first;
			y = xy.second;
			data = retrieve_overzoom(zxy(zoom, x, y));
			current_tile_is_overzoomed = true;

			return;
		}

		current_tile_is_overzoomed = false;

		if (db != NULL) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				zoom = sqlite3_column_int(stmt, 0);
				x = sqlite3_column_int(stmt, 1);
				int tms_y = sqlite3_column_int(stmt, 2);
				y = (1LL << zoom) - 1 - tms_y;
				const char *s = (const char *) sqlite3_column_blob(stmt, 3);
				size_t len = sqlite3_column_bytes(stmt, 3);

				data = std::string(s, len);
			} else {
				done = true;
			}
		} else if (pmtiles_map != NULL) {
			if (pmtiles_entries.size() == 0) {
				done = true;
			} else {
				zoom = pmtiles_entries.back().z;
				x = pmtiles_entries.back().x;
				y = pmtiles_entries.back().y;
				data = std::string(pmtiles_map + pmtiles_entries.back().offset, pmtiles_entries.back().length);

				pmtiles_entries.pop_back();
			}
		} else {
			if (dirtiles.size() == 0) {
				done = true;
			} else {
				zoom = dirtiles[0].z;
				x = dirtiles[0].x;
				y = dirtiles[0].y;
				data = dir_read_tile(dirbase, dirtiles[0]);

				dirtiles.erase(dirtiles.begin());
			}
		}

		if (done) {
			if (want_overzoom) {
				next_overzoom();
				advance();
			} else {
				zoom = 32;
			}
		} else {
			if (zoom > maxzoom_so_far) {
				maxzoom_so_far = zoom;
				tiles_at_maxzoom_so_far.clear();
			}

			if (want_overzoom) {
				tiles_at_maxzoom_so_far.push_back(std::pair<unsigned, unsigned>(x, y));
			}
		}
	}

	void close() {
		if (pmtiles_map) {
			db = pmtilesmeta2tmp(name.c_str(), pmtiles_map);
			// json, strategies
		} else if (db == NULL) {
			db = dirmeta2tmp(dirbase.c_str());
		} else {
			sqlite3_finalize(stmt);
		}
	}

	void next_overzoom() {
		zoom++;
		overzoomed_tiles.clear();

		// +1 because maxzoom_so_far is initially -1, an invalid shift
		long long scale = (1LL << (zoom + 1)) / (1LL << (maxzoom_so_far + 1));

		// If this is the first overzoomed level, we don't know yet
		// which tiles will be useful, so spell out all 4 child tiles
		// from each parent tile.
		//
		// If it is further overzoomed than that, we have a list of
		// which child tiles will have features in them, so use that.

		if (zoom == maxzoom_so_far + 1) {
			for (auto const &xy : tiles_at_maxzoom_so_far) {
				for (long long xx = 0; xx < scale; xx++) {
					for (long long yy = 0; yy < scale; yy++) {
						overzoomed_tiles.push_back(std::pair<unsigned, unsigned>(xy.first * scale + xx, xy.second * scale + yy));
					}
				}
			}
		} else {
			overzoomed_tiles = std::move(next_overzoomed_tiles);
			next_overzoomed_tiles.clear();
		}

		std::stable_sort(overzoomed_tiles.begin(), overzoomed_tiles.end(), tilecmp);
		overzoom_consumed_at_this_zoom = false;
	}

	mvt_tile get_tile(zxy tile) {
		std::string source;

		if (db != NULL) {
			const char *sql = "SELECT tile_data from tiles where zoom_level = ? and tile_column = ? and tile_row = ?;";
			sqlite3_stmt *query;
			if (sqlite3_prepare_v2(db, sql, -1, &query, NULL) != SQLITE_OK) {
				fprintf(stderr, "%s: select failed: %s\n", name.c_str(), sqlite3_errmsg(db));
				exit(EXIT_SQLITE);
			}

			sqlite3_bind_int(query, 1, tile.z);
			sqlite3_bind_int(query, 2, tile.x);
			sqlite3_bind_int(query, 3, (1LL << tile.z) - 1 - tile.y);

			if (sqlite3_step(query) == SQLITE_ROW) {
				const char *s = (const char *) sqlite3_column_blob(query, 0);
				size_t len = sqlite3_column_bytes(query, 0);

				source = std::string(s, len);
			}

			sqlite3_finalize(query);
		} else if (pmtiles_map != NULL) {
			uint64_t tile_offset;
			uint32_t tile_length;
			std::tie(tile_offset, tile_length) = pmtiles_get_tile(pmtiles_map, tile.z, tile.x, tile.y);
			if (tile_length > 0) {
				source = std::string(pmtiles_map + tile_offset, tile_length);
			}
		} else {
			source = dir_read_tile(dirbase, tile);
		}

		mvt_tile content;
		if (source.size() == 0) {
			return content;
		}

		try {
			bool was_compressed;
			if (!content.decode(source, was_compressed)) {
				fprintf(stderr, "Couldn't parse tile %lld/%lld/%lld\n", tile.z, tile.x, tile.y);
				exit(EXIT_MVT);
			}
		} catch (std::exception const &e) {
			fprintf(stderr, "PBF decoding error in tile %lld/%lld/%lld\n", tile.z, tile.x, tile.y);
			exit(EXIT_PROTOBUF);
		}

		return content;
	}

	// Sort in z/x/tms_y order, because that is the order of the
	// straightforward query of the mbtiles tiles table.
	bool operator<(const struct tileset_reader &r) const {
		// must match behavior of tilecmp

		if (zoom < r.zoom) {
			return true;
		}
		if (zoom > r.zoom) {
			return false;
		}

		if (x < r.x) {
			return true;
		}
		if (x > r.x) {
			return false;
		}

		int sorty = (1LL << zoom) - 1 - y;
		int r_sorty = (1LL << r.zoom) - 1 - r.y;

		if (sorty < r_sorty) {
			return true;
		}
		if (sorty > r_sorty) {
			return false;
		}

		if (data < r.data) {
			return true;
		}

		return false;
	}

	std::string retrieve_overzoom(zxy tile) {
		// lock around sqlite3 access
		static pthread_mutex_t retrieve_lock = PTHREAD_MUTEX_INITIALIZER;

		zxy parent_tile = tile;
		while (parent_tile.z > maxzoom_so_far) {
			parent_tile.z--;
			parent_tile.x /= 2;
			parent_tile.y /= 2;
		}

		if (pthread_mutex_lock(&retrieve_lock) != 0) {
			perror("pthread_mutex_lock");
		}

		std::function<mvt_tile(zxy)> getter = [&](zxy tileno) {
			return get_tile(tileno);
		};

		mvt_tile source = cache.get(parent_tile, getter);

		if (pthread_mutex_unlock(&retrieve_lock) != 0) {
			perror("pthread_mutex_unlock");
		}

		if (source.layers.size() != 0) {
			std::vector<source_tile> tv;
			source_tile t;
			t.tile = std::move(source);
			t.z = parent_tile.z;
			t.x = parent_tile.x;
			t.y = parent_tile.y;
			tv.push_back(std::move(t));

			std::string ret = overzoom(tv, tile.z, tile.x, tile.y, -1, buffer,
						   std::set<std::string>(), std::set<std::string>(), std::vector<std::string>(),
						   false, &next_overzoomed_tiles, false, NULL, false,
						   std::unordered_map<std::string, attribute_op>(), unidecode_data, 0, 0,
						   std::vector<mvt_layer>(), "", "", SIZE_MAX,
						   std::vector<clipbbox>(), false);
			return ret;
		}

		return "";
	}
};

struct tileset_reader *begin_reading(char *fname) {
	struct tileset_reader *r = new tileset_reader(fname);

	// The reason this prefetches is so the tileset_reader queue can be
	// priority-ordered, so the one with the next relevant tile
	// is first in line.
	r->advance();
	return r;
}

void *join_worker(void *v) {
	arg *a = (arg *) v;

	for (auto ai = a->inputs.begin(); ai != a->inputs.end(); ++ai) {
		mvt_tile tile;

		for (size_t i = 0; i < ai->second.size(); i++) {
			append_tile(ai->second[i], ai->first.z, ai->first.x, ai->first.y, *(a->layermap), *(a->header), *(a->mapping), a->db, *(a->exclude), *(a->include), *(a->keep_layers), *(a->remove_layers), a->ifmatched, tile, a->filter, a);
		}

		ai->second.clear();

		bool anything = false;
		mvt_tile outtile;
		for (size_t i = 0; i < tile.layers.size(); i++) {
			if (tile.layers[i].features.size() > 0) {
				outtile.layers.push_back(tile.layers[i]);
				anything = true;
			}
		}

		if (anything) {
			std::string pbf = outtile.encode();
			std::string compressed;

			if (!pC) {
				compress(pbf, compressed, true);
			} else {
				compressed = pbf;
			}

			if (!pk && compressed.size() > 500000) {
				fprintf(stderr, "Tile %lld/%lld/%lld size is %lld, >500000. Skipping this tile.\n", ai->first.z, ai->first.x, ai->first.y, (long long) compressed.size());
			} else {
				a->outputs.insert(std::pair<zxy, std::string>(ai->first, compressed));
			}
		}
	}

	return NULL;
}

void dispatch_tasks(std::map<zxy, std::vector<std::string>> &tasks, std::vector<std::map<std::string, layermap_entry>> &layermaps, sqlite3 *outdb, const char *outdir, std::vector<std::string> &header, std::map<std::string, std::vector<std::string>> &mapping, sqlite3 *db, std::set<std::string> &exclude, std::set<std::string> &include, int ifmatched, std::set<std::string> &keep_layers, std::set<std::string> &remove_layers, json_object *filter, struct tileset_reader *readers, double *minlat, double *minlon, double *maxlat, double *maxlon, double *minlon2, double *maxlon2) {
	pthread_t pthreads[CPUS];
	std::vector<arg> args;

	for (size_t i = 0; i < CPUS; i++) {
		args.push_back(arg());

		args[i].layermap = &layermaps[i];
		args[i].header = &header;
		args[i].mapping = &mapping;
		args[i].db = db;
		args[i].exclude = &exclude;
		args[i].include = &include;
		args[i].keep_layers = &keep_layers;
		args[i].remove_layers = &remove_layers;
		args[i].ifmatched = ifmatched;
		args[i].filter = filter;
		args[i].readers = readers;
		args[i].minlat = *minlat;
		args[i].minlon = *minlon;
		args[i].maxlat = *maxlat;
		args[i].maxlon = *maxlon;
		args[i].minlon2 = *minlon2;
		args[i].maxlon2 = *maxlon2;
	}

	size_t count = 0;
	// This isn't careful about distributing tasks evenly across CPUs,
	// but, from testing, it actually takes a little longer to do
	// the proper allocation than is saved by perfectly balanced threads.
	for (auto ai = tasks.begin(); ai != tasks.end(); ++ai) {
		args[count].inputs.insert(*ai);
		count = (count + 1) % CPUS;

		if (ai == tasks.begin()) {
			if (!quiet) {
				fprintf(stderr, "%lld/%lld/%lld  \r", ai->first.z, ai->first.x, ai->first.y);
				fflush(stderr);
			}
		}
	}

	for (size_t i = 0; i < CPUS; i++) {
		if (thread_create(&pthreads[i], NULL, join_worker, &args[i]) != 0) {
			perror("pthread_create");
			exit(EXIT_PTHREAD);
		}
	}

	for (size_t i = 0; i < CPUS; i++) {
		void *retval;

		if (pthread_join(pthreads[i], &retval) != 0) {
			perror("pthread_join");
		}

		*minlat = std::min(*minlat, args[i].minlat);
		*minlon = std::min(*minlon, args[i].minlon);
		*maxlat = std::max(*maxlat, args[i].maxlat);
		*maxlon = std::max(*maxlon, args[i].maxlon);
		*minlon2 = std::min(*minlon2, args[i].minlon2);
		*maxlon2 = std::max(*maxlon2, args[i].maxlon2);

		for (auto ai = args[i].outputs.begin(); ai != args[i].outputs.end(); ++ai) {
			if (outdb != NULL) {
				mbtiles_write_tile(outdb, ai->first.z, ai->first.x, ai->first.y, ai->second.data(), ai->second.size());
			} else if (outdir != NULL) {
				dir_write_tile(outdir, ai->first.z, ai->first.x, ai->first.y, ai->second);
			}
		}
	}
}

void handle_strategies(const unsigned char *s, std::vector<strategy> *st) {
	json_pull *jp = json_begin_string((const char *) s);
	json_object *o = json_read_tree(jp);

	if (o != NULL && o->type == JSON_ARRAY) {
		for (size_t i = 0; i < o->value.array.length; i++) {
			json_object *h = o->value.array.array[i];
			if (h->type == JSON_HASH) {
				for (size_t j = 0; j < h->value.object.length; j++) {
					json_object *k = h->value.object.keys[j];
					json_object *v = h->value.object.values[j];

					if (k->type != JSON_STRING) {
						fprintf(stderr, "Key %zu of %zu is not a string: %s\n", j, i, s);
					} else if (v->type != JSON_NUMBER) {
						fprintf(stderr, "Value %zu of %zu is not a number: %s\n", j, i, s);
					} else {
						if (i >= st->size()) {
							st->resize(i + 1);
						}

						if (strcmp(k->value.string.string, "dropped_by_rate") == 0) {
							(*st)[i].dropped_by_rate += v->value.number.number;
						} else if (strcmp(k->value.string.string, "dropped_by_gamma") == 0) {
							(*st)[i].dropped_by_gamma += v->value.number.number;
						} else if (strcmp(k->value.string.string, "dropped_as_needed") == 0) {
							(*st)[i].dropped_as_needed += v->value.number.number;
						} else if (strcmp(k->value.string.string, "coalesced_as_needed") == 0) {
							(*st)[i].coalesced_as_needed += v->value.number.number;
						} else if (strcmp(k->value.string.string, "truncated_zooms") == 0) {
							(*st)[i].truncated_zooms += v->value.number.number;
						} else if (strcmp(k->value.string.string, "detail_reduced") == 0) {
							(*st)[i].detail_reduced += v->value.number.number;
						} else if (strcmp(k->value.string.string, "tiny_polygons") == 0) {
							(*st)[i].tiny_polygons += v->value.number.number;
						} else if (strcmp(k->value.string.string, "tile_size_desired") == 0) {
							(*st)[i].tile_size += v->value.number.number;
						} else if (strcmp(k->value.string.string, "feature_count_desired") == 0) {
							(*st)[i].feature_count += v->value.number.number;
						}
					}
				}
			} else {
				fprintf(stderr, "Element %zu is not a hash: %s\n", i, s);
			}
		}
		json_free(o);
	}

	json_end(jp);
}

void handle_vector_layers(json_object *vector_layers, std::map<std::string, layermap_entry> &layermap, std::map<std::string, std::string> &attribute_descriptions) {
	if (vector_layers != NULL && vector_layers->type == JSON_ARRAY) {
		for (size_t i = 0; i < vector_layers->value.array.length; i++) {
			if (vector_layers->value.array.array[i]->type == JSON_HASH) {
				json_object *id = json_hash_get(vector_layers->value.array.array[i], "id");
				json_object *desc = json_hash_get(vector_layers->value.array.array[i], "description");

				if (id != NULL && desc != NULL && id->type == JSON_STRING && desc->type == JSON_STRING) {
					std::string sid = id->value.string.string;
					std::string sdesc = desc->value.string.string;

					if (sdesc.size() != 0) {
						auto f = layermap.find(sid);
						if (f != layermap.end()) {
							f->second.description = sdesc;
						}
					}
				}

				json_object *fields = json_hash_get(vector_layers->value.array.array[i], "fields");
				if (fields != NULL && fields->type == JSON_HASH) {
					for (size_t j = 0; j < fields->value.object.length; j++) {
						if (fields->value.object.keys[j]->type == JSON_STRING && fields->value.object.values[j]->type) {
							const char *desc2 = fields->value.object.values[j]->value.string.string;

							if (strcmp(desc2, "Number") != 0 &&
							    strcmp(desc2, "String") != 0 &&
							    strcmp(desc2, "Boolean") != 0 &&
							    strcmp(desc2, "Mixed") != 0) {
								attribute_descriptions.insert(std::pair<std::string, std::string>(fields->value.object.keys[j]->value.string.string, desc2));
							}
						}
					}
				}
			}
		}
	}
}

void decode(struct tileset_reader *readers, std::map<std::string, layermap_entry> &layermap, sqlite3 *outdb, const char *outdir, struct stats *st, std::vector<std::string> &header, std::map<std::string, std::vector<std::string>> &mapping, sqlite3 *db, std::set<std::string> &exclude, std::set<std::string> &include, int ifmatched, std::string &attribution, std::string &description, std::set<std::string> &keep_layers, std::set<std::string> &remove_layers, std::string &name, json_object *filter, std::map<std::string, std::string> &attribute_descriptions, std::string &generator_options, std::vector<strategy> *strategies) {
	std::vector<std::map<std::string, layermap_entry>> layermaps;
	for (size_t i = 0; i < CPUS; i++) {
		layermaps.push_back(std::map<std::string, layermap_entry>());
	}

	std::map<zxy, std::vector<std::string>> tasks;
	double minlat = INT_MAX;
	double minlon = INT_MAX;
	double maxlat = INT_MIN;
	double maxlon = INT_MIN;
	double minlon2 = INT_MAX;
	double maxlon2 = INT_MIN;

	while (readers != NULL && !readers->all_done()) {
		std::pair<zxy, std::string> current = readers->current();

		if (current.first.z >= minzoom && current.first.z <= maxzoom) {
			zxy tile = current.first;
			if (tasks.count(tile) == 0) {
				tasks.insert(std::pair<zxy, std::vector<std::string>>(tile, std::vector<std::string>()));
			}
			auto f = tasks.find(tile);
			f->second.push_back(current.second);
		}

		// Advance the tileset_reader that we just added as a task.
		// The reason this prefetches is so the tileset_reader queue can be
		// priority-ordered, so the one with the next relevant tile
		// is first in line.
		readers->advance();

		// pull the tileset_reader off the front of the queue for reordering

		tileset_reader *r = readers;
		readers = readers->next;
		r->next = NULL;

		// Is the next tileset_reader on the tileset_reader queue looking at a different tile?
		// Then this tile is done and we can safely run the output queue.

		if (readers == NULL || readers->zoom != current.first.z || readers->x != current.first.x || readers->y != current.first.y) {
			if (tasks.size() > 100 * CPUS) {
				dispatch_tasks(tasks, layermaps, outdb, outdir, header, mapping, db, exclude, include, ifmatched, keep_layers, remove_layers, filter, readers, &minlat, &minlon, &maxlat, &maxlon, &minlon2, &maxlon2);
				tasks.clear();
			}
		}

		// put the tileset_reader back onto the queue,
		// in whatever sequence its next tile calls for

		struct tileset_reader **rr;
		for (rr = &readers; *rr != NULL; rr = &((*rr)->next)) {
			if (*r < **rr) {
				break;
			}
		}

		r->next = *rr;
		*rr = r;
	}

	dispatch_tasks(tasks, layermaps, outdb, outdir, header, mapping, db, exclude, include, ifmatched, keep_layers, remove_layers, filter, readers, &minlat, &minlon, &maxlat, &maxlon, &minlon2, &maxlon2);
	layermap = merge_layermaps(layermaps);

	st->minlon = std::min(minlon, st->minlon);
	st->maxlon = std::max(maxlon, st->maxlon);
	st->minlat = std::min(minlat, st->minlat);
	st->maxlat = std::max(maxlat, st->maxlat);

	st->minlon2 = std::min(minlon2, st->minlon2);
	st->maxlon2 = std::max(maxlon2, st->maxlon2);
	st->minlat2 = std::min(minlat, st->minlat2);
	st->maxlat2 = std::max(maxlat, st->maxlat2);

	struct tileset_reader *next;
	for (struct tileset_reader *r = readers; r != NULL; r = next) {
		next = r->next;
		r->close();

		sqlite3_stmt *stmt;
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'minzoom'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				int minz = std::max(sqlite3_column_int(stmt, 0), minzoom);
				st->minzoom = std::min(st->minzoom, minz);
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'maxzoom'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				int maxz = std::min(sqlite3_column_int(stmt, 0), maxzoom);

				if (!want_overzoom) {
					if (st->maxzoom >= 0 && maxz != st->maxzoom) {
						fprintf(stderr, "Warning: mismatched maxzooms: %d in %s vs previous %d\n", maxz, r->name.c_str(), st->maxzoom);
					}
				}

				st->maxzoom = std::max(st->maxzoom, maxz);
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'center'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *s = sqlite3_column_text(stmt, 0);
				if (s != NULL) {
					sscanf((char *) s, "%lf,%lf", &st->midlon, &st->midlat);
				}
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'attribution'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *s = sqlite3_column_text(stmt, 0);
				if (s != NULL) {
					attribution = std::string((char *) s);
				}
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'description'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *s = sqlite3_column_text(stmt, 0);
				if (s != NULL) {
					description = std::string((char *) s);
				}
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'name'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *s = sqlite3_column_text(stmt, 0);
				if (s != NULL) {
					if (name.size() == 0) {
						name = std::string((char *) s);
					} else {
						std::string proposed = name + " + " + std::string((char *) s);
						if (proposed.size() < 255) {
							name = proposed;
						}
					}
				}
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'json'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *s = sqlite3_column_text(stmt, 0);

				if (s != NULL) {
					json_pull *jp = json_begin_string((const char *) s);
					json_object *o = json_read_tree(jp);

					if (o != NULL && o->type == JSON_HASH) {
						json_object *vector_layers = json_hash_get(o, "vector_layers");

						handle_vector_layers(vector_layers, layermap, attribute_descriptions);
						json_free(o);
					}

					json_end(jp);
				}
			}

			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'generator_options'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *s = sqlite3_column_text(stmt, 0);
				if (s != NULL) {
					if (generator_options.size() != 0) {
						generator_options.append("; ");
						generator_options.append((const char *) s);
					} else {
						generator_options = (const char *) s;
					}
				}
			}
			sqlite3_finalize(stmt);
		}
		if (sqlite3_prepare_v2(r->db, "SELECT value from metadata where name = 'strategies'", -1, &stmt, NULL) == SQLITE_OK) {
			if (sqlite3_step(stmt) == SQLITE_ROW) {
				const unsigned char *s = sqlite3_column_text(stmt, 0);
				handle_strategies(s, strategies);
			}
			sqlite3_finalize(stmt);
		}

		// Closes either real r->db or temp mirror of metadata.json
		if (sqlite3_close(r->db) != SQLITE_OK) {
			fprintf(stderr, "Could not close database: %s\n", sqlite3_errmsg(r->db));
			exit(EXIT_CLOSE);
		}

		delete r;
	}
}

void usage(char **argv) {
	fprintf(stderr, "Usage: %s [-f] [-i] [-pk] [-pC] [-c joins.csv] [-X] [-x exclude ...] [-y include ...] [-r inputfile.txt ] -o new.mbtiles source.mbtiles ...\n", argv[0]);
	exit(EXIT_ARGS);
}

int main(int argc, char **argv) {
	char *out_mbtiles = NULL;
	char *out_dir = NULL;
	sqlite3 *outdb = NULL;
	char *csv = NULL;
	int force = 0;
	int ifmatched = 0;
	int filearg = 0;
	json_object *filter = NULL;

	std::string join_sqlite_fname;

	struct tileset_reader *readers = NULL;

	CPUS = get_num_avail_cpus();

	const char *TIPPECANOE_MAX_THREADS = getenv("TIPPECANOE_MAX_THREADS");
	if (TIPPECANOE_MAX_THREADS != NULL) {
		CPUS = atoi(TIPPECANOE_MAX_THREADS);
	}
	if (CPUS < 1) {
		CPUS = 1;
	}

	if (sqlite3_config(SQLITE_CONFIG_SERIALIZED) != SQLITE_OK) {
		fprintf(stderr, "Could not enable sqlite3 serialized multithreading\n");
		exit(EXIT_SQLITE);
	}

	std::vector<std::string> header;
	std::map<std::string, std::vector<std::string>> mapping;
	sqlite3 *db = NULL;

	std::set<std::string> exclude;
	std::set<std::string> include;
	std::set<std::string> keep_layers;
	std::set<std::string> remove_layers;

	std::string set_name, set_description, set_attribution;

	struct option long_options[] = {
		{"output", required_argument, 0, 'o'},
		{"output-to-directory", required_argument, 0, 'e'},
		{"force", no_argument, 0, 'f'},
		{"overzoom", no_argument, 0, 'O'},
		{"buffer", required_argument, 0, 'b'},
		{"if-matched", no_argument, 0, 'i'},
		{"attribution", required_argument, 0, 'A'},
		{"name", required_argument, 0, 'n'},
		{"description", required_argument, 0, 'N'},
		{"prevent", required_argument, 0, 'p'},
		{"csv", required_argument, 0, 'c'},
		{"exclude", required_argument, 0, 'x'},
		{"exclude-all", no_argument, 0, 'X'},
		{"include", required_argument, 0, 'y'},
		{"exclude-all-tile-attributes", no_argument, 0, '~'},
		{"layer", required_argument, 0, 'l'},
		{"exclude-layer", required_argument, 0, 'L'},
		{"quiet", no_argument, 0, 'q'},
		{"maximum-zoom", required_argument, 0, 'z'},
		{"minimum-zoom", required_argument, 0, 'Z'},
		{"feature-filter-file", required_argument, 0, 'J'},
		{"feature-filter", required_argument, 0, 'j'},
		{"rename-layer", required_argument, 0, 'R'},
		{"read-from", required_argument, 0, 'r'},

		{"join-sqlite", required_argument, 0, '~'},
		{"join-tile-attribute", required_argument, 0, '~'},
		{"join-table-expression", required_argument, 0, '~'},
		{"join-table", required_argument, 0, '~'},
		{"join-count-limit", required_argument, 0, '~'},
		{"use-attribute-for-id", required_argument, 0, '~'},

		{"no-tile-size-limit", no_argument, &pk, 1},
		{"no-tile-compression", no_argument, &pC, 1},
		{"empty-csv-columns-are-null", no_argument, &pe, 1},
		{"no-tile-stats", no_argument, &pg, 1},
		{"tile-stats-attributes-limit", required_argument, 0, '~'},
		{"tile-stats-sample-values-limit", required_argument, 0, '~'},
		{"tile-stats-values-limit", required_argument, 0, '~'},
		{"unidecode-data", required_argument, 0, '~'},

		{0, 0, 0, 0},
	};

	std::string getopt_str;
	for (size_t lo = 0; long_options[lo].name != NULL; lo++) {
		if (long_options[lo].val > ' ') {
			getopt_str.push_back(long_options[lo].val);

			if (long_options[lo].has_arg == required_argument) {
				getopt_str.push_back(':');
			}
		}
	}

	extern int optind;
	extern char *optarg;
	int i;

	std::string commandline = format_commandline(argc, argv);

	int option_index = 0;
	while ((i = getopt_long(argc, argv, getopt_str.c_str(), long_options, &option_index)) != -1) {
		switch (i) {
		case 0:
			break;

		case 'o':
			out_mbtiles = optarg;
			break;

		case 'e':
			out_dir = optarg;
			break;

		case 'f':
			force = 1;
			break;

		case 'O':
			want_overzoom = true;
			break;

		case 'b':
			buffer = atoi(optarg);
			break;

		case 'i':
			ifmatched = 1;
			break;

		case 'A':
			set_attribution = optarg;
			break;

		case 'n':
			set_name = optarg;
			break;

		case 'N':
			set_description = optarg;
			break;

		case 'z':
			maxzoom = atoi(optarg);
			break;

		case 'Z':
			minzoom = atoi(optarg);
			break;

		case 'J':
			filter = read_filter(optarg);
			break;

		case 'j':
			filter = parse_filter(optarg);
			break;

		case 'p':
			if (strcmp(optarg, "k") == 0) {
				pk = true;
			} else if (strcmp(optarg, "C") == 0) {
				pC = true;
			} else if (strcmp(optarg, "g") == 0) {
				pg = true;
			} else if (strcmp(optarg, "e") == 0) {
				pe = true;
			} else {
				fprintf(stderr, "%s: Unknown option for -p%s\n", argv[0], optarg);
				exit(EXIT_ARGS);
			}
			break;

		case 'c':
			if (csv != NULL) {
				fprintf(stderr, "Only one -c for now\n");
				exit(EXIT_ARGS);
			}

			csv = optarg;
			readcsv(csv, header, mapping);
			break;

		case 'x':
			exclude.insert(std::string(optarg));
			break;

		case 'X':
			exclude_all = true;
			break;

		case 'y':
			exclude_all = true;
			include.insert(std::string(optarg));
			break;

		case 'l':
			keep_layers.insert(std::string(optarg));
			break;

		case 'L':
			remove_layers.insert(std::string(optarg));
			break;

		case 'R': {
			char *cp = strchr(optarg, ':');
			if (cp == NULL || cp == optarg) {
				fprintf(stderr, "%s: -R requires old:new\n", argv[0]);
				exit(EXIT_ARGS);
			}
			std::string before = std::string(optarg).substr(0, cp - optarg);
			std::string after = std::string(cp + 1);
			renames.insert(std::pair<std::string, std::string>(before, after));
			break;
		}

		case 'r': {
			std::fstream read_file;
			read_file.open(std::string(optarg), std::ios::in);
			if (read_file.is_open()) {
				std::string sa;
				filearg = 1;
				while (getline(read_file, sa)) {
					char *c = const_cast<char *>(sa.c_str());
					tileset_reader *r = begin_reading(c);

					// put the new tileset_reader in priority order
					struct tileset_reader **rr;
					for (rr = &readers; *rr != NULL; rr = &((*rr)->next)) {
						if (*r < **rr) {
							break;
						}
					}
					r->next = *rr;
					*rr = r;
				}
				read_file.close();
			}
			break;
		}

		case 'q':
			quiet = true;
			break;

		case '~': {
			const char *opt = long_options[option_index].name;
			if (strcmp(opt, "tile-stats-attributes-limit") == 0) {
				max_tilestats_attributes = atoi(optarg);
			} else if (strcmp(opt, "tile-stats-sample-values-limit") == 0) {
				max_tilestats_sample_values = atoi(optarg);
			} else if (strcmp(opt, "tile-stats-values-limit") == 0) {
				max_tilestats_values = atoi(optarg);
			} else if (strcmp(opt, "unidecode-data") == 0) {
				unidecode_data = read_unidecode(optarg);
			} else if (strcmp(opt, "join-sqlite") == 0) {
				join_sqlite_fname = optarg;
				if (sqlite3_open(optarg, &db) != SQLITE_OK) {
					fprintf(stderr, "%s: %s\n", optarg, sqlite3_errmsg(db));
					exit(EXIT_SQLITE);
				}
			} else if (strcmp(opt, "join-table") == 0) {
				join_table = optarg;
			} else if (strcmp(opt, "join-table-expression") == 0) {
				join_table_expression = optarg;
			} else if (strcmp(opt, "join-tile-attribute") == 0) {
				join_tile_attribute = optarg;
			} else if (strcmp(opt, "use-attribute-for-id") == 0) {
				attribute_for_id = optarg;
			} else if (strcmp(opt, "exclude-all-tile-attributes") == 0) {
				exclude_all_tile_attributes = true;
			} else if (strcmp(opt, "join-count-limit") == 0) {
				join_count_limit = atoi(optarg);
			} else {
				fprintf(stderr, "%s: Unrecognized option --%s\n", argv[0], opt);
				exit(EXIT_ARGS);
			}
			break;
		}

		default:
			usage(argv);
		}
	}

	if ((argc - optind < 1) && (filearg == 0)) {
		usage(argv);
	}

	if (out_mbtiles == NULL && out_dir == NULL) {
		fprintf(stderr, "%s: must specify -o out.mbtiles or -e directory\n", argv[0]);
		usage(argv);
	}

	if (out_mbtiles != NULL && out_dir != NULL) {
		fprintf(stderr, "%s: Options -o and -e cannot be used together\n", argv[0]);
		usage(argv);
	}

	if (minzoom > maxzoom) {
		fprintf(stderr, "%s: Minimum zoom -Z%d cannot be greater than maxzoom -z%d\n", argv[0], minzoom, maxzoom);
		exit(EXIT_ARGS);
	}

	if (buffer < 0) {
		fprintf(stderr, "%s: buffer cannot be less than 0\n", argv[0]);
		exit(EXIT_ARGS);
	}

	if (out_mbtiles != NULL) {
		if (force) {
			unlink(out_mbtiles);
		} else {
			if (pmtiles_has_suffix(out_mbtiles)) {
				check_pmtiles(out_mbtiles, argv, false);
			}
		}

		outdb = mbtiles_open(out_mbtiles, argv, 0);
	}
	if (out_dir != NULL) {
		check_dir(out_dir, argv, force, false);
	}

	struct stats st;
	st.minzoom = st.minlat = st.minlon = st.minlat2 = st.minlon2 = INT_MAX;
	st.maxzoom = st.maxlat = st.maxlon = st.maxlat2 = st.maxlon2 = INT_MIN;

	std::map<std::string, layermap_entry> layermap;
	std::string attribution;
	std::string description;
	std::string name;

	if (filearg == 0) {
		for (i = optind; i < argc; i++) {
			tileset_reader *r = begin_reading(argv[i]);

			// put the new tileset_reader in priority order
			struct tileset_reader **rr;
			for (rr = &readers; *rr != NULL; rr = &((*rr)->next)) {
				if (*r < **rr) {
					break;
				}
			}

			r->next = *rr;
			*rr = r;
		}
	}

	std::map<std::string, std::string> attribute_descriptions;
	std::string generator_options;
	std::vector<strategy> strategies;

	decode(readers, layermap, outdb, out_dir, &st, header, mapping, db, exclude, include, ifmatched, attribution, description, keep_layers, remove_layers, name, filter, attribute_descriptions, generator_options, &strategies);

	if (set_attribution.size() != 0) {
		attribution = set_attribution;
	}
	if (set_description.size() != 0) {
		description = set_description;
	}
	if (set_name.size() != 0) {
		name = set_name;
	}

	if (generator_options.size() != 0) {
		generator_options.append("; ");
	}
	generator_options.append(commandline);

	// don't trust the source metadata maxzooms;
	// claim the zooms that were actually written
	st.maxzoom = INT_MIN;
	st.minzoom = INT_MAX;

	for (auto &l : layermap) {
		if (l.second.minzoom < st.minzoom) {
			st.minzoom = l.second.minzoom;
		}
		if (l.second.maxzoom > st.maxzoom) {
			st.maxzoom = l.second.maxzoom;
		}
	}

	if (st.maxlon < st.minlon) {
		st.maxlon = st.minlon = st.maxlat = st.minlat = st.minlon2 = st.maxlon2 = st.minlat2 = st.maxlat2 = 0;
	}

	if (st.maxlon - st.minlon <= st.maxlon2 - st.minlon2) {
		st.minlon2 = st.minlon;
		st.maxlon2 = st.maxlon;
	}

	metadata m = make_metadata(name.c_str(), st.minzoom, st.maxzoom, st.minlat, st.minlon, st.maxlat, st.maxlon, st.minlat2, st.minlon2, st.maxlat2, st.maxlon2, st.midlat, st.midlon, attribution.size() != 0 ? attribution.c_str() : NULL, layermap, true, description.c_str(), !pg, attribute_descriptions, "tile-join", generator_options, strategies, st.maxzoom, 2.5, 1);

	if (outdb != NULL) {
		mbtiles_write_metadata(outdb, m, true);
	} else {
		dir_write_metadata(out_dir, m);
	}

	if (outdb != NULL) {
		mbtiles_close(outdb, argv[0]);
	}

	if (filter != NULL) {
		json_free(filter);
	}

	if (pmtiles_has_suffix(out_mbtiles)) {
		mbtiles_map_image_to_pmtiles(out_mbtiles, m, !pC, quiet, false);
	}

	return 0;
}
