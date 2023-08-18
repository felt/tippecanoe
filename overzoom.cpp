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

std::string overzoom(std::string s, int oz, int ox, int oy, int nz, int nx, int ny) {
	mvt_tile tile, outtile;
	bool was_compressed;

	try {
		if (!tile.decode(s, was_compressed)) {
			fprintf(stderr, "Couldn't parse tile %d/%u/%u\n", oz, ox, oy);
			exit(EXIT_MVT);
		}
	} catch (std::exception const &e) {
		fprintf(stderr, "PBF decoding error in tile %d/%u/%u\n", oz, ox, oy);
		exit(EXIT_PROTOBUF);
	}

	for (auto const &layer : tile.layers) {
		mvt_layer outlayer = mvt_layer();

		outlayer.name = layer.name;
		outlayer.version = layer.version;
		outlayer.extent = 1LL << detail;

		for (auto const &feature : layer.features) {
			mvt_feature outfeature;
			drawvec geom;
			int t = feature.type;

			// Convert feature geometry to world coordinates

			long long tilesize = 1LL << (32 - oz);	// source tile size in world coordinates
			draw ring_closure(0, 0, 0);

			for (auto const &g : feature.geometry) {
				if (g.op == mvt_closepath) {
					geom.push_back(ring_closure);
				} else {
					geom.emplace_back(g.op,
							  g.x * tilesize / layer.extent + ox * tilesize,
							  g.y * tilesize / layer.extent + oy * tilesize);

					if (g.op == mvt_moveto) {
						ring_closure = geom.back();
						ring_closure.op = mvt_lineto;
					}
				}
			}

			// Now offset from world coordinates to output tile coordinates,
			// but retain world scale, because that is what tippecanoe clipping expects

			long long outtilesize = 1LL << (32 - nz);  // destination tile size in world coordinates
			for (auto &g : geom) {
				g.x -= nx * outtilesize;
				g.y -= ny * outtilesize;
			}

			// Clip to output tile

			if (t == VT_LINE) {
				geom = clip_lines(geom, nz, buffer);
			} else if (t == VT_POLYGON) {
				geom = simple_clip_poly(geom, nz, buffer);
			} else if (t == VT_POINT) {
				geom = clip_point(geom, nz, buffer);
			}

			// Scale to output tile extent

			to_tile_scale(geom, nz, detail);

			// Clean geometries

			geom = remove_noop(geom, t, 0);
			if (t == VT_POLYGON) {
				geom = clean_or_clip_poly(geom, 0, 0, false);
				geom = close_poly(geom);
			}

			// Add geometry to output feature

			outfeature.type = t;
			for (auto const &g : geom) {
				outfeature.geometry.emplace_back(g.op, g.x, g.y);
			}

			// ID and attributes, if it didn't get clipped away

			if (outfeature.geometry.size() > 0) {
				if (feature.has_id) {
					outfeature.has_id = true;
					outfeature.id = feature.id;
				}

				for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
					if (keep.size() == 0 || keep.find(layer.keys[feature.tags[i]]) != keep.end()) {
						outlayer.tag(outfeature, layer.keys[feature.tags[i]], layer.values[feature.tags[i + 1]]);
					}
				}

				outlayer.features.push_back(outfeature);
			}
		}

		if (outlayer.features.size() > 0) {
			outtile.layers.push_back(outlayer);
		}
	}

	if (outtile.layers.size() > 0) {
		std::string pbf = outtile.encode();
		std::string compressed;
		compress(pbf, compressed, true);

		return compressed;
	} else {
		return "";
	}
}

void usage(char **argv) {
	fprintf(stderr, "Usage: %s -o newtile.pbf.gz tile.pbf.gz oz/ox/oy nz/nx/ny\n", argv[0]);
	fprintf(stderr, "to create tile nz/nx/ny from tile oz/ox/oy\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
	int i;
	const char *outfile = NULL;

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

		default:
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

	std::string out = overzoom(tile, oz, ox, oy, nz, nx, ny);
	fwrite(out.c_str(), sizeof(char), out.size(), f);
	fclose(f);

	return 0;
}
