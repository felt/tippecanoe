#ifndef JSONPULL_H
#define JSONPULL_H

#include <stdio.h>
#include <sys/types.h>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

typedef enum json_type {
	// These types can be returned by json_read()
	JSON_HASH,
	JSON_ARRAY,
	JSON_NUMBER,
	JSON_STRING,
	JSON_TRUE,
	JSON_FALSE,
	JSON_NULL,

	// These and JSON_HASH and JSON_ARRAY can be called back by json_read_with_separators()
	JSON_COMMA,
	JSON_COLON,

	// These are only used internally as expectations of what comes next
	JSON_ITEM,
	JSON_KEY,
	JSON_VALUE,
} json_type;

struct json_object;
struct json_pull;

typedef std::shared_ptr<json_object> json_object_ptr;
typedef std::shared_ptr<json_pull> json_pull_ptr;

// A single key/value pair inside a JSON_HASH. The pairs are stored in
// insertion order in a single std::vector<json_entry> on json_hash, so
// callers can range-for over `o->entries()` with structured bindings
// (`for (auto &[k, v] : o->entries()) ...`) while still preserving the
// order keys appeared in the source document.
struct json_entry {
	json_object_ptr key;
	json_object_ptr value;
};

// json_object is a small base type that just records the JSON type and
// the back-pointers to its parent and parser. The actual value payload
// lives in a type-specific subclass (json_number, json_string, json_array,
// json_hash), so that JSON_TRUE / JSON_FALSE / JSON_NULL nodes pay only
// the base-class cost and a JSON_HASH does not also drag along a string
// or a number field. Type-tagged accessor methods on the base class
// downcast and return references to the underlying subclass storage.
//
// Children are owned by their parent (via std::vector<json_object_ptr>
// inside json_array / json_hash); the raw `parent` and `parser`
// back-pointers stay valid as long as the node is attached to the tree.
// json_disconnect() splices a node out of its parent and walks the
// detached subtree clearing those back-pointers so the subtree can
// outlive the original parser.
//
// json_object intentionally has no virtual functions and no virtual
// destructor: subclasses are constructed via std::make_shared<json_xxx>(),
// and std::shared_ptr remembers the deleter from the original type, so
// destroying a shared_ptr<json_object> that actually points at a
// json_string still runs ~json_string(). Dispatch on `type` is what the
// rest of the code already does. The accessor methods assert at debug
// time that the type matches before downcasting.

struct json_object {
	json_object *parent = nullptr;
	json_pull *parser = nullptr;

	json_type type;

	json_object(json_type t) : type(t) {}
	json_object(json_type t, json_object *p, json_pull *pl) : parent(p), parser(pl), type(t) {}

	// Type-tagged accessors. Each one asserts that the receiver is of
	// the right kind, then downcasts to the storage in the appropriate
	// subclass. Inline so the assert and cast disappear at -O.
	inline std::string &string();
	inline const std::string &string() const;

	// Numbers are stored in a discriminated union (double / unsigned /
	// signed) so a json_number is only 40 bytes instead of 48. The
	// large_*() accessors return 0 when the number is not currently
	// stored in that representation, matching the prior convention
	// where "0" meant "not set, fall through to the next slot".
	inline double number() const;
	inline unsigned long long large_unsigned() const;
	inline long long large_signed() const;
	inline void set_number(double d);
	inline void set_large_unsigned(unsigned long long u);
	inline void set_large_signed(long long s);

	inline std::vector<json_object_ptr> &array();
	inline const std::vector<json_object_ptr> &array() const;

	inline std::vector<json_entry> &entries();
	inline const std::vector<json_entry> &entries() const;
};

struct json_number : json_object {
	enum repr_t { REPR_DOUBLE,
		      REPR_LARGE_UNSIGNED,
		      REPR_LARGE_SIGNED };

	repr_t repr = REPR_DOUBLE;
	union value_t {
		double d;
		unsigned long long u;
		long long s;
		value_t() : d(0) {}
	} value;

	json_number() : json_object(JSON_NUMBER) {}
	json_number(json_object *p, json_pull *pl) : json_object(JSON_NUMBER, p, pl) {}
};

struct json_string : json_object {
	std::string string_value;

	json_string() : json_object(JSON_STRING) {}
	json_string(json_object *p, json_pull *pl) : json_object(JSON_STRING, p, pl) {}
};

struct json_array : json_object {
	std::vector<json_object_ptr> array_value;

	// Coordinate-heavy GeoJSON dominates the parse workload, and every
	// `[x, y]` (or `[x, y, z]`) pair would otherwise force the inner
	// vector through 0 -> 1 -> 2 -> 4 growths plus the matching
	// shared_ptr copies. Reserving 2 slots up front eliminates those
	// reallocations for the common case and adds only a single small
	// allocation for larger rings (which still grow geometrically).
	json_array() : json_object(JSON_ARRAY) { array_value.reserve(2); }
	json_array(json_object *p, json_pull *pl) : json_object(JSON_ARRAY, p, pl) { array_value.reserve(2); }
};

struct json_hash : json_object {
	std::vector<json_entry> entries_value;

	// Most GeoJSON property hashes have a handful of keys (type, id,
	// properties, geometry, plus a few attribute fields). Reserving 4
	// slots avoids the 0 -> 1 -> 2 -> 4 growth chain for the typical
	// case while only modestly over-allocating for one-key hashes.
	json_hash() : json_object(JSON_HASH) { entries_value.reserve(4); }
	json_hash(json_object *p, json_pull *pl) : json_object(JSON_HASH, p, pl) { entries_value.reserve(4); }
};

