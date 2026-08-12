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
#include "read_json.hpp"
#include "projection.hpp"
#include "usage.hpp"

extern char *optarg;
extern int optind;

int detail = 12;  // tippecanoe-style: mvt extent == 1 << detail
int buffer = 5;	  // tippecanoe-style: mvt buffer == extent * buffer / 256;
bool demultiply = false;
bool do_compress = true;
bool deduplicate_by_id = false;

std::string filter;
bool preserve_input_order = false;
std::unordered_map<std::string, attribute_op> attribute_accum;
std::vector<std::string> unidecode_data;

std::set<std::string> keep;
std::set<std::string> exclude;
std::vector<std::string> exclude_prefix;

static const struct option long_options[] = {
	{"Output tile", 0, 0, 0},
	{"output", required_argument, 0, 'o'},
	{"source-tile", required_argument, 0, 't'},
	{"no-tile-compression", no_argument, 0, 'd' & 0x1F},

	{"Tile resolution", 0, 0, 0},
	{"full-detail", required_argument, 0, 'd'},
	{"buffer", required_argument, 0, 'b'},

	{"Filtering feature attributes", 0, 0, 0},
	{"include", required_argument, 0, 'y'},
	{"exclude", required_argument, 0, 'x'},
	{"exclude-prefix", required_argument, 0, 'x' & 0x1F},

	{"Modifying feature attributes", 0, 0, 0},
	{"accumulate-attribute", required_argument, 0, 'E'},

	{"Filtering features", 0, 0, 0},
	{"feature-filter", required_argument, 0, 'j'},
	{"feature-filter-file", required_argument, 0, 'J'},
	{"filter-points-multiplier", no_argument, 0, 'm'},
	{"deduplicate-by-id", no_argument, 0, 'i' & 0x1F},

	{"Line and polygon simplification", 0, 0, 0},
	{"line-simplification", required_argument, 0, 'S'},
	{"tiny-polygon-size", required_argument, 0, 's' & 0x1F},

	{"Reordering features within the tile", 0, 0, 0},
	{"preserve-input-order", no_argument, 0, 'o' & 0x1F},

	{"", 0, 0, 0},
	{"unidecode-data", required_argument, 0, 'u' & 0x1F},

	{0, 0, 0, 0},
};

// the options above, with the usage message headings removed
static struct option real_long_options[sizeof(long_options) / sizeof(long_options[0])];

void usage(char **argv) {
	static const char *const forms[] = {
		"[options] tile.pbf.gz oz/ox/oy nz/nx/ny",
		"[options] --source-tile=nz/nx/ny tile.pbf.gz oz/ox/oy ...",
		NULL,
	};
	static const struct usage_required_option required[] = {
		{"output", "newtile.pbf.gz", 0},
		{NULL, NULL, 0},
	};

	print_usage(stderr, argv[0], forms, long_options, required);
	fprintf(stderr, "\nThe tile nz/nx/ny is created from the tile or tiles oz/ox/oy that contain it.\n");
	fprintf(stderr, "In the second form, each source tile is named by a file name and a z/x/y pair.\n");
	exit(EXIT_FAILURE);
}

std::string read_json_file(const char *fname) {
	std::string out;

	FILE *f = fopen(fname, "r");
	if (f == NULL) {
		perror(optarg);
		exit(EXIT_OPEN);
	}

	char buf[2000];
	size_t nread;
	while ((nread = fread(buf, sizeof(char), 2000, f)) != 0) {
		out += std::string(buf, nread);
	}

	fclose(f);

	return out;
}

int main(int argc, char **argv) {
	int i;
	const char *outtile = NULL;
	const char *outfile = NULL;
	double simplification = 0;
	double tiny_polygon_size = 0;

	std::vector<input_tile> sources;

	strip_usage_headings(long_options, real_long_options);
	std::string getopt_str = getopt_string(real_long_options);

	int option_index = 0;
	while ((i = getopt_long(argc, argv, getopt_str.c_str(), real_long_options, &option_index)) != -1) {
		switch (i) {
		case 'y':
			keep.insert(optarg);
			break;

		case 'x':
			exclude.insert(optarg);
			break;

		case 'x' & 0x1F:
			exclude_prefix.push_back(optarg);
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

		case 'J':
			filter = read_json_file(optarg);
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

		case 't':
			outtile = optarg;
			break;

		case 's' & 0x1F:
			tiny_polygon_size = atof(optarg);
			break;

		case 'S':
			simplification = atof(optarg);
			break;

		case 'd' & 0x1F:
			do_compress = false;
			break;

		case 'i' & 0x1F: {
			deduplicate_by_id = true;
			break;
		}

		default:
			fprintf(stderr, "Unrecognized flag -%c\n", i);
			usage(argv);
		}
	}

	std::vector<input_tile> its;
	int nz, nx, ny;

	if (outfile == NULL) {
		fprintf(stderr, "%s: must specify -o newtile.pbf.gz\n", argv[0]);
		usage(argv);
	}

	if (outtile == NULL) {	// single input
		if (argc - optind != 3) {
			fprintf(stderr, "Wrong number of arguments\n");
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

		input_tile s;
		s.tile = infile;
		s.z = oz;
		s.x = ox;
		s.y = oy;

		sources.push_back(s);
	} else {  // multiple inputs
		if ((argc - optind) % 2 != 0) {
			fprintf(stderr, "Unpaired arguments\n");
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

			input_tile s;
			s.tile = argv[i];
			s.z = oz;
			s.x = ox;
			s.y = oy;

			sources.push_back(s);
		}
	}

	std::string out;

	{
		json_object_ptr json_filter;
		if (filter.size() > 0) {
			json_filter = parse_filter(filter.c_str());
		}

		for (auto const &s : sources) {
			std::string tile;
			char buf[1000];
			int len;

			FILE *f = fopen(s.tile.c_str(), "rb");
			if (f == NULL) {
				perror(s.tile.c_str());
				exit(EXIT_FAILURE);
			}

			while ((len = fread(buf, sizeof(char), 1000, f)) > 0) {
				tile.append(std::string(buf, len));
			}
			fclose(f);

			input_tile t = s;
			t.tile = std::move(tile);
			its.push_back(std::move(t));
		}

		out = overzoom(its, nz, nx, ny, detail, buffer, keep, exclude, exclude_prefix, do_compress, NULL, demultiply, json_filter.get(), preserve_input_order, attribute_accum, unidecode_data, simplification, tiny_polygon_size, std::vector<mvt_layer>(), "", "", SIZE_MAX, std::vector<clipbbox>(), deduplicate_by_id);
	}

	FILE *f = fopen(outfile, "wb");
	if (f == NULL) {
		perror(outfile);
		exit(EXIT_FAILURE);
	}

	fwrite(out.c_str(), sizeof(char), out.size(), f);
	fclose(f);

	return 0;
}
