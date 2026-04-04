#include <iostream>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sqlite3.h>
#include "jsonpull/jsonpull.h"
#include "mbtiles.hpp"
#include "dirtiles.hpp"
#include "errors.hpp"
#include "write_json.hpp"

namespace fs = std::filesystem;

std::string dir_read_tile(std::string base, struct zxy tile) {
	std::ifstream pbfFile(base + "/" + tile.path(), std::ios::in | std::ios::binary);
	std::ostringstream contents;
	contents << pbfFile.rdbuf();
	pbfFile.close();

	return (contents.str());
}

void dir_write_tile(const char *outdir, int z, int tx, int ty, std::string const &pbf) {
	fs::path dirPath(outdir);
	fs::path zPath = dirPath / std::to_string(z);
	fs::path xPath = zPath / std::to_string(tx);
	fs::path tilePath = xPath / (std::to_string(ty) + ".pbf");

	// create_directories() does not treat a directory already existing
	// as an error, which is fine since most directories will already exist.
	std::error_code ec;
	fs::create_directories(xPath, ec);
	if (ec) {
		fprintf(stderr, "Failed to create directories: %s\n", ec.message().c_str());
		exit(EXIT_WRITE);
	}

	// Set permissions equivalent to S_IRWXU | S_IRWXG | S_IRWXO (0777)
	fs::permissions(dirPath, fs::perms::all, fs::perm_options::add, ec);
	fs::permissions(zPath, fs::perms::all, fs::perm_options::add, ec);
	fs::permissions(xPath, fs::perms::all, fs::perm_options::add, ec);

	if (fs::exists(tilePath)) {
		fprintf(stderr, "Can't write tile to already existing %s\n", tilePath.c_str());
		exit(EXIT_EXISTS);
	}

	FILE *fp = fopen(tilePath.c_str(), "wb");
	if (fp == NULL) {
		fprintf(stderr, "%s: %s\n", tilePath.c_str(), strerror(errno));
		exit(EXIT_WRITE);
	}

	if (fwrite(pbf.c_str(), sizeof(char), pbf.size(), fp) != pbf.size()) {
		fprintf(stderr, "%s: %s\n", tilePath.c_str(), strerror(errno));
		exit(EXIT_WRITE);
	}

	if (fclose(fp) != 0) {
		fprintf(stderr, "%s: %s\n", tilePath.c_str(), strerror(errno));
		exit(EXIT_CLOSE);
	}
}

static bool numeric(const char *s) {
	if (*s == '\0') {
		return false;
	}
	for (; *s != 0; s++) {
		if (*s < '0' || *s > '9') {
			return false;
		}
	}
	return true;
}

static bool pbfname(const char *s) {
	while (*s >= '0' && *s <= '9') {
		s++;
	}

	return strcmp(s, ".pbf") == 0 || strcmp(s, ".mvt") == 0;
}

void check_dir(const char *dir, char **argv, bool force, bool forcetable) {
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		fprintf(stderr, "Failed to create directory: %s\n", ec.message().c_str());
		exit(EXIT_WRITE);
	}
	fs::permissions(dir, fs::perms::all, fs::perm_options::add, ec);
	std::string meta = fs::path(dir) / "metadata.json";
	if (force) {
		fs::remove(meta, ec);  // error OK since it may not exist
	} else {
		if (fs::exists(meta)) {
			fprintf(stderr, "%s: Tileset \"%s\" already exists. You can use --force if you want to delete the old tileset.\n", argv[0], dir);
			fprintf(stderr, "%s: %s: file exists\n", argv[0], meta.c_str());
			if (!forcetable) {
				exit(EXIT_EXISTS);
			}
		}
	}

	if (forcetable) {
		// Don't clear existing tiles
		return;
	}

	std::vector<zxy> tiles = enumerate_dirtiles(dir, INT_MIN, INT_MAX);

	for (size_t i = 0; i < tiles.size(); i++) {
		std::string fn = std::string(dir) + "/" + tiles[i].path();

		if (force) {
			fs::remove(fn, ec);
			if (ec) {
				perror(fn.c_str());
				exit(EXIT_UNLINK);
			}
		} else {
			fprintf(stderr, "%s: file exists\n", fn.c_str());
			exit(EXIT_EXISTS);
		}
	}
}

