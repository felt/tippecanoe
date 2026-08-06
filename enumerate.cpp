#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string>
#include <sqlite3.h>
#include "errors.hpp"
#include "usage.hpp"

void enumerate(char *fname) {
	sqlite3 *db;

	if (sqlite3_open(fname, &db) != SQLITE_OK) {
		fprintf(stderr, "%s: %s\n", fname, sqlite3_errmsg(db));
		exit(EXIT_OPEN);
	}

	char *err = NULL;
	if (sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: integrity_check: %s\n", fname, err);
		exit(EXIT_SQLITE);
	}

	const char *sql = "SELECT zoom_level, tile_column, tile_row from tiles order by zoom_level, tile_column, tile_row;";

	sqlite3_stmt *stmt;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "%s: select failed: %s\n", fname, sqlite3_errmsg(db));
		exit(EXIT_SQLITE);
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		long long zoom = sqlite3_column_int(stmt, 0);
		long long x = sqlite3_column_int(stmt, 1);
		long long y = sqlite3_column_int(stmt, 2);

		if (zoom < 0 || zoom > 31) {
			fprintf(stderr, "Corrupt mbtiles file: impossible zoom level %lld\n", zoom);
			exit(EXIT_IMPOSSIBLE);
		}

		y = (1LL << zoom) - 1 - y;
		printf("%s %lld %lld %lld\n", fname, zoom, x, y);
	}

	sqlite3_finalize(stmt);

	if (sqlite3_close(db) != SQLITE_OK) {
		fprintf(stderr, "%s: could not close database: %s\n", fname, sqlite3_errmsg(db));
		exit(EXIT_CLOSE);
	}
}

// there are no options, but the table is still what the usage message
// and the getopt string are derived from, so that they will keep up
// with any options that are added later
static const struct option long_options[] = {
	{0, 0, 0, 0},
};

void usage(char **argv) {
	static const char *const forms[] = {
		"file.mbtiles ...",
		NULL,
	};

	print_usage(stderr, argv[0], forms, long_options, NULL);
	exit(EXIT_ARGS);
}

int main(int argc, char **argv) {
	extern int optind;
	// extern char *optarg;
	int i;

	std::string getopt_str = getopt_string(long_options);

	while ((i = getopt_long(argc, argv, getopt_str.c_str(), long_options, NULL)) != -1) {
		usage(argv);
	}

	if (optind >= argc) {
		usage(argv);
	}

	for (i = optind; i < argc; i++) {
		enumerate(argv[i]);
	}

	return 0;
}
