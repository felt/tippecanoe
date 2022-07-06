#ifndef PMTILES_HPP
#define PMTILES_HPP

#include <vector>
#include <fstream>
#include <map>

struct pmtiles_file {
	uint64_t offset = 0;
	std::string tmptilesname;
	std::ofstream ostream;
	std::ofstream tilestmp;
	std::string json_metadata;
};

#include "mbtiles.hpp"

pmtiles_file *pmtiles_open(const char *filename, char **argv, int force, const char *tmpdir);

void pmtiles_write_tile(pmtiles_file *outfile, int z, int tx, int ty, const char *data, int size);

void pmtiles_write_metadata(pmtiles_file *outfile, const char *fname, int minzoom, int maxzoom, double minlat, double minlon, double maxlat, double maxlon, double midlat, double midlon, const char *attribution, std::map<std::string, layermap_entry> const &layermap, bool vector, const char *description, bool do_tilestats, std::map<std::string, std::string> const &attribute_descriptions, std::string const &program, std::string const &commandline);

void pmtiles_finalize(pmtiles_file *outfile);

#endif