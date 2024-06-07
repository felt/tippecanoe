#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include "main.hpp"
#include "memfile.hpp"
#include "pool.hpp"
#include "errors.hpp"
#include "text.hpp"

inline long long swizzlecmp(const char *a, int atype, unsigned long long ahash, const char *b, int btype, unsigned long long bhash) {
	if ((long long) ahash == (long long) bhash) {
		if (atype == btype) {
			return strcmp(a, b);
		} else {
			return atype - btype;
		}
	} else {
		return (long long) (ahash - bhash);
	}
}

long long addpool(struct memfile *poolfile, struct memfile *treefile, const char *s, char type, std::vector<ssize_t> &dedup) {
	unsigned long long hash = fnv1a(s, type);
	size_t hash_off = hash % dedup.size();

	if (dedup[hash_off] >= 0 &&
	    poolfile->map[dedup[hash_off]] == type &&
	    strcmp(poolfile->map.c_str() + dedup[hash_off] + 1, s) == 0) {
		// printf("hit for %s\n", s);
		return dedup[hash_off];
	} else {
		// printf("miss for %s\n", s);
	}

	unsigned long *sp = &treefile->tree;
	size_t depth = 0;

	// In typical data, traversal depth generally stays under 2.5x
	size_t max = 3 * log(treefile->off / sizeof(struct stringpool)) / log(2);
	if (max < 30) {
		max = 30;
	}

	while (*sp != 0) {
		long long cmp = swizzlecmp(s, type, hash,
					   poolfile->map.c_str() + ((struct stringpool *) (treefile->map.c_str() + *sp))->off + 1,
					   (poolfile->map.c_str() + ((struct stringpool *) (treefile->map.c_str() + *sp))->off)[0],
					   ((struct stringpool *) (treefile->map.c_str() + *sp))->hash);

		if (cmp < 0) {
			sp = &(((struct stringpool *) (treefile->map.c_str() + *sp))->left);
		} else if (cmp > 0) {
			sp = &(((struct stringpool *) (treefile->map.c_str() + *sp))->right);
		} else {
			dedup[hash_off] = ((struct stringpool *) (treefile->map.c_str() + *sp))->off;
			return ((struct stringpool *) (treefile->map.c_str() + *sp))->off;
		}

		depth++;
		if (depth > max) {
			// Search is very deep, so string is probably unique.
			// Add it to the pool without adding it to the search tree.
			// This might go either to memory or the file, depending on whether
			// the pool is full yet.

			long long off = poolfile->off;
			bool in_memory = false;

			if (memfile_write(poolfile, &type, 1, in_memory) < 0) {
				perror("memfile write");
				exit(EXIT_WRITE);
			}
			if (memfile_write(poolfile, (void *) s, strlen(s) + 1, in_memory) < 0) {
				perror("memfile write");
				exit(EXIT_WRITE);
			}

			if (in_memory) {
				dedup[hash_off] = off;
			}
			return off;
		}
	}

	// Size of memory divided by 10 from observation of OOM errors (when supposedly
	// 20% of memory is full) and onset of thrashing (when supposedly 15% of memory
	// is full) on ECS.
	if ((size_t) (poolfile->off + treefile->off) > memsize / CPUS / 10) {
		// If the pool and search tree get to be larger than physical memory,
		// then searching will start thrashing. Switch to appending strings
		// to the file instead of keeping them in memory.

		if (poolfile->fp == NULL) {
			memfile_full(poolfile);
		}
	}

	if (poolfile->fp != NULL) {
		// We are now appending to the file, so don't try to keep tree references
		// to the newly-added strings.

		long long off = poolfile->off;
		bool in_memory;
		if (memfile_write(poolfile, &type, 1, in_memory) < 0) {
			perror("memfile write");
			exit(EXIT_WRITE);
		}
		if (memfile_write(poolfile, (void *) s, strlen(s) + 1, in_memory) < 0) {
			perror("memfile write");
			exit(EXIT_WRITE);
		}
		return off;
	}

	// *sp is probably in the memory-mapped file, and will move if the file grows.
	long long ssp;
	if (sp == &treefile->tree) {
		ssp = -1;
	} else {
		ssp = ((char *) sp) - treefile->map.c_str();
	}

	long long off = poolfile->off;
	bool in_memory = false;
	if (memfile_write(poolfile, &type, 1, in_memory) < 0) {
		perror("memfile write");
		exit(EXIT_WRITE);
	}
	if (memfile_write(poolfile, (void *) s, strlen(s) + 1, in_memory) < 0) {
		perror("memfile write");
		exit(EXIT_WRITE);
	}
	if (in_memory) {
		dedup[hash_off] = off;
	}

	if (off >= LONG_MAX || treefile->off >= LONG_MAX) {
		// Tree or pool is bigger than 2GB
		static bool warned = false;
		if (!warned) {
			fprintf(stderr, "Warning: string pool is very large.\n");
			warned = true;
		}
		return off;
	}

	struct stringpool tsp;
	tsp.left = 0;
	tsp.right = 0;
	tsp.off = off;
	tsp.hash = hash;

	long long p = treefile->off;
	if (memfile_write(treefile, &tsp, sizeof(struct stringpool), in_memory) < 0) {
		perror("memfile write");
		exit(EXIT_WRITE);
	}

	if (ssp == -1) {
		treefile->tree = p;
	} else {
		*((long long *) (treefile->map.c_str() + ssp)) = p;
	}
	return off;
}
