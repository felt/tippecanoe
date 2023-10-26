#include "geometry.hpp"
#include "mapbox/geometry/earcut.hpp"

using Coord = long long;
using N = size_t;
using Point = std::array<Coord, 2>;

// Return false if the requested adjustment would give
// any of the polygons that contain vertex N a negative area
bool can_adjust(drawvec const &dv, std::vector<N> const &indices, size_t n, double *dx, double *dy) {
	printf("looking for %zu\n", n);

	bool again = true;
	while (again) {
		again = false;
		for (size_t i = 0; i + 2 < indices.size(); i += 3) {
			for (size_t j = 0; j < 3; j++) {
				if (indices[i + j] == indices[n]) {
					drawvec tri;
					for (size_t k = 0; k < 3; k++) {
						printf("found %zu %lld,%lld\n", i + j, dv[indices[i + k]].x, dv[indices[i + k]].y);

						tri.push_back(dv[indices[i + k]]);
						tri[k].op = VT_LINETO;

						if (indices[i + k] == indices[n]) {
							tri[k].x += *dx;
							tri[k].y += *dy;
						}
					}
					tri.push_back(tri[0]);
					tri[0].op = VT_MOVETO;
					printf("area %f\n", get_area(tri, 0, tri.size()));
					if (get_area(tri, 0, tri.size()) < 0) {
						*dx /= 2;
						*dy /= 2;
						again = true;
					}
				}
			}
		}
	}

	return true;
}

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
	bool again = true;
	while (again) {
		again = false;
		for (size_t i = 0; i + 2 < indices.size(); i += 3) {
			for (size_t j = 0; j < 3; j++) {
				size_t v1 = i + j;
				size_t v2 = i + ((j + 1) % 3);
				size_t v3 = i + ((j + 2) % 3);

				if (std::llabs(std::llround(out[indices[v1]].x / scale) - std::llround(out[indices[v2]].x / scale)) < 2 &&
				    std::llabs(std::llround(out[indices[v1]].y / scale) - std::llround(out[indices[v2]].y / scale)) < 2) {
					double ang = atan2(out[indices[v2]].y - out[indices[v1]].y, out[indices[v2]].x - out[indices[v1]].x);

					drawvec tri;
					tri.emplace_back(VT_MOVETO, (long long) out[indices[v3]].x, (long long) out[indices[v3]].y);
					tri.emplace_back(VT_LINETO, out[indices[v1]].x - scale * cos(ang) * sqrt(2) * 2, out[indices[v1]].y - scale * sin(ang) * sqrt(2) * 2);
					tri.emplace_back(VT_LINETO, out[indices[v2]].x + scale * cos(ang) * sqrt(2) * 2, out[indices[v2]].y + scale * sin(ang) * sqrt(2) * 2);
					tri.emplace_back(VT_LINETO, (long long) out[indices[v3]].x, (long long) out[indices[v3]].y);
					printf("%f\n", get_area(tri, 0, tri.size()));

					for (auto const &d : tri) {
						out2.push_back(d);
					}
				}
			}
		}
	}

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

			// re-close the ring
			out2.push_back(draw(VT_LINETO, out[i].x, out[i].y));
			i = j - 1;
		}
	}

	return out2;
}
