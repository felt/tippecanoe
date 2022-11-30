#ifndef PMTILES_FILE_HPP
#define PMTILES_FILE_HPP

#include <vector>
#include <fstream>
#include <map>
#include "pmtiles/pmtiles.hpp"

struct pmtiles_file {
	uint64_t offset = 0;
	std::ofstream ostream;
	std::ofstream tmp_ostream;
	std::string tmp_name;
	std::string json_metadata;
	pthread_mutex_t lock;
	std::vector<pmtiles::entryv3> entries;
	pmtiles::headerv3 header;
};

struct pmtiles_zxy_entry {
	long long z;
	long long x;
	long long y;
	uint64_t offset;
	uint32_t length;

	pmtiles_zxy_entry(long long _z, long long _x, long long _y, uint64_t _offset, uint32_t _length)
	    : z(_z), x(_x), y(_y), offset(_offset), length(_length) {
	}
};

#include "mbtiles.hpp"

bool pmtiles_has_suffix(const char *filename);

pmtiles_file *pmtiles_open(const char *filename, char **argv, int force);

void pmtiles_write_tile(pmtiles_file *outfile, int z, int tx, int ty, const char *data, int size);

void pmtiles_write_metadata(pmtiles_file *outfile, const char *fname, int minzoom, int maxzoom, double minlat, double minlon, double maxlat, double maxlon, double midlat, double midlon, const char *attribution, std::map<std::string, layermap_entry> const &layermap, bool vector, const char *description, bool do_tilestats, std::map<std::string, std::string> const &attribute_descriptions, std::string const &program, std::string const &commandline);

void pmtiles_finalize(pmtiles_file *outfile);

std::vector<pmtiles_zxy_entry> pmtiles_entries_colmajor(const char *pmtiles_map);

#endif