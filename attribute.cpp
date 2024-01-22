#include <string>
#include <map>
#include "attribute.hpp"
#include "errors.hpp"
#include "jsonpull/jsonpull.h"

void set_attribute_accum(std::map<std::string, attribute_op> &attribute_accum, std::string name, std::string type) {
	attribute_op t;

	if (type == "sum") {
		t = op_sum;
	} else if (type == "product") {
		t = op_product;
	} else if (type == "mean") {
		t = op_mean;
	} else if (type == "max") {
		t = op_max;
	} else if (type == "min") {
		t = op_min;
	} else if (type == "concat") {
		t = op_concat;
	} else if (type == "comma") {
		t = op_comma;
	} else {
		fprintf(stderr, "Attribute method (%s) must be sum, product, mean, max, min, concat, or comma\n", type.c_str());
		exit(EXIT_ARGS);
	}

	attribute_accum.insert(std::pair<std::string, attribute_op>(name, t));
}

void set_attribute_accum(std::map<std::string, attribute_op> &attribute_accum, const char *arg, char **argv) {
	if (*arg == '{') {
		json_pull *jp = json_begin_string(arg);
		json_object *o = json_read_tree(jp);

		if (o == NULL) {
			fprintf(stderr, "%s: -E%s: %s\n", *argv, arg, jp->error);
			exit(EXIT_JSON);
		}

		if (o->type != JSON_HASH) {
			fprintf(stderr, "%s: -E%s: not a JSON object\n", *argv, arg);
			exit(EXIT_JSON);
		}

		for (size_t i = 0; i < o->value.object.length; i++) {
			json_object *k = o->value.object.keys[i];
			json_object *v = o->value.object.values[i];

			if (k->type != JSON_STRING) {
				fprintf(stderr, "%s: -E%s: key %zu not a string\n", *argv, arg, i);
				exit(EXIT_JSON);
			}
			if (v->type != JSON_STRING) {
				fprintf(stderr, "%s: -E%s: value %zu not a string\n", *argv, arg, i);
				exit(EXIT_JSON);
			}

			set_attribute_accum(attribute_accum, k->value.string.string, v->value.string.string);
		}

		json_free(o);
		json_end(jp);
		return;
	}

	const char *s = strchr(arg, ':');
	if (s == NULL) {
		fprintf(stderr, "-E%s option must be in the form -Ename:method\n", arg);
		exit(EXIT_ARGS);
	}

	std::string name = std::string(arg, s - arg);
	std::string type = std::string(s + 1);

	set_attribute_accum(attribute_accum, name, type);
}
