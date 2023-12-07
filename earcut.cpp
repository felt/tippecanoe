#include "geometry.hpp"
#include "mapbox/geometry/earcut.hpp"

using Coord = long long;
using N = size_t;
using Point = std::array<Coord, 2>;

drawvec reinforce(drawvec const &pts, std::vector<std::vector<Point>> polygon, double scale) {
	std::vector<N> indices = mapbox::earcut<N>(polygon);

	drawvec out2;
	for (size_t i = 0; i + 2 < indices.size(); i += 3) {
		std::vector<double> lengths;

		for (size_t j = 0; j < 3; j++) {
			size_t v1 = i + j;
			size_t v2 = i + ((j + 1) % 3);
			size_t v3 = i + ((j + 2) % 3);

#if 0
			out2.push_back(draw(VT_MOVETO, pts[indices[v1]].x, pts[indices[v1]].y));
			out2.push_back(draw(VT_LINETO, pts[indices[v2]].x, pts[indices[v2]].y));
			out2.push_back(draw(VT_LINETO, pts[indices[v3]].x, pts[indices[v3]].y));
			out2.push_back(draw(VT_LINETO, pts[indices[v1]].x, pts[indices[v1]].y));
#endif

			double px, py;
			double d = distance_from_line_noclamp(pts[indices[v1]].x, pts[indices[v1]].y,
							      pts[indices[v2]].x, pts[indices[v2]].y,
							      pts[indices[v3]].x, pts[indices[v3]].y,
							      &px, &py);

			if (d < scale) {
				double ang = atan2(py - pts[indices[v1]].y, px - pts[indices[v1]].x);
				double dist = scale - d;

				out2.push_back(draw(VT_MOVETO, pts[indices[v1]].x, pts[indices[v1]].y));
				out2.push_back(draw(VT_LINETO, pts[indices[v2]].x + dist * cos(ang), pts[indices[v2]].y + dist * sin(ang)));
				out2.push_back(draw(VT_LINETO, pts[indices[v3]].x + dist * cos(ang), pts[indices[v3]].y + dist * sin(ang)));
				out2.push_back(draw(VT_LINETO, pts[indices[v1]].x, pts[indices[v1]].y));
			}
		}
	}

	return out2;
}

drawvec fix_by_triangulation(drawvec const &dv, int z, int detail) {
	std::vector<std::vector<Point>> polygon;
	drawvec out, out2;
	double scale = 1LL << (32 - z - detail);

	for (size_t i = 0; i < dv.size(); i++) {
		if (dv[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < dv.size(); j++) {
				if (dv[j].op != VT_LINETO) {
					break;
				}
			}

			if (get_area(dv, i, j) > 0) {
				// outer ring, so process whatever we have so far
				drawvec additional = reinforce(out, polygon, scale);
				for (auto const &d : additional) {
					out2.push_back(d);
				}

				polygon.clear();
				out.clear();
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

	drawvec additional = reinforce(out, polygon, scale);
	for (auto const &d : additional) {
		out2.push_back(d);
	}

	for (auto const &d : dv) {
		out2.push_back(d);
	}

	return out2;
}
