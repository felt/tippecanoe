#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "memfile.hpp"

#define INCREMENT 131072
#define INITIAL 256

struct memfile *memfile_open(int fd) {
	if (ftruncate(fd, INITIAL) != 0) {
		return NULL;
	}

	struct memfile *mf = new memfile;
	if (mf == NULL) {
		return NULL;
	}

	mf->fd = fd;
	mf->tree = 0;

	return mf;
}

int memfile_close(struct memfile *file) {
	if (write(file->fd, file->map.c_str(), file->map.size()) != (ssize_t) file->map.size()) {
		return -1;
	}

	if (file->fd >= 0) {
		if (close(file->fd) != 0) {
			return -1;
		}
	}

	delete file;
	return 0;
}

int memfile_write(struct memfile *file, void *s, long long len) {
	file->map.append(std::string((char *) s, len));
	return len;
}
