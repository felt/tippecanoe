#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // for asprintf()
#endif
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <memory>
#include <string>
#include <vector>
#include "jsonpull.h"
#include "../milo/milo.h"

#define BUFFER 10000

json_pull_ptr json_begin(ssize_t (*read)(struct json_pull *, char *buffer, size_t n), void *source) {
	auto j = std::make_shared<json_pull>();
	j->read = read;
	j->source = source;
	j->buffer.resize(BUFFER);
	return j;
}

static inline int peek(json_pull *j) {
	if (j->buffer_head < j->buffer_tail) {
		return (unsigned char) j->buffer[j->buffer_head];
	} else {
		j->buffer_head = 0;
		j->buffer_tail = j->read(j, j->buffer.data(), BUFFER);
		if (j->buffer_head >= j->buffer_tail) {
			return EOF;
		}
		return (unsigned char) j->buffer[j->buffer_head];
	}
}

static inline int next(json_pull *j) {
	if (j->buffer_head < j->buffer_tail) {
		return (unsigned char) j->buffer[j->buffer_head++];
	} else {
		j->buffer_head = 0;
		j->buffer_tail = j->read(j, j->buffer.data(), BUFFER);
		if (j->buffer_head >= j->buffer_tail) {
			return EOF;
		}
		return (unsigned char) j->buffer[j->buffer_head++];
	}
}

static ssize_t read_file(json_pull *j, char *buffer, size_t n) {
	return fread(buffer, 1, n, (FILE *) j->source);
}

json_pull_ptr json_begin_file(FILE *f) {
	return json_begin(read_file, f);
}

static ssize_t read_string(json_pull *j, char *buffer, size_t n) {
	const char *cp = (const char *) j->source;
	size_t out = 0;

	while (out < n && cp[out] != '\0') {
		buffer[out] = cp[out];
		out++;
	}

	j->source = (void *) (cp + out);
	return out;
}

json_pull_ptr json_begin_string(const char *s) {
	return json_begin(read_string, (void *) s);
}

void json_end(json_pull_ptr &p) {
	p.reset();
}

static inline int read_wrap(json_pull *j) {
	int c = next(j);

	if (c == '\n') {
		j->line++;
	}

	return c;
}

// JSON_TRUE / JSON_FALSE / JSON_NULL and the parse-token types are bare
// json_objects; the value-bearing types each get their own subclass.
static json_object_ptr make_object(json_type type, json_object *parent, json_pull *jp) {
	switch (type) {
	case JSON_NUMBER:
		return json_object_ptr(new json_number(parent, jp));
	case JSON_STRING:
		return json_object_ptr(new json_string(parent, jp));
	case JSON_ARRAY:
		return json_object_ptr(new json_array(parent, jp));
	case JSON_HASH:
		return json_object_ptr(new json_hash(parent, jp));
	default:
		return json_object_ptr(new json_object(type, parent, jp));
	}
}

static inline json_pull::parse_frame *current_frame(json_pull *j) {
	return j->container_stack.empty() ? nullptr : &j->container_stack.back();
}

// Install a new node of `type` in the current container, or as the parser's
// root if the stack is empty. Returns it borrowed, or nullptr after setting
// j->error.
static json_object *add_object(json_pull *j, json_type type) {
	json_pull::parse_frame *f = current_frame(j);
	json_object *c = f ? f->container : nullptr;
	json_object_ptr o = make_object(type, c, j);
	json_object *raw = o.get();

	if (f != nullptr) {
		if (c->type == JSON_ARRAY) {
			if (f->expect == JSON_ITEM) {
				c->array().push_back(std::move(o));
				f->expect = JSON_COMMA;
			} else {
				j->error = "Expected a comma, not a list item";
				return nullptr;
			}
		} else if (c->type == JSON_HASH) {
			if (f->expect == JSON_VALUE) {
				// A colon is the only thing that sets JSON_VALUE, and it
				// requires a key already pushed.
				assert(!c->entries().empty());
				c->entries().back().value = std::move(o);
				f->expect = JSON_COMMA;
			} else if (f->expect == JSON_KEY) {
				if (type != JSON_STRING) {
					j->error = "Hash key is not a string";
					return nullptr;
				}

				c->entries().push_back({std::move(o), nullptr});
				f->expect = JSON_COLON;
			} else {
				j->error = "Expected a comma or colon";
				return nullptr;
			}
		}
	} else {
		// Replacing the parser's root destroys the previous top-level
		// value (if no one called json_disconnect / json_read_tree to
		// take ownership of it).
		j->root = std::move(o);
	}

	return raw;
}

