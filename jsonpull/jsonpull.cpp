#define _GNU_SOURCE  // for asprintf()
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

static json_object_ptr fabricate_object(json_pull *jp, json_object *parent, json_type type) {
	auto o = std::make_shared<json_object>();
	o->type = type;
	o->parent = parent;
	o->parser = jp;
	return o;
}

static json_object_ptr add_object(json_pull *j, json_type type) {
	json_object *c = j->container.get();
	json_object_ptr o = fabricate_object(j, c, type);

	if (c != nullptr) {
		if (c->type == JSON_ARRAY) {
			if (c->expect == JSON_ITEM) {
				c->value.array.array.push_back(o);
				c->expect = JSON_COMMA;
			} else {
				j->error = "Expected a comma, not a list item";
				return nullptr;
			}
		} else if (c->type == JSON_HASH) {
			if (c->expect == JSON_VALUE) {
				c->value.object.values.back() = o;
				c->expect = JSON_COMMA;
			} else if (c->expect == JSON_KEY) {
				if (type != JSON_STRING) {
					j->error = "Hash key is not a string";
					return nullptr;
				}

				c->value.object.keys.push_back(o);
				c->value.object.values.push_back(nullptr);
				c->expect = JSON_COLON;
			} else {
				j->error = "Expected a comma or colon";
				return nullptr;
			}
		}
	} else {
		// Drop the previous top-level value; replacing the parser's root
		// shared_ptr will free it if no one else holds a reference.
		j->root = o;
	}

	return o;
}

json_object_ptr json_hash_get(json_object_ptr o, const char *s) {
	if (o == nullptr || o->type != JSON_HASH) {
		return nullptr;
	}

	for (size_t i = 0; i < o->value.object.keys.size(); i++) {
		const auto &key = o->value.object.keys[i];
		if (key != nullptr && key->type == JSON_STRING) {
			if (key->value.string.string == s) {
				return o->value.object.values[i];
			}
		}
	}

	return nullptr;
}

