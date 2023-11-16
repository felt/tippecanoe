#ifdef __APPLE__
#define _DARWIN_UNLIMITED_STREAMS
#endif

#include "compression.hpp"
#include "errors.hpp"
#include "protozero/varint.hpp"
#include "serial.hpp"

void decompressor::begin() {
	within = true;

	zs.zalloc = NULL;
	zs.zfree = NULL;
	zs.opaque = NULL;
	zs.msg = (char *) "";

	int d = inflateInit(&zs);
	if (d != Z_OK) {
		fprintf(stderr, "initialize decompression: %d %s\n", d, zs.msg);
		exit(EXIT_IMPOSSIBLE);
	}
}

int decompressor::fread(void *p, size_t size, size_t nmemb, std::atomic<long long> *geompos) {
	zs.next_out = (Bytef *) p;
	zs.avail_out = size * nmemb;

	while (zs.avail_out > 0) {
		if (zs.avail_in == 0) {
			size_t n = ::fread((Bytef *) buf.c_str(), sizeof(char), buf.size(), fp);
			if (n == 0) {
				if (within) {
					fprintf(stderr, "Reached EOF while decompressing\n");
					exit(EXIT_IMPOSSIBLE);
				} else {
					break;
				}
			}
			zs.next_in = (Bytef *) buf.c_str();
			zs.avail_in = n;
		}

		size_t avail_before = zs.avail_in;

		if (within) {
			int d = inflate(&zs, Z_NO_FLUSH);
			*geompos += avail_before - zs.avail_in;

			if (d == Z_OK) {
				// it made some progress
			} else if (d == Z_STREAM_END) {
				// it may have made some progress and now we are done
				within = false;
				break;
			} else {
				fprintf(stderr, "decompression error %d %s\n", d, zs.msg);
				exit(EXIT_IMPOSSIBLE);
			}
		} else {
			size_t n = std::min(zs.avail_in, zs.avail_out);
			memcpy(zs.next_out, zs.next_in, n);
			*geompos += n;

			zs.avail_out -= n;
			zs.avail_in -= n;
			zs.next_out += n;
			zs.next_in += n;
		}
	}

	return (size * nmemb - zs.avail_out) / size;
}

void decompressor::end(std::atomic<long long> *geompos) {
	// "within" means that we haven't received end-of-stream yet,
	// so consume more compressed data until we get there.
	// This can be necessary if the caller knows that it is at
	// the end of the feature stream (because it got a 0-length
	// feature) but the decompressor doesn't know yet.

	if (within) {
		while (true) {
			if (zs.avail_in == 0) {
				size_t n = ::fread((Bytef *) buf.c_str(), sizeof(char), buf.size(), fp);
				zs.next_in = (Bytef *) buf.c_str();
				zs.avail_in = n;
			}

			zs.avail_out = 0;

			size_t avail_before = zs.avail_in;
			int d = inflate(&zs, Z_NO_FLUSH);
			*geompos += avail_before - zs.avail_in;

			if (d == Z_STREAM_END) {
				break;
			}
			if (d == Z_OK) {
				continue;
			}

			fprintf(stderr, "decompression: got %d, not Z_STREAM_END\n", d);
			exit(EXIT_IMPOSSIBLE);
		}

		within = false;
	}

	int d = inflateEnd(&zs);
	if (d != Z_OK) {
		fprintf(stderr, "end decompression: %d %s\n", d, zs.msg);
		exit(EXIT_IMPOSSIBLE);
	}
}

int decompressor::deserialize_ulong_long(unsigned long long *zigzag, std::atomic<long long> *geompos) {
	*zigzag = 0;
	int shift = 0;

	while (1) {
		char c;
		if (fread(&c, sizeof(char), 1, geompos) != 1) {
			return 0;
		}

		if ((c & 0x80) == 0) {
			*zigzag |= ((unsigned long long) c) << shift;
			shift += 7;
			break;
		} else {
			*zigzag |= ((unsigned long long) (c & 0x7F)) << shift;
			shift += 7;
		}
	}

	return 1;
}

int decompressor::deserialize_long_long(long long *n, std::atomic<long long> *geompos) {
	unsigned long long zigzag = 0;
	int ret = deserialize_ulong_long(&zigzag, geompos);
	*n = protozero::decode_zigzag64(zigzag);
	return ret;
}

