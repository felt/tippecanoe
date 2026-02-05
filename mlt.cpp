#include "mlt.hpp"
#include "jsonpull/jsonpull.h"

#include <mlt/encoder.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using Vertex = mlt::Encoder::Vertex;

// Try to parse a JSON object string into MLT STRUCT (flat string children only)
static bool try_parse_json_object(const std::string &s, mlt::Encoder::StructValue &out) {
	if (s.empty() || s[0] != '{') {
		return false;
	}

	json_pull *jp = json_begin_string(s.c_str());
	json_object *obj = json_read_tree(jp);

	if (obj == nullptr || obj->type != JSON_HASH) {
		json_free(obj);
		json_end(jp);
		return false;
	}

	for (size_t i = 0; i < obj->value.object.length; i++) {
		json_object *key = obj->value.object.keys[i];
		json_object *val = obj->value.object.values[i];

		if (key->type != JSON_STRING) continue;

		std::string child_key = key->value.string.string;
		std::string child_val;

		switch (val->type) {
		case JSON_STRING:
			child_val = val->value.string.string;
			break;
		case JSON_NUMBER:
			if (val->value.number.large_unsigned != 0) {
				child_val = std::to_string(val->value.number.large_unsigned);
			} else if (val->value.number.large_signed != 0) {
				child_val = std::to_string(val->value.number.large_signed);
			} else {
				child_val = std::to_string(val->value.number.number);
			}
			break;
		case JSON_TRUE:
			child_val = "true";
			break;
		case JSON_FALSE:
			child_val = "false";
			break;
		case JSON_NULL:
			child_val = "null";
			break;
		default:
			// Nested object/array - stringify back
			char *nested = json_stringify(val);
			child_val = nested;
			free(nested);
			break;
		}

		out[child_key] = child_val;
	}

	json_free(obj);
	json_end(jp);
	return true;
}

static mlt::Encoder::PropertyValue convert_value(const mvt_value &val) {
	switch (val.type) {
	case mvt_bool:
		return val.numeric_value.bool_value;
	case mvt_int:
		if (val.numeric_value.int_value >= INT32_MIN && val.numeric_value.int_value <= INT32_MAX) {
			return static_cast<std::int32_t>(val.numeric_value.int_value);
		}
		return static_cast<std::int64_t>(val.numeric_value.int_value);
	case mvt_uint:
		if (val.numeric_value.uint_value <= UINT32_MAX) {
			return static_cast<std::uint32_t>(val.numeric_value.uint_value);
		}
		return static_cast<std::uint64_t>(val.numeric_value.uint_value);
	case mvt_sint:
		if (val.numeric_value.sint_value >= INT32_MIN && val.numeric_value.sint_value <= INT32_MAX) {
			return static_cast<std::int32_t>(val.numeric_value.sint_value);
		}
		return static_cast<std::int64_t>(val.numeric_value.sint_value);
	case mvt_float:
		return val.numeric_value.float_value;
	case mvt_double:
		return val.numeric_value.double_value;
	case mvt_string: {
		std::string s = val.get_string_value();
		mlt::Encoder::StructValue struct_val;
		if (try_parse_json_object(s, struct_val)) {
			return struct_val;
		}
		return s;
	}
	default:
		return std::string{};
	}
}

// Split MVT command stream into coordinate rings (sequences between moveto commands).
// Each ring is a vector of vertices. For polygons, closepath is implicit (MLT strips closing points).
static std::vector<std::vector<Vertex>> extract_rings(const mvt_feature &feature) {
	std::vector<std::vector<Vertex>> rings;

	for (size_t i = 0; i < feature.geometry.size(); i++) {
		const auto &g = feature.geometry[i];
		if (g.op == mvt_moveto) {
			rings.emplace_back();
			rings.back().push_back({static_cast<int32_t>(g.x), static_cast<int32_t>(g.y)});
		} else if (g.op == mvt_lineto) {
			rings.back().push_back({static_cast<int32_t>(g.x), static_cast<int32_t>(g.y)});
		}
		// mvt_closepath: polygon ring close — MLT stores without closing point
	}
	return rings;
}

