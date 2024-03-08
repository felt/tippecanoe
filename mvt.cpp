#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <zlib.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include "mvt.hpp"
#include "geometry.hpp"
#include "protozero/varint.hpp"
#include "protozero/pbf_reader.hpp"
#include "protozero/pbf_writer.hpp"
#include "milo/dtoa_milo.h"
#include "errors.hpp"
#include "serial.hpp"
#include "text.hpp"

mvt_geometry::mvt_geometry(int nop, long long nx, long long ny) {
	this->op = nop;
	this->x = nx;
	this->y = ny;
}

// https://github.com/mapbox/mapnik-vector-tile/blob/master/src/vector_tile_compression.hpp
bool is_compressed(std::string const &data) {
	return data.size() > 2 && (((uint8_t) data[0] == 0x78 && (uint8_t) data[1] == 0x9C) || ((uint8_t) data[0] == 0x1F && (uint8_t) data[1] == 0x8B));
}

// https://github.com/mapbox/mapnik-vector-tile/blob/master/src/vector_tile_compression.hpp
int decompress(std::string const &input, std::string &output) {
	z_stream inflate_s;
	inflate_s.zalloc = Z_NULL;
	inflate_s.zfree = Z_NULL;
	inflate_s.opaque = Z_NULL;
	inflate_s.avail_in = 0;
	inflate_s.next_in = Z_NULL;
	if (inflateInit2(&inflate_s, 32 + 15) != Z_OK) {
		fprintf(stderr, "Decompression error: %s\n", inflate_s.msg);
	}
	inflate_s.next_in = (Bytef *) input.data();
	inflate_s.avail_in = input.size();
	inflate_s.next_out = (Bytef *) output.data();
	inflate_s.avail_out = output.size();

	while (true) {
		size_t existing_output = inflate_s.next_out - (Bytef *) output.data();

		output.resize(existing_output + 2 * inflate_s.avail_in + 100);
		inflate_s.next_out = (Bytef *) output.data() + existing_output;
		inflate_s.avail_out = output.size() - existing_output;

		int ret = inflate(&inflate_s, 0);
		if (ret < 0) {
			fprintf(stderr, "Decompression error: ");
			if (ret == Z_DATA_ERROR) {
				fprintf(stderr, "data error");
			}
			if (ret == Z_STREAM_ERROR) {
				fprintf(stderr, "stream error");
			}
			if (ret == Z_MEM_ERROR) {
				fprintf(stderr, "out of memory");
			}
			if (ret == Z_BUF_ERROR) {
				fprintf(stderr, "no data in buffer");
			}
			fprintf(stderr, "\n");
			return 0;
		}

		if (ret == Z_STREAM_END) {
			break;
		}

		// ret must be Z_OK or Z_NEED_DICT;
		// continue decompresing
	}

	output.resize(inflate_s.next_out - (Bytef *) output.data());
	inflateEnd(&inflate_s);
	return 1;
}

// https://github.com/mapbox/mapnik-vector-tile/blob/master/src/vector_tile_compression.hpp
int compress(std::string const &input, std::string &output, bool gz) {
	z_stream deflate_s;
	deflate_s.zalloc = Z_NULL;
	deflate_s.zfree = Z_NULL;
	deflate_s.opaque = Z_NULL;
	deflate_s.avail_in = 0;
	deflate_s.next_in = Z_NULL;
	deflateInit2(&deflate_s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, gz ? 31 : 15, 8, Z_DEFAULT_STRATEGY);
	deflate_s.next_in = (Bytef *) input.data();
	deflate_s.avail_in = input.size();
	size_t length = 0;
	do {
		size_t increase = input.size() / 2 + 1024;
		output.resize(length + increase);
		deflate_s.avail_out = increase;
		deflate_s.next_out = (Bytef *) (output.data() + length);
		int ret = deflate(&deflate_s, Z_FINISH);
		if (ret != Z_STREAM_END && ret != Z_OK && ret != Z_BUF_ERROR) {
			return -1;
		}
		length += (increase - deflate_s.avail_out);
	} while (deflate_s.avail_out == 0);
	deflateEnd(&deflate_s);
	output.resize(length);
	return 0;
}

