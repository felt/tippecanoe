#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "projection.hpp"
#include "errors.hpp"
#include "serial.hpp"

#define MAX_MEMORY (1024 * 1024 * 1024)	 // 1 GB

void fqsort(std::vector<FILE *> &inputs, size_t width, int (*cmp)(const void *, const void *), FILE *out, size_t mem) {
	std::string pivot;
	FILE *fp1, *fp2;

	if (mem > MAX_MEMORY) {
		mem = MAX_MEMORY;
	}

	{
		// read some elements into memory to choose a pivot from
		//
		// this is in its own scope so `buf` can go out of scope
		// before trying to do any sub-sorts.

		std::string buf;

		bool read_everything = false;
		for (size_t i = 0; i < inputs.size(); i++) {
			if (buf.size() > mem) {
				break;
			}

			while (true) {
				std::string element;
				element.resize(width);

				size_t n = fread((void *) element.c_str(), width, 1, inputs[i]);
				if (n == 0) {
					if (i + 1 == inputs.size()) {
						read_everything = true;
					}
					break;
				}

				buf.append(element);

				if (buf.size() > mem) {
					break;
				}
			}
		}

		qsort((void *) buf.c_str(), buf.size() / width, width, cmp);

		// If that was everything we have to sort, we are done.

		if (read_everything) {
			fwrite((void *) buf.c_str(), buf.size() / width, width, out);
			return;
		}

		// Otherwise, choose a pivot from it, make some temporary files,
		// write what we have to those files, and then partition the rest
		// of the input into them.

		// This would be unstable if the pivot is one of several elements
		// that compare equal. Does it matter?

		size_t pivot_off = width * (buf.size() / width / 2);
		pivot = std::string(buf, pivot_off, width);

		std::string t1 = "/tmp/sort1.XXXXXX";
		std::string t2 = "/tmp/sort2.XXXXXX";

		int fd1 = mkstemp((char *) t1.c_str());
		unlink(t1.c_str());
		int fd2 = mkstemp((char *) t2.c_str());
		unlink(t2.c_str());

		fp1 = fdopen(fd1, "w+b");
		if (fp1 == NULL) {
			perror(t1.c_str());
			exit(EXIT_FAILURE);
		}
		fp2 = fdopen(fd2, "w+b");
		if (fp2 == NULL) {
			perror(t2.c_str());
			exit(EXIT_FAILURE);
		}

		fwrite((void *) buf.c_str(), sizeof(char), pivot_off, fp1);
		fwrite((void *) ((char *) buf.c_str() + pivot_off), sizeof(char), buf.size() - pivot_off, fp2);
	}

	// read the remaining input into the temporary files

	for (size_t i = 0; i < inputs.size(); i++) {
		while (true) {
			std::string element;
			element.resize(width);

			size_t n = fread((void *) element.c_str(), width, 1, inputs[i]);
			if (n == 0) {
				break;
			}

			if (cmp((void *) element.c_str(), (void *) pivot.c_str()) < 0) {
				fwrite((void *) element.c_str(), width, 1, fp1);
			} else {
				fwrite((void *) element.c_str(), width, 1, fp2);
			}
		}
	}

	// Now sort the sub-ranges into the output.

	rewind(fp1);
	rewind(fp2);

	std::vector<FILE *> v1;
	v1.emplace_back(fp1);
	fqsort(v1, width, cmp, out, mem);
	fclose(fp1);

	std::vector<FILE *> v2;
	v2.emplace_back(fp2);
	fqsort(v2, width, cmp, out, mem);
	fclose(fp2);
}

#if 0

struct indexed_feature {
	std::string feature;
	index_t index;
	size_t seq;

	bool operator<(indexed_feature const &f) const {
		if (index < f.index) {
			return true;
		}
		if (index == f.index) {
			if (seq < f.seq) {
				return true;
			}
		}
		return false;
	}
};

