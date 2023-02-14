#ifdef __APPLE__
#define _DARWIN_UNLIMITED_STREAMS
#endif

#include <stdio.h>
#include <string>
#include <atomic>
#include <zlib.h>

struct decompressor {
	FILE *fp = NULL;
	z_stream zs;
	std::string buf;

	// from begin() to receiving end-of-stream
	bool within = false;

	decompressor(FILE *f) {
		fp = f;
		buf.resize(5000);

		zs.next_in = (Bytef *) buf.c_str();
		zs.avail_in = 0;
	}

	decompressor() {
	}

	void begin();
	int fread(void *p, size_t size, size_t nmemb, std::atomic<long long> *geompos);
	void end(std::atomic<long long> *geompos);
	int deserialize_ulong_long(unsigned long long *zigzag, std::atomic<long long> *geompos);
	int deserialize_long_long(long long *n, std::atomic<long long> *geompos);
	int deserialize_int(int *n, std::atomic<long long> *geompos);
	int deserialize_uint(unsigned *n, std::atomic<long long> *geompos);
};

struct compressor {
	FILE *fp = NULL;
	z_stream zs;

	compressor(FILE *f) {
		fp = f;
	}

	compressor() {
	}

	void begin();
	void end(std::atomic<long long> *fpos, const char *fname);
	int fclose();
	void fwrite_check(const char *p, size_t size, size_t nmemb, std::atomic<long long> *fpos, const char *fname);
	void serialize_ulong_long(unsigned long long val, std::atomic<long long> *fpos, const char *fname);

	void serialize_long_long(long long val, std::atomic<long long> *fpos, const char *fname);
	void serialize_int(int val, std::atomic<long long> *fpos, const char *fname);
	void serialize_uint(unsigned val, std::atomic<long long> *fpos, const char *fname);
};
