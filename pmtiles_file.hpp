#ifndef PMTILES_FILE_HPP
#define PMTILES_FILE_HPP

#include "pmtiles/pmtiles.hpp"
#include "mbtiles.hpp"

bool pmtiles_has_suffix(const char *filename);
void check_pmtiles(const char *filename, char **argv, bool forcetable);

void mbtiles_map_image_to_pmtiles(char *dbname, metadata m, bool tile_compression, bool quiet, bool quiet_progress);

std::vector<pmtiles::entry_zxy> pmtiles_entries_tms(const char *pmtiles_map, int minzoom, int maxzoom);
std::pair<uint64_t, uint32_t> pmtiles_get_tile(const char *pmtiles_map, int z, int x, int y);
sqlite3 *pmtilesmeta2tmp(const char *fname, const char *pmtiles_map);

#endif
