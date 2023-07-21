#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string>
#include "errors.hpp"
#include "mvt.hpp"
#include "geometry.hpp"

extern char *optarg;
extern int optind;

int detail = 12;  // tippecanoe-style: mvt extent == 1 << detail
int buffer = 16;  // tippecanoe-style: mvtjbuffer == extent * buffer / 256;

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

			// Convert feature geometry to world coordinates
			int t = feature.type;
			drawvec geom;
			long long tilesize = 1LL << (32 - oz);	// source tile size in world coordinates
			for (auto const &g : feature.geometry) {
				geom.emplace_back(g.op,
						  g.x * tilesize / layer.extent + ox * tilesize,
						  g.y * tilesize / layer.extent + oy * tilesize);
			}

			// Now offset from world coordinates to output tile coordinates,
			// but retain world scale, because that is what tippecanoe clipping expects

			long long outtilesize = 1LL << (32 - nz);  // destination tile size in world coordinates
			for (auto &g : geom) {
				g.x -= nx * outtilesize;
				g.y -= ny * outtilesize;

				// printf("%lld,%lld ", g.x, g.y);
			}
			// printf("\n");

			// Clip to output tile

			if (t == VT_LINE) {
				geom = clip_lines(geom, nz, buffer);
			} else if (t == VT_POLYGON) {
				geom = simple_clip_poly(geom, nz, buffer);
			} else if (t == VT_POINT) {
				geom = clip_point(geom, nz, buffer);
			}

			// printf("points now: ");
			for (auto const &g : geom) {
				// printf("%lld,%lld ", g.x, g.y);
			}
			// printf("\n");

			// Scale to output tile extent

			to_tile_scale(geom, nz, detail);

			// printf("scale, now: ");
			for (auto const &g : geom) {
				// printf("%lld,%lld ", g.x, g.y);
			}
			// printf("\n");

			// Clean polygon geometries

			if (t == VT_POLYGON) {
				geom = clean_or_clip_poly(geom, 0, 0, false);
			}

			// Add geometry to output feature

			for (auto const &g : geom) {
				outfeature.geometry.emplace_back(g.op, g.x, g.y);
			}
			outfeature.type = t;

			// Feature ID

			if (feature.has_id) {
				outfeature.has_id = true;
				outfeature.id = feature.id;
			}

			// XXX attributes

			if (outfeature.geometry.size() > 0) {
				outlayer.features.push_back(outfeature);
			}
		}

		outtile.layers.push_back(outlayer);
	}

	std::string pbf = outtile.encode();
	std::string compressed;
	compress(pbf, compressed, true);

	return compressed;
}

void usage(char **argv) {
	fprintf(stderr, "Usage: %s oz ox oy nz nx ny < tile.pbf.gz > newtile.pbf.gz\n", argv[0]);
	fprintf(stderr, "to create tile nz/nx/ny from tile oz/ox/oy\n");
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
	int i;

	while ((i = getopt(argc, argv, "")) != -1) {
		switch (i) {
		default:
			usage(argv);
		}
	}

	if (argc - optind != 6) {
		usage(argv);
	}

	int oz = atoi(argv[optind]);
	int ox = atoi(argv[optind + 1]);
	int oy = atoi(argv[optind + 2]);

	int nz = atoi(argv[optind + 3]);
	int nx = atoi(argv[optind + 4]);
	int ny = atoi(argv[optind + 5]);

	std::string tile;
	char buf[1000];
	int len;

	while ((len = fread(buf, sizeof(char), 1000, stdin)) > 0) {
		tile.append(std::string(buf, len));
	}

	std::string out = overzoom(tile, oz, ox, oy, nz, nx, ny);
	fwrite(out.c_str(), sizeof(char), out.size(), stdout);
}