json_object_ptr json_read_separators(json_pull_ptr jp, json_separator_callback cb, void *state) {
	int c;
	json_pull *j = jp.get();

	// In case there is an error at the top level
	if (j->container == nullptr) {
		j->root.reset();
	}

again:
	c = read_wrap(j);
	if (c == EOF) {
		if (j->container != nullptr) {
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
		json_object_ptr o = add_object(j, JSON_ARRAY);
		if (o == nullptr) {
			return nullptr;
		}
		j->container = o;
		j->container->expect = JSON_ITEM;

		if (cb != nullptr) {
			cb(JSON_ARRAY, j, state);
		}

		goto again;
	}

	case ']': {
		if (j->container == nullptr) {
			j->error = "Found ] at top level";
			return nullptr;
		}

		if (j->container->type != JSON_ARRAY) {
			j->error = "Found ] not in an array";
			return nullptr;
		}

		if (j->container->expect != JSON_COMMA) {
			if (!(j->container->expect == JSON_ITEM && j->container->value.array.array.size() == 0)) {
				j->error = "Found ] without final element";
				return nullptr;
			}
		}

		json_object_ptr ret = j->container;
		// Walk up to the parent container. The parent (if any) still owns
		// `ret` via its own array vector, so the raw `parent` pointer is
		// still valid and we can resurrect a shared_ptr to it.
		if (ret->parent != nullptr) {
			j->container = ret->parent->shared_from_this();
		} else {
			j->container.reset();
		}
		return ret;
	}

		/////////////////////////// Hashes

	case '{': {
		json_object_ptr o = add_object(j, JSON_HASH);
		if (o == nullptr) {
			return nullptr;
		}
		j->container = o;
		j->container->expect = JSON_KEY;

		if (cb != nullptr) {
			cb(JSON_HASH, j, state);
		}

		goto again;
	}

	case '}': {
		if (j->container == nullptr) {
			j->error = "Found } at top level";
			return nullptr;
		}

		if (j->container->type != JSON_HASH) {
			j->error = "Found } not in a hash";
			return nullptr;
		}

		if (j->container->expect != JSON_COMMA) {
			if (!(j->container->expect == JSON_KEY && j->container->value.object.keys.size() == 0)) {
				j->error = "Found } without final element";
				return nullptr;
			}
		}

		json_object_ptr ret = j->container;
		if (ret->parent != nullptr) {
			j->container = ret->parent->shared_from_this();
		} else {
			j->container.reset();
		}
		return ret;
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
		if (j->container != nullptr) {
			if (j->container->expect != JSON_COMMA) {
				j->error = "Found unexpected comma";
				return nullptr;
			}

			if (j->container->type == JSON_HASH) {
				j->container->expect = JSON_KEY;
			} else {
				j->container->expect = JSON_ITEM;
			}
		}

		if (cb != nullptr) {
			cb(JSON_COMMA, j, state);
		}

		goto again;
	}

		/////////////////////////// Colon

	case ':': {
		if (j->container == nullptr) {
			j->error = "Found colon at top level";
			return nullptr;
		}

		if (j->container->expect != JSON_COLON) {
			j->error = "Found unexpected colon";
			return nullptr;
		}

		j->container->expect = JSON_VALUE;

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

		json_object_ptr n = add_object(j, JSON_NUMBER);
		if (n != nullptr) {
			n->value.number.number = atof(j->number_buffer.c_str());
			n->value.number.large_signed = 0;
			n->value.number.large_unsigned = 0;

#define MAX_SAFE_INTEGER 9007199254740991.0
#define MIN_SAFE_INTEGER -9007199254740991.0

			if (!decimal && n->value.number.number > MAX_SAFE_INTEGER) {
				errno = 0;
				char *err = nullptr;
				unsigned long long ull = strtoull(j->number_buffer.c_str(), &err, 10);
				if (errno == 0 && (err == nullptr || *err == '\0')) {
					n->value.number.large_unsigned = ull;
				}
			}
			if (!decimal && n->value.number.number < MIN_SAFE_INTEGER) {
				errno = 0;
				char *err = nullptr;
				long long ll = strtoll(j->number_buffer.c_str(), &err, 10);
				if (errno == 0 && (err == nullptr || *err == '\0')) {
					n->value.number.large_signed = ll;
				}
			}
		}
		return n;
	}

		/////////////////////////// Strings

	case '"': {
		std::string val;

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
					} else if (ch >= 0xdc00 && c <= 0xdfff) {
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
					} else if (ch < 0xFFFF) {
						val.push_back(0xE0 | (ch >> 12));
						val.push_back(0x80 | ((ch >> 6) & 0x3F));
						val.push_back(0x80 | (ch & 0x3F));
					} else {
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

		json_object_ptr s = add_object(j, JSON_STRING);
		if (s != nullptr) {
			s->value.string.string = std::move(val);
			s->value.string.refcon = nullptr;
		}
		return s;
	}
	}

	j->error = "Found unexpected character";
	return nullptr;
}

json_object_ptr json_read(json_pull_ptr j) {
	return json_read_separators(j, nullptr, nullptr);
}

json_object_ptr json_read_tree(json_pull_ptr p) {
	json_object_ptr j;

	while ((j = json_read(p)) != nullptr) {
		if (j->parent == nullptr) {
			return j;
		}
	}

	return nullptr;
}

void json_free(json_object_ptr &o) {
	o.reset();
}

// Walk the subtree clearing parent/parser back-pointers so the detached
// subtree can outlive the original parser.
static void clear_back_pointers(json_object *o) {
	if (o == nullptr) {
		return;
	}

	if (o->type == JSON_HASH) {
		for (size_t i = 0; i < o->value.object.keys.size(); i++) {
			clear_back_pointers(o->value.object.keys[i].get());
			clear_back_pointers(o->value.object.values[i].get());
		}
	} else if (o->type == JSON_ARRAY) {
		for (size_t i = 0; i < o->value.array.array.size(); i++) {
			clear_back_pointers(o->value.array.array[i].get());
		}
	}

	o->parent = nullptr;
	o->parser = nullptr;
}

