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

// Construct an instance of the right subclass for the given type.
// JSON_TRUE / JSON_FALSE / JSON_NULL and the parse-token types are bare
// json_objects; the value-bearing types each get their own subclass.
static json_object_ptr make_object(json_type type, json_object *parent, json_pull *jp) {
	json_object_ptr o;
	switch (type) {
	case JSON_NUMBER:
		o = std::make_shared<json_number>(parent, jp);
		break;
	case JSON_STRING:
		o = std::make_shared<json_string>(parent, jp);
		break;
	case JSON_ARRAY:
		o = std::make_shared<json_array>(parent, jp);
		break;
	case JSON_HASH:
		o = std::make_shared<json_hash>(parent, jp);
		break;
	default:
		o = std::make_shared<json_object>(type, parent, jp);
		break;
	}
	return o;
}

static json_object_ptr fabricate_object(json_pull *jp, json_object *parent, json_type type) {
	return make_object(type, parent, jp);
}

static inline json_pull::parse_frame *current_frame(json_pull *j) {
	return j->container_stack.empty() ? nullptr : &j->container_stack.back();
}

static json_object_ptr add_object(json_pull *j, json_type type) {
	json_pull::parse_frame *f = current_frame(j);
	json_object *c = f ? f->container.get() : nullptr;
	json_object_ptr o = make_object(type, c, j);

	if (f != nullptr) {
		if (c->type == JSON_ARRAY) {
			if (f->expect == JSON_ITEM) {
				c->array().push_back(o);
				f->expect = JSON_COMMA;
			} else {
				j->error = "Expected a comma, not a list item";
				return nullptr;
			}
		} else if (c->type == JSON_HASH) {
			if (f->expect == JSON_VALUE) {
				c->entries().back().value = o;
				f->expect = JSON_COMMA;
			} else if (f->expect == JSON_KEY) {
				if (type != JSON_STRING) {
					j->error = "Hash key is not a string";
					return nullptr;
				}

				c->entries().push_back({o, nullptr});
				f->expect = JSON_COLON;
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

json_object_ptr json_hash_get(json_object *o, const char *s) {
	if (o == nullptr || o->type != JSON_HASH) {
		return nullptr;
	}

	for (const auto &e : o->entries()) {
		if (e.key != nullptr && e.key->type == JSON_STRING && e.key->string() == s) {
			return e.value;
		}
	}

	return nullptr;
}

json_object_ptr json_hash_get(json_object_ptr o, const char *s) {
	return json_hash_get(o.get(), s);
}

json_object_ptr json_read_separators(json_pull_ptr jp, json_separator_callback cb, void *state) {
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
		json_object_ptr o = add_object(j, JSON_ARRAY);
		if (o == nullptr) {
			return nullptr;
		}
		// add_object already installed `o` in the parent (or the
		// parser's root); moving the local copy into the frame
		// avoids one shared_ptr atomic inc/dec pair per container.
		j->container_stack.push_back({std::move(o), JSON_ITEM});

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

		json_object *cc = f->container.get();
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

		// Move the container out of the frame so pop_back doesn't
		// drop the last reference; saves one atomic inc/dec.
		json_object_ptr ret = std::move(f->container);
		j->container_stack.pop_back();
		return ret;
	}

		/////////////////////////// Hashes

	case '{': {
		json_object_ptr o = add_object(j, JSON_HASH);
		if (o == nullptr) {
			return nullptr;
		}
		// See the [ case above: move into the frame to skip a
		// shared_ptr atomic inc/dec round-trip.
		j->container_stack.push_back({std::move(o), JSON_KEY});

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

		json_object *cc = f->container.get();
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

		// See the ] case: move out to skip an atomic refcount round-trip.
		json_object_ptr ret = std::move(f->container);
		j->container_stack.pop_back();
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

		json_object_ptr n = add_object(j, JSON_NUMBER);
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
		// Reuse the parser-wide string buffer so we don't construct a
		// fresh std::string (with its inevitable SSO->heap promotion
		// and capacity doublings) for every JSON_STRING token.
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
			// Copy (don't move) so j->string_buffer retains its
			// grown capacity for the next token. The copy is a
			// single right-sized allocation plus one memcpy, which
			// is cheaper than the multiple capacity doublings the
			// per-token std::string would otherwise incur.
			s->string() = val;
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
		for (const auto &e : o->entries()) {
			clear_back_pointers(e.key.get());
			clear_back_pointers(e.value.get());
		}
	} else if (o->type == JSON_ARRAY) {
		const auto &arr = o->array();
		for (size_t i = 0; i < arr.size(); i++) {
			clear_back_pointers(arr[i].get());
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
			auto &arr = parent->array();
			for (size_t i = 0; i < arr.size(); i++) {
				if (arr[i].get() == o.get()) {
					arr.erase(arr.begin() + i);
					break;
				}
			}
		} else if (parent->type == JSON_HASH) {
			auto &entries = parent->entries();

			for (size_t i = 0; i < entries.size(); i++) {
				auto &e = entries[i];
				if (e.key.get() == o.get()) {
					// Leave a NULL placeholder in the key slot so the
					// surrounding value isn't shifted; if the corresponding
					// value is also detached the pair is removed below.
					e.key = fabricate_object(parent->parser, parent, JSON_NULL);

					if (e.value != nullptr && e.value->type == JSON_NULL && e.key->type == JSON_NULL) {
						entries.erase(entries.begin() + i);
					}
					break;
				}
				if (e.value.get() == o.get()) {
					e.value = fabricate_object(parent->parser, parent, JSON_NULL);

					if (e.key != nullptr && e.key->type == JSON_NULL && e.value->type == JSON_NULL) {
						entries.erase(entries.begin() + i);
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

		for (const char *cp = o->string().c_str(); *cp != '\0'; cp++) {
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
		if (o->large_signed() != 0) {
			char s[65];
			snprintf(s, sizeof(s), "%lld", o->large_signed());
			string_append(val, s);
		} else if (o->large_unsigned() != 0) {
			char s[65];
			snprintf(s, sizeof(s), "%llu", o->large_unsigned());
			string_append(val, s);
		} else {
			char *s = dtoa_milo(o->number());
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

		const auto &entries = o->entries();
		for (size_t i = 0; i < entries.size(); i++) {
			json_print(val, entries[i].key.get());
			string_append_c(val, ':');
			json_print(val, entries[i].value.get());
			if (i + 1 < entries.size()) {
				string_append_c(val, ',');
			}
		}
		string_append_c(val, '}');
	} else if (o->type == JSON_ARRAY) {
		string_append_c(val, '[');
		const auto &arr = o->array();
		for (size_t i = 0; i < arr.size(); i++) {
			json_print(val, arr[i].get());
			if (i + 1 < arr.size()) {
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