json_object *json_hash_get(json_object *o, const char *s) {
	if (o == nullptr || o->type != JSON_HASH) {
		return nullptr;
	}

	for (const auto &e : o->entries()) {
		if (e.key != nullptr && e.key->type == JSON_STRING && e.key->string() == s) {
			return e.value.get();
		}
	}

	return nullptr;
}

json_object *json_hash_get(const json_object_ptr &o, const char *s) {
	return json_hash_get(o.get(), s);
}

json_object *json_read_separators(json_pull_ptr &jp, json_separator_callback cb, void *state) {
	int c;
	json_pull *j = jp.get();

	// In case there is an error at the top level
	if (j->container_stack.empty()) {
		j->root.reset();
	}

again:
	c = read_wrap(j);
	if (c == EOF) {
		if (!j->container_stack.empty()) {
			j->error = "Reached EOF without all containers being closed";
		}

		return nullptr;
	}

	switch (c) {
		/////////////////////////// Byte order mark

	case 0xEF: {
		int c2 = peek(j);
		if (c2 == 0xBB) {
			c2 = read_wrap(j);
			c2 = peek(j);
			if (c2 == 0xBF) {
				c2 = read_wrap(j);
				c = ' ';
				goto again;
			}
		}
		j->error = "Corrupt byte-order mark found";
		return nullptr;
	}

		/////////////////////////// Whitespace

	case ' ':
	case '\t':
	case '\r':
	case '\n':
	case 0x1E:
		goto again;

		/////////////////////////// Arrays

	case '[': {
		json_object *o = add_object(j, JSON_ARRAY);
		if (o == nullptr) {
			return nullptr;
		}
		j->container_stack.push_back({o, JSON_ITEM});

		if (cb != nullptr) {
			cb(JSON_ARRAY, j, state);
		}

		goto again;
	}

	case ']': {
		json_pull::parse_frame *f = current_frame(j);
		if (f == nullptr) {
			j->error = "Found ] at top level";
			return nullptr;
		}

		json_object *cc = f->container;
		if (cc->type != JSON_ARRAY) {
			j->error = "Found ] not in an array";
			return nullptr;
		}

		if (f->expect != JSON_COMMA) {
			if (!(f->expect == JSON_ITEM && cc->array().size() == 0)) {
				j->error = "Found ] without final element";
				return nullptr;
			}
		}

		j->container_stack.pop_back();
		return cc;
	}

		/////////////////////////// Hashes

	case '{': {
		json_object *o = add_object(j, JSON_HASH);
		if (o == nullptr) {
			return nullptr;
		}
		j->container_stack.push_back({o, JSON_KEY});

		if (cb != nullptr) {
			cb(JSON_HASH, j, state);
		}

		goto again;
	}

	case '}': {
		json_pull::parse_frame *f = current_frame(j);
		if (f == nullptr) {
			j->error = "Found } at top level";
			return nullptr;
		}

		json_object *cc = f->container;
		if (cc->type != JSON_HASH) {
			j->error = "Found } not in a hash";
			return nullptr;
		}

		if (f->expect != JSON_COMMA) {
			if (!(f->expect == JSON_KEY && cc->entries().size() == 0)) {
				j->error = "Found } without final element";
				return nullptr;
			}
		}

		j->container_stack.pop_back();
		return cc;
	}

		/////////////////////////// Null

	case 'n': {
		if (read_wrap(j) != 'u' || read_wrap(j) != 'l' || read_wrap(j) != 'l') {
			j->error = "Found misspelling of null";
			return nullptr;
		}

		return add_object(j, JSON_NULL);
	}

		/////////////////////////// NaN

	case 'N': {
		if (read_wrap(j) != 'a' || read_wrap(j) != 'N') {
			j->error = "Found misspelling of NaN";
			return nullptr;
		}

		j->error = "JSON does not allow NaN";
		return nullptr;
	}

		/////////////////////////// Infinity

	case 'I': {
		if (read_wrap(j) != 'n' || read_wrap(j) != 'f' || read_wrap(j) != 'i' ||
		    read_wrap(j) != 'n' || read_wrap(j) != 'i' || read_wrap(j) != 't' ||
		    read_wrap(j) != 'y') {
			j->error = "Found misspelling of Infinity";
			return nullptr;
		}

		j->error = "JSON does not allow Infinity";
		return nullptr;
	}

		/////////////////////////// True

	case 't': {
		if (read_wrap(j) != 'r' || read_wrap(j) != 'u' || read_wrap(j) != 'e') {
			j->error = "Found misspelling of true";
			return nullptr;
		}

		return add_object(j, JSON_TRUE);
	}

		/////////////////////////// False

	case 'f': {
		if (read_wrap(j) != 'a' || read_wrap(j) != 'l' || read_wrap(j) != 's' || read_wrap(j) != 'e') {
			j->error = "Found misspelling of false";
			return nullptr;
		}

		return add_object(j, JSON_FALSE);
	}

		/////////////////////////// Comma

	case ',': {
		json_pull::parse_frame *f = current_frame(j);
		if (f != nullptr) {
			if (f->expect != JSON_COMMA) {
				j->error = "Found unexpected comma";
				return nullptr;
			}

			if (f->container->type == JSON_HASH) {
				f->expect = JSON_KEY;
			} else {
				f->expect = JSON_ITEM;
			}
		}

		if (cb != nullptr) {
			cb(JSON_COMMA, j, state);
		}

		goto again;
	}

		/////////////////////////// Colon

	case ':': {
		json_pull::parse_frame *f = current_frame(j);
		if (f == nullptr) {
			j->error = "Found colon at top level";
			return nullptr;
		}

		if (f->expect != JSON_COLON) {
			j->error = "Found unexpected colon";
			return nullptr;
		}

		f->expect = JSON_VALUE;

		if (cb != nullptr) {
			cb(JSON_COLON, j, state);
		}

		goto again;
	}

		/////////////////////////// Numbers

	case '-':
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9': {
		j->number_buffer.clear();
		int decimal = 0;

		if (c == '-') {
			j->number_buffer.push_back(c);
			c = read_wrap(j);
		}

		if (c == '0') {
			j->number_buffer.push_back(c);
		} else if (c >= '1' && c <= '9') {
			j->number_buffer.push_back(c);
			c = peek(j);

			while (c >= '0' && c <= '9') {
				j->number_buffer.push_back(read_wrap(j));
				c = peek(j);
			}
		}

		if (peek(j) == '.') {
			j->number_buffer.push_back(read_wrap(j));
			decimal = 1;

			c = peek(j);
			if (c < '0' || c > '9') {
				j->error = "Decimal point without digits";
				return nullptr;
			}
			while (c >= '0' && c <= '9') {
				j->number_buffer.push_back(read_wrap(j));
				c = peek(j);
			}
		}

		c = peek(j);
		if (c == 'e' || c == 'E') {
			j->number_buffer.push_back(read_wrap(j));
			decimal = 1;

			c = peek(j);
			if (c == '+' || c == '-') {
				j->number_buffer.push_back(read_wrap(j));
			}

			c = peek(j);
			if (c < '0' || c > '9') {
				j->error = "Exponent without digits";
				return nullptr;
			}
			while (c >= '0' && c <= '9') {
				j->number_buffer.push_back(read_wrap(j));
				c = peek(j);
			}
		}

		json_object *n = add_object(j, JSON_NUMBER);
		if (n != nullptr) {
			double d = atof(j->number_buffer.c_str());
			n->set_number(d);

#define MAX_SAFE_INTEGER 9007199254740991.0
#define MIN_SAFE_INTEGER -9007199254740991.0

			if (!decimal && d > MAX_SAFE_INTEGER) {
				errno = 0;
				char *err = nullptr;
				unsigned long long ull = strtoull(j->number_buffer.c_str(), &err, 10);
				if (errno == 0 && (err == nullptr || *err == '\0')) {
					n->set_large_unsigned(ull);
				}
			}
			if (!decimal && d < MIN_SAFE_INTEGER) {
				errno = 0;
				char *err = nullptr;
				long long ll = strtoll(j->number_buffer.c_str(), &err, 10);
				if (errno == 0 && (err == nullptr || *err == '\0')) {
					n->set_large_signed(ll);
				}
			}
		}
		return n;
	}

		/////////////////////////// Strings

	case '"': {
		// Reused across tokens; see json_pull::string_buffer.
		std::string &val = j->string_buffer;
		val.clear();

		int surrogate = -1;
		while ((c = read_wrap(j)) != EOF) {
			if (c == '"') {
				if (surrogate >= 0) {
					val.push_back(0xE0 | (surrogate >> 12));
					val.push_back(0x80 | ((surrogate >> 6) & 0x3F));
					val.push_back(0x80 | (surrogate & 0x3F));
					surrogate = -1;
				}

				break;
			} else if (c == '\\') {
				c = read_wrap(j);

				if (c == 'u') {
					char hex[5] = "aaaa";
					int i;
					for (i = 0; i < 4; i++) {
						hex[i] = read_wrap(j);
						if (hex[i] < '0' || (hex[i] > '9' && hex[i] < 'A') || (hex[i] > 'F' && hex[i] < 'a') || hex[i] > 'f') {
							j->error = "Invalid \\u hex character";
							return nullptr;
						}
					}

					unsigned long ch = strtoul(hex, nullptr, 16);
					if (ch >= 0xd800 && ch <= 0xdbff) {
						if (surrogate < 0) {
							surrogate = ch;
						} else {
							// Impossible surrogate, so output the first half,
							// keep what might be a legitimate new first half.
							val.push_back(0xE0 | (surrogate >> 12));
							val.push_back(0x80 | ((surrogate >> 6) & 0x3F));
							val.push_back(0x80 | (surrogate & 0x3F));
							surrogate = ch;
						}
						continue;
					} else if (ch >= 0xdc00 && ch <= 0xdfff) {
						if (surrogate >= 0) {
							long c1 = surrogate - 0xd800;
							long c2 = ch - 0xdc00;
							ch = ((c1 << 10) | c2) + 0x010000;
							surrogate = -1;
						}
					}

					if (surrogate >= 0) {
						val.push_back(0xE0 | (surrogate >> 12));
						val.push_back(0x80 | ((surrogate >> 6) & 0x3F));
						val.push_back(0x80 | (surrogate & 0x3F));
						surrogate = -1;
					}

					if (ch <= 0x7F) {
						val.push_back(ch);
					} else if (ch <= 0x7FF) {
						val.push_back(0xC0 | (ch >> 6));
						val.push_back(0x80 | (ch & 0x3F));
					} else if (ch <= 0xFFFF) {
						val.push_back(0xE0 | (ch >> 12));
						val.push_back(0x80 | ((ch >> 6) & 0x3F));
						val.push_back(0x80 | (ch & 0x3F));
					} else {
						// Only reachable for a code point assembled from a
						// surrogate pair above, since `ch` on its own comes
						// from four hex digits and so cannot exceed 0xFFFF.
						val.push_back(0xF0 | (ch >> 18));
						val.push_back(0x80 | ((ch >> 12) & 0x3F));
						val.push_back(0x80 | ((ch >> 6) & 0x3F));
						val.push_back(0x80 | (ch & 0x3F));
					}
				} else {
					if (surrogate >= 0) {
						val.push_back(0xE0 | (surrogate >> 12));
						val.push_back(0x80 | ((surrogate >> 6) & 0x3F));
						val.push_back(0x80 | (surrogate & 0x3F));
						surrogate = -1;
					}

					if (c == '"') {
						val.push_back('"');
					} else if (c == '\\') {
						val.push_back('\\');
					} else if (c == '/') {
						val.push_back('/');
					} else if (c == 'b') {
						val.push_back('\b');
					} else if (c == 'f') {
						val.push_back('\f');
					} else if (c == 'n') {
						val.push_back('\n');
					} else if (c == 'r') {
						val.push_back('\r');
					} else if (c == 't') {
						val.push_back('\t');
					} else {
						j->error = "Found backslash followed by unknown character";
						return nullptr;
					}
				}
			} else if (c < ' ') {
				j->error = "Found control character in string";
				return nullptr;
			} else {
				if (surrogate >= 0) {
					val.push_back(0xE0 | (surrogate >> 12));
					val.push_back(0x80 | ((surrogate >> 6) & 0x3F));
					val.push_back(0x80 | (surrogate & 0x3F));
					surrogate = -1;
				}

				val.push_back(c);
			}
		}
		if (c == EOF) {
			j->error = "String without closing quote mark";
			return nullptr;
		}

		json_object *s = add_object(j, JSON_STRING);
		if (s != nullptr) {
			// Copy, not move, so the buffer keeps its capacity.
			s->string() = val;
		}
		return s;
	}
	}

	j->error = "Found unexpected character";
	return nullptr;
}

