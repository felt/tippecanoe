#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string>
#include <set>
#include "errors.hpp"
#include "mvt.hpp"
#include "geometry.hpp"

extern char *optarg;
extern int optind;

int detail = 12;  // tippecanoe-style: mvt extent == 1 << detail
int buffer = 5;	  // tippecanoe-style: mvt buffer == extent * buffer / 256;

std::set<std::string> keep;

void usage(char **argv) {
	fprintf(stderr, "Usage: %s -o newtile.pbf.gz tile.pbf.gz oz/ox/oy nz/nx/ny\n", argv[0]);
	fprintf(stderr, "to create tile nz/nx/ny from tile oz/ox/oy\n");
	fprintf(stderr, "Usage: %s -o newtile.pbf.gz -t nz/nx/ny tile.pbf.gz oz/ox/oy tile2.pbf.gz oz2/ox2/oy2\n", argv[0]);
	fprintf(stderr, "to create tile nz/nx/ny from tiles oz/ox/oy and oz2/ox2/oy2\n");
	exit(EXIT_FAILURE);
}

struct source {
	std::string file;
	int z;
	int x;
	int y;
};

int main(int argc, char **argv) {
	int i;
	const char *outfile = NULL;
	const char *outtile = NULL;

	std::vector<source> sources;

	while ((i = getopt(argc, argv, "y:o:d:b:")) != -1) {
		switch (i) {
		case 'y':
			keep.insert(optarg);
			break;

		case 'o':
			outfile = optarg;
			break;

		case 'd':
			detail = atoi(optarg);
			break;

		case 'b':
			buffer = atoi(optarg);
			break;

		case 't':
			outtile = optarg;
			break;

		default:
			usage(argv);
		}
	}

	if (outfile == NULL) {
		usage(argv);
	}

	if (outtile == NULL) {
		if (argc - optind != 3) {
			usage(argv);
		}

		const char *infile = argv[optind + 0];

		int oz, ox, oy;
		if (sscanf(argv[optind + 1], "%d/%d/%d", &oz, &ox, &oy) != 3) {
			fprintf(stderr, "%s: not in z/x/y form\n", argv[optind + 1]);
			usage(argv);
		}

		int nz, nx, ny;
		if (sscanf(argv[optind + 2], "%d/%d/%d", &nz, &nx, &ny) != 3) {
			fprintf(stderr, "%s: not in z/x/y form\n", argv[optind + 2]);
			usage(argv);
		}

		std::string tile;
		char buf[1000];
		int len;

		FILE *f = fopen(infile, "rb");
		if (f == NULL) {
			perror(infile);
			exit(EXIT_FAILURE);
		}

		while ((len = fread(buf, sizeof(char), 1000, f)) > 0) {
			tile.append(std::string(buf, len));
		}
		fclose(f);

		f = fopen(outfile, "wb");
		if (f == NULL) {
			perror(outfile);
			exit(EXIT_FAILURE);
		}

		input_tile it;
		it.tile = tile;
		it.z = oz;
		it.x = ox;
		it.y = oy;

		std::vector<input_tile> its;
		its.push_back(it);

		std::string out = overzoom(its, nz, nx, ny, detail, buffer, keep, true, NULL);
		fwrite(out.c_str(), sizeof(char), out.size(), f);
		fclose(f);
	} else {
	}

	return 0;
}
