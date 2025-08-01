#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <functional>
#include "mvt.hpp"
#include "evaluator.hpp"
#include "errors.hpp"
#include "milo/dtoa_milo.h"
#include "text.hpp"

int compare(mvt_value const &one, json_object *two, bool &fail) {
	switch (one.type) {
	case mvt_string:
		if (two->type != JSON_STRING) {
			fail = true;
			return false;  // string vs non-string
		}

		return strcmp(one.c_str(), two->value.string.string);

	case mvt_double:
	case mvt_float:
	case mvt_int:
	case mvt_uint:
	case mvt_sint:
		if (two->type != JSON_NUMBER) {
			fail = true;
			return false;  // number vs non-number
		}

		double v;
		switch (one.type) {
		case mvt_double:
			v = one.numeric_value.double_value;
			break;
		case mvt_float:
			v = one.numeric_value.float_value;
			break;
		case mvt_int:
			v = one.numeric_value.int_value;
			break;
		case mvt_uint:
			v = one.numeric_value.uint_value;
			break;
		case mvt_sint:
			v = one.numeric_value.sint_value;
			break;
		case mvt_no_such_key:
		default:
			fprintf(stderr, "Internal error: bad mvt type %d\n", one.type);
			exit(EXIT_IMPOSSIBLE);
		}

		if (v < two->value.number.number) {
			return -1;
		} else if (v > two->value.number.number) {
			return 1;
		} else {
			return 0;
		}

	case mvt_bool:
		if (two->type != JSON_TRUE && two->type != JSON_FALSE) {
			fail = true;
			return false;  // bool vs non-bool
		}

		{
			bool b = two->type != JSON_FALSE;
			return one.numeric_value.bool_value > b;
		}

	case mvt_null:
		if (two->type != JSON_NULL) {
			fail = true;
			return false;  // null vs non-null
		}

		return 0;  // null equals null

	case mvt_no_such_key:
	default:
		break;
	}

	fprintf(stderr, "Internal error: bad mvt type %d\n", one.type);
	exit(EXIT_IMPOSSIBLE);
}

// 0: false
// 1: true
// -1: incomparable (sql null), treated as false in final output
static int eval(std::function<mvt_value(std::string const &)> feature, json_object *f, std::set<std::string> &exclude_attributes, std::vector<std::string> const &unidecode_data) {
	if (f != NULL) {
		if (f->type == JSON_TRUE) {
			return 1;
		} else if (f->type == JSON_FALSE) {
			return 0;
		} else if (f->type == JSON_NULL) {
			return 1;
		}

		if (f->type == JSON_NUMBER) {
			if (f->value.number.number == 0) {
				return 0;
			} else {
				return 1;
			}
		}

		if (f->type == JSON_STRING) {
			if (f->value.string.string[0] == '\0') {
				return 0;
			} else {
				return 1;
			}
		}
	}

	if (f == NULL || f->type != JSON_ARRAY) {
		fprintf(stderr, "Filter is not an array: %s\n", json_stringify(f));
		exit(EXIT_FILTER);
	}

	if (f->value.array.length < 1) {
		fprintf(stderr, "Array too small in filter: %s\n", json_stringify(f));
		exit(EXIT_FILTER);
	}

	if (f->value.array.array[0]->type != JSON_STRING) {
		fprintf(stderr, "Filter operation is not a string: %s\n", json_stringify(f));
		exit(EXIT_FILTER);
	}

	if (strcmp(f->value.array.array[0]->value.string.string, "has") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, "!has") == 0) {
		if (f->value.array.length != 2) {
			fprintf(stderr, "Wrong number of array elements in filter: %s\n", json_stringify(f));
			exit(EXIT_FILTER);
		}

		if (strcmp(f->value.array.array[0]->value.string.string, "has") == 0) {
			if (f->value.array.array[1]->type != JSON_STRING) {
				fprintf(stderr, "\"has\" key is not a string: %s\n", json_stringify(f));
				exit(EXIT_FILTER);
			}
			return feature(std::string(f->value.array.array[1]->value.string.string)).type != mvt_no_such_key;
		}

		if (strcmp(f->value.array.array[0]->value.string.string, "!has") == 0) {
			if (f->value.array.array[1]->type != JSON_STRING) {
				fprintf(stderr, "\"!has\" key is not a string: %s\n", json_stringify(f));
				exit(EXIT_FILTER);
			}
			return feature(std::string(f->value.array.array[1]->value.string.string)).type == mvt_no_such_key;
		}
	}

	if (strcmp(f->value.array.array[0]->value.string.string, "==") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, "!=") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, ">") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, ">=") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, "<") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, "<=") == 0) {
		if (f->value.array.length != 3) {
			fprintf(stderr, "Wrong number of array elements in filter: %s\n", json_stringify(f));
			exit(EXIT_FILTER);
		}
		if (f->value.array.array[1]->type != JSON_STRING) {
			fprintf(stderr, "comparison key is not a string: %s\n", json_stringify(f));
			exit(EXIT_FILTER);
		}

		mvt_value ff = feature(std::string(f->value.array.array[1]->value.string.string));
		if (ff.type == mvt_no_such_key) {
			static bool warned = false;
			if (!warned) {
				const char *s = json_stringify(f);
				fprintf(stderr, "Warning: attribute not found for comparison: %s\n", s);
				free((void *) s);
				warned = true;
			}
			if (strcmp(f->value.array.array[0]->value.string.string, "!=") == 0) {
				return true;  //  attributes that aren't found are not equal
			}
			return false;  // not found: comparison is false
		}

		bool fail = false;
		int cmp = compare(ff, f->value.array.array[2], fail);

		if (fail) {
			static bool warned = false;
			if (!warned) {
				const char *s = json_stringify(f);
				fprintf(stderr, "Warning: mismatched type in comparison: %s\n", s);
				free((void *) s);
				warned = true;
			}
			if (strcmp(f->value.array.array[0]->value.string.string, "!=") == 0) {
				return true;  // mismatched types are not equal
			}
			return false;
		}

		if (strcmp(f->value.array.array[0]->value.string.string, "==") == 0) {
			return cmp == 0;
		}
		if (strcmp(f->value.array.array[0]->value.string.string, "!=") == 0) {
			return cmp != 0;
		}
		if (strcmp(f->value.array.array[0]->value.string.string, ">") == 0) {
			return cmp > 0;
		}
		if (strcmp(f->value.array.array[0]->value.string.string, ">=") == 0) {
			return cmp >= 0;
		}
		if (strcmp(f->value.array.array[0]->value.string.string, "<") == 0) {
			return cmp < 0;
		}
		if (strcmp(f->value.array.array[0]->value.string.string, "<=") == 0) {
			return cmp <= 0;
		}

		fprintf(stderr, "Internal error: can't happen: %s\n", json_stringify(f));
		exit(EXIT_IMPOSSIBLE);
	}

	if (strcmp(f->value.array.array[0]->value.string.string, "all") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, "any") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, "none") == 0) {
		bool v;

		if (strcmp(f->value.array.array[0]->value.string.string, "all") == 0) {
			v = true;
		} else {
			v = false;
		}

		for (size_t i = 1; i < f->value.array.length; i++) {
			int out = eval(feature, f->value.array.array[i], exclude_attributes, unidecode_data);

			if (out >= 0) {	 // nulls are ignored in boolean and/or expressions
				if (strcmp(f->value.array.array[0]->value.string.string, "all") == 0) {
					v = v && out;
					if (!v) {
						break;
					}
				} else {
					v = v || out;
					if (v) {
						break;
					}
				}
			}
		}

		if (strcmp(f->value.array.array[0]->value.string.string, "none") == 0) {
			return !v;
		} else {
			return v;
		}
	}

	if (strcmp(f->value.array.array[0]->value.string.string, "in") == 0 ||
	    strcmp(f->value.array.array[0]->value.string.string, "!in") == 0) {
		if (f->value.array.length < 2) {
			fprintf(stderr, "Array too small in filter: %s\n", json_stringify(f));
			exit(EXIT_FILTER);
		}

		if (f->value.array.array[1]->type != JSON_STRING) {
			fprintf(stderr, "\"!in\" key is not a string: %s\n", json_stringify(f));
			exit(EXIT_FILTER);
		}

		mvt_value ff = feature(std::string(f->value.array.array[1]->value.string.string));
		if (ff.type == mvt_no_such_key) {
			static bool warned = false;
			if (!warned) {
				const char *s = json_stringify(f);
				fprintf(stderr, "Warning: attribute not found for comparison: %s\n", s);
				free((void *) s);
				warned = true;
			}
			if (strcmp(f->value.array.array[0]->value.string.string, "!in") == 0) {
				return true;  // attributes that aren't found are not in
			}
			return false;  // not found: comparison is false
		}

		bool found = false;
		for (size_t i = 2; i < f->value.array.length; i++) {
			bool fail = false;
			int cmp = compare(ff, f->value.array.array[i], fail);

			if (fail) {
				static bool warned = false;
				if (!warned) {
					const char *s = json_stringify(f);
					fprintf(stderr, "Warning: mismatched type in comparison: %s\n", s);
					free((void *) s);
					warned = true;
				}
				cmp = 1;
			}

			if (cmp == 0) {
				found = true;
				break;
			}
		}

		if (strcmp(f->value.array.array[0]->value.string.string, "in") == 0) {
			return found;
		} else {
			return !found;
		}
	}

	if (strcmp(f->value.array.array[0]->value.string.string, "attribute-filter") == 0) {
		if (f->value.array.length != 3) {
			fprintf(stderr, "Wrong number of array elements in filter: %s\n", json_stringify(f));
			exit(EXIT_FILTER);
		}

		if (f->value.array.array[1]->type != JSON_STRING) {
			fprintf(stderr, "\"attribute-filter\" key is not a string: %s\n", json_stringify(f));
			exit(EXIT_FILTER);
		}

		bool ok = eval(feature, f->value.array.array[2], exclude_attributes, unidecode_data) > 0;
		if (!ok) {
			exclude_attributes.insert(f->value.array.array[1]->value.string.string);
		}

		return true;
	}

	fprintf(stderr, "Unknown filter %s\n", json_stringify(f));
	exit(EXIT_FILTER);
}