json_object *json_read(json_pull_ptr &j) {
	return json_read_separators(j, nullptr, nullptr);
}

static void detach_subtree(json_object *o);

json_object_ptr json_read_tree(json_pull_ptr &p) {
	json_object *j;

	while ((j = json_read(p)) != nullptr) {
		if (j->parent == nullptr) {
			json_object_ptr tree = std::move(p->root);
			detach_subtree(tree.get());
			return tree;
		}
	}

	return nullptr;
}

// Move the owning json_object_ptr out of whatever vector slot or hash entry
// holds `o`. Empty if nothing tracked owns it -- already detached, or borrowed
// from elsewhere.
//
// Detaching one half of a hash entry would disturb the surrounding key/value
// pairing, so the extracted half is replaced by a JSON_NULL placeholder and the
// entry is erased only once both halves are gone.
static json_object_ptr take_from_owner(json_object *o) {
	if (o == nullptr) {
		return nullptr;
	}

	json_object *parent = o->parent;
	if (parent == nullptr) {
		// Top-level value: the parser owns it via root, unless the
		// caller already moved it out.
		json_pull *parser = o->parser;
		if (parser != nullptr && parser->root.get() == o) {
			return std::move(parser->root);
		}
		return nullptr;
	}

	if (parent->type == JSON_ARRAY) {
		auto &arr = parent->array();
		for (size_t i = 0; i < arr.size(); i++) {
			if (arr[i].get() == o) {
				json_object_ptr taken = std::move(arr[i]);
				arr.erase(arr.begin() + i);
				return taken;
			}
		}
	} else if (parent->type == JSON_HASH) {
		auto &entries = parent->entries();
		for (size_t i = 0; i < entries.size(); i++) {
			auto &e = entries[i];
			if (e.key.get() == o) {
				json_object_ptr taken = std::move(e.key);
				e.key = make_object(JSON_NULL, parent, parent->parser);
				if (e.value != nullptr && e.value->type == JSON_NULL && e.key->type == JSON_NULL) {
					entries.erase(entries.begin() + i);
				}
				return taken;
			}
			if (e.value.get() == o) {
				json_object_ptr taken = std::move(e.value);
				e.value = make_object(JSON_NULL, parent, parent->parser);
				if (e.key != nullptr && e.key->type == JSON_NULL && e.value->type == JSON_NULL) {
					entries.erase(entries.begin() + i);
				}
				return taken;
			}
		}
	}

	return nullptr;
}

