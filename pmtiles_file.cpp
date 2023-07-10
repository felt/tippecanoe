#include <unordered_map>
#include <vector>
#include <fstream>
#include <string.h>
#include <algorithm>
#include <unistd.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include "errors.hpp"
#include "pmtiles_file.hpp"
#include "mvt.hpp"
#include "write_json.hpp"
#include "main.hpp"

bool pmtiles_has_suffix(const char *filename) {
	if (filename == nullptr) {
		return false;
	}
	size_t lenstr = strlen(filename);
	if (lenstr < 8) {
		return false;
	}
	if (strncmp(filename + (lenstr - 8), ".pmtiles", 8) == 0) {
		return true;
	}
	return false;
}

void check_pmtiles(const char *filename, char **argv, bool forcetable) {
	struct stat st;
	if (stat(filename, &st) == 0) {
		fprintf(stderr, "%s: Tileset \"%s\" already exists. You can use --force if you want to delete the old tileset.\n", argv[0], filename);
		fprintf(stderr, "%s: %s: file exists\n", argv[0], filename);

		if (forcetable) {
			fprintf(stderr, "%s: --allow-existing is not supported for pmtiles\n", argv[0]);
		}

		exit(EXIT_EXISTS);
	}
}

std::string decompress_fn(const std::string &input, uint8_t compression) {
	std::string output;
	if (compression == pmtiles::COMPRESSION_NONE) {
		output = input;
	} else if (compression == pmtiles::COMPRESSION_GZIP) {
		decompress(input, output);
	} else {
		throw std::runtime_error("Unknown or unsupported compression.");
	}

	return output;
}

std::string compress_fn(const std::string &input, uint8_t compression) {
	std::string output;
	if (compression == pmtiles::COMPRESSION_NONE) {
		output = input;
	} else if (compression == pmtiles::COMPRESSION_GZIP) {
		compress(input, output, true);
	} else {
		throw std::runtime_error("Unknown or unsupported compression.");
	}

	return output;
}

std::vector<pmtiles::entry_zxy> pmtiles_entries_tms(const char *pmtiles_map, int minzoom, int maxzoom) {
	std::vector<pmtiles::entry_zxy> filtered;
	auto all_entries = pmtiles::entries_tms(&decompress_fn, pmtiles_map);
	std::copy_if(all_entries.begin(), all_entries.end(), std::back_inserter(filtered), [minzoom, maxzoom](pmtiles::entry_zxy e) { return e.z >= minzoom && e.z <= maxzoom; });
	return filtered;
}

std::pair<uint64_t, uint32_t> pmtiles_get_tile(const char *pmtiles_map, int z, int x, int y) {
	return pmtiles::get_tile(&decompress_fn, pmtiles_map, z, x, y);
}

static void out(json_writer &state, std::string k, std::string v) {
	state.json_comma_newline();
	state.json_write_string(k);
	state.json_write_string(v);
}

std::string metadata_to_pmtiles_json(metadata m) {
	std::string buf;
	json_writer state(&buf);

	state.json_write_hash();
	state.json_write_newline();

	out(state, "name", m.name);
	out(state, "format", m.format);
	out(state, "type", m.type);
	out(state, "description", m.description);
	out(state, "version", std::to_string(m.version));
	if (m.attribution.size() > 0) {
		out(state, "attribution", m.attribution);
	}
	if (m.strategies_json.size() > 0) {
		state.json_comma_newline();
		state.json_write_string("strategies");
		state.json_write_json(m.strategies_json);
	}
	out(state, "generator", m.generator);
	out(state, "generator_options", m.generator_options);

	std::string bounds2 = std::to_string(m.minlon2) + "," + std::to_string(m.minlat2) + "," + std::to_string(m.maxlon2) + "," + std::to_string(m.maxlat2);
	out(state, "antimeridian_adjusted_bounds", bounds2);

	if (m.vector_layers_json.size() > 0) {
		state.json_comma_newline();
		state.json_write_string("vector_layers");
		state.json_write_json(m.vector_layers_json);
	}

	if (m.tilestats_json.size() > 0) {
		state.json_comma_newline();
		state.json_write_string("tilestats");
		state.json_write_json(m.tilestats_json);
	}

	state.json_write_newline();
	state.json_end_hash();
	state.json_write_newline();
	std::string compressed;
	compress(buf, compressed, true);
	return compressed;
}

