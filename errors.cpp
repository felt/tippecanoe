#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include "errors.hpp"

[[noreturn]] void throw_tippecanoe_error(int code, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);

	char *msg = NULL;
	if (vasprintf(&msg, fmt, ap) < 0) {
		va_end(ap);
		throw tippecanoe_error(code, "error (could not format message)");
	}
	va_end(ap);

	std::string s(msg);
	free(msg);

	// Remove trailing newline if present (stderr messages often have one)
	while (s.size() > 0 && s.back() == '\n') {
		s.pop_back();
	}

	throw tippecanoe_error(code, s);
}

[[noreturn]] void throw_perror(int code, const char *msg) {
	throw tippecanoe_error(code, std::string(msg) + ": " + strerror(errno));
}
