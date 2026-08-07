#include "mlt.hpp"
#include "errors.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef NO_MLT
#include <mlt/encoder.hpp>

using Vertex = mlt::Encoder::Vertex;
#endif

int output_format = OUTPUT_MVT;
bool mlt_sort_features = true;
bool mlt_pretessellate = false;

void set_output_format(char **argv, const char *format) {
	if (strcmp(format, "mvt") == 0 || strcmp(format, "pbf") == 0) {
		output_format = OUTPUT_MVT;
	} else if (strcmp(format, "mlt") == 0) {
#ifdef NO_MLT
		fprintf(stderr, "%s: this build was compiled without MapLibre Tile support\n", argv[0]);
		exit(EXIT_ARGS);
#else
		output_format = OUTPUT_MLT;
#endif
	} else {
		fprintf(stderr, "%s: --output-format must be 'mvt' or 'mlt'\n", argv[0]);
		exit(EXIT_ARGS);
	}
}

const char *tile_format_name(int format) {
	return (format == OUTPUT_MLT) ? "mlt" : "pbf";
}

const char *tile_format_extension(int format) {
	return (format == OUTPUT_MLT) ? ".mlt" : ".pbf";
}

std::string encode_tile(mvt_tile &tile, int format) {
#ifndef NO_MLT
	if (format == OUTPUT_MLT) {
		return encode_as_mlt(tile, mlt_sort_features, mlt_pretessellate);
	}
#else
	(void) format;
#endif

	return tile.encode();
}

#ifndef NO_MLT

// An MLT property column has a single type for the whole layer, while MVT
// values each carry their own type, so a type that can hold every value of
// an attribute has to be chosen before the layer can be converted.

enum mlt_column_type {
	MLT_COLUMN_BOOL,
	MLT_COLUMN_INT32,
	MLT_COLUMN_INT64,
	MLT_COLUMN_UINT32,
	MLT_COLUMN_UINT64,
	MLT_COLUMN_DOUBLE,
	MLT_COLUMN_STRING,
};

struct column_summary {
	bool has_bool = false;
	bool has_string = false;
	bool has_floating = false;
	bool has_signed = false;
	bool has_unsigned = false;
	long long min_signed = 0;
	long long max_signed = 0;
	unsigned long long max_unsigned = 0;

	void add(const mvt_value &val) {
		switch (val.type) {
		case mvt_bool:
			has_bool = true;
			break;

		case mvt_float:
		case mvt_double:
			has_floating = true;
			break;

		case mvt_int:
		case mvt_sint: {
			long long v = (val.type == mvt_int) ? val.numeric_value.int_value : val.numeric_value.sint_value;
			if (!has_signed) {
				min_signed = max_signed = v;
				has_signed = true;
			} else {
				min_signed = std::min(min_signed, v);
				max_signed = std::max(max_signed, v);
			}
			break;
		}

		case mvt_uint:
			has_unsigned = true;
			max_unsigned = std::max(max_unsigned, val.numeric_value.uint_value);
			break;

		default:
			has_string = true;
			break;
		}
	}

	mlt_column_type resolve() const {
		if (has_string) {
			return MLT_COLUMN_STRING;
		}

		bool has_number = has_floating || has_signed || has_unsigned;
		if (has_bool) {
			// A column of booleans and numbers has no common numeric type
			return has_number ? MLT_COLUMN_STRING : MLT_COLUMN_BOOL;
		}
		if (has_floating) {
			return MLT_COLUMN_DOUBLE;
		}

		if (has_unsigned && !has_signed) {
			return max_unsigned <= UINT32_MAX ? MLT_COLUMN_UINT32 : MLT_COLUMN_UINT64;
		}
		if (has_signed && !has_unsigned) {
			return (min_signed >= INT32_MIN && max_signed <= INT32_MAX) ? MLT_COLUMN_INT32 : MLT_COLUMN_INT64;
		}
		if (has_signed && has_unsigned) {
			if (max_unsigned > (unsigned long long) INT64_MAX) {
				// Too wide for any integer type that can also hold the signed values
				return MLT_COLUMN_DOUBLE;
			}
			return MLT_COLUMN_INT64;
		}

		return MLT_COLUMN_STRING;
	}
};

static mlt::Encoder::PropertyValue convert_value(const mvt_value &val, mlt_column_type type) {
	switch (type) {
	case MLT_COLUMN_BOOL:
		return val.numeric_value.bool_value;
	case MLT_COLUMN_INT32:
		return static_cast<std::int32_t>(mvt_value_to_long_long(val));
	case MLT_COLUMN_INT64:
		return static_cast<std::int64_t>(mvt_value_to_long_long(val));
	case MLT_COLUMN_UINT32:
		return static_cast<std::uint32_t>(val.numeric_value.uint_value);
	case MLT_COLUMN_UINT64:
		return static_cast<std::uint64_t>(val.numeric_value.uint_value);
	case MLT_COLUMN_DOUBLE:
		return val.to_double();
	case MLT_COLUMN_STRING:
	default:
		// Nested JSON objects stay JSON text, the way MVT carries them,
		// because MLT struct columns can only hold strings and are
		// flattened into their parent column name when decoded.
		return val.get_string_value();
	}
}

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
		std::vector<std::vector<std::vector<Vertex>>> polygons;

		for (auto &ring : rings) {
			long long area2 = 0;
			for (size_t i = 0; i < ring.size(); i++) {
				size_t j = (i + 1) % ring.size();
				area2 += (long long) ring[i].x * ring[j].y - (long long) ring[j].x * ring[i].y;
			}

			if (area2 >= 0) {
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

	std::vector<column_summary> summaries(layer.keys.size());
	for (const auto &feature : layer.features) {
		for (size_t t = 0; t + 1 < feature.tags.size(); t += 2) {
			unsigned key_idx = feature.tags[t];
			unsigned val_idx = feature.tags[t + 1];
			if (key_idx < layer.keys.size() && val_idx < layer.values.size()) {
				const auto &val = layer.values[val_idx];
				if (val.type != mvt_null) {
					summaries[key_idx].add(val);
				}
			}
		}
	}

	std::vector<mlt_column_type> types(layer.keys.size(), MLT_COLUMN_STRING);
	for (size_t i = 0; i < summaries.size(); i++) {
		types[i] = summaries[i].resolve();
	}

	for (const auto &feature : layer.features) {
		mlt::Encoder::Feature f;
		if (feature.has_id) {
			f.id = feature.id;
		} else {
			f.id = std::nullopt;
		}
		f.geometry = convert_geometry(feature);

		for (size_t t = 0; t + 1 < feature.tags.size(); t += 2) {
			unsigned key_idx = feature.tags[t];
			unsigned val_idx = feature.tags[t + 1];
			if (key_idx < layer.keys.size() && val_idx < layer.values.size()) {
				const auto &val = layer.values[val_idx];
				if (val.type != mvt_null) {
					f.properties[layer.keys[key_idx]] = convert_value(val, types[key_idx]);
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
		if (any_has_id) {
			break;
		}
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

#endif
