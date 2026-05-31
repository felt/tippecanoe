#ifndef EVALUATOR_HPP
#define EVALUATOR HPP

#include <unordered_map>
#include <string>
#include <set>
#include "jsonpull/jsonpull.h"
#include "mvt.hpp"

// The `filter` parameters take a borrowed pointer; the caller (in
// main.cpp, tile-join, overzoom) keeps the json_object_ptr alive
// across every per-feature evaluate() call. A raw pointer avoids
// touching unique_ptr at all on this hot path.
bool evaluate(std::unordered_map<std::string, mvt_value> const &feature, std::string const &layer, json_object *filter, std::set<std::string> &exclude_attributes, std::vector<std::string> const &unidecode_data);
json_object_ptr parse_filter(const char *s);
json_object_ptr read_filter(const char *fname);

bool evaluate(mvt_feature const &feat, mvt_layer const &layer, json_object *filter, std::set<std::string> &exclude_attributes, int z, std::vector<std::string> const &unidecode_data);

#endif
