#include <string.h>
#include <set>
#include <string>
#include "usage.hpp"

// Options are wrapped to fit within this many columns
#define USAGE_WIDTH 80

// The indentation of the continuation lines of the option list
#define USAGE_INDENT 8

std::string getopt_string(const struct option *long_options) {
	std::string getopt_str;

	for (size_t lo = 0; long_options[lo].name != NULL; lo++) {
		if (long_options[lo].val > ' ') {
			getopt_str.push_back(long_options[lo].val);

			if (long_options[lo].has_arg == required_argument) {
				getopt_str.push_back(':');
			}
		}
	}

	return getopt_str;
}

void strip_usage_headings(const struct option *long_options, struct option *real_long_options) {
	size_t out = 0;

	for (size_t lo = 0; long_options[lo].name != NULL; lo++) {
		if (long_options[lo].val != 0) {
			real_long_options[out++] = long_options[lo];
		}
	}

	real_long_options[out] = {0, 0, 0, 0};
}

// The entry for `name` in the list of options that must be specified,
// or NULL if it is an optional option
static const struct usage_required_option *required_for(const char *name, const struct usage_required_option *required) {
	for (size_t i = 0; required != NULL && required[i].name != NULL; i++) {
		if (strcmp(required[i].name, name) == 0) {
			return &required[i];
		}
	}

	return NULL;
}

// "--option", or "--option=placeholder" if the option takes an argument
static std::string option_text(const struct option *opt, const struct usage_required_option *req) {
	std::string text = std::string("--") + opt->name;

	if (opt->has_arg != no_argument) {
		text += "=";
		text += (req != NULL && req->placeholder != NULL) ? req->placeholder : "...";
	}

	return text;
}

// The alternatives that `req` belongs to, as "(--this=... | --that=...)"
static std::string alternation_text(const struct option *long_options, const struct usage_required_option *required, int alternation) {
	std::string text;
	size_t found = 0;

	for (size_t lo = 0; long_options[lo].name != NULL && long_options[lo].name[0] != '\0'; lo++) {
		const struct usage_required_option *req = required_for(long_options[lo].name, required);

		if (req != NULL && req->alternation == alternation) {
			if (found++ > 0) {
				text += " | ";
			}

			text += option_text(&long_options[lo], req);
		}
	}

	if (found > 1) {
		text = "(" + text + ")";
	}

	return text;
}

void print_usage(FILE *out, const char *program, const char *const *forms,
		 const struct option *long_options,
		 const struct usage_required_option *required) {
	for (size_t f = 0; forms[f] != NULL; f++) {
		const char *lead = (f == 0) ? "Usage: " : "\n   or: ";
		fprintf(out, "%s%s %s", lead, program, forms[f]);
	}

	// whatever the forms took up, the option list starts on a line of its own
	size_t width = USAGE_WIDTH;
	std::set<int> alternations_listed;

	for (size_t lo = 0; long_options[lo].name != NULL && long_options[lo].name[0] != '\0'; lo++) {
		if (long_options[lo].val == 0) {
			fprintf(out, "\n  %s\n%*s", long_options[lo].name, USAGE_INDENT, "");
			width = USAGE_INDENT;
			continue;
		}

		const struct usage_required_option *req = required_for(long_options[lo].name, required);
		std::string text;

		if (req == NULL) {
			text = "[" + option_text(&long_options[lo], NULL) + "]";
		} else if (req->alternation == 0) {
			text = option_text(&long_options[lo], req);
		} else {
			if (alternations_listed.count(req->alternation) != 0) {
				continue;  // already listed with the first of its alternatives
			}
			alternations_listed.insert(req->alternation);

			text = alternation_text(long_options, required, req->alternation);
		}

		if (width + 1 + text.size() >= USAGE_WIDTH) {
			fprintf(out, "\n%*s", USAGE_INDENT, "");
			width = USAGE_INDENT;
		}

		fprintf(out, " %s", text.c_str());
		width += 1 + text.size();
	}

	fprintf(out, "\n");
}
