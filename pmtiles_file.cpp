#include <sys/stat.h>
#include <unistd.h>
#include "pmtiles_file.hpp"
#include "write_json.hpp"
#include "version.hpp"
#include "errors.hpp"

bool pmtiles_has_suffix(const char *filename) {
	size_t lenstr = strlen(filename);
	if (lenstr < 8) {
		fprintf(stderr, "Archive name missing suffix: %s\n", filename);
		exit(EXIT_ARGS);
	}
	if (strncmp(filename + (lenstr - 8), ".pmtiles", 8) == 0) {
		return true;
	}
	return false;
}

pmtiles_file *pmtiles_open(const char *filename, char **argv, int force) {
	pmtiles_file *outfile = new pmtiles_file;

	struct stat st;
	if (force) {
		unlink(filename);
	} else {
		if (stat(filename, &st) == 0) {
			fprintf(stderr, "%s: %s: file exists\n", argv[0], filename);
			exit(EXIT_ARGS);
		}
	}
	outfile->ostream.open(filename, std::ios::out | std::ios::binary);
	outfile->tmp_name = std::string(filename) + ".tmp";
	outfile->tmp_ostream.open(outfile->tmp_name, std::ios::out | std::ios::binary);
	outfile->offset = 0;

	outfile->lock = PTHREAD_MUTEX_INITIALIZER;
	return outfile;
}

void pmtiles_write_tile(pmtiles_file *outfile, int z, int tx, int ty, const char *data, int size) {
	if (pthread_mutex_lock(&outfile->lock) != 0) {
		perror("pthread_mutex_lock");
		exit(EXIT_PTHREAD);
	}

	outfile->tmp_ostream.write(data, size);

	auto tile_id = pmtiles::zxy_to_tileid(z, tx, ty);
	outfile->entries.emplace_back(tile_id, outfile->offset, size, 1);
	outfile->offset += size;

	if (pthread_mutex_unlock(&outfile->lock) != 0) {
		perror("pthread_mutex_unlock");
		exit(EXIT_PTHREAD);
	}
}

std::string pmtiles_metadata_json(const char *fname, const char *attribution, std::map<std::string, layermap_entry> const &layermap, bool vector, const char *description, bool do_tilestats, std::map<std::string, std::string> const &attribute_descriptions, std::string const &program, std::string const &commandline) {
	std::string buf;
	json_writer state(&buf);
	state.json_write_hash();
	state.nospace = true;

	state.json_write_newline();

	state.json_write_string("name");
	state.json_write_string(fname);
	state.json_comma_newline();

	state.json_write_string("description");
	state.json_write_string(description != NULL ? description : fname);
	state.json_comma_newline();

	state.json_write_string("version");
	state.json_write_string("2");
	state.json_comma_newline();

	state.json_write_string("type");
	state.json_write_string("overlay");
	state.json_comma_newline();

	if (attribution != NULL) {
		state.json_write_string("attribution");
		state.json_write_string(attribution);
		state.json_comma_newline();
	}

	std::string version = program + " " + VERSION;
	state.json_write_string("generator");
	state.json_write_string(version);
	state.json_comma_newline();

	state.json_write_string("generator_options");
	state.json_write_string(commandline);
	state.json_comma_newline();

	if (vector) {
		size_t elements = max_tilestats_values;

		{
			state.json_write_string("vector_layers");
			state.json_write_array();

			std::vector<std::string> lnames;
			for (auto ai = layermap.begin(); ai != layermap.end(); ++ai) {
				lnames.push_back(ai->first);
			}

			for (size_t i = 0; i < lnames.size(); i++) {
				auto fk = layermap.find(lnames[i]);
				state.json_write_hash();

				state.json_write_string("id");
				state.json_write_string(lnames[i]);

				state.json_write_string("description");
				state.json_write_string(fk->second.description);

				state.json_write_string("minzoom");
				state.json_write_signed(fk->second.minzoom);

				state.json_write_string("maxzoom");
				state.json_write_signed(fk->second.maxzoom);

				state.json_write_string("fields");
				state.json_write_hash();
				state.nospace = true;

				bool first = true;
				for (auto j = fk->second.file_keys.begin(); j != fk->second.file_keys.end(); ++j) {
					if (first) {
						first = false;
					}

					state.json_write_string(j->first);

					auto f = attribute_descriptions.find(j->first);
					if (f == attribute_descriptions.end()) {
						int type = 0;
						for (auto s : j->second.sample_values) {
							type |= (1 << s.type);
						}

						if (type == (1 << mvt_double)) {
							state.json_write_string("Number");
						} else if (type == (1 << mvt_bool)) {
							state.json_write_string("Boolean");
						} else if (type == (1 << mvt_string)) {
							state.json_write_string("String");
						} else {
							state.json_write_string("Mixed");
						}
					} else {
						state.json_write_string(f->second);
					}
				}

				state.nospace = true;
				state.json_end_hash();
				state.json_end_hash();
			}

			state.json_end_array();

			if (do_tilestats && elements > 0) {
				state.nospace = true;
				state.json_write_string("tilestats");
				tilestats(layermap, elements, state);
			}
		}
	}

	state.nospace = true;
	state.json_end_hash();

	std::string compressed;
	compress(buf, compressed);
	return compressed;
}

