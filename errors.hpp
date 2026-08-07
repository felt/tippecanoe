#ifndef ERRORS_HPP
#define ERRORS_HPP

#define EXIT_INCOMPLETE 100
#define EXIT_ARGS 101
#define EXIT_CLOSE 102
#define EXIT_CSV 103
#define EXIT_EXISTS 104
#define EXIT_FILTER 105
#define EXIT_IMPOSSIBLE 106
#define EXIT_JSON 107
#define EXIT_MEMORY 108
#define EXIT_MVT 109
#define EXIT_NODATA 110
#define EXIT_OPEN 111
#define EXIT_PROTOBUF 112
#define EXIT_PTHREAD 113
#define EXIT_READ 114
#define EXIT_SEEK 115
#define EXIT_SQLITE 116
#define EXIT_STAT 117
#define EXIT_UNLINK 118
#define EXIT_UTF8 119
#define EXIT_WRITE 120

// avoid 124, 125, 126, 127, 137, which are used by GNU timeout

#include <stdexcept>
#include <string>
#include <exception>

class tippecanoe_error : public std::runtime_error {
       public:
	int exit_code;
	tippecanoe_error(int code, const std::string &message)
	    : std::runtime_error(message), exit_code(code) {
	}
};

// Format and throw a tippecanoe_error. Use in place of fprintf(stderr,...)+exit().
[[noreturn]] void throw_tippecanoe_error(int code, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

// Throw from errno-based errors (replaces perror()+exit() pattern)
[[noreturn]] void throw_perror(int code, const char *msg);

// Check the void* return value from pthread_join. If a thread caught an
// exception, the return value is a heap-allocated std::exception_ptr.
// This re-throws it on the joining thread, or does nothing if retval is NULL.
inline void rethrow_if_thread_failed(void *retval) {
	if (retval != NULL) {
		std::exception_ptr *ep = (std::exception_ptr *) retval;
		std::exception_ptr copy = *ep;
		delete ep;
		std::rethrow_exception(copy);
	}
}

#endif