// Splice `o` out of its owner and destroy it; `o` dangles afterwards. No need
// to clear back-pointers, since nothing will observe them again.
void json_free(json_object *o) {
	(void) take_from_owner(o);
}

// See Ownership model in jsonpull.h for why only `parser` is cleared here.
static void clear_parser_pointers(json_object *o) {
	if (o == nullptr) {
		return;
	}

	if (o->type == JSON_HASH) {
		for (const auto &e : o->entries()) {
			clear_parser_pointers(e.key.get());
			clear_parser_pointers(e.value.get());
		}
	} else if (o->type == JSON_ARRAY) {
		const auto &arr = o->array();
		for (size_t i = 0; i < arr.size(); i++) {
			clear_parser_pointers(arr[i].get());
		}
	}

	o->parser = nullptr;
}

// The root's `parent` pointed out of the subtree, at a node the parser still
// owns; leaving it set would make a later json_free look for this node in a
// container that no longer holds it.
static void detach_subtree(json_object *o) {
	if (o == nullptr) {
		return;
	}

	clear_parser_pointers(o);
	o->parent = nullptr;
}

json_object_ptr json_disconnect(json_object *o) {
	json_object_ptr taken = take_from_owner(o);
	if (taken != nullptr) {
		detach_subtree(taken.get());
	}
	return taken;
}