bool mvt_tile::decode(const std::string &message, bool &was_compressed) {
	layers.clear();
	std::string src;

	if (is_compressed(message)) {
		std::string uncompressed;
		if (decompress(message, uncompressed) == 0) {
			exit(EXIT_MVT);
		}
		src = uncompressed;
		was_compressed = true;
	} else {
		src = message;
		was_compressed = false;
	}

	protozero::pbf_reader reader(src);
	std::shared_ptr<std::string> string_pool = std::make_shared<std::string>();

	while (reader.next()) {
		switch (reader.tag()) {
		case 3: /* layer */
		{
			protozero::pbf_reader layer_reader(reader.get_message());
			mvt_layer layer;

			while (layer_reader.next()) {
				switch (layer_reader.tag()) {
				case 1: /* name */
					layer.name = layer_reader.get_string();
					break;

				case 3: /* key */
					layer.keys.push_back(layer_reader.get_string());
					break;

				case 4: /* value */
				{
					protozero::pbf_reader value_reader(layer_reader.get_message());
					mvt_value value;

					value.type = mvt_null;
					value.numeric_value.null_value = 0;

					while (value_reader.next()) {
						switch (value_reader.tag()) {
						case 1: /* string */
							value.type = mvt_string;
							value.s = string_pool;
							{
								auto v = value_reader.get_view();
								std::string_view sv(v.data(), v.size());
								value.set_string_value(sv);
							}
							break;

						case 2: /* float */
							value.type = mvt_float;
							value.numeric_value.float_value = value_reader.get_float();
							break;

						case 3: /* double */
							value.type = mvt_double;
							value.numeric_value.double_value = value_reader.get_double();
							break;

						case 4: /* int */
							value.type = mvt_int;
							value.numeric_value.int_value = value_reader.get_int64();
							break;

						case 5: /* uint */
							value.type = mvt_uint;
							value.numeric_value.uint_value = value_reader.get_uint64();
							break;

						case 6: /* sint */
							value.type = mvt_sint;
							value.numeric_value.sint_value = value_reader.get_sint64();
							break;

						case 7: /* bool */
							value.type = mvt_bool;
							value.numeric_value.bool_value = value_reader.get_bool();
							break;

						default:
							value_reader.skip();
							break;
						}
					}

					layer.values.push_back(std::move(value));
					break;
				}

				case 5: /* extent */
					layer.extent = layer_reader.get_uint32();
					break;

				case 15: /* version */
					layer.version = layer_reader.get_uint32();
					break;

				case 2: /* feature */
				{
					protozero::pbf_reader feature_reader(layer_reader.get_message());
					mvt_feature feature;
					std::vector<uint32_t> geoms;

					while (feature_reader.next()) {
						switch (feature_reader.tag()) {
						case 1: /* id */
							feature.id = feature_reader.get_uint64();
							feature.has_id = true;
							break;

						case 2: /* tag */
						{
							auto pi = feature_reader.get_packed_uint32();
							feature.tags.reserve(std::distance(pi.first, pi.second));
							for (auto it = pi.first; it != pi.second; ++it) {
								feature.tags.push_back(*it);
							}
							break;
						}

						case 3: /* feature type */
							feature.type = feature_reader.get_enum();
							break;

						case 4: /* geometry */
						{
							auto pi = feature_reader.get_packed_uint32();
							geoms.reserve(std::distance(pi.first, pi.second));
							for (auto it = pi.first; it != pi.second; ++it) {
								geoms.push_back(*it);
							}
							break;
						}

						default:
							feature_reader.skip();
							break;
						}
					}

					long long px = 0, py = 0;
					feature.geometry.reserve(geoms.size());	 // probably not quite right, but still plausible
					for (size_t g = 0; g < geoms.size(); g++) {
						uint32_t geom = geoms[g];
						uint32_t op = geom & 7;
						uint32_t count = geom >> 3;

						if (op == mvt_moveto || op == mvt_lineto) {
							for (size_t k = 0; k < count && g + 2 < geoms.size(); k++) {
								px += protozero::decode_zigzag32(geoms[g + 1]);
								py += protozero::decode_zigzag32(geoms[g + 2]);
								g += 2;

								feature.geometry.emplace_back(op, px, py);
							}
						} else {
							feature.geometry.emplace_back(op, 0, 0);
						}
					}

					layer.features.push_back(std::move(feature));
					break;
				}

				default:
					layer_reader.skip();
					break;
				}
			}

			layers.push_back(std::move(layer));
			break;
		}

		default:
			reader.skip();
			break;
		}
	}

	return true;
}

struct sorted_value {
	std::string val;
	size_t orig;

	bool operator<(const sorted_value &sv) const {
		if (val < sv.val) {
			return true;
		} else if (val == sv.val) {
			if (orig < sv.orig) {
				return true;
			}
		}

		return false;
	}
};

std::string mvt_tile::encode() {
	std::string data;
	protozero::pbf_writer writer(data);

	for (size_t i = 0; i < layers.size(); i++) {
		std::string layer_string;
		protozero::pbf_writer layer_writer(layer_string);

		layer_writer.add_uint32(15, layers[i].version); /* version */
		layer_writer.add_string(1, layers[i].name);	/* name */
		layer_writer.add_uint32(5, layers[i].extent);	/* extent */

		for (size_t j = 0; j < layers[i].keys.size(); j++) {
			layer_writer.add_string(3, layers[i].keys[j]); /* key */
		}

		std::vector<sorted_value> sorted_values;

		for (size_t v = 0; v < layers[i].values.size(); v++) {
			std::string value_string;
			protozero::pbf_writer value_writer(value_string);
			mvt_value &pbv = layers[i].values[v];

			switch (pbv.type) {
			case mvt_string:
				value_writer.add_string(1, pbv.get_string_value());
				break;
			case mvt_float:
				value_writer.add_float(2, pbv.numeric_value.float_value);
				break;
			case mvt_double:
				value_writer.add_double(3, pbv.numeric_value.double_value);
				break;
			case mvt_int:
				value_writer.add_int64(4, pbv.numeric_value.int_value);
				break;
			case mvt_uint:
				value_writer.add_uint64(5, pbv.numeric_value.uint_value);
				break;
			case mvt_sint:
				value_writer.add_sint64(6, pbv.numeric_value.sint_value);
				break;
			case mvt_bool:
				value_writer.add_bool(7, pbv.numeric_value.bool_value);
				break;
			case mvt_null:
				fprintf(stderr, "Internal error: trying to write null attribute to tile\n");
				exit(EXIT_IMPOSSIBLE);
			default:
				fprintf(stderr, "Internal error: trying to write undefined attribute type to tile\n");
				exit(EXIT_IMPOSSIBLE);
			}

			sorted_value sv;
			sv.val = std::move(value_string);
			sv.orig = v;
			sorted_values.push_back(std::move(sv));
		}

		std::stable_sort(sorted_values.begin(), sorted_values.end());
		std::vector<size_t> mapping;
		mapping.resize(sorted_values.size());

		size_t value_index = 0;
		for (size_t v = 0; v < sorted_values.size(); v++) {
			mapping[sorted_values[v].orig] = value_index;
			layer_writer.add_message(4, sorted_values[v].val);

			// crunch out duplicates that were missed by the hashing
			while (v + 1 < sorted_values.size() && sorted_values[v].val == sorted_values[v + 1].val) {
				mapping[sorted_values[v + 1].orig] = value_index;
				v++;
			}

			value_index++;
		}

		for (size_t f = 0; f < layers[i].features.size(); f++) {
			std::string feature_string;
			protozero::pbf_writer feature_writer(feature_string);

			feature_writer.add_enum(3, layers[i].features[f].type);

			std::vector<unsigned> sorted_tags = layers[i].features[f].tags;
			for (size_t v = 1; v < sorted_tags.size(); v += 2) {
				sorted_tags[v] = mapping[sorted_tags[v]];
			}
			feature_writer.add_packed_uint32(2, std::begin(sorted_tags), std::end(sorted_tags));

			if (layers[i].features[f].has_id) {
				feature_writer.add_uint64(1, layers[i].features[f].id);
			}

			std::vector<uint32_t> geometry;

			long long px = 0, py = 0;
			int cmd_idx = -1;
			int cmd = -1;
			int length = 0;

			std::vector<mvt_geometry> &geom = layers[i].features[f].geometry;

			for (size_t g = 0; g < geom.size(); g++) {
				int op = geom[g].op;

				if (op != cmd) {
					if (cmd_idx >= 0) {
						geometry[cmd_idx] = (length << 3) | (cmd & ((1 << 3) - 1));
					}

					cmd = op;
					length = 0;
					cmd_idx = geometry.size();
					geometry.push_back(0);
				}

				if (op == mvt_moveto || op == mvt_lineto) {
					long long wwx = geom[g].x;
					long long wwy = geom[g].y;

					long long dx = wwx - px;
					long long dy = wwy - py;

					if (dx < INT_MIN || dx > INT_MAX || dy < INT_MIN || dy > INT_MAX) {
						fprintf(stderr, "Internal error: Geometry delta is too big: %lld,%lld\n", dx, dy);
						exit(EXIT_IMPOSSIBLE);
					}

					geometry.push_back(protozero::encode_zigzag32(dx));
					geometry.push_back(protozero::encode_zigzag32(dy));

					px = wwx;
					py = wwy;
					length++;
				} else if (op == mvt_closepath) {
					length++;
				} else {
					fprintf(stderr, "\nInternal error: corrupted geometry\n");
					exit(EXIT_IMPOSSIBLE);
				}
			}

			if (cmd_idx >= 0) {
				geometry[cmd_idx] = (length << 3) | (cmd & ((1 << 3) - 1));
			}

			feature_writer.add_packed_uint32(4, std::begin(geometry), std::end(geometry));
			layer_writer.add_message(2, feature_string);
		}

		writer.add_message(3, layer_string);
	}

	return data;
}