void mbtiles_map_image_to_pmtiles(char *fname, metadata m, bool tile_compression, bool fquiet, bool fquiet_progress) {
	sqlite3 *db;

	if (sqlite3_open(fname, &db) != SQLITE_OK) {
		fprintf(stderr, "%s: %s\n", fname, sqlite3_errmsg(db));
		exit(EXIT_SQLITE);
	}

	char *err = NULL;
	if (sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: integrity_check: %s\n", fname, err);
		exit(EXIT_SQLITE);
	}

	// materialize list of all tile IDs
	std::vector<uint64_t> tile_ids;

	{
		const char *sql = "SELECT zoom_level, tile_column, tile_row FROM map";
		sqlite3_stmt *stmt;

		if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
			fprintf(stderr, "%s: select failed: %s\n", fname, sqlite3_errmsg(db));
			exit(EXIT_SQLITE);
		}

		while (sqlite3_step(stmt) == SQLITE_ROW) {
			int zoom = sqlite3_column_int(stmt, 0);
			int x = sqlite3_column_int(stmt, 1);
			int sorty = sqlite3_column_int(stmt, 2);
			int y = (1LL << zoom) - 1 - sorty;
			uint64_t res = pmtiles::zxy_to_tileid(zoom, x, y);
			tile_ids.push_back(res);
		}

		sqlite3_finalize(stmt);
	}

	std::sort(tile_ids.begin(), tile_ids.end());

	std::unordered_map<std::string, std::pair<unsigned long long, unsigned long>> hash_to_offset_len;
	std::vector<pmtiles::entryv3> entries;
	unsigned long long offset = 0;

	std::string tmpname = (std::string(fname) + ".tmp");

	// write tile data to tempfile in clustered order
	{
		const char *map_sql = "SELECT tile_id FROM map WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?";
		sqlite3_stmt *map_stmt;

		if (sqlite3_prepare_v2(db, map_sql, -1, &map_stmt, NULL) != SQLITE_OK) {
			fprintf(stderr, "%s: select failed: %s\n", fname, sqlite3_errmsg(db));
			exit(EXIT_SQLITE);
		}

		const char *image_sql = "SELECT tile_data FROM images WHERE tile_id = ?";
		sqlite3_stmt *image_stmt;

		if (sqlite3_prepare_v2(db, image_sql, -1, &image_stmt, NULL) != SQLITE_OK) {
			fprintf(stderr, "%s: select failed: %s\n", fname, sqlite3_errmsg(db));
			exit(EXIT_SQLITE);
		}

		std::ofstream tmp_ostream;

		tmp_ostream.open(tmpname.c_str(), std::ios::out | std::ios::binary);

		int idx = 0;
		int progress_reported = -1;
		for (auto const &tile_id : tile_ids) {
			idx = idx + 1;
			double progress = ((double) idx / tile_ids.size()) * 100;
			pmtiles::zxy zxy = pmtiles::tileid_to_zxy(tile_id);
			if (!fquiet && !fquiet_progress && progress_time() && (int) progress != progress_reported) {
				fprintf(stderr, "  %3.1f%%  %d/%u/%u  \r", progress, zxy.z, zxy.x, zxy.y);
				progress_reported = (int) progress;
				fflush(stderr);
			}
			sqlite3_bind_int(map_stmt, 1, zxy.z);
			sqlite3_bind_int(map_stmt, 2, zxy.x);
			sqlite3_bind_int(map_stmt, 3, (1LL << zxy.z) - 1 - zxy.y);

			if (sqlite3_step(map_stmt) != SQLITE_ROW) {
				fprintf(stderr, "Corrupt mbtiles file: null entry in map table\n");
				exit(EXIT_SQLITE);
			}

			std::string hsh{reinterpret_cast<const char *>(sqlite3_column_text(map_stmt, 0))};

			if (hash_to_offset_len.count(hsh) > 0) {
				auto offset_len = hash_to_offset_len.at(hsh);
				if (entries.size() > 0 && tile_id == entries[entries.size() - 1].tile_id + 1 && entries[entries.size() - 1].offset == std::get<0>(offset_len)) {
					entries[entries.size() - 1].run_length++;
				} else {
					entries.emplace_back(tile_id, std::get<0>(offset_len), std::get<1>(offset_len), 1);
				}
			} else {
				sqlite3_bind_text(image_stmt, 1, hsh.data(), hsh.size(), SQLITE_STATIC);
				if (sqlite3_step(image_stmt) != SQLITE_ROW) {
					fprintf(stderr, "Corrupt mbtiles file: null entry in image table\n");
					exit(EXIT_SQLITE);
				}

				int len = sqlite3_column_bytes(image_stmt, 0);
				const char *blob = (const char *) sqlite3_column_blob(image_stmt, 0);

				tmp_ostream.write(blob, len);

				entries.emplace_back(tile_id, offset, len, 1);
				hash_to_offset_len.emplace(hsh, std::make_pair(offset, len));
				offset += len;

				sqlite3_reset(image_stmt);
				sqlite3_clear_bindings(image_stmt);
			}

			sqlite3_reset(map_stmt);
			sqlite3_clear_bindings(map_stmt);
		}
		tmp_ostream.close();
		sqlite3_finalize(map_stmt);
		sqlite3_finalize(image_stmt);
	}

	// finalize PMTiles archive.
	{
		std::sort(entries.begin(), entries.end(), pmtiles::entryv3_cmp);

		std::string root_bytes;
		std::string leaves_bytes;
		int num_leaves;
		std::tie(root_bytes, leaves_bytes, num_leaves) = make_root_leaves(&compress_fn, pmtiles::COMPRESSION_GZIP, entries);

		pmtiles::headerv3 header;

		header.min_zoom = m.minzoom;
		header.max_zoom = m.maxzoom;
		header.min_lon_e7 = m.minlon * 10000000;
		header.min_lat_e7 = m.minlat * 10000000;
		header.max_lon_e7 = m.maxlon * 10000000;
		header.max_lat_e7 = m.maxlat * 10000000;
		header.center_zoom = m.center_z;
		header.center_lon_e7 = m.center_lon * 10000000;
		header.center_lat_e7 = m.center_lat * 10000000;

		std::string json_metadata = metadata_to_pmtiles_json(m);

		sqlite3_close(db);

		header.clustered = 0x1;
		header.internal_compression = pmtiles::COMPRESSION_GZIP;

		if (tile_compression) {
			header.tile_compression = pmtiles::COMPRESSION_GZIP;
		} else {
			header.tile_compression = pmtiles::COMPRESSION_NONE;
		}

		if (m.format == "pbf") {
			header.tile_type = pmtiles::TILETYPE_MVT;
		} else if (m.format == "png") {
			header.tile_type = pmtiles::TILETYPE_PNG;
		} else {
			header.tile_type = pmtiles::TILETYPE_UNKNOWN;
		}

		header.root_dir_offset = 127;
		header.root_dir_bytes = root_bytes.size();

		header.json_metadata_offset = header.root_dir_offset + header.root_dir_bytes;
		header.json_metadata_bytes = json_metadata.size();
		header.leaf_dirs_offset = header.json_metadata_offset + header.json_metadata_bytes;
		header.leaf_dirs_bytes = leaves_bytes.size();
		header.tile_data_offset = header.leaf_dirs_offset + header.leaf_dirs_bytes;
		header.tile_data_bytes = offset;

		header.addressed_tiles_count = tile_ids.size();
		header.tile_entries_count = entries.size();
		header.tile_contents_count = hash_to_offset_len.size();

		std::ifstream tmp_istream(tmpname.c_str(), std::ios::in | std::ios_base::binary);

		std::ofstream ostream;
		ostream.open(fname, std::ios::out | std::ios::binary);

		auto header_str = header.serialize();
		ostream.write(header_str.data(), header_str.length());
		ostream.write(root_bytes.data(), root_bytes.length());
		ostream.write(json_metadata.data(), json_metadata.size());
		ostream.write(leaves_bytes.data(), leaves_bytes.length());
		ostream << tmp_istream.rdbuf();

		tmp_istream.close();
		unlink(tmpname.c_str());
		ostream.close();
	}
}

