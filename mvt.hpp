#ifndef MVT_HPP
#define MVT_HPP

#include <sqlite3.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <set>
#include <vector>
#include <optional>
#include <memory>

#include "errors.hpp"
#include "text.hpp"

struct mvt_value;
struct mvt_layer;

enum mvt_operation {
	mvt_moveto = 1,
	mvt_lineto = 2,
	mvt_closepath = 7
};

struct mvt_geometry {
	long long x = 0;
	long long y = 0;
	int /* mvt_operation */ op = 0;

	mvt_geometry(int op, long long x, long long y);

	bool operator<(mvt_geometry const &s) const {
		if (y < s.y || (y == s.y && x < s.x)) {
			return true;
		} else {
			return false;
		}
	}

	bool operator==(mvt_geometry const &s) const {
		return y == s.y && x == s.x;
	}
};

enum mvt_geometry_type {
	mvt_point = 1,
	mvt_linestring = 2,
	mvt_polygon = 3
};

struct mvt_feature {
	std::vector<unsigned> tags{};
	std::vector<mvt_geometry> geometry{};
	int /* mvt_geometry_type */ type = 0;
	unsigned long long id = 0;
	bool has_id = false;
	int dropped = 0;
	size_t seq = 0;	 // used for ordering in overzoom

	mvt_feature() {
		has_id = false;
		id = 0;
	}
};

enum mvt_value_type {
	mvt_string,
	mvt_float,
	mvt_double,
	mvt_int,
	mvt_uint,
	mvt_sint,
	mvt_bool,
	mvt_null,

	mvt_no_such_key,
};

struct mvt_value {
	mvt_value_type type;
	std::shared_ptr<std::string> s;

	union {
		float float_value;
		double double_value;
		long long int_value;
		unsigned long long uint_value;
		long long sint_value;
		bool bool_value;
		int null_value;
		struct {
			size_t off;
			size_t len;
		} string_value;
	} numeric_value;

	std::string get_string_value() const {
		return std::string(*s, numeric_value.string_value.off, numeric_value.string_value.len);
	}

	std::string_view get_string_view() const {
		return std::string_view(s->c_str() + numeric_value.string_value.off, numeric_value.string_value.len);
	}

	const char *c_str() const {
		return s->c_str() + numeric_value.string_value.off;
	}

	void set_string_value(const std::string_view &val) {
		if (s == nullptr) {
			s = std::make_shared<std::string>();
		}

		type = mvt_string;
		numeric_value.string_value.off = s->size();
		numeric_value.string_value.len = val.size();
		s->append(val);
		s->push_back('\0');
	}

	bool operator<(const mvt_value &o) const;
	bool operator==(const mvt_value &o) const;
	std::string toString() const;

	mvt_value() {
		this->type = mvt_double;
		this->numeric_value.double_value = 0;
	}
};

template <>
struct std::hash<mvt_value> {
	std::size_t operator()(const mvt_value &k) const {
		switch (k.type) {
		case mvt_string:
			return fnv1a(k.c_str(), 0);

		case mvt_float:
			return fnv1a(sizeof(float), (void *) &k.numeric_value.float_value);

		case mvt_double:
			return fnv1a(sizeof(double), (void *) &k.numeric_value.double_value);

		case mvt_int:
			return fnv1a(sizeof(long long), (void *) &k.numeric_value.int_value);

		case mvt_uint:
			return fnv1a(sizeof(unsigned long long), (void *) &k.numeric_value.uint_value);

		case mvt_sint:
			return fnv1a(sizeof(long long), (void *) &k.numeric_value.sint_value);

		case mvt_bool:
			return fnv1a(sizeof(bool), (void *) &k.numeric_value.bool_value);

		case mvt_null:
			return fnv1a(sizeof(int), (void *) &k.numeric_value.null_value);

		default:
			fprintf(stderr, "mvt_value hash can't happen\n");
			exit(EXIT_IMPOSSIBLE);
		}
	}
};

struct mvt_layer {
	int version = 0;
	std::string name = "";
	std::vector<mvt_feature> features{};
	std::vector<std::string> keys{};
	std::vector<mvt_value> values{};
	long long extent = 0;

	// Add a key-value pair to a feature, using this layer's constant pool
	void tag(mvt_feature &feature, std::string const &key, mvt_value const &value);

	// For tracking the key-value constants already used in this layer
	std::vector<ssize_t> key_dedup = std::vector<ssize_t>(65536, -1);
	std::vector<ssize_t> value_dedup = std::vector<ssize_t>(65536, -1);
};

struct mvt_tile {
	std::vector<mvt_layer> layers{};

	std::string encode();
	bool decode(const std::string &message, bool &was_compressed);
};

bool is_compressed(std::string const &data);
int decompress(std::string const &input, std::string &output);
int compress(std::string const &input, std::string &output, bool gz);
int dezig(unsigned n);

mvt_value stringified_to_mvt_value(int type, const char *s, std::shared_ptr<std::string> const &tile_stringpool);
long long mvt_value_to_long_long(mvt_value const &v);

bool is_integer(const char *s, long long *v);
bool is_unsigned_integer(const char *s, unsigned long long *v);

struct serial_val;
serial_val mvt_value_to_serial_val(mvt_value const &v);
#endif
