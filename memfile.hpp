#ifndef MEMFILE_HPP
#define MEMFILE_HPP

#include <atomic>
#include <string>
#include "errors.hpp"

struct memfile {
	int fd = 0;
	std::string map;
	unsigned long tree = 0;
	FILE *fp = NULL;
	size_t off = 0;
};

struct memfile *memfile_open(int fd);
int memfile_close(struct memfile *file);
int memfile_write(struct memfile *file, void *s, long long len);
void memfile_full(struct memfile *file);

#endif