bool evaluate(std::function<mvt_value(std::string const &)> feature, std::string const &layer, json_object *filter, std::set<std::string> &exclude_attributes, std::vector<std::string> const &unidecode_data) {
	if (filter == NULL || filter->type != JSON_HASH) {
		fprintf(stderr, "Error: filter is not a hash: %s\n", json_stringify(filter));
		exit(EXIT_JSON);
	}

	bool ok = true;
	json_object *f;

	f = json_hash_get(filter, layer.c_str());
	if (ok && f != NULL) {
		ok = eval(feature, f, exclude_attributes, unidecode_data) > 0;
	}

	f = json_hash_get(filter, "*");
	if (ok && f != NULL) {
		ok = eval(feature, f, exclude_attributes, unidecode_data) > 0;
	}

	return ok;
}

json_object *read_filter(const char *fname) {
	FILE *fp = fopen(fname, "r");
	if (fp == NULL) {
		perror(fname);
		exit(EXIT_OPEN);
	}

	json_pull *jp = json_begin_file(fp);
	json_object *filter = json_read_tree(jp);
	if (filter == NULL) {
		fprintf(stderr, "%s: %s\n", fname, jp->error);
		exit(EXIT_JSON);
	}
	json_disconnect(filter);
	json_end(jp);
	fclose(fp);
	return filter;
}

json_object *parse_filter(const char *s) {
	json_pull *jp = json_begin_string(s);
	json_object *filter = json_read_tree(jp);
	if (filter == NULL) {
		fprintf(stderr, "Could not parse filter %s\n", s);
		fprintf(stderr, "%s\n", jp->error);
		exit(EXIT_JSON);
	}
	json_disconnect(filter);
	json_end(jp);
	return filter;
}

bool evaluate(std::unordered_map<std::string, mvt_value> const &feature, std::string const &layer, json_object *filter, std::set<std::string> &exclude_attributes, std::vector<std::string> const &unidecode_data) {
	std::function<mvt_value(std::string const &)> getter = [&](std::string const &key) {
		auto f = feature.find(key);
		if (f != feature.end()) {
			return f->second;
		} else {
			mvt_value v;
			v.type = mvt_no_such_key;
			v.numeric_value.null_value = 0;
			return v;
		}
	};

	return evaluate(getter, layer, filter, exclude_attributes, unidecode_data);
}

bool evaluate(mvt_feature const &feat, mvt_layer const &layer, json_object *filter, std::set<std::string> &exclude_attributes, int z, std::vector<std::string> const &unidecode_data) {
	std::function<mvt_value(std::string const &)> getter = [&](std::string const &key) {
		const static std::string dollar_id = "$id";
		if (key == dollar_id && feat.has_id) {
			mvt_value v;
			v.type = mvt_uint;
			v.numeric_value.uint_value = feat.id;
			return v;
		}

		const static std::string dollar_type = "$type";
		if (key == dollar_type) {
			mvt_value v;
			v.type = mvt_string;

			if (feat.type == mvt_point) {
				const static std::string point = "Point";
				v.set_string_value(point);
			} else if (feat.type == mvt_linestring) {
				const static std::string linestring = "LineString";
				v.set_string_value(linestring);
			} else if (feat.type == mvt_polygon) {
				const static std::string polygon = "Polygon";
				v.set_string_value(polygon);
			}
			return v;
		}

		const static std::string dollar_zoom = "$zoom";
		if (key == dollar_zoom) {
			mvt_value v2;
			v2.type = mvt_uint;
			v2.numeric_value.uint_value = z;
			return v2;
		}

		for (size_t i = 0; i + 1 < feat.tags.size(); i += 2) {
			if (layer.keys[feat.tags[i]] == key) {
				return layer.values[feat.tags[i + 1]];
			}
		}

		mvt_value v;
		v.type = mvt_no_such_key;
		v.numeric_value.null_value = 0;
		return v;
	};

	return evaluate(getter, layer.name, filter, exclude_attributes, unidecode_data);
}