std::vector<zxy> enumerate_dirtiles(const char *fname, int minzoom, int maxzoom) {
	std::vector<zxy> tiles;

	DIR *d1 = opendir(fname);
	if (d1 != NULL) {
		struct dirent *dp;
		while ((dp = readdir(d1)) != NULL) {
			if (numeric(dp->d_name) && atoi(dp->d_name) >= minzoom && atoi(dp->d_name) <= maxzoom) {
				std::string z = std::string(fname) + "/" + dp->d_name;
				int tz = atoi(dp->d_name);

				DIR *d2 = opendir(z.c_str());
				if (d2 == NULL) {
					perror(z.c_str());
					exit(EXIT_OPEN);
				}

				struct dirent *dp2;
				while ((dp2 = readdir(d2)) != NULL) {
					if (numeric(dp2->d_name)) {
						std::string x = z + "/" + dp2->d_name;
						int tx = atoi(dp2->d_name);

						DIR *d3 = opendir(x.c_str());
						if (d3 == NULL) {
							perror(x.c_str());
							exit(EXIT_OPEN);
						}

						struct dirent *dp3;
						while ((dp3 = readdir(d3)) != NULL) {
							if (pbfname(dp3->d_name)) {
								int ty = atoi(dp3->d_name);
								zxy tile(tz, tx, ty);
								if (strstr(dp3->d_name, ".mvt") != NULL) {
									tile.extension = ".mvt";
								}

								tiles.push_back(tile);
							}
						}

						closedir(d3);
					}
				}

				closedir(d2);
			}
		}

		closedir(d1);
	}

	std::stable_sort(tiles.begin(), tiles.end());
	return tiles;
}

void dir_erase_zoom(const char *fname, int zoom) {
	DIR *d1 = opendir(fname);
	if (d1 != NULL) {
		struct dirent *dp;
		while ((dp = readdir(d1)) != NULL) {
			if (numeric(dp->d_name) && atoi(dp->d_name) == zoom) {
				std::string z = std::string(fname) + "/" + dp->d_name;

				DIR *d2 = opendir(z.c_str());
				if (d2 == NULL) {
					perror(z.c_str());
					exit(EXIT_OPEN);
				}

				struct dirent *dp2;
				while ((dp2 = readdir(d2)) != NULL) {
					if (numeric(dp2->d_name)) {
						std::string x = z + "/" + dp2->d_name;

						DIR *d3 = opendir(x.c_str());
						if (d3 == NULL) {
							perror(x.c_str());
							exit(EXIT_OPEN);
						}

						struct dirent *dp3;
						while ((dp3 = readdir(d3)) != NULL) {
							if (pbfname(dp3->d_name)) {
								std::string y = x + "/" + dp3->d_name;
								std::error_code ec;
								fs::remove(y, ec);
								if (ec) {
									perror(y.c_str());
									exit(EXIT_UNLINK);
								}
							}
						}

						closedir(d3);
					}
				}

				closedir(d2);
			}
		}

		closedir(d1);
	}
}

