#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
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
	mf->off = 0;

	return mf;
}

int memfile_close(struct memfile *file) {
	// If it isn't full yet, flush out the string to the file now.
	// If it is full, close out the buffered file writer.

	if (file->fp == NULL) {
		if (write(file->fd, file->map.c_str(), file->map.size()) != (ssize_t) file->map.size()) {
			return -1;
		}

		if (file->fd >= 0) {
			if (close(file->fd) != 0) {
				return -1;
			}
		}
	} else {
		if (fclose(file->fp) != 0) {
			return -1;
		}
	}

	delete file;
	return 0;
}

int memfile_write(struct memfile *file, void *s, long long len) {
	// If it is full, append to the file.
	// If it is not full yet, append to the string in memory.

	if (file->fp != NULL) {
		if (fwrite(s, sizeof(char), len, file->fp) != (size_t) len) {
			return 0;
		}
		file->off += len;
	} else {
		file->map.append(std::string((char *) s, len));
		file->off += len;
	}

	return len;
}

void memfile_full(struct memfile *file) {
	// The file is full. Write out a copy of whatever has accumulated in memory
	// to the file, and switch to appending to the file. Existing references
	// into the memory still work.

	if (file->fp != NULL) {
		fprintf(stderr, "memfile marked full twice\n");
		exit(EXIT_IMPOSSIBLE);
	}

	file->fp = fdopen(file->fd, "wb");
	if (file->fp == NULL) {
		fprintf(stderr, "fdopen memfile: %s\n", strerror(errno));
		exit(EXIT_OPEN);
	}

	if (fwrite(file->map.c_str(), sizeof(char), file->map.size(), file->fp) != file->map.size()) {
		fprintf(stderr, "memfile write: %s\n", strerror(errno));
		exit(EXIT_WRITE);
	}
}
