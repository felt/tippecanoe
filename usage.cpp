#include <string.h>
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

static const char *placeholder_for(const char *name, const struct usage_required_option *required) {
	for (size_t i = 0; required != NULL && required[i].name != NULL; i++) {
		if (strcmp(required[i].name, name) == 0) {
			return required[i].placeholder;
		}
	}

	return NULL;
}

void print_usage(FILE *out, const char *program, const char *const *forms,
		 const struct option *long_options,
		 const struct usage_required_option *required) {
	size_t width = 0;

	for (size_t f = 0; forms[f] != NULL; f++) {
		const char *lead = (f == 0) ? "Usage: " : "\n   or: ";
		fprintf(out, "%s%s %s", lead, program, forms[f]);
		width = strlen(lead) + strlen(program) + 1 + strlen(forms[f]);
	}

	for (size_t lo = 0; long_options[lo].name != NULL && long_options[lo].name[0] != '\0'; lo++) {
		if (long_options[lo].val == 0) {
			fprintf(out, "\n  %s\n%*s", long_options[lo].name, USAGE_INDENT, "");
			width = USAGE_INDENT;
			continue;
		}

		const char *placeholder = placeholder_for(long_options[lo].name, required);

		std::string text = std::string("--") + long_options[lo].name;
		if (long_options[lo].has_arg != no_argument) {
			text += "=";
			text += (placeholder != NULL) ? placeholder : "...";
		}
		if (placeholder == NULL) {
			text = "[" + text + "]";
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