void pmtiles_write_metadata(pmtiles_file *outfile, const char *fname, int minzoom, int maxzoom, double minlat, double minlon, double maxlat, double maxlon, double midlat, double midlon, const char *attribution, std::map<std::string, layermap_entry> const &layermap, bool vector, const char *description, bool do_tilestats, std::map<std::string, std::string> const &attribute_descriptions, std::string const &program, std::string const &commandline) {
	// set header fields
	outfile->json_metadata = pmtiles_metadata_json(fname, attribution, layermap, vector, description, do_tilestats, attribute_descriptions, program, commandline);

	pmtiles::headerv3 *header = &outfile->header;

	header->clustered = 0x0;
	header->internal_compression = 0x2;  // gzip
	header->tile_compression = 0x2;	     // gzip
	header->tile_type = 0x1;	     // mvt
	header->min_zoom = minzoom;
	header->max_zoom = maxzoom;
	header->min_lon_e7 = minlon * 10000000;
	header->min_lat_e7 = minlat * 10000000;
	header->max_lon_e7 = maxlon * 10000000;
	header->max_lat_e7 = maxlat * 10000000;
	header->center_zoom = minzoom;	// can be improved
	header->center_lon_e7 = midlon * 10000000;
	header->center_lat_e7 = midlat * 10000000;
}

std::tuple<std::string, std::string, int> build_root_leaves(const std::vector<pmtiles::entryv3> &entries, int leaf_size) {
	std::vector<pmtiles::entryv3> root_entries;
	std::string leaves_bytes;
	int num_leaves = 0;
	for (size_t i = 0; i <= entries.size(); i += leaf_size) {
		num_leaves++;
		int end = i + leaf_size;
		if (i + leaf_size > entries.size()) {
			end = entries.size();
		}
		std::vector<pmtiles::entryv3> subentries = {entries.begin() + i, entries.begin() + end};
		auto uncompressed_leaf = pmtiles::serialize_directory(subentries);
		std::string compressed_leaf;
		compress(uncompressed_leaf, compressed_leaf);
		root_entries.emplace_back(entries[i].tile_id, leaves_bytes.size(), compressed_leaf.size(), 0);
		leaves_bytes += compressed_leaf;
	}
	auto uncompressed_root = pmtiles::serialize_directory(root_entries);
	std::string compressed_root;
	compress(uncompressed_root, compressed_root);
	return std::make_tuple(compressed_root, leaves_bytes, num_leaves);
}

std::tuple<std::string, std::string, int> make_root_leaves(const std::vector<pmtiles::entryv3> &entries) {
	auto test_bytes = pmtiles::serialize_directory(entries);
	std::string compressed;
	compress(test_bytes, compressed);
	if (compressed.size() <= 16384 - 127) {
		return std::make_tuple(compressed, "", 0);
	}
	int leaf_size = 4096;
	while (true) {
		std::string root_bytes;
		std::string leaves_bytes;
		int num_leaves;
		std::tie(root_bytes, leaves_bytes, num_leaves) = build_root_leaves(entries, leaf_size);
		if (root_bytes.length() < 16384 - 127) {
			return std::make_tuple(root_bytes, leaves_bytes, num_leaves);
		}
		leaf_size *= 2;
	}
}

