#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sqlite3.h>

void enumerate(char *fname) {
	sqlite3 *db;

	if (sqlite3_open(fname, &db) != SQLITE_OK) {
		fprintf(stderr, "%s: %s\n", fname, sqlite3_errmsg(db));
		exit(132);
	}

	char *err = NULL;
	if (sqlite3_exec(db, "PRAGMA integrity_check;", NULL, NULL, &err) != SQLITE_OK) {
		fprintf(stderr, "%s: integrity_check: %s\n", fname, err);
		exit(133);
	}

	const char *sql = "SELECT zoom_level, tile_column, tile_row from tiles order by zoom_level, tile_column, tile_row;";

	sqlite3_stmt *stmt;
	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
		fprintf(stderr, "%s: select failed: %s\n", fname, sqlite3_errmsg(db));
		exit(134);
	}

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		long long zoom = sqlite3_column_int(stmt, 0);
		long long x = sqlite3_column_int(stmt, 1);
		long long y = sqlite3_column_int(stmt, 2);

		if (zoom < 0 || zoom > 31) {
			fprintf(stderr, "Corrupt mbtiles file: impossible zoom level %lld\n", zoom);
			exit(135);
		}

		y = (1LL << zoom) - 1 - y;
		printf("%s %lld %lld %lld\n", fname, zoom, x, y);
	}

	sqlite3_finalize(stmt);

	if (sqlite3_close(db) != SQLITE_OK) {
		fprintf(stderr, "%s: could not close database: %s\n", fname, sqlite3_errmsg(db));
		exit(136);
	}
}

void usage(char **argv) {
	fprintf(stderr, "Usage: %s file.mbtiles ...\n", argv[0]);
	exit(137);
}

int main(int argc, char **argv) {
	extern int optind;
	// extern char *optarg;
	int i;

	while ((i = getopt(argc, argv, "")) != -1) {
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
