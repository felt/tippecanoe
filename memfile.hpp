#ifndef MEMFILE_HPP
#define MEMFILE_HPP

#include <atomic>
#include <string>

struct memfile {
	int fd = 0;
	std::string map;
	unsigned long tree = 0;
};

struct memfile *memfile_open(int fd);
int memfile_close(struct memfile *file);
int memfile_write(struct memfile *file, void *s, long long len);

#endif
