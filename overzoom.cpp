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
std::vector<mvt_layer> bins;
std::string accumulate_numeric;

std::set<std::string> keep;
std::set<std::string> exclude;
std::vector<std::string> exclude_prefix;
std::vector<clipbbox> clipbboxes;

void usage(char **argv) {
	fprintf(stderr, "Usage: %s -o newtile.pbf.gz tile.pbf.gz oz/ox/oy nz/nx/ny\n", argv[0]);
	fprintf(stderr, "to create tile nz/nx/ny from tile oz/ox/oy\n");
	fprintf(stderr, "Usage: %s -o newtile.pbf.gz -t nz/nx/ny tile.pbf.gz oz/ox/oy tile2.pbf.gz oz2/ox2/oy2\n", argv[0]);
	fprintf(stderr, "to create tile nz/nx/ny from tiles oz/ox/oy and oz2/ox2/oy2\n");
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
	std::string assign_to_bins;
	std::string bin_by_id_list;

	std::vector<input_tile> sources;

	struct option long_options[] = {
		{"include", required_argument, 0, 'y'},
		{"exclude", required_argument, 0, 'x'},
		{"exclude-prefix", required_argument, 0, 'x' & 0x1F},
		{"full-detail", required_argument, 0, 'd'},
		{"buffer", required_argument, 0, 'b'},
		{"output", required_argument, 0, 'o'},
		{"filter-points-multiplier", no_argument, 0, 'm'},
		{"feature-filter", required_argument, 0, 'j'},
		{"feature-filter-file", required_argument, 0, 'J'},
		{"preserve-input-order", no_argument, 0, 'o' & 0x1F},
		{"accumulate-attribute", required_argument, 0, 'E'},
		{"unidecode-data", required_argument, 0, 'u' & 0x1F},
		{"line-simplification", required_argument, 0, 'S'},
		{"tiny-polygon-size", required_argument, 0, 's' & 0x1F},
		{"source-tile", required_argument, 0, 't'},
		{"assign-to-bins", required_argument, 0, 'b' & 0x1F},
		{"bin-by-id-list", required_argument, 0, 'c' & 0x1F},
		{"accumulate-numeric-attributes", required_argument, 0, 'a' & 0x1F},
		{"no-tile-compression", no_argument, 0, 'd' & 0x1F},
		{"clip-bounding-box", required_argument, 0, 'k' & 0x1F},
		{"clip-polygon", required_argument, 0, 'l' & 0x1F},
		{"clip-polygon-file", required_argument, 0, 'm' & 0x1F},
		{"deduplicate-by-id", no_argument, 0, 'i' & 0x1F},

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

		case 'b' & 0x1F:
			assign_to_bins = optarg;
			break;

		case 'c' & 0x1F:
			bin_by_id_list = optarg;
			break;

		case 'a' & 0x1F:
			accumulate_numeric = optarg;
			break;

		case 'd' & 0x1F:
			do_compress = false;
			break;

		case 'k' & 0x1F: {
			clipbbox clip;
			if (sscanf(optarg, "%lf,%lf,%lf,%lf", &clip.lon1, &clip.lat1, &clip.lon2, &clip.lat2) == 4) {
				projection->project(clip.lon1, clip.lat1, 32, &clip.minx, &clip.maxy);
				projection->project(clip.lon2, clip.lat2, 32, &clip.maxx, &clip.miny);
				clipbboxes.push_back(clip);
			} else {
				fprintf(stderr, "%s: Can't parse bounding box --clip-bounding-box=%s\n", argv[0], optarg);
				exit(EXIT_ARGS);
			}
			break;
		}

		case 'l' & 0x1F: {
			clipbbox clip = parse_clip_poly(optarg);
			clipbboxes.push_back(clip);
			break;
		}

		case 'm' & 0x1F: {
			clipbbox clip = parse_clip_poly(read_json_file(optarg));
			clipbboxes.push_back(clip);
			break;
		}

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

	if (assign_to_bins.size() != 0) {
		FILE *f = fopen(assign_to_bins.c_str(), "r");
		if (f == NULL) {
			perror(assign_to_bins.c_str());
			exit(EXIT_OPEN);
		}

		int det = detail;
		if (det < 0) {
			det = 12;
		}
		bins = parse_layers(f, nz, nx, ny, 1LL << det, true);
		fclose(f);
	}

	// clip the clip polygons, if any, to the tile bounds,
	// to reduce their complexity

	bool clipped_to_nothing = false;
	if (clipbboxes.size() > 0) {
		long long wx1 = (nx - buffer / 256.0) * (1LL << (32 - nz));
		long long wy1 = (ny - buffer / 256.0) * (1LL << (32 - nz));
		long long wx2 = (nx + 1 + buffer / 256.0) * (1LL << (32 - nz));
		long long wy2 = (ny + 1 + buffer / 256.0) * (1LL << (32 - nz));

		drawvec tile_bounds;
		tile_bounds.emplace_back(VT_MOVETO, wx1, wy1);
		tile_bounds.emplace_back(VT_LINETO, wx2, wy1);
		tile_bounds.emplace_back(VT_LINETO, wx2, wy2);
		tile_bounds.emplace_back(VT_LINETO, wx1, wy2);
		tile_bounds.emplace_back(VT_LINETO, wx1, wy1);

		for (auto &c : clipbboxes) {
			c.minx = std::max(c.minx, wx1);
			c.miny = std::max(c.miny, wy1);
			c.maxx = std::min(c.maxx, wx2);
			c.maxy = std::min(c.maxy, wy2);

			if (c.dv.size() > 0) {
				c.dv = clip_poly_poly(c.dv, tile_bounds);

				if (c.dv.size() == 0) {
					clipped_to_nothing = true;
					break;
				}
			}
		}
	}

	std::string out;

	if (!clipped_to_nothing) {
		json_object *json_filter = NULL;
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

		out = overzoom(its, nz, nx, ny, detail, buffer, keep, exclude, exclude_prefix, do_compress, NULL, demultiply, json_filter, preserve_input_order, attribute_accum, unidecode_data, simplification, tiny_polygon_size, bins, bin_by_id_list, accumulate_numeric, SIZE_MAX, clipbboxes, deduplicate_by_id);
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
