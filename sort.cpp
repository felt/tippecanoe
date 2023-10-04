#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <vector>
#include <string>

#define MAX_MEMORY (10 * 1024 * 1024)

void fqsort(std::vector<FILE *> &inputs, size_t width, int (*cmp)(const void *, const void *), FILE *out, size_t mem) {
	std::string pivot;
	FILE *fp1, *fp2;

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
