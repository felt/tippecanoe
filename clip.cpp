#include <mapbox/geometry/point.hpp>
#include <mapbox/geometry/multi_polygon.hpp>
#include <mapbox/geometry/wagyu/wagyu.hpp>
#include <mapbox/geometry/wagyu/quick_clip.hpp>
#include <mapbox/geometry/snap_rounding.hpp>
#include "geometry.hpp"
#include "errors.hpp"
#include "compression.hpp"
#include "mvt.hpp"

std::string overzoom(std::string s, int oz, int ox, int oy, int nz, int nx, int ny,
		     int detail, int buffer, std::set<std::string> const &keep) {
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

		int det = detail;
		if (det <= 0) {
			det = std::round(log(layer.extent) / log(2));
		}

		outlayer.name = layer.name;
		outlayer.version = layer.version;
		outlayer.extent = 1LL << det;

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
                drawvec dv;
				geom = simple_clip_poly(geom, nz, buffer, dv);
			} else if (t == VT_POINT) {
				geom = clip_point(geom, nz, buffer);
			}

			// Scale to output tile extent

			to_tile_scale(geom, nz, det);

			// Clean geometries

			geom = remove_noop(geom, t, 0);
			if (t == VT_POLYGON) {
				geom = clean_or_clip_poly(geom, 0, 0, false, false);
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
