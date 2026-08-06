#ifndef MLT_HPP
#define MLT_HPP

#include <string>
#include "mvt.hpp"

std::string encode_as_mlt(const mvt_tile &tile, bool sort_features, bool pretessellate);

// Does this (already decompressed) tile look like MapLibre Tile rather than
// Mapbox Vector Tile data?
bool is_mlt(const std::string &message);

// Decode a MapLibre Tile into the equivalent vector tile, returning false
// (and complaining to stderr) if it can't be parsed.
bool decode_mlt(const std::string &message, mvt_tile &out);

#endif