inline std::string &json_object::string() {
	assert(type == JSON_STRING);
	return static_cast<json_string *>(this)->string_value;
}
inline const std::string &json_object::string() const {
	assert(type == JSON_STRING);
	return static_cast<const json_string *>(this)->string_value;
}

inline double json_object::number() const {
	assert(type == JSON_NUMBER);
	auto *n = static_cast<const json_number *>(this);
	switch (n->repr) {
	case json_number::REPR_LARGE_UNSIGNED:
		return static_cast<double>(n->value.u);
	case json_number::REPR_LARGE_SIGNED:
		return static_cast<double>(n->value.s);
	case json_number::REPR_DOUBLE:
	default:
		return n->value.d;
	}
}
inline unsigned long long json_object::large_unsigned() const {
	assert(type == JSON_NUMBER);
	auto *n = static_cast<const json_number *>(this);
	return n->repr == json_number::REPR_LARGE_UNSIGNED ? n->value.u : 0;
}
inline long long json_object::large_signed() const {
	assert(type == JSON_NUMBER);
	auto *n = static_cast<const json_number *>(this);
	return n->repr == json_number::REPR_LARGE_SIGNED ? n->value.s : 0;
}
inline void json_object::set_number(double d) {
	assert(type == JSON_NUMBER);
	auto *n = static_cast<json_number *>(this);
	n->repr = json_number::REPR_DOUBLE;
	n->value.d = d;
}
inline void json_object::set_large_unsigned(unsigned long long u) {
	assert(type == JSON_NUMBER);
	auto *n = static_cast<json_number *>(this);
	n->repr = json_number::REPR_LARGE_UNSIGNED;
	n->value.u = u;
}
inline void json_object::set_large_signed(long long s) {
	assert(type == JSON_NUMBER);
	auto *n = static_cast<json_number *>(this);
	n->repr = json_number::REPR_LARGE_SIGNED;
	n->value.s = s;
}

inline std::vector<json_object_ptr> &json_object::array() {
	assert(type == JSON_ARRAY);
	return static_cast<json_array *>(this)->array_value;
}
inline const std::vector<json_object_ptr> &json_object::array() const {
	assert(type == JSON_ARRAY);
	return static_cast<const json_array *>(this)->array_value;
}

inline std::vector<json_entry> &json_object::entries() {
	assert(type == JSON_HASH);
	return static_cast<json_hash *>(this)->entries_value;
}
inline const std::vector<json_entry> &json_object::entries() const {
	assert(type == JSON_HASH);
	return static_cast<const json_hash *>(this)->entries_value;
}

struct json_pull {
	const char *error = nullptr;  // points at a string literal; no allocation
	int line = 1;

	ssize_t (*read)(struct json_pull *, char *buf, size_t n) = nullptr;
	void *source = nullptr;
	std::vector<char> buffer;
	ssize_t buffer_tail = 0;
	ssize_t buffer_head = 0;

	// Stack of currently-open containers; the top is the innermost container
	// being parsed. Each frame also remembers what token is expected next
	// (an item, a comma, a key, a colon, or a value). This stack is the
	// only place the parser-only `expect` state lives, so it does not
	// pollute json_object once parsing finishes. Replaces the previous
	// single `container` pointer / parent walk, which previously required
	// enable_shared_from_this<json_object> on every json_object instance.
	struct parse_frame {
		json_object_ptr container;
		json_type expect;
	};
	std::vector<parse_frame> container_stack;
	json_object_ptr root;

	// Scratch buffers reused across tokens so we don't reallocate per
	// number/string. number_buffer accumulates raw digits before atof();
	// string_buffer accumulates decoded bytes before being copied into
	// the final json_string. Both are cleared (capacity preserved) at
	// the start of each token, so once they grow to the largest seen
	// size they stop reallocating entirely.
	std::string number_buffer;
	std::string string_buffer;
};

json_pull_ptr json_begin_file(FILE *f);
json_pull_ptr json_begin_string(const char *s);

json_pull_ptr json_begin(ssize_t (*read)(struct json_pull *, char *buffer, size_t n), void *source);

// json_end is now a thin convenience that resets the caller's json_pull_ptr.
// The parser (and any tree it still owns) is freed when the last shared_ptr
// to it is dropped, so calling json_end is optional if the json_pull_ptr will
// go out of scope on its own.
void json_end(json_pull_ptr &p);

typedef void (*json_separator_callback)(json_type type, json_pull *j, void *state);

json_object_ptr json_read_tree(json_pull_ptr j);
json_object_ptr json_read(json_pull_ptr j);
json_object_ptr json_read_separators(json_pull_ptr j, json_separator_callback cb, void *state);

// json_free now just resets the caller's json_object_ptr. The subtree is
// destroyed when the last shared_ptr to it is dropped (typically by also
// being removed from its parent or parser).
void json_free(json_object_ptr &j);

// Splice o out of its parent's array/object and clear parent/parser back-pointers
// throughout the detached subtree so it can outlive the original parser.
void json_disconnect(json_object_ptr j);

json_object_ptr json_hash_get(json_object_ptr o, const char *s);
json_object_ptr json_hash_get(json_object *o, const char *s);

std::string json_stringify(json_object_ptr o);

#endif
