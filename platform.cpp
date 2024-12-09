#include "platform.hpp"
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/resource.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/param.h>
#include <sys/mount.h>
#endif

#include "errors.hpp"

long get_num_avail_cpus() {
	return sysconf(_SC_NPROCESSORS_ONLN);
}

long get_page_size() {
	return sysconf(_SC_PAGESIZE);
}

size_t calc_memsize() {
	size_t mem;

#ifdef __APPLE__
	int64_t hw_memsize;
	size_t len = sizeof(int64_t);
	if (sysctlbyname("hw.memsize", &hw_memsize, &len, NULL, 0) < 0) {
		perror("sysctl hw.memsize");
		exit(EXIT_MEMORY);
	}
	mem = hw_memsize;
#else
	long long pagesize = sysconf(_SC_PAGESIZE);
	long long pages = sysconf(_SC_PHYS_PAGES);
	if (pages < 0 || pagesize < 0) {
		perror("sysconf _SC_PAGESIZE or _SC_PHYS_PAGES");
		exit(EXIT_MEMORY);
	}

	mem = (long long) pages * pagesize;
#endif

	return mem;
}

size_t get_max_open_files() {
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
		perror("getrlimit");
		exit(EXIT_PTHREAD);
	}
	return rl.rlim_cur;
}