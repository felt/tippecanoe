#include "geometry.hpp"
#include "mapbox/geometry/earcut.hpp"

using Coord = long long;
using N = size_t;
using Point = std::array<Coord, 2>;

drawvec fix_by_triangulation(drawvec const &dv, int z, int detail) {
	std::vector<std::vector<Point>> polygon;
	drawvec out;
	double scale = 1LL << (32 - z - detail);

	for (size_t i = 0; i < dv.size(); i++) {
		if (dv[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < dv.size(); j++) {
				if (dv[j].op != VT_LINETO) {
					break;
				}
			}

			std::vector<Point> ring;
			// j - 1 because earcut docs indicate that it doesn't expect
			// a duplicate last point in each ring
			for (size_t k = i; k < j - 1; k++) {
				Point p = {(long long) dv[k].x, (long long) dv[k].y};
				ring.push_back(p);
				out.push_back(dv[k]);
			}
			polygon.push_back(ring);

			i = j - 1;
		}
	}

	std::vector<N> indices = mapbox::earcut<N>(polygon);

	drawvec out2;
	for (size_t i = 0; i + 2 < indices.size(); i += 3) {
		std::vector<double> lengths;

		for (size_t j = 0; j < 3; j++) {
			size_t v1 = i + j;
			size_t v2 = i + ((j + 1) % 3);
			size_t v3 = i + ((j + 2) % 3);

			double px, py;

			if (distance_from_line(out[indices[v1]].x, out[indices[v1]].y,	// the point
					       out[indices[v2]].x, out[indices[v2]].y,	// start of opposite side
					       out[indices[v3]].x, out[indices[v3]].y,	// end of opposite side
					       &px, &py) < 2 * scale) {
				double ang = atan2(out[indices[v1]].y - py, out[indices[v1]].x - px);

				// make a new triangle that is not so flat
				out2.push_back(draw(VT_MOVETO, out[indices[v2]].x, out[indices[v2]].y));
				out2.push_back(draw(VT_LINETO, out[indices[v3]].x, out[indices[v3]].y));
				out2.push_back(draw(VT_LINETO, px + 2 * scale * cos(ang), py + 2 * scale * sin(ang)));
				out2.push_back(draw(VT_LINETO, out[indices[v2]].x, out[indices[v2]].y));
			}
		}
	}

	// re-close the rings from which we removed the last points earlier

	for (size_t i = 0; i < out.size(); i++) {
		if (out[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < out.size(); j++) {
				if (out[j].op != VT_LINETO) {
					break;
				}
			}

			for (size_t k = i; k < j; k++) {
				out2.push_back(out[k]);
			}

			out2.push_back(draw(VT_LINETO, out[i].x, out[i].y));
			i = j - 1;
		}
	}

	return out2;
}
