#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include "mvt.hpp"
#include "evaluator.hpp"
#include "errors.hpp"

static std::string mvt_value_to_string(mvt_value one, bool &fail) {
	if (one.type == mvt_string) {
		return one.string_value;
	} else if (one.type == mvt_float) {
		return std::to_string(one.numeric_value.float_value);
	} else if (one.type == mvt_double) {
		return std::to_string(one.numeric_value.double_value);
	} else if (one.type == mvt_int) {
		return std::to_string(one.numeric_value.int_value);
	} else if (one.type == mvt_uint) {
		return std::to_string(one.numeric_value.uint_value);
	} else if (one.type == mvt_sint) {
		return std::to_string(one.numeric_value.sint_value);
	} else if (one.type == mvt_bool) {
		return one.numeric_value.bool_value ? "true" : "false";
	} else if (one.type == mvt_null) {
		fail = true;  // null op string => null
		return "";
	} else {
		fprintf(stderr, "unhandled mvt_type %d\n", one.type);
		exit(EXIT_IMPOSSIBLE);
	}
}

int compare_fsl(mvt_value one, json_object *two, bool &fail) {
	// In FSL expressions, the attribute value is coerced to the type
	// of the JSON literal value it is being compared to.
	//
	// If it cannot be converted, the comparison returns null
	// (which is distinct from true and false but is falsy if
	// it is the final output value).

	if (two->type == JSON_NULL) {
		fail = true;  // anything op null => null
		return 0;
	}

	if (two->type == JSON_NUMBER) {
		double lhs;

		if (one.type == mvt_string) {
			char *endptr = NULL;
			const char *s = one.string_value.c_str();
			lhs = strtod(s, &endptr);
			if (endptr == s) {
				fail = true;  // non-numeric-string op number => null
				return 0;
			}
		} else if (one.type == mvt_float) {
			lhs = one.numeric_value.float_value;
		} else if (one.type == mvt_double) {
			lhs = one.numeric_value.double_value;
		} else if (one.type == mvt_int) {
			lhs = one.numeric_value.int_value;
		} else if (one.type == mvt_uint) {
			lhs = one.numeric_value.uint_value;
		} else if (one.type == mvt_sint) {
			lhs = one.numeric_value.sint_value;
		} else if (one.type == mvt_bool) {
			lhs = one.numeric_value.bool_value;
		} else if (one.type == mvt_null) {
			fail = true;  // null op number => null
			return 0;
		} else {
			fprintf(stderr, "unhandled mvt_type %d\n", one.type);
			exit(EXIT_IMPOSSIBLE);
		}

		if (lhs < two->value.number.number) {
			return -1;
		} else if (lhs > two->value.number.number) {
			return 1;
		} else {
			return 0;
		}
	}

	if (two->type == JSON_STRING) {
		std::string lhs = mvt_value_to_string(one, fail);

		return strcmp(lhs.c_str(), two->value.string.string);
	}

	if (two->type == JSON_TRUE || two->type == JSON_FALSE) {
		bool lhs;

		if (one.type == mvt_string) {
			lhs = one.string_value.size() > 0;
		} else if (one.type == mvt_float) {
			lhs = one.numeric_value.float_value != 0;
		} else if (one.type == mvt_double) {
			lhs = one.numeric_value.double_value != 0;
		} else if (one.type == mvt_int) {
			lhs = one.numeric_value.int_value != 0;
		} else if (one.type == mvt_uint) {
			lhs = one.numeric_value.uint_value != 0;
		} else if (one.type == mvt_sint) {
			lhs = one.numeric_value.sint_value != 0;
		} else if (one.type == mvt_bool) {
			lhs = one.numeric_value.bool_value;
		} else if (one.type == mvt_null) {
			fail = true;  // null op bool => null
			return 0;
		} else {
			fprintf(stderr, "unhandled mvt_type %d\n", one.type);
			exit(EXIT_IMPOSSIBLE);
		}

		bool rhs = two->type == JSON_TRUE;
		return lhs - rhs;
	}

	fprintf(stderr, "unhandled JSON type %d\n", two->type);
	exit(EXIT_IMPOSSIBLE);
}

