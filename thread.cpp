#include <stdlib.h>
#include "thread.hpp"

// It is harder to profile tippecanoe because of its normal use of multiple threads.
// If you set TIPPECANOE_NO_THREADS in the environment, it will run everything in
// the main thread instead for more straightforward profiling. (The threads will
// then still be created so they will be reaped, but they will immediately return.)

static const char *no_threads = getenv("TIPPECANOE_NO_THREADS");

static void *do_nothing(void *arg) {
	return arg;
}

int thread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
	if (no_threads != NULL) {
		void *ret = start_routine(arg);
		return pthread_create(thread, attr, do_nothing, ret);
	} else {
		return pthread_create(thread, attr, start_routine, arg);
	}
}
