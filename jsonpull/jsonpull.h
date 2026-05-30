#ifndef JSONPULL_H
#define JSONPULL_H

#include <stdio.h>
#include <sys/types.h>
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

// json_object owns its descendants via std::shared_ptr in std::vector<>s,
// and keeps raw back-pointers to its parent and to the parser. The back-pointers
// remain valid as long as the node is attached to the tree (the parent is kept
// alive by holding a shared_ptr to this child, and the parser is kept alive by
// the caller's json_pull_ptr). json_disconnect() splices a node out of its
// parent and clears those back-pointers in the detached subtree, so the
// detached subtree can outlive the original parser.

struct json_object : public std::enable_shared_from_this<json_object> {
	json_object *parent = nullptr;
	json_pull *parser = nullptr;

	json_type type = JSON_NULL;
	int expect = 0;

	// Members named to match the previous C union layout so that existing
	// access paths like `o->value.string.string` and `o->value.array.array[i]`
	// continue to work. This is no longer a union because std::string and
	// std::vector have non-trivial destructors.
	struct value_t {
		struct {
			double number = 0;
			unsigned long long large_unsigned = 0;
			long long large_signed = 0;
		} number;

		struct {
			std::string string;
			void *refcon = nullptr;	 // reference constant for caller's use
		} string;

		struct {
			std::vector<json_object_ptr> array;
		} array;

		struct {
			std::vector<json_object_ptr> keys;
			std::vector<json_object_ptr> values;
		} object;
	} value;
};

struct json_pull {
	const char *error = nullptr;  // points at a string literal; no allocation
	int line = 1;

	ssize_t (*read)(struct json_pull *, char *buf, size_t n) = nullptr;
	void *source = nullptr;
	std::vector<char> buffer;
	ssize_t buffer_tail = 0;
	ssize_t buffer_head = 0;

	json_object_ptr container;
	json_object_ptr root;

	std::string number_buffer;
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

std::string json_stringify(json_object_ptr o);

#endif
