#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string>
#include <set>
#include "errors.hpp"
#include "mvt.hpp"
#include "geometry.hpp"
#include "evaluator.hpp"
#include "attribute.hpp"
#include "text.hpp"

extern char *optarg;
extern int optind;

int detail = 12;  // tippecanoe-style: mvt extent == 1 << detail
int buffer = 5;	  // tippecanoe-style: mvt buffer == extent * buffer / 256;
bool demultiply = false;
std::string filter;
bool preserve_input_order = false;
std::unordered_map<std::string, attribute_op> attribute_accum;
std::vector<std::string> unidecode_data;

std::set<std::string> keep;

void usage(char **argv) {
	fprintf(stderr, "Usage: %s -o newtile.pbf.gz tile.pbf.gz oz/ox/oy nz/nx/ny\n", argv[0]);
	fprintf(stderr, "to create tile nz/nx/ny from tile oz/ox/oy\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
	int i;
	const char *outfile = NULL;

	struct option long_options[] = {
		{"include", required_argument, 0, 'y'},
		{"full-detail", required_argument, 0, 'd'},
		{"buffer", required_argument, 0, 'b'},
		{"output", required_argument, 0, 'o'},
		{"filter-points-multiplier", no_argument, 0, 'm'},
		{"feature-filter", required_argument, 0, 'j'},
		{"preserve-input-order", no_argument, 0, 'o' & 0x1F},
		{"accumulate-attribute", required_argument, 0, 'E'},
		{"unidecode-data", required_argument, 0, 'u' & 0x1F},

		{0, 0, 0, 0},
	};

	std::string getopt_str;
	for (size_t lo = 0; long_options[lo].name != NULL; lo++) {
		if (long_options[lo].val > ' ') {
			getopt_str.push_back(long_options[lo].val);

			if (long_options[lo].has_arg == required_argument) {
				getopt_str.push_back(':');
			}
		}
	}

	int option_index = 0;
	while ((i = getopt_long(argc, argv, getopt_str.c_str(), long_options, &option_index)) != -1) {
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

		case 'm':
			demultiply = true;
			break;

		case 'j':
			filter = optarg;
			break;

		case 'o' & 0x1F:
			preserve_input_order = true;
			break;

		case 'E':
			set_attribute_accum(attribute_accum, optarg, argv);
			break;

		case 'u' & 0x1F:
			unidecode_data = read_unidecode(optarg);
			break;

		default:
			fprintf(stderr, "Unrecognized flag -%c\n", i);
			usage(argv);
		}
	}

	if (argc - optind != 3) {
		usage(argv);
	}

	if (outfile == NULL) {
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

	json_object *json_filter = NULL;
	if (filter.size() > 0) {
		json_filter = parse_filter(filter.c_str());
	}

	std::string out = overzoom(tile, oz, ox, oy, nz, nx, ny, detail, buffer, keep, true, NULL, demultiply, json_filter, preserve_input_order, attribute_accum, unidecode_data);
	fwrite(out.c_str(), sizeof(char), out.size(), f);
	fclose(f);

	return 0;
}