bool mvt_value::operator<(const mvt_value &o) const {
	if (type < o.type) {
		return true;
	}
	if (type == o.type) {
		switch (type) {
		case mvt_string:
			return get_string_view() < o.get_string_view();

		case mvt_float:
			return numeric_value.float_value < o.numeric_value.float_value;

		case mvt_double:
			return numeric_value.double_value < o.numeric_value.double_value;

		case mvt_int:
			return numeric_value.int_value < o.numeric_value.int_value;

		case mvt_uint:
			return numeric_value.uint_value < o.numeric_value.uint_value;

		case mvt_sint:
			return numeric_value.sint_value < o.numeric_value.sint_value;

		case mvt_bool:
			return numeric_value.bool_value < o.numeric_value.bool_value;

		case mvt_null:
			return numeric_value.null_value < o.numeric_value.null_value;

		default:
			fprintf(stderr, "mvt_value::operator<<: can't happen\n");
			exit(EXIT_IMPOSSIBLE);
		}
	}

	return false;
}

bool mvt_value::operator==(const mvt_value &o) const {
	if (type == o.type) {
		switch (type) {
		case mvt_string:
			return get_string_view() == o.get_string_view();

		case mvt_float:
			return numeric_value.float_value == o.numeric_value.float_value;

		case mvt_double:
			return numeric_value.double_value == o.numeric_value.double_value;

		case mvt_int:
			return numeric_value.int_value == o.numeric_value.int_value;

		case mvt_uint:
			return numeric_value.uint_value == o.numeric_value.uint_value;

		case mvt_sint:
			return numeric_value.sint_value == o.numeric_value.sint_value;

		case mvt_bool:
			return numeric_value.bool_value == o.numeric_value.bool_value;

		case mvt_null:
			return numeric_value.null_value == o.numeric_value.null_value;

		default:
			fprintf(stderr, "mvt_value::operator==: can't happen\n");
			exit(EXIT_IMPOSSIBLE);
		}
	}

	return false;
}