void pmtiles_finalize(pmtiles_file *outfile) {
	outfile->tmp_ostream.close();

	std::sort(outfile->entries.begin(), outfile->entries.end(), pmtiles::entryv3_cmp);

	std::string root_bytes;
	std::string leaves_bytes;
	int num_leaves;
	std::tie(root_bytes, leaves_bytes, num_leaves) = make_root_leaves(outfile->entries);

	pmtiles::headerv3 *header = &outfile->header;

	header->root_dir_offset = 127;
	header->root_dir_bytes = root_bytes.size();

	header->json_metadata_offset = header->root_dir_offset + header->root_dir_bytes;
	header->json_metadata_bytes = outfile->json_metadata.size();
	header->leaf_dirs_offset = header->json_metadata_offset + header->json_metadata_bytes;
	header->leaf_dirs_bytes = leaves_bytes.size();
	header->tile_data_offset = header->leaf_dirs_offset + header->leaf_dirs_bytes;
	header->tile_data_bytes = outfile->offset;

	header->addressed_tiles_count = outfile->entries.size();
	header->tile_entries_count = outfile->entries.size();
	header->tile_contents_count = outfile->entries.size();

	std::ifstream tmp_istream(outfile->tmp_name, std::ios::in | std::ios_base::binary);

	auto header_str = header->serialize();
	outfile->ostream.write(header_str.data(), header_str.length());
	outfile->ostream.write(root_bytes.data(), root_bytes.length());
	outfile->ostream.write(outfile->json_metadata.data(), outfile->json_metadata.size());
	outfile->ostream.write(leaves_bytes.data(), leaves_bytes.length());
	outfile->ostream << tmp_istream.rdbuf();

	tmp_istream.close();
	unlink(outfile->tmp_name.c_str());

	outfile->ostream.close();

	delete outfile;
};

void collect_tile_entries(std::vector<pmtiles_zxy_entry> &tile_entries, const char *pmtiles_map, uint64_t dir_offset, uint64_t dir_len, uint64_t leaf_offset) {
	std::string dir_s{pmtiles_map + dir_offset, dir_len};
	std::string decompressed_dir;
	decompress(dir_s, decompressed_dir);
	auto dir_entries = pmtiles::deserialize_directory(decompressed_dir);
	for (auto const &entry : dir_entries) {
		if (entry.run_length == 0) {
			collect_tile_entries(tile_entries, pmtiles_map, leaf_offset + entry.offset, leaf_offset + entry.length, leaf_offset);
		} else {
			for (uint64_t i = entry.tile_id; i < entry.tile_id + entry.run_length; i++) {
				pmtiles::zxy zxy = pmtiles::tileid_to_zxy(entry.tile_id);
				tile_entries.emplace_back(zxy.z, zxy.x, zxy.y, entry.offset, entry.length);
			}
		}
	}
}

struct {
	bool operator()(pmtiles_zxy_entry a, pmtiles_zxy_entry b) const {
		if (a.z != b.z) {
			return a.z < b.z;
		}
		if (a.x != b.x) {
			return a.x < b.x;
		}
		return a.y < b.y;
	}
} colmajor_cmp;

std::vector<pmtiles_zxy_entry> pmtiles_entries_colmajor(const char *pmtiles_map) {
	std::string header_s{pmtiles_map, 127};
	auto header = pmtiles::deserialize_header(header_s);

	std::vector<pmtiles_zxy_entry> tile_entries;

	collect_tile_entries(tile_entries, pmtiles_map, header.root_dir_offset, header.root_dir_bytes, header.leaf_dirs_offset);

	std::sort(tile_entries.begin(), tile_entries.end(), colmajor_cmp);

	return tile_entries;
}