void json_disconnect(json_object_ptr o) {
	if (o == nullptr) {
		return;
	}

	// Splice o out of its parent's array or object. The parent's vector
	// holds the shared_ptr to this child; erasing it removes one reference,
	// but the caller still holds `o`, so the subtree stays alive.

	json_object *parent = o->parent;
	if (parent != nullptr) {
		if (parent->type == JSON_ARRAY) {
			auto &arr = parent->value.array.array;
			for (size_t i = 0; i < arr.size(); i++) {
				if (arr[i].get() == o.get()) {
					arr.erase(arr.begin() + i);
					break;
				}
			}
		} else if (parent->type == JSON_HASH) {
			auto &keys = parent->value.object.keys;
			auto &vals = parent->value.object.values;

			for (size_t i = 0; i < keys.size(); i++) {
				if (keys[i].get() == o.get()) {
					// Leave a NULL placeholder in the key slot so the
					// surrounding value isn't shifted; if the corresponding
					// value is also detached the pair is removed below.
					keys[i] = fabricate_object(parent->parser, parent, JSON_NULL);

					if (vals[i] != nullptr && vals[i]->type == JSON_NULL && keys[i]->type == JSON_NULL) {
						keys.erase(keys.begin() + i);
						vals.erase(vals.begin() + i);
					}
					break;
				}
				if (vals[i].get() == o.get()) {
					vals[i] = fabricate_object(parent->parser, parent, JSON_NULL);

					if (keys[i] != nullptr && keys[i]->type == JSON_NULL && vals[i]->type == JSON_NULL) {
						keys.erase(keys.begin() + i);
						vals.erase(vals.begin() + i);
					}
					break;
				}
			}
		}
	}

	// Drop the parser's reference to this subtree if it was the root.
	json_pull *parser = o->parser;
	if (parser != nullptr && parser->root.get() == o.get()) {
		parser->root.reset();
	}

	clear_back_pointers(o.get());
}

static void string_append_c(std::string &val, char c) {
	val.push_back(c);
}

static void string_append(std::string &val, const char *add) {
	val.append(add);
}

static void json_print_one(std::string &val, json_object *o) {
	if (o == nullptr) {
		string_append(val, "...");
	} else if (o->type == JSON_STRING) {
		string_append_c(val, '\"');

		for (const char *cp = o->value.string.string.c_str(); *cp != '\0'; cp++) {
			if (*cp == '\\' || *cp == '"') {
				string_append_c(val, '\\');
				string_append_c(val, *cp);
			} else if (*cp >= 0 && *cp < ' ') {
				char *s;
				if (asprintf(&s, "\\u%04x", *cp) >= 0) {
					string_append(val, s);
					free(s);
				}
			} else {
				string_append_c(val, *cp);
			}
		}

		string_append_c(val, '\"');
	} else if (o->type == JSON_NUMBER) {
		if (o->value.number.large_signed != 0) {
			char s[65];
			snprintf(s, sizeof(s), "%lld", o->value.number.large_signed);
			string_append(val, s);
		} else if (o->value.number.large_unsigned != 0) {
			char s[65];
			snprintf(s, sizeof(s), "%llu", o->value.number.large_unsigned);
			string_append(val, s);
		} else {
			char *s = dtoa_milo(o->value.number.number);
			string_append(val, s);
			free(s);
		}
	} else if (o->type == JSON_NULL) {
		string_append(val, "null");
	} else if (o->type == JSON_TRUE) {
		string_append(val, "true");
	} else if (o->type == JSON_FALSE) {
		string_append(val, "false");
	} else if (o->type == JSON_HASH) {
		string_append_c(val, '}');
	} else if (o->type == JSON_ARRAY) {
		string_append_c(val, ']');
	}
}

static void json_print(std::string &val, json_object *o) {
	if (o == nullptr) {
		// Hash value in incompletely read hash
		string_append(val, "...");
	} else if (o->type == JSON_HASH) {
		string_append_c(val, '{');

		for (size_t i = 0; i < o->value.object.keys.size(); i++) {
			json_print(val, o->value.object.keys[i].get());
			string_append_c(val, ':');
			json_print(val, o->value.object.values[i].get());
			if (i + 1 < o->value.object.keys.size()) {
				string_append_c(val, ',');
			}
		}
		string_append_c(val, '}');
	} else if (o->type == JSON_ARRAY) {
		string_append_c(val, '[');
		for (size_t i = 0; i < o->value.array.array.size(); i++) {
			json_print(val, o->value.array.array[i].get());
			if (i + 1 < o->value.array.array.size()) {
				string_append_c(val, ',');
			}
		}
		string_append_c(val, ']');
	} else {
		json_print_one(val, o);
	}
}

std::string json_stringify(json_object_ptr o) {
	std::string val;
	json_print(val, o.get());
	return val;
}