// this should go away if we get rid of temporary metadata DBs.
// it transforms the PMTiles header + json into string keys/string values
// consistent with mbtiles and dirtiles, for the test suite
sqlite3 *pmtilesmeta2tmp(const char *fname, const char *pmtiles_map) {
	sqlite3 *db;
	char *sql;
	char *err;

	if (sqlite3_open("", &db) != SQLITE_OK) {
		fprintf(stderr, "Temporary db: %s\n", sqlite3_errmsg(db));
		exit(EXIT_SQLITE);
	}
	if (sqlite3_exec(db, "CREATE TABLE metadata (name text, value text);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "Create metadata table: %s\n", err);
		exit(EXIT_SQLITE);
	}

	std::string header_s{pmtiles_map, 127};
	auto header = pmtiles::deserialize_header(header_s);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('minzoom', %d);", header.min_zoom);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set minzoom: %s\n", err);
	}
	sqlite3_free(sql);

	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('maxzoom', %d);", header.max_zoom);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set maxzoom: %s\n", err);
	}
	sqlite3_free(sql);

	double center_lon = double(header.center_lon_e7) / 10000000.0;
	double center_lat = double(header.center_lat_e7) / 10000000.0;
	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('center', '%f,%f,%d');", center_lon, center_lat, header.center_zoom);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set center: %s\n", err);
	}
	sqlite3_free(sql);

	double minlon = double(header.min_lon_e7) / 10000000.0;
	double minlat = double(header.min_lat_e7) / 10000000.0;
	double maxlon = double(header.max_lon_e7) / 10000000.0;
	double maxlat = double(header.max_lat_e7) / 10000000.0;
	sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('bounds', '%f,%f,%f,%f');", minlon, minlat, maxlon, maxlat);
	if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "set bounds: %s\n", err);
	}
	sqlite3_free(sql);

	std::string json_s{pmtiles_map + header.json_metadata_offset, header.json_metadata_bytes};
	std::string decompressed_json;

	if (header.internal_compression == pmtiles::COMPRESSION_NONE) {
		decompressed_json = json_s;
	} else if (header.internal_compression == pmtiles::COMPRESSION_GZIP) {
		decompress(json_s, decompressed_json);
	} else {
		fprintf(stderr, "Unknown or unsupported pmtiles compression: %d\n", header.internal_compression);
		exit(EXIT_OPEN);
	}

	json_pull *jp = json_begin_string(decompressed_json.c_str());
	json_object *o = json_read_tree(jp);
	if (o == NULL) {
		fprintf(stderr, "%s: metadata parsing error: %s\n", fname, jp->error);
		exit(EXIT_JSON);
	}

	if (o->type != JSON_HASH) {
		fprintf(stderr, "%s: bad metadata format\n", fname);
		exit(EXIT_JSON);
	}

	bool has_json = false;
	std::string buf;
	json_writer state(&buf);
	state.nospace = true;
	state.json_write_hash();

	for (size_t i = 0; i < o->value.object.length; i++) {
		const char *key = o->value.object.keys[i]->value.string.string;
		if (strcmp(key, "vector_layers") == 0 && o->value.object.values[i]->type == JSON_ARRAY) {
			has_json = true;
			state.nospace = true;
			state.json_write_string("vector_layers");
			state.nospace = true;
			state.json_write_json(json_stringify(o->value.object.values[i]));
		} else if (strcmp(key, "tilestats") == 0 && o->value.object.values[i]->type == JSON_HASH) {
			has_json = true;
			state.nospace = true;
			state.json_write_string("tilestats");
			state.nospace = true;
			state.json_write_json(json_stringify(o->value.object.values[i]));
		} else if (strcmp(key, "strategies") == 0 && o->value.object.values[i]->type == JSON_ARRAY) {
			sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('strategies', %Q);", json_stringify(o->value.object.values[i]));
			if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
				fprintf(stderr, "set %s in metadata: %s\n", key, err);
			}
			sqlite3_free(sql);
		} else if (o->value.object.keys[i]->type != JSON_STRING || o->value.object.values[i]->type != JSON_STRING) {
			fprintf(stderr, "%s\n", key);
			fprintf(stderr, "%s: non-string in metadata\n", fname);
		} else {
			sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES (%Q, %Q);", key, o->value.object.values[i]->value.string.string);
			if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
				fprintf(stderr, "set %s in metadata: %s\n", key, err);
			}
			sqlite3_free(sql);
		}
	}

	json_end(jp);
	state.nospace = true;
	state.json_end_hash();

	if (has_json) {
		sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES ('json', %Q);", buf.c_str());
		if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
			fprintf(stderr, "set json in metadata: %s\n", err);
		}
		sqlite3_free(sql);
	}

	return db;
}
