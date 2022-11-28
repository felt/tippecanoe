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
	if (strncmp(filename+(lenstr-8),".pmtiles",8) == 0) {
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
	outfile->ostream.open(filename,std::ios::out | std::ios::binary);
	outfile->tmp_name = std::string(filename) + ".tmp";
	outfile->tmp_ostream.open(outfile->tmp_name,std::ios::out | std::ios::binary);
	outfile->offset = 0;

	outfile->lock = PTHREAD_MUTEX_INITIALIZER;
	return outfile;
}

void pmtiles_write_tile(pmtiles_file  *outfile, int z, int tx, int ty, const char *data, int size) {
	if (pthread_mutex_lock(&outfile->lock) != 0) {
		perror("pthread_mutex_lock");
		exit(EXIT_PTHREAD);
	}

	fprintf(stderr, "%d %d %d\n", z, tx, ty);
	// TODO: add entry to entries
	outfile->tmp_ostream.write(data, size);
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
	compress(buf,compressed);
	return compressed;
}

void pmtiles_write_metadata(pmtiles_file *outfile, const char *fname, int minzoom, int maxzoom, double minlat, double minlon, double maxlat, double maxlon, double midlat, double midlon, const char *attribution, std::map<std::string, layermap_entry> const &layermap, bool vector, const char *description, bool do_tilestats, std::map<std::string, std::string> const &attribute_descriptions, std::string const &program, std::string const &commandline) {
	// set header fields
	outfile->json_metadata = pmtiles_metadata_json(fname, attribution, layermap, vector, description, do_tilestats, attribute_descriptions, program, commandline);

	fprintf(stderr, "%d %d %f %f %f %f %f %f\n", minzoom, maxzoom, minlon, minlat, maxlon, maxlat, midlon, midlat);
}

void pmtiles_finalize(pmtiles_file *outfile) {
	outfile->tmp_ostream.close();

	// TODO: serialize sorted directories and set header fields

	std::ifstream tmp_istream(outfile->tmp_name, std::ios::in | std::ios_base::binary);

	outfile->ostream.write(outfile->json_metadata.data(), outfile->json_metadata.size());
	// TODO: write leaf dirs
	outfile->ostream << tmp_istream.rdbuf();

	tmp_istream.close();
	unlink(outfile->tmp_name.c_str());

	outfile->ostream.close();

	delete outfile;
};