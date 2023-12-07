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
	std::string fname;
	int z;
	int x;
	int y;
};

int main(int argc, char **argv) {
	int i;
	const char *outfile = NULL;
	const char *outtile = NULL;

	std::vector<source> sources;

	while ((i = getopt(argc, argv, "y:o:d:b:t:")) != -1) {
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

	std::vector<input_tile> its;
	int nz, nx, ny;

	if (outtile == NULL) {	// single input
		if (argc - optind != 3) {
			usage(argv);
		}

		const char *infile = argv[optind + 0];

		int oz, ox, oy;
		if (sscanf(argv[optind + 1], "%d/%d/%d", &oz, &ox, &oy) != 3) {
			fprintf(stderr, "%s: not in z/x/y form\n", argv[optind + 1]);
			usage(argv);
		}

		if (sscanf(argv[optind + 2], "%d/%d/%d", &nz, &nx, &ny) != 3) {
			fprintf(stderr, "%s: not in z/x/y form\n", argv[optind + 2]);
			usage(argv);
		}

		source s;
		s.fname = infile;
		s.z = oz;
		s.x = ox;
		s.y = oy;

		sources.push_back(s);
	} else {  // multiple inputs
		if ((argc - optind) % 2 != 0) {
			usage(argv);
		}

		if (sscanf(outtile, "%d/%d/%d", &nz, &nx, &ny) != 3) {
			fprintf(stderr, "%s: not in z/x/y form\n", outtile);
			usage(argv);
		}

		for (i = optind; i + 1 < argc; i += 2) {
			int oz, ox, oy;
			if (sscanf(argv[i + 1], "%d/%d/%d", &oz, &ox, &oy) != 3) {
				fprintf(stderr, "%s: not in z/x/y form\n", argv[i + 1]);
				usage(argv);
			}

			source s;
			s.fname = argv[i];
			s.z = oz;
			s.x = ox;
			s.y = oy;

			sources.push_back(s);
		}
	}

	for (auto const &s : sources) {
		std::string tile;
		char buf[1000];
		int len;

		FILE *f = fopen(s.fname.c_str(), "rb");
		if (f == NULL) {
			perror(s.fname.c_str());
			exit(EXIT_FAILURE);
		}

		while ((len = fread(buf, sizeof(char), 1000, f)) > 0) {
			tile.append(std::string(buf, len));
		}
		fclose(f);

		input_tile it;
		it.tile = tile;
		it.z = s.z;
		it.x = s.x;
		it.y = s.y;

		its.push_back(it);
	}

	std::string out = overzoom(its, nz, nx, ny, detail, buffer, keep, true, NULL);

	FILE *f = fopen(outfile, "wb");
	if (f == NULL) {
		perror(outfile);
		exit(EXIT_FAILURE);
	}
	fwrite(out.c_str(), sizeof(char), out.size(), f);
	fclose(f);

	return 0;
}
