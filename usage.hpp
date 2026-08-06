#ifndef USAGE_HPP
#define USAGE_HPP

#include <stdio.h>
#include <getopt.h>
#include <string>

// An option that must be specified rather than being optional, and the
// placeholder to show for its argument in the usage message.
//
// Options that share the same non-zero `alternation` are alternatives to
// each other: one of them must be specified, but not more than one, and
// they are listed together as `(--this=... | --that=...)`.
struct usage_required_option {
	const char *name;
	const char *placeholder;
	int alternation;
};

// Returns the short option string to pass to getopt_long() for the
// options in `long_options`, so that the two can't disagree about
// which short options exist or take arguments.
std::string getopt_string(const struct option *long_options);

// Copies `long_options` to `real_long_options`, leaving out the headings
// of the usage message, which are not real options and so must not be
// passed on to getopt_long(). The destination must be at least as large
// as the source.
void strip_usage_headings(const struct option *long_options, struct option *real_long_options);

// Prints a usage message for `program` to `out`:
//
//	Usage: program forms[0]
//	   or: program forms[1]
//	        [--some-option] [--another-option=...] ...
//
// where `forms` is a NULL-terminated list of the ways the non-option
// arguments can be given, and the list of options is derived from
// `long_options`, the same table that is passed to getopt_long(), so that
// the message stays in sync with the options that are really accepted.
//
// Options named in `required` (a list terminated by a NULL name, or NULL
// if there are none) are shown without brackets, using the placeholder
// given there for their argument, and grouped with any alternatives to
// them. An entry in `long_options` with no `val` is printed as a heading
// for the options that follow it, and an entry with an empty name ends
// the listing, hiding any options after it.
void print_usage(FILE *out, const char *program, const char *const *forms,
		 const struct option *long_options,
		 const struct usage_required_option *required);

#endif