sqlite3 *dirmeta2tmp(const char *fname) {
	sqlite3 *db;
	char *err = NULL;

	if (sqlite3_open("", &db) != SQLITE_OK) {
		fprintf(stderr, "Temporary db: %s\n", sqlite3_errmsg(db));
		exit(EXIT_SQLITE);
	}
	if (sqlite3_exec(db, "CREATE TABLE metadata (name text, value text);", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "Create metadata table: %s\n", err);
		exit(EXIT_SQLITE);
	}

	std::string name = fname;
	name += "/metadata.json";

	FILE *f = fopen(name.c_str(), "r");
	if (f == NULL) {
		perror(name.c_str());
	} else {
		json_pull *jp = json_begin_file(f);
		json_object *o = json_read_tree(jp);
		if (o == NULL) {
			fprintf(stderr, "%s: metadata parsing error: %s\n", name.c_str(), jp->error);
			exit(EXIT_JSON);
		}

		if (o->type != JSON_HASH) {
			fprintf(stderr, "%s: bad metadata format\n", name.c_str());
			exit(EXIT_JSON);
		}

		for (size_t i = 0; i < o->value.object.length; i++) {
			if (o->value.object.keys[i]->type != JSON_STRING || o->value.object.values[i]->type != JSON_STRING) {
				fprintf(stderr, "%s: non-string in metadata\n", name.c_str());
			}

			char *sql = sqlite3_mprintf("INSERT INTO metadata (name, value) VALUES (%Q, %Q);", o->value.object.keys[i]->value.string.string, o->value.object.values[i]->value.string.string);
			if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
				fprintf(stderr, "set %s in metadata: %s\n", o->value.object.keys[i]->value.string.string, err);
			}
			sqlite3_free(sql);
		}

		json_end(jp);
		fclose(f);
	}

	return db;
}

static void out(json_writer &state, std::string k, std::string v) {
	state.json_comma_newline();
	state.json_write_string(k);
	state.json_write_string(v);
}

void dir_write_metadata(const char *outdir, const metadata &m) {
	std::string metadata = std::string(outdir) + "/metadata.json";

	if (fs::exists(metadata)) {
		// Leave existing metadata in place with --allow-existing
	} else {
		FILE *fp = fopen(metadata.c_str(), "w");
		if (fp == NULL) {
			perror(metadata.c_str());
			exit(EXIT_OPEN);
		}

		json_writer state(fp);

		state.json_write_hash();
		state.json_write_newline();

		out(state, "name", m.name);
		out(state, "description", m.description);
		out(state, "version", std::to_string(m.version));
		out(state, "minzoom", std::to_string(m.minzoom));
		out(state, "maxzoom", std::to_string(m.maxzoom));
		out(state, "center", std::to_string(m.center_lon) + "," + std::to_string(m.center_lat) + "," + std::to_string(m.center_z));
		out(state, "bounds", std::to_string(m.minlon) + "," + std::to_string(m.minlat) + "," + std::to_string(m.maxlon) + "," + std::to_string(m.maxlat));
		out(state, "antimeridian_adjusted_bounds", std::to_string(m.minlon2) + "," + std::to_string(m.minlat2) + "," + std::to_string(m.maxlon2) + "," + std::to_string(m.maxlat2));
		out(state, "type", m.type);
		if (m.attribution.size() > 0) {
			out(state, "attribution", m.attribution);
		}
		if (m.strategies_json.size() > 0) {
			out(state, "strategies", m.strategies_json);
		}
		if (m.decisions_json.size() > 0) {
			out(state, "tippecanoe_decisions", m.decisions_json);
		}
		out(state, "format", m.format);
		out(state, "generator", m.generator);
		out(state, "generator_options", m.generator_options);

		if (m.vector_layers_json.size() > 0 || m.tilestats_json.size() > 0) {
			std::string json = "{";

			if (m.vector_layers_json.size() > 0) {
				json += "\"vector_layers\":" + m.vector_layers_json;

				if (m.tilestats_json.size() > 0) {
					json += ",\"tilestats\":" + m.tilestats_json;
				}
			} else {
				if (m.tilestats_json.size() > 0) {
					json += "\"tilestats\":" + m.tilestats_json;
				}
			}

			json += "}";

			out(state, "json", json);
		}

		state.json_write_newline();
		state.json_end_hash();
		state.json_write_newline();
		fclose(fp);
	}
}
