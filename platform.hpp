#ifndef PLATFORM_HPP
#define PLATFORM_HPP

#include <cstddef>

long get_num_avail_cpus();

long get_page_size();

size_t calc_memsize();

size_t get_max_open_files();

constexpr const char *get_null_device() {
	return "/dev/null";
}

#endif	// PLATFORM_HPP