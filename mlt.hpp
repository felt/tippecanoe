#ifndef MLT_HPP
#define MLT_HPP

#include <string>
#include "mvt.hpp"

std::string encode_as_mlt(const mvt_tile &tile, bool sort_features, bool pretessellate);

#endif