static void json_print_one(std::string &val, const json_object *o) {
	if (o == nullptr) {
		val.append("...");
	} else if (o->type == JSON_STRING) {
		val.push_back('\"');

		// Range, not c_str(): the value may contain an embedded NUL, which the
		// control-character branch below escapes like any other.
		for (char c : o->string()) {
			if (c == '\\' || c == '"') {
				val.push_back('\\');
				val.push_back(c);
			} else if (c >= 0 && c < ' ') {
				char *s;
				if (asprintf(&s, "\\u%04x", c) >= 0) {
					val.append(s);
					free(s);
				}
			} else {
				val.push_back(c);
			}
		}

		val.push_back('\"');
	} else if (o->type == JSON_NUMBER) {
		if (o->large_signed() != 0) {
			char s[65];
			snprintf(s, sizeof(s), "%lld", o->large_signed());
			val.append(s);
		} else if (o->large_unsigned() != 0) {
			char s[65];
			snprintf(s, sizeof(s), "%llu", o->large_unsigned());
			val.append(s);
		} else {
			char *s = dtoa_milo(o->number());
			val.append(s);
			free(s);
		}
	} else if (o->type == JSON_NULL) {
		val.append("null");
	} else if (o->type == JSON_TRUE) {
		val.append("true");
	} else if (o->type == JSON_FALSE) {
		val.append("false");
	}
	// JSON_HASH and JSON_ARRAY never reach here: json_print handles both
	// itself and only delegates to json_print_one for the scalar types.
}

static void json_print(std::string &val, const json_object *o) {
	if (o == nullptr) {
		// Hash value in incompletely read hash
		val.append("...");
	} else if (o->type == JSON_HASH) {
		val.push_back('{');

		const auto &entries = o->entries();
		for (size_t i = 0; i < entries.size(); i++) {
			json_print(val, entries[i].key.get());
			val.push_back(':');
			json_print(val, entries[i].value.get());
			if (i + 1 < entries.size()) {
				val.push_back(',');
			}
		}
		val.push_back('}');
	} else if (o->type == JSON_ARRAY) {
		val.push_back('[');
		const auto &arr = o->array();
		for (size_t i = 0; i < arr.size(); i++) {
			json_print(val, arr[i].get());
			if (i + 1 < arr.size()) {
				val.push_back(',');
			}
		}
		val.push_back(']');
	} else {
		json_print_one(val, o);
	}
}

std::string json_stringify(const json_object *o) {
	std::string val;
	json_print(val, o);
	return val;
}