static std::string quote(std::string const &s) {
	std::string buf;

	for (size_t i = 0; i < s.size(); i++) {
		unsigned char ch = s[i];

		if (ch == '\\' || ch == '\"') {
			buf.push_back('\\');
			buf.push_back(ch);
		} else if (ch < ' ') {
			char tmp[7];
			snprintf(tmp, sizeof(tmp), "\\u%04x", ch);
			buf.append(std::string(tmp));
		} else {
			buf.push_back(ch);
		}
	}

	return buf;
}

std::string mvt_value::toString() const {
	switch (type) {
	case mvt_string:
		return quote(get_string_value());
	case mvt_int:
		return std::to_string(numeric_value.int_value);
	case mvt_double: {
		double v = numeric_value.double_value;
		if (v == (long long) v) {
			return std::to_string((long long) v);
		} else {
			return milo::dtoa_milo(v);
		}
	}
	case mvt_float: {
		double v = numeric_value.float_value;
		if (v == (long long) v) {
			return std::to_string((long long) v);
		} else {
			return milo::dtoa_milo(v);
		}
	}
	case mvt_sint:
		return std::to_string(numeric_value.sint_value);
	case mvt_uint:
		return std::to_string(numeric_value.uint_value);
	case mvt_bool:
		return numeric_value.bool_value ? "true" : "false";
	case mvt_null:
		return "null";
	default:
		return "unknown " + std::to_string(type);
	}
}

void mvt_layer::tag(mvt_feature &feature, std::string const &key, mvt_value const &value) {
	size_t key_hash = fnv1a(key) % key_dedup.size();
	if (key_dedup[key_hash] >= 0 &&
	    keys[key_dedup[key_hash]] == key) {
	} else {
		key_dedup[key_hash] = keys.size();
		keys.push_back(key);
	}
	feature.tags.push_back(key_dedup[key_hash]);

	size_t value_hash = std::hash<mvt_value>()(value) % value_dedup.size();
	if (value_dedup[value_hash] >= 0 &&
	    values[value_dedup[value_hash]] == value) {
	} else {
		value_dedup[value_hash] = values.size();
		values.push_back(value);
	}
	feature.tags.push_back(value_dedup[value_hash]);
}

bool is_integer(const char *s, long long *v) {
	errno = 0;
	char *endptr;

	*v = strtoll(s, &endptr, 0);
	if (*v == 0 && errno != 0) {
		return 0;
	}
	if ((*v == LLONG_MIN || *v == LLONG_MAX) && (errno == ERANGE || errno == EINVAL)) {
		return 0;
	}
	if (*endptr != '\0') {
		// Special case: If it is an integer followed by .0000 or similar,
		// it is still an integer

		if (*endptr != '.') {
			return 0;
		}
		endptr++;
		for (; *endptr != '\0'; endptr++) {
			if (*endptr != '0') {
				return 0;
			}
		}

		return 1;
	}

	return 1;
}

bool is_unsigned_integer(const char *s, unsigned long long *v) {
	errno = 0;
	char *endptr;

	// Special check because MacOS stroull() returns 1
	// for -18446744073709551615
	while (isspace(*s)) {
		s++;
	}
	if (*s == '-') {
		return 0;
	}

	*v = strtoull(s, &endptr, 0);
	if (*v == 0 && errno != 0) {
		return 0;
	}
	if ((*v == ULLONG_MAX) && (errno == ERANGE || errno == EINVAL)) {
		return 0;
	}
	if (*endptr != '\0') {
		// Special case: If it is an integer followed by .0000 or similar,
		// it is still an integer

		if (*endptr != '.') {
			return 0;
		}
		endptr++;
		for (; *endptr != '\0'; endptr++) {
			if (*endptr != '0') {
				return 0;
			}
		}

		return 1;
	}

	return 1;
}