int compare(mvt_value one, json_object *two, bool &fail) {
	if (one.type == mvt_string) {
		if (two->type != JSON_STRING) {
			fail = true;
			return false;  // string vs non-string
		}

		return strcmp(one.string_value.c_str(), two->value.string.string);
	}

	if (one.type == mvt_double || one.type == mvt_float || one.type == mvt_int || one.type == mvt_uint || one.type == mvt_sint) {
		if (two->type != JSON_NUMBER) {
			fail = true;
			return false;  // number vs non-number
		}

		double v;
		if (one.type == mvt_double) {
			v = one.numeric_value.double_value;
		} else if (one.type == mvt_float) {
			v = one.numeric_value.float_value;
		} else if (one.type == mvt_int) {
			v = one.numeric_value.int_value;
		} else if (one.type == mvt_uint) {
			v = one.numeric_value.uint_value;
		} else if (one.type == mvt_sint) {
			v = one.numeric_value.sint_value;
		} else {
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
	}

	if (one.type == mvt_bool) {
		if (two->type != JSON_TRUE && two->type != JSON_FALSE) {
			fail = true;
			return false;  // bool vs non-bool
		}

		bool b = two->type != JSON_FALSE;
		return one.numeric_value.bool_value > b;
	}

	if (one.type == mvt_null) {
		if (two->type != JSON_NULL) {
			fail = true;
			return false;  // null vs non-null
		}

		return 0;  // null equals null
	}

	fprintf(stderr, "Internal error: bad mvt type %d\n", one.type);
	exit(EXIT_IMPOSSIBLE);
}

// 0: false
// 1: true
// -1: incomparable (sql null), treated as false in final output
static int eval(std::map<std::string, mvt_value> const &feature, json_object *f, std::set<std::string> &exclude_attributes) {
	if (f != NULL) {
		if (f->type == JSON_TRUE) {
			return 1;
		} else if (f->type == JSON_FALSE) {
			return 0;
		} else if (f->type == JSON_NULL) {
			return -1;
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

	// FSL conjunctions
	if (f->value.array.length == 3 &&
	    f->value.array.array[1]->type == JSON_STRING &&
	    (strcmp(f->value.array.array[1]->value.string.string, "or") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "and") == 0)) {
		int lhs = eval(feature, f->value.array.array[0], exclude_attributes);
		int rhs = eval(feature, f->value.array.array[2], exclude_attributes);
		if (lhs < 0 || rhs < 0) {
			return -1;  // null op anything => null, anything op null => null
		}
		if (strcmp(f->value.array.array[1]->value.string.string, "or") == 0) {
			return lhs || rhs;
		} else {
			return lhs && rhs;
		}
	}

	// FSL comparators
	if (f->value.array.length == 3 &&
	    f->value.array.array[0]->type == JSON_STRING &&
	    f->value.array.array[1]->type == JSON_STRING &&
	    (strcmp(f->value.array.array[1]->value.string.string, "lt") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "gt") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "le") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "ge") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "eq") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "ne") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "cn") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "nc") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "in") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "ni") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "is") == 0 ||
	     strcmp(f->value.array.array[1]->value.string.string, "isnt") == 0 ||
	     false)) {
		mvt_value lhs;
		lhs.type = mvt_null;  // attributes that aren't found are nulls
		auto ff = feature.find(std::string(f->value.array.array[0]->value.string.string));
		if (ff != feature.end()) {
			lhs = ff->second;
		}

		if (f->value.array.array[2]->type == JSON_NULL && strcmp(f->value.array.array[1]->value.string.string, "is") == 0) {
			return lhs.type == mvt_null;  // null is null => true, anything is null => false
		}
		if (f->value.array.array[2]->type == JSON_NULL && strcmp(f->value.array.array[1]->value.string.string, "isnt") == 0) {
			return lhs.type != mvt_null;  // null isnt null => false, anything isnt null => true
		}

		if (lhs.type == mvt_null) {
			return -1;  // null op anything => null
		}

		bool fail = false;

		if (f->value.array.array[2]->type == JSON_STRING &&
		    (strcmp(f->value.array.array[1]->value.string.string, "cn") == 0 ||
		     strcmp(f->value.array.array[1]->value.string.string, "nc") == 0)) {
			std::string s = mvt_value_to_string(lhs, fail);
			if (fail) {
				return -1;  // null cn anything => false
			}

			bool contains = strstr(s.c_str(), f->value.array.array[2]->value.string.string);
			if (strcmp(f->value.array.array[1]->value.string.string, "cn") == 0) {
				return contains;
			} else {
				return !contains;
			}
		}

		if (f->value.array.array[2]->type == JSON_ARRAY &&
		    (strcmp(f->value.array.array[1]->value.string.string, "in") == 0 ||
		     strcmp(f->value.array.array[1]->value.string.string, "ni") == 0)) {
			std::string s = mvt_value_to_string(lhs, fail);
			if (fail) {
				return -1;  // null in anything => false
			}

			bool contains = false;
			for (size_t i = 0; i < f->value.array.array[2]->value.array.length; i++) {
				if (f->value.array.array[2]->value.array.array[i]->type != JSON_STRING) {
					return -1;  // anything in [not-a-string] => null
				}

				if (s == f->value.array.array[2]->value.array.array[i]->value.string.string) {
					contains = true;
					break;
				}
			}

			if (strcmp(f->value.array.array[1]->value.string.string, "in") == 0) {
				return contains;
			} else {
				return !contains;
			}
		}

		int cmp = compare_fsl(ff->second, f->value.array.array[2], fail);
		if (fail) {
			printf("cast fail\n");
			return -1;  // null
		}

		if (strcmp(f->value.array.array[1]->value.string.string, "eq") == 0) {
			return cmp == 0;
		}
		if (strcmp(f->value.array.array[1]->value.string.string, "ne") == 0) {
			return cmp != 0;
		}
		if (strcmp(f->value.array.array[1]->value.string.string, "gt") == 0) {
			return cmp > 0;
		}
		if (strcmp(f->value.array.array[1]->value.string.string, "ge") == 0) {
			return cmp >= 0;
		}
		if (strcmp(f->value.array.array[1]->value.string.string, "lt") == 0) {
			return cmp < 0;
		}
		if (strcmp(f->value.array.array[1]->value.string.string, "le") == 0) {
			return cmp <= 0;
		}

		fprintf(stderr, "expression fsl comparison: can't happen %s\n", f->value.array.array[1]->value.string.string);
		exit(EXIT_IMPOSSIBLE);
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
			return feature.count(std::string(f->value.array.array[1]->value.string.string)) != 0;
		}

		if (strcmp(f->value.array.array[0]->value.string.string, "!has") == 0) {
			if (f->value.array.array[1]->type != JSON_STRING) {
				fprintf(stderr, "\"!has\" key is not a string: %s\n", json_stringify(f));
				exit(EXIT_FILTER);
			}
			return feature.count(std::string(f->value.array.array[1]->value.string.string)) == 0;
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

		auto ff = feature.find(std::string(f->value.array.array[1]->value.string.string));
		if (ff == feature.end()) {
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
		int cmp = compare(ff->second, f->value.array.array[2], fail);

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
			int out = eval(feature, f->value.array.array[i], exclude_attributes);

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

		auto ff = feature.find(std::string(f->value.array.array[1]->value.string.string));
		if (ff == feature.end()) {
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
			int cmp = compare(ff->second, f->value.array.array[i], fail);

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

		bool ok = eval(feature, f->value.array.array[2], exclude_attributes) > 0;
		if (!ok) {
			exclude_attributes.insert(f->value.array.array[1]->value.string.string);
		}

		return true;
	}

	fprintf(stderr, "Unknown filter %s\n", json_stringify(f));
	exit(EXIT_FILTER);
}

bool evaluate(std::map<std::string, mvt_value> const &feature, std::string const &layer, json_object *filter, std::set<std::string> &exclude_attributes) {
	if (filter == NULL || filter->type != JSON_HASH) {
		fprintf(stderr, "Error: filter is not a hash: %s\n", json_stringify(filter));
		exit(EXIT_JSON);
	}

	bool ok = true;
	json_object *f;

	f = json_hash_get(filter, layer.c_str());
	if (ok && f != NULL) {
		ok = eval(feature, f, exclude_attributes) > 0;
	}

	f = json_hash_get(filter, "*");
	if (ok && f != NULL) {
		ok = eval(feature, f, exclude_attributes) > 0;
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

bool evaluate(mvt_feature const &feat, mvt_layer const &layer, json_object *filter, std::set<std::string> &exclude_attributes, int z) {
	if (filter != NULL) {
		std::map<std::string, mvt_value> attributes;

		for (size_t t = 0; t + 1 < feat.tags.size(); t += 2) {
			std::string key = layer.keys[feat.tags[t]];
			const mvt_value &val = layer.values[feat.tags[t + 1]];

			attributes.insert(std::pair<std::string, mvt_value>(key, val));
		}

		if (feat.has_id) {
			mvt_value v;
			v.type = mvt_uint;
			v.numeric_value.uint_value = feat.id;

			attributes.insert(std::pair<std::string, mvt_value>("$id", v));
		}

		mvt_value v;
		v.type = mvt_string;

		if (feat.type == mvt_point) {
			v.string_value = "Point";
		} else if (feat.type == mvt_linestring) {
			v.string_value = "LineString";
		} else if (feat.type == mvt_polygon) {
			v.string_value = "Polygon";
		}

		attributes.insert(std::pair<std::string, mvt_value>("$type", v));

		mvt_value v2;
		v2.type = mvt_uint;
		v2.numeric_value.uint_value = z;

		attributes.insert(std::pair<std::string, mvt_value>("$zoom", v2));

		if (!evaluate(attributes, layer.name, filter, exclude_attributes)) {
			return false;
		}
	}

	return true;
}
