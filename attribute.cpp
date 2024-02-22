#include <string>
#include <unordered_map>
#include "attribute.hpp"
#include "errors.hpp"
#include "serial.hpp"
#include "jsonpull/jsonpull.h"
#include "milo/dtoa_milo.h"

void set_attribute_accum(std::unordered_map<std::string, attribute_op> &attribute_accum, std::string name, std::string type) {
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

void set_attribute_accum(std::unordered_map<std::string, attribute_op> &attribute_accum, const char *arg, char **argv) {
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

void preserve_attribute(attribute_op const &op, std::string const &key, serial_val const &val, std::vector<std::string> &full_keys, std::vector<serial_val> &full_values, std::unordered_map<std::string, accum_state> &attribute_accum_state) {
	for (size_t i = 0; i < full_keys.size(); i++) {
		if (key == full_keys[i]) {
			switch (op) {
			case op_sum:
				full_values[i].s = milo::dtoa_milo(atof(full_values[i].s.c_str()) + atof(val.s.c_str()));
				full_values[i].type = mvt_double;
				break;

			case op_product:
				full_values[i].s = milo::dtoa_milo(atof(full_values[i].s.c_str()) * atof(val.s.c_str()));
				full_values[i].type = mvt_double;
				break;

			case op_max: {
				double existing = atof(full_values[i].s.c_str());
				double maybe = atof(val.s.c_str());
				if (maybe > existing) {
					full_values[i].s = val.s.c_str();
					full_values[i].type = mvt_double;
				}
				break;
			}

			case op_min: {
				double existing = atof(full_values[i].s.c_str());
				double maybe = atof(val.s.c_str());
				if (maybe < existing) {
					full_values[i].s = val.s.c_str();
					full_values[i].type = mvt_double;
				}
				break;
			}

			case op_mean: {
				auto state = attribute_accum_state.find(key);
				if (state == attribute_accum_state.end()) {
					accum_state s;
					s.sum = atof(full_values[i].s.c_str()) + atof(val.s.c_str());
					s.count = 2;
					attribute_accum_state.insert(std::pair<std::string, accum_state>(key, s));

					full_values[i].s = milo::dtoa_milo(s.sum / s.count);
				} else {
					state->second.sum += atof(val.s.c_str());
					state->second.count += 1;

					full_values[i].s = milo::dtoa_milo(state->second.sum / state->second.count);
				}
				break;
			}

			case op_concat:
				full_values[i].s += val.s;
				full_values[i].type = mvt_string;
				break;

			case op_comma:
				full_values[i].s += std::string(",") + val.s;
				full_values[i].type = mvt_string;
				break;
			}
		}
	}
}