// This converts a serial_val-style attribute value to an mvt_value
// to store in a tile. If the value is numeric, it tries to choose
// the type (int, uint, sint, float, or double) that will give the
// smallest representation in the tile without losing precision,
// regardless of how the value was represented in the original source.
mvt_value stringified_to_mvt_value(int type, const char *s, std::shared_ptr<std::string> const &tile_stringpool) {
	mvt_value tv;
	tv.s = tile_stringpool;

	switch (type) {
	case mvt_double: {
		long long v;
		unsigned long long uv;
		if (is_unsigned_integer(s, &uv)) {
			if (uv <= LLONG_MAX) {
				tv.type = mvt_int;
				tv.numeric_value.int_value = uv;
			} else {
				tv.type = mvt_uint;
				tv.numeric_value.uint_value = uv;
			}
		} else if (is_integer(s, &v)) {
			tv.type = mvt_sint;
			tv.numeric_value.sint_value = v;
		} else {
			errno = 0;
			char *endptr;

			float f = strtof(s, &endptr);

			if (endptr == s || ((f == HUGE_VAL || f == HUGE_VALF || f == HUGE_VALL) && errno == ERANGE)) {
				double d = strtod(s, &endptr);
				if (endptr == s || ((d == HUGE_VAL || d == HUGE_VALF || d == HUGE_VALL) && errno == ERANGE)) {
					fprintf(stderr, "Warning: numeric value %s could not be represented\n", s);
				}
				tv.type = mvt_double;
				tv.numeric_value.double_value = d;
			} else {
				double d = atof(s);
				if (f == d) {
					tv.type = mvt_float;
					tv.numeric_value.float_value = f;
				} else {
					// Conversion succeeded, but lost precision, so use double
					tv.type = mvt_double;
					tv.numeric_value.double_value = d;
				}
			}
		}
	} break;
	case mvt_bool:
		tv.type = mvt_bool;
		tv.numeric_value.bool_value = (s[0] == 't');
		break;
	case mvt_null:
		tv.type = mvt_null;
		tv.numeric_value.null_value = 0;
		break;
	default:
		tv.type = mvt_string;
		tv.set_string_value(s);
	}

	return tv;
}

// This converts a mvt_value attribute value from a tile back into
// a serial_val for more convenient parsing and comparison without
// having to handle all of the vector tile numeric types separately.
// All numeric types are given the type mvt_double in the serial_val
// whether the actual value is integer or floating point.
serial_val mvt_value_to_serial_val(mvt_value const &v) {
	serial_val sv;

	switch (v.type) {
	case mvt_string:
		sv.type = mvt_string;
		sv.s = v.get_string_value();
		break;
	case mvt_float:
		sv.type = mvt_double;
		sv.s = milo::dtoa_milo(v.numeric_value.float_value);
		break;
	case mvt_double:
		sv.type = mvt_double;
		sv.s = milo::dtoa_milo(v.numeric_value.double_value);
		break;
	case mvt_int:
		sv.type = mvt_double;
		sv.s = std::to_string(v.numeric_value.int_value);
		break;
	case mvt_uint:
		sv.type = mvt_double;
		sv.s = std::to_string(v.numeric_value.uint_value);
		break;
	case mvt_sint:
		sv.type = mvt_double;
		sv.s = std::to_string(v.numeric_value.sint_value);
		break;
	case mvt_bool:
		sv.type = mvt_bool;
		sv.s = v.numeric_value.bool_value ? "true" : "false";
		break;
	case mvt_null:
		sv.type = mvt_null;
		sv.s = "null";
		break;
	default:
		fprintf(stderr, "unhandled mvt_type %d\n", v.type);
		exit(EXIT_IMPOSSIBLE);
	}

	return sv;
}

// This extracts an integer value from an mvt_value
long long mvt_value_to_long_long(mvt_value const &v) {
	switch (v.type) {
	case mvt_string:
		return atoll(v.c_str());
	case mvt_float:
		return v.numeric_value.float_value;
	case mvt_double:
		return v.numeric_value.double_value;
	case mvt_int:
		return v.numeric_value.int_value;
	case mvt_uint:
		return v.numeric_value.uint_value;
	case mvt_sint:
		return v.numeric_value.sint_value;
	case mvt_bool:
		return v.numeric_value.bool_value;
	case mvt_null:
		return 0;
	default:
		fprintf(stderr, "unhandled mvt_type %d\n", v.type);
		exit(EXIT_IMPOSSIBLE);
	}
}