int deserialize_ulong_long(FILE *fp, unsigned long long *zigzag) {
	*zigzag = 0;
	int shift = 0;

	while (true) {
		char c;
		if (fread(&c, sizeof(char), 1, fp) != 1) {
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

void feature_sort(std::vector<FILE *> &inputs, FILE *out, size_t mem) {
	FILE *fp1, *fp2;
	size_t seq = 0;
	indexed_feature pivot;

	if (mem > MAX_MEMORY) {
		mem = MAX_MEMORY;
	}

	{
		// read some elements into memory to choose a pivot from
		//
		// this is in its own scope so `buf` can go out of scope
		// before trying to do any sub-sorts.

		std::vector<indexed_feature> buf;
		size_t bufsize = 0;

		bool read_everything = false;
		for (size_t i = 0; i < inputs.size(); i++) {
			if (bufsize > mem) {
				break;
			}

			while (true) {
				unsigned long long len;

				if (deserialize_ulong_long(inputs[i], &len) == 0) {
					if (i + 1 == inputs.size()) {
						read_everything = true;
					}
					break;
				}

				indexed_feature f;
				f.feature.resize(len);
				if (fread((char *) f.feature.c_str(), len, sizeof(char), inputs[i]) != 1) {
					perror("fread");
					exit(EXIT_READ);
				}

				const char *cp = f.feature.c_str();
				deserialize_ulong_long(&cp, &f.index);
				f.seq = seq++;

				buf.push_back(std::move(f));
				bufsize += len;
				if (bufsize > mem) {
					break;
				}
			}
		}

		std::stable_sort(buf.begin(), buf.end());

		// If that was everything we have to sort, we are done.

		if (read_everything) {
			std::atomic<long long> fpos(0);
			for (auto const &f : buf) {
				serialize_ulong_long(out, f.feature.size(), &fpos, "sort output");
				fwrite_check(f.feature.c_str(), f.feature.size(), 1, out, &fpos, "sort output");
			}
			return;
		}

		// Otherwise, choose a pivot from it, make some temporary files,
		// write what we have to those files, and then partition the rest
		// of the input into them.

		// This would be unstable if the pivot is one of several elements
		// that compare equal. Does it matter?

		size_t pivot_off = buf.size() / 2;
		pivot = buf[pivot_off];

		std::string t1 = "/tmp/sort1.XXXXXX";
		std::string t2 = "/tmp/sort2.XXXXXX";

		int fd1 = mkstemp((char *) t1.c_str());
		unlink(t1.c_str());
		int fd2 = mkstemp((char *) t2.c_str());
		unlink(t2.c_str());

		fp1 = fdopen(fd1, "w+b");
		if (fp1 == NULL) {
			perror(t1.c_str());
			exit(EXIT_FAILURE);
		}
		fp2 = fdopen(fd2, "w+b");
		if (fp2 == NULL) {
			perror(t2.c_str());
			exit(EXIT_FAILURE);
		}

		std::atomic<long long> fpos(0);
		for (size_t i = 0; i < pivot_off; i++) {
			serialize_ulong_long(fp1, buf[i].feature.size(), &fpos, "sort output");
			fwrite_check(buf[i].feature.c_str(), buf[i].feature.size(), 1, fp1, &fpos, "sort output");
		}

		for (size_t i = pivot_off; i < buf.size(); i++) {
			serialize_ulong_long(fp2, buf[i].feature.size(), &fpos, "sort output");
			fwrite_check(buf[i].feature.c_str(), buf[i].feature.size(), 1, fp2, &fpos, "sort output");
		}
	}

	// read the remaining input into the temporary files

	std::atomic<long long> fpos(0);
	for (size_t i = 0; i < inputs.size(); i++) {
		while (true) {
			unsigned long long len;

			if (deserialize_ulong_long(inputs[i], &len) == 0) {
				break;
			}

			indexed_feature f;
			f.feature.resize(len);
			if (fread((char *) f.feature.c_str(), len, sizeof(char), inputs[i]) != 1) {
				perror("fread");
				exit(EXIT_READ);
			}

			const char *cp = f.feature.c_str();
			deserialize_ulong_long(&cp, &f.index);
			f.seq = seq++;

			if (f < pivot) {
				serialize_ulong_long(fp1, f.feature.size(), &fpos, "sort output");
				fwrite_check(f.feature.c_str(), f.feature.size(), 1, fp1, &fpos, "sort output");
			} else {
				serialize_ulong_long(fp2, f.feature.size(), &fpos, "sort output");
				fwrite_check(f.feature.c_str(), f.feature.size(), 1, fp2, &fpos, "sort output");
			}
		}
	}

	// Now sort the sub-ranges into the output.

	rewind(fp1);
	rewind(fp2);

	std::vector<FILE *> v1;
	v1.emplace_back(fp1);
	feature_sort(v1, out, mem);
	fclose(fp1);

	std::vector<FILE *> v2;
	v2.emplace_back(fp2);
	feature_sort(v2, out, mem);
	fclose(fp2);
}

#endif