static mlt::Encoder::Geometry convert_geometry(const mvt_feature &feature) {
	mlt::Encoder::Geometry geom;

	auto rings = extract_rings(feature);

	switch (feature.type) {
	case mvt_point:
		if (rings.size() == 1 && rings[0].size() == 1) {
			geom.type = mlt::Encoder::GeometryType::POINT;
			geom.coordinates = std::move(rings[0]);
		} else {
			geom.type = mlt::Encoder::GeometryType::MULTIPOINT;
			for (auto &ring : rings) {
				for (auto &v : ring) {
					geom.coordinates.push_back(v);
				}
			}
		}
		break;

	case mvt_linestring:
		if (rings.size() == 1) {
			geom.type = mlt::Encoder::GeometryType::LINESTRING;
			geom.coordinates = std::move(rings[0]);
		} else {
			geom.type = mlt::Encoder::GeometryType::MULTILINESTRING;
			geom.parts = std::move(rings);
		}
		break;

	case mvt_polygon: {
		// Outer rings are clockwise (positive area), holes are counter-clockwise.
		// Group into polygons: each outer ring starts a new polygon.
		std::vector<std::vector<std::vector<Vertex>>> polygons;

		for (auto &ring : rings) {
			// Signed area to detect winding: positive = clockwise = outer ring (in MVT screen coords)
			long long area2 = 0;
			for (size_t i = 0; i < ring.size(); i++) {
				size_t j = (i + 1) % ring.size();
				area2 += (long long) ring[i].x * ring[j].y - (long long) ring[j].x * ring[i].y;
			}

			if (area2 >= 0) {
				// Outer ring — start new polygon
				polygons.emplace_back();
			}
			if (!polygons.empty()) {
				polygons.back().push_back(std::move(ring));
			}
		}

		if (polygons.size() == 1) {
			geom.type = mlt::Encoder::GeometryType::POLYGON;
			for (auto &ring : polygons[0]) {
				geom.ringSizes.push_back(static_cast<uint32_t>(ring.size()));
				geom.coordinates.insert(geom.coordinates.end(), ring.begin(), ring.end());
			}
		} else {
			geom.type = mlt::Encoder::GeometryType::MULTIPOLYGON;
			for (auto &poly : polygons) {
				std::vector<Vertex> part_verts;
				std::vector<uint32_t> part_rings;
				for (auto &ring : poly) {
					part_rings.push_back(static_cast<uint32_t>(ring.size()));
					part_verts.insert(part_verts.end(), ring.begin(), ring.end());
				}
				geom.parts.push_back(std::move(part_verts));
				geom.partRingSizes.push_back(std::move(part_rings));
			}
		}
		break;
	}
	}

	return geom;
}

static mlt::Encoder::Layer convert_layer(const mvt_layer &layer) {
	mlt::Encoder::Layer out;
	out.name = layer.name;
	out.extent = static_cast<uint32_t>(layer.extent);

	for (const auto &feature : layer.features) {
		mlt::Encoder::Feature f;
		f.id = feature.id;
		f.geometry = convert_geometry(feature);

		for (size_t t = 0; t + 1 < feature.tags.size(); t += 2) {
			unsigned key_idx = feature.tags[t];
			unsigned val_idx = feature.tags[t + 1];
			if (key_idx < layer.keys.size() && val_idx < layer.values.size()) {
				const auto &val = layer.values[val_idx];
				if (val.type != mvt_null) {
					f.properties[layer.keys[key_idx]] = convert_value(val);
				}
			}
		}

		out.features.push_back(std::move(f));
	}

	return out;
}

std::string encode_as_mlt(const mvt_tile &tile, bool sort_features, bool pretessellate) {
	mlt::Encoder encoder;
	mlt::EncoderConfig config;
	config.sortFeatures = sort_features;
	config.preTessellate = pretessellate;

	bool any_has_id = false;
	for (const auto &layer : tile.layers) {
		for (const auto &feature : layer.features) {
			if (feature.has_id) {
				any_has_id = true;
				break;
			}
		}
		if (any_has_id) break;
	}
	config.includeIds = any_has_id;

	std::vector<mlt::Encoder::Layer> layers;
	layers.reserve(tile.layers.size());
	for (const auto &layer : tile.layers) {
		layers.push_back(convert_layer(layer));
	}

	auto bytes = encoder.encode(layers, config);
	return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
