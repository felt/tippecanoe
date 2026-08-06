#ifndef MLT_HPP
#define MLT_HPP

#include <string>
#include "mvt.hpp"

// The tile format to write, and the MLT encoder options to write it with,
// as selected on the command line.
extern int output_format;
extern bool mlt_sort_features;
extern bool mlt_pretessellate;

// Set output_format from an --output-format argument, complaining and
// exiting if it names a format we can't write.
void set_output_format(char **argv, const char *format);

// What this tile format is called in tileset metadata
const char *tile_format_name(int format);

// What tiles of this format are named in a tile directory
const char *tile_format_extension(int format);

// Encode a tile in the specified format, using whichever MLT encoder
// options were selected on the command line
std::string encode_tile(mvt_tile &tile, int format);

#ifndef NO_MLT
std::string encode_as_mlt(const mvt_tile &tile, bool sort_features, bool pretessellate);
#endif

// Does this (already decompressed) tile look like MapLibre Tile rather than
// Mapbox Vector Tile data?
bool is_mlt(const std::string &message);

// Decode a MapLibre Tile into the equivalent vector tile, returning false
// (and complaining to stderr) if it can't be parsed, or if this build
// doesn't have MLT support compiled in.
bool decode_mlt(const std::string &message, mvt_tile &out);

#endif
