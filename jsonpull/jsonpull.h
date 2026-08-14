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

// Ownership model
//
// Every node has exactly one owner: its parent (a json_object_ptr in the
// parent's vector or hash entry), the parser (jp->root) for a top-level value,
// or the caller once json_read_tree / json_disconnect hands the tree over.
// json_read and json_hash_get return borrowed pointers, valid while the owning
// container is intact.
//
// `parent` and `parser` are non-owning, so they cannot form a cycle. Detaching
// clears every `parser` in the subtree, since the json_pull may die first, and
// clears `parent` only on the detached root, which pointed out of the subtree.
// Interior `parent` links stay, so a detached tree is still walkable upwards
// and json_free / json_disconnect still work inside it -- both find a node's
// owner through `parent`.
//
// json_object has no vptr; json_object_deleter switches on `type` and
// static_casts so the right subclass destructor runs. Payloads live in those
// subclasses rather than one wide struct, and the accessors assert on `type`
// before downcasting.
//
// json_pull_ptr is a shared_ptr: a parser is created and freed once.

// Stateless, so json_object_ptr stays one pointer wide. See Ownership model.
struct json_object_deleter {
	void operator()(json_object *p) const noexcept;
};

typedef std::unique_ptr<json_object, json_object_deleter> json_object_ptr;
typedef std::shared_ptr<json_pull> json_pull_ptr;

// One key/value pair in a JSON_HASH, held in source order.
struct json_entry {
	json_object_ptr key;
	json_object_ptr value;
};

struct json_object {
	json_object *parent = nullptr;
	json_pull *parser = nullptr;

	json_type type;

	json_object(json_type t)
	    : type(t) {
	}
	json_object(json_type t, json_object *p, json_pull *pl)
	    : parent(p), parser(pl), type(t) {
	}

	inline std::string &string();
	inline const std::string &string() const;

	// large_unsigned() / large_signed() return 0 when the number is not
	// held in that representation, the convention callers already expect.
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
		value_t()
		    : d(0) {
		}
	} value;

	json_number()
	    : json_object(JSON_NUMBER) {
	}
	json_number(json_object *p, json_pull *pl)
	    : json_object(JSON_NUMBER, p, pl) {
	}
};

struct json_string : json_object {
	std::string string_value;

	json_string()
	    : json_object(JSON_STRING) {
	}
	json_string(json_object *p, json_pull *pl)
	    : json_object(JSON_STRING, p, pl) {
	}
};

struct json_array : json_object {
	std::vector<json_object_ptr> array_value;

	// 2 slots: coordinate pairs dominate the parse workload.
	json_array()
	    : json_object(JSON_ARRAY) {
		array_value.reserve(2);
	}
	json_array(json_object *p, json_pull *pl)
	    : json_object(JSON_ARRAY, p, pl) {
		array_value.reserve(2);
	}
};

struct json_hash : json_object {
	std::vector<json_entry> entries_value;

	// 4 slots: the typical GeoJSON property hash.
	json_hash()
	    : json_object(JSON_HASH) {
		entries_value.reserve(4);
	}
	json_hash(json_object *p, json_pull *pl)
	    : json_object(JSON_HASH, p, pl) {
		entries_value.reserve(4);
	}
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

inline void json_object_deleter::operator()(json_object *p) const noexcept {
	if (p == nullptr) {
		return;
	}
	switch (p->type) {
	case JSON_NUMBER:
		delete static_cast<json_number *>(p);
		break;
	case JSON_STRING:
		delete static_cast<json_string *>(p);
		break;
	case JSON_ARRAY:
		delete static_cast<json_array *>(p);
		break;
	case JSON_HASH:
		delete static_cast<json_hash *>(p);
		break;
	default:
		// JSON_TRUE / JSON_FALSE / JSON_NULL: no extra fields.
		delete p;
		break;
	}
}

struct json_pull {
	const char *error = nullptr;  // points at a string literal; no allocation
	int line = 1;

	ssize_t (*read)(struct json_pull *, char *buf, size_t n) = nullptr;
	void *source = nullptr;
	std::vector<char> buffer;
	ssize_t buffer_tail = 0;
	ssize_t buffer_head = 0;

	// Currently-open containers, innermost last, each with the token it
	// expects next. `container` is borrowed; the owner is the surrounding
	// container, or `root` for the outermost.
	struct parse_frame {
		json_object *container;
		json_type expect;
	};
	std::vector<parse_frame> container_stack;

	// Most recently completed top-level value, owned until the next one
	// starts parsing or the caller takes it.
	json_object_ptr root;

	// Reused across tokens, cleared but not shrunk, so they stop
	// reallocating once grown to the largest token seen.
	std::string number_buffer;
	std::string string_buffer;
};

json_pull_ptr json_begin_file(FILE *f);
json_pull_ptr json_begin_string(const char *s);

json_pull_ptr json_begin(ssize_t (*read)(struct json_pull *, char *buffer, size_t n), void *source);

// Resets the caller's pointer. Optional: the parser frees itself when the
// last json_pull_ptr to it goes away.
void json_end(json_pull_ptr &p);

typedef void (*json_separator_callback)(json_type type, json_pull *j, void *state);

// The next completed node, borrowed. Valid until the next call that extends
// or trims the parser's tree. nullptr at end of input or on error.
json_object *json_read(json_pull_ptr &j);
json_object *json_read_separators(json_pull_ptr &j, json_separator_callback cb, void *state);

// Drains the next top-level value out of the parser and hands it over. The
// returned tree can outlive the json_pull. See Ownership model for what the
// detach does and does not clear.
json_object_ptr json_read_tree(json_pull_ptr &j);

// json_free splices `o` out of its parent (if any), or clears the
// parser's root if `o` is the parser's current top-level value, and
// destroys the subtree. After this call, `o` is a dangling pointer
// that must not be used. Safe to call with nullptr.
void json_free(json_object *o);

// Splices `o` out of whatever owns it and hands it over, same detach as
// json_read_tree. See Ownership model.
json_object_ptr json_disconnect(json_object *o);

// Borrowed value for `s`. nullptr if `o` is not a hash, `s` is absent, or the
// hash is mid-parse with that value slot unfilled -- a JSON `null` is none of
// those, and comes back as a JSON_NULL node.
json_object *json_hash_get(const json_object_ptr &o, const char *s);
json_object *json_hash_get(json_object *o, const char *s);

std::string json_stringify(const json_object *o);

#endif