int decompressor::deserialize_int(int *n, std::atomic<long long> *geompos) {
	long long ll = 0;
	int ret = deserialize_long_long(&ll, geompos);
	*n = ll;
	return ret;
}

int decompressor::deserialize_uint(unsigned *n, std::atomic<long long> *geompos) {
	unsigned long long v;
	deserialize_ulong_long(&v, geompos);
	*n = v;
	return 1;
}

void compressor::begin() {
	zs.zalloc = NULL;
	zs.zfree = NULL;
	zs.opaque = NULL;
	zs.msg = (char *) "";

	int d = deflateInit(&zs, Z_DEFAULT_COMPRESSION);
	if (d != Z_OK) {
		fprintf(stderr, "initialize compression: %d %s\n", d, zs.msg);
		exit(EXIT_IMPOSSIBLE);
	}
}

void compressor::compressor::end(std::atomic<long long> *fpos, const char *fname) {
	std::string buf;
	buf.resize(5000);

	if (zs.avail_in != 0) {
		fprintf(stderr, "compression end called with data available\n");
		exit(EXIT_IMPOSSIBLE);
	}

	zs.next_in = (Bytef *) buf.c_str();
	zs.avail_in = 0;

	while (true) {
		zs.next_out = (Bytef *) buf.c_str();
		zs.avail_out = buf.size();

		int d = deflate(&zs, Z_FINISH);
		::fwrite_check(buf.c_str(), sizeof(char), zs.next_out - (Bytef *) buf.c_str(), fp, fpos, fname);

		if (d == Z_OK || d == Z_BUF_ERROR) {
			// it can take several calls to flush out all the buffered data
			continue;
		}

		if (d != Z_STREAM_END) {
			fprintf(stderr, "%s: finish compression: %d %s\n", fname, d, zs.msg);
			exit(EXIT_IMPOSSIBLE);
		}

		break;
	}

	zs.next_out = (Bytef *) buf.c_str();
	zs.avail_out = buf.size();

	int d = deflateEnd(&zs);
	if (d != Z_OK) {
		fprintf(stderr, "%s: end compression: %d %s\n", fname, d, zs.msg);
		exit(EXIT_IMPOSSIBLE);
	}

	::fwrite_check(buf.c_str(), sizeof(char), zs.next_out - (Bytef *) buf.c_str(), fp, fpos, fname);
}

int compressor::fclose() {
	return ::fclose(fp);
}

void compressor::fwrite_check(const char *p, size_t size, size_t nmemb, std::atomic<long long> *fpos, const char *fname) {
	std::string buf;
	buf.resize(size * nmemb * 2 + 200);

	zs.next_in = (Bytef *) p;
	zs.avail_in = size * nmemb;

	while (zs.avail_in > 0) {
		zs.next_out = (Bytef *) buf.c_str();
		zs.avail_out = buf.size();

		int d = deflate(&zs, Z_NO_FLUSH);
		if (d != Z_OK) {
			fprintf(stderr, "%s: deflate: %d %s\n", fname, d, zs.msg);
			exit(EXIT_IMPOSSIBLE);
		}

		::fwrite_check(buf.c_str(), sizeof(char), zs.next_out - (Bytef *) buf.c_str(), fp, fpos, fname);
	}
}

void compressor::serialize_ulong_long(unsigned long long val, std::atomic<long long> *fpos, const char *fname) {
	while (1) {
		unsigned char b = val & 0x7F;
		if ((val >> 7) != 0) {
			b |= 0x80;
			fwrite_check((const char *) &b, 1, 1, fpos, fname);
			val >>= 7;
		} else {
			fwrite_check((const char *) &b, 1, 1, fpos, fname);
			break;
		}
	}
}

void compressor::serialize_long_long(long long val, std::atomic<long long> *fpos, const char *fname) {
	unsigned long long zigzag = protozero::encode_zigzag64(val);

	serialize_ulong_long(zigzag, fpos, fname);
}

void compressor::serialize_int(int val, std::atomic<long long> *fpos, const char *fname) {
	serialize_long_long(val, fpos, fname);
}

void compressor::serialize_uint(unsigned val, std::atomic<long long> *fpos, const char *fname) {
	serialize_ulong_long(val, fpos, fname);
}
