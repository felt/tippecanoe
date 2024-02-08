#include <stdlib.h>
#include <mapbox/geometry/point.hpp>
#include <mapbox/geometry/multi_polygon.hpp>
#include <mapbox/geometry/wagyu/wagyu.hpp>
#include <limits.h>
#include "geometry.hpp"
#include "errors.hpp"
#include "compression.hpp"
#include "mvt.hpp"
#include "evaluator.hpp"
#include "serial.hpp"
#include "attribute.hpp"

static std::vector<std::pair<double, double>> clip_poly1(std::vector<std::pair<double, double>> &geom,
							 long long minx, long long miny, long long maxx, long long maxy,
							 long long ax, long long ay, long long bx, long long by, drawvec &edge_nodes,
							 bool prevent_simplify_shared_nodes);

drawvec simple_clip_poly(drawvec &geom, long long minx, long long miny, long long maxx, long long maxy,
			 long long ax, long long ay, long long bx, long long by, drawvec &edge_nodes, bool prevent_simplify_shared_nodes) {
	drawvec out;
	if (prevent_simplify_shared_nodes) {
		geom = remove_noop(geom, VT_POLYGON, 0);
	}

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			std::vector<std::pair<double, double>> tmp;
			for (size_t k = i; k < j; k++) {
				double x = geom[k].x;
				double y = geom[k].y;
				tmp.emplace_back(x, y);
			}
			tmp = clip_poly1(tmp, minx, miny, maxx, maxy, ax, ay, bx, by, edge_nodes, prevent_simplify_shared_nodes);
			if (tmp.size() > 0) {
				if (tmp[0].first != tmp[tmp.size() - 1].first || tmp[0].second != tmp[tmp.size() - 1].second) {
					fprintf(stderr, "Internal error: Polygon ring not closed\n");
					exit(EXIT_FAILURE);
				}
			}
			for (size_t k = 0; k < tmp.size(); k++) {
				if (k == 0) {
					out.push_back(draw(VT_MOVETO, std::round(tmp[k].first), std::round(tmp[k].second)));
				} else {
					out.push_back(draw(VT_LINETO, std::round(tmp[k].first), std::round(tmp[k].second)));
				}
			}

			i = j - 1;
		} else {
			fprintf(stderr, "Unexpected operation in polygon %d\n", (int) geom[i].op);
			exit(EXIT_IMPOSSIBLE);
		}
	}

	return out;
}

drawvec simple_clip_poly(drawvec &geom, long long minx, long long miny, long long maxx, long long maxy, bool prevent_simplify_shared_nodes) {
	drawvec dv;
	return simple_clip_poly(geom, minx, miny, maxx, maxy, minx, miny, maxx, maxy, dv, prevent_simplify_shared_nodes);
}

drawvec simple_clip_poly(drawvec &geom, int z, int buffer, drawvec &edge_nodes, bool prevent_simplify_shared_nodes) {
	long long area = 1LL << (32 - z);
	long long clip_buffer = buffer * area / 256;

	return simple_clip_poly(geom, -clip_buffer, -clip_buffer, area + clip_buffer, area + clip_buffer,
				0, 0, area, area, edge_nodes, prevent_simplify_shared_nodes);
}

drawvec clip_point(drawvec &geom, int z, long long buffer) {
	long long min = 0;
	long long area = 1LL << (32 - z);

	min -= buffer * area / 256;
	area += buffer * area / 256;

	return clip_point(geom, min, min, area, area);
}

drawvec clip_point(drawvec &geom, long long minx, long long miny, long long maxx, long long maxy) {
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].x >= minx && geom[i].y >= miny && geom[i].x <= maxx && geom[i].y <= maxy) {
			out.push_back(geom[i]);
		}
	}

	return out;
}

drawvec clip_lines(drawvec &geom, int z, long long buffer) {
	long long min = 0;
	long long area = 1LL << (32 - z);
	min -= buffer * area / 256;
	area += buffer * area / 256;

	return clip_lines(geom, min, min, area, area);
}

drawvec clip_lines(drawvec &geom, long long minx, long long miny, long long maxx, long long maxy) {
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		if (i > 0 && (geom[i - 1].op == VT_MOVETO || geom[i - 1].op == VT_LINETO) && geom[i].op == VT_LINETO) {
			long long x1 = geom[i - 1].x;
			long long y1 = geom[i - 1].y;

			long long x2 = geom[i - 0].x;
			long long y2 = geom[i - 0].y;

			int c = clip(&x1, &y1, &x2, &y2, minx, miny, maxx, maxy);

			if (c > 1) {  // clipped
				out.push_back(draw(VT_MOVETO, x1, y1));
				out.push_back(draw(VT_LINETO, x2, y2));
				out.push_back(draw(VT_MOVETO, geom[i].x, geom[i].y));
			} else if (c == 1) {  // unchanged
				out.push_back(geom[i]);
			} else {  // clipped away entirely
				out.push_back(draw(VT_MOVETO, geom[i].x, geom[i].y));
			}
		} else {
			out.push_back(geom[i]);
		}
	}

	return out;
}

#define INSIDE 0
#define LEFT 1
#define RIGHT 2
#define BOTTOM 4
#define TOP 8

static int computeOutCode(long long x, long long y, long long xmin, long long ymin, long long xmax, long long ymax) {
	int code = INSIDE;

	if (x < xmin) {
		code |= LEFT;
	} else if (x > xmax) {
		code |= RIGHT;
	}

	if (y < ymin) {
		code |= BOTTOM;
	} else if (y > ymax) {
		code |= TOP;
	}

	return code;
}

int clip(long long *x0, long long *y0, long long *x1, long long *y1, long long xmin, long long ymin, long long xmax, long long ymax) {
	int outcode0 = computeOutCode(*x0, *y0, xmin, ymin, xmax, ymax);
	int outcode1 = computeOutCode(*x1, *y1, xmin, ymin, xmax, ymax);
	int accept = 0;
	int changed = 0;

	while (1) {
		if (!(outcode0 | outcode1)) {  // Bitwise OR is 0. Trivially accept and get out of loop
			accept = 1;
			break;
		} else if (outcode0 & outcode1) {  // Bitwise AND is not 0. Trivially reject and get out of loop
			break;
		} else {
			// failed both tests, so calculate the line segment to clip
			// from an outside point to an intersection with clip edge
			long long x = *x0, y = *y0;

			// At least one endpoint is outside the clip rectangle; pick it.
			int outcodeOut = outcode0 ? outcode0 : outcode1;

			// XXX truncating division

			// Now find the intersection point;
			// use formulas y = y0 + slope * (x - x0), x = x0 + (1 / slope) * (y - y0)
			if (outcodeOut & TOP) {	 // point is above the clip rectangle
				x = *x0 + (*x1 - *x0) * (ymax - *y0) / (*y1 - *y0);
				y = ymax;
			} else if (outcodeOut & BOTTOM) {  // point is below the clip rectangle
				x = *x0 + (*x1 - *x0) * (ymin - *y0) / (*y1 - *y0);
				y = ymin;
			} else if (outcodeOut & RIGHT) {  // point is to the right of clip rectangle
				y = *y0 + (*y1 - *y0) * (xmax - *x0) / (*x1 - *x0);
				x = xmax;
			} else if (outcodeOut & LEFT) {	 // point is to the left of clip rectangle
				y = *y0 + (*y1 - *y0) * (xmin - *x0) / (*x1 - *x0);
				x = xmin;
			}

			// Now we move outside point to intersection point to clip
			// and get ready for next pass.
			if (outcodeOut == outcode0) {
				*x0 = x;
				*y0 = y;
				outcode0 = computeOutCode(*x0, *y0, xmin, ymin, xmax, ymax);
				changed = 1;
			} else {
				*x1 = x;
				*y1 = y;
				outcode1 = computeOutCode(*x1, *y1, xmin, ymin, xmax, ymax);
				changed = 1;
			}
		}
	}

	if (accept == 0) {
		return 0;
	} else {
		return changed + 1;
	}
}

static void decode_clipped(mapbox::geometry::multi_polygon<long long> &t, drawvec &out, double scale) {
	out.clear();

	for (size_t i = 0; i < t.size(); i++) {
		for (size_t j = 0; j < t[i].size(); j++) {
			drawvec ring;

			for (size_t k = 0; k < t[i][j].size(); k++) {
				ring.push_back(draw((k == 0) ? VT_MOVETO : VT_LINETO, std::round(t[i][j][k].x / scale), std::round(t[i][j][k].y / scale)));
			}

			if (ring.size() > 0 && ring[ring.size() - 1] != ring[0]) {
				fprintf(stderr, "Had to close ring\n");
				ring.push_back(draw(VT_LINETO, ring[0].x, ring[0].y));
			}

			double area = get_area(ring, 0, ring.size());

			if ((j == 0 && area < 0) || (j != 0 && area > 0)) {
				fprintf(stderr, "Ring area has wrong sign: %f for %zu\n", area, j);
				exit(EXIT_IMPOSSIBLE);
			}

			for (size_t k = 0; k < ring.size(); k++) {
				out.push_back(ring[k]);
			}
		}
	}
}

drawvec clean_or_clip_poly(drawvec &geom, int z, int buffer, bool clip, bool try_scaling) {
	geom = remove_noop(geom, VT_POLYGON, 0);
	mapbox::geometry::multi_polygon<long long> result;

	double scale = 16.0;
	if (!try_scaling) {
		scale = 1.0;
	}

	bool again = true;
	while (again) {
		mapbox::geometry::wagyu::wagyu<long long> wagyu;
		again = false;

		for (size_t i = 0; i < geom.size(); i++) {
			if (geom[i].op == VT_MOVETO) {
				size_t j;
				for (j = i + 1; j < geom.size(); j++) {
					if (geom[j].op != VT_LINETO) {
						break;
					}
				}

				if (j >= i + 4) {
					mapbox::geometry::linear_ring<long long> lr;

					for (size_t k = i; k < j; k++) {
						lr.push_back(mapbox::geometry::point<long long>(geom[k].x * scale, geom[k].y * scale));
					}

					if (lr.size() >= 3) {
						wagyu.add_ring(lr);
					}
				}

				i = j - 1;
			}
		}

		if (clip) {
			long long area = 0xFFFFFFFF;
			if (z != 0) {
				area = 1LL << (32 - z);
			}
			long long clip_buffer = buffer * area / 256;

			mapbox::geometry::linear_ring<long long> lr;

			lr.push_back(mapbox::geometry::point<long long>(scale * -clip_buffer, scale * -clip_buffer));
			lr.push_back(mapbox::geometry::point<long long>(scale * -clip_buffer, scale * (area + clip_buffer)));
			lr.push_back(mapbox::geometry::point<long long>(scale * (area + clip_buffer), scale * (area + clip_buffer)));
			lr.push_back(mapbox::geometry::point<long long>(scale * (area + clip_buffer), scale * -clip_buffer));
			lr.push_back(mapbox::geometry::point<long long>(scale * -clip_buffer, scale * -clip_buffer));

			wagyu.add_ring(lr, mapbox::geometry::wagyu::polygon_type_clip);
		}

		try {
			result.clear();
			wagyu.execute(mapbox::geometry::wagyu::clip_type_union, result, mapbox::geometry::wagyu::fill_type_positive, mapbox::geometry::wagyu::fill_type_positive);
		} catch (std::runtime_error &e) {
			FILE *f = fopen("/tmp/wagyu.log", "w");
			fprintf(f, "%s\n", e.what());
			fprintf(stderr, "%s\n", e.what());
			fprintf(f, "[");

			for (size_t i = 0; i < geom.size(); i++) {
				if (geom[i].op == VT_MOVETO) {
					size_t j;
					for (j = i + 1; j < geom.size(); j++) {
						if (geom[j].op != VT_LINETO) {
							break;
						}
					}

					if (j >= i + 4) {
						mapbox::geometry::linear_ring<long long> lr;

						if (i != 0) {
							fprintf(f, ",");
						}
						fprintf(f, "[");

						for (size_t k = i; k < j; k++) {
							lr.push_back(mapbox::geometry::point<long long>(geom[k].x, geom[k].y));
							if (k != i) {
								fprintf(f, ",");
							}
							fprintf(f, "[%lld,%lld]", geom[k].x, geom[k].y);
						}

						fprintf(f, "]");

						if (lr.size() >= 3) {
						}
					}

					i = j - 1;
				}
			}

			fprintf(f, "]");
			fprintf(f, "\n\n\n\n\n");

			fclose(f);
			fprintf(stderr, "Internal error: Polygon cleaning failed. Log in /tmp/wagyu.log\n");
			exit(EXIT_IMPOSSIBLE);
		}

		if (scale != 1) {
			for (auto const &outer : result) {
				for (auto const &ring : outer) {
					for (auto const &p : ring) {
						if (p.x / scale != std::round(p.x / scale) ||
						    p.y / scale != std::round(p.y / scale)) {
							scale = 1;
							again = true;
							break;
						}
					}
				}
			}
		}
	}

	drawvec ret;
	decode_clipped(result, ret, scale);
	return ret;
}

void to_tile_scale(drawvec &geom, int z, int detail) {
	if (32 - detail - z < 0) {
		for (size_t i = 0; i < geom.size(); i++) {
			geom[i].x = std::round((double) geom[i].x * (1LL << (-(32 - detail - z))));
			geom[i].y = std::round((double) geom[i].y * (1LL << (-(32 - detail - z))));
		}
	} else {
		for (size_t i = 0; i < geom.size(); i++) {
			geom[i].x = std::round((double) geom[i].x / (1LL << (32 - detail - z)));
			geom[i].y = std::round((double) geom[i].y / (1LL << (32 - detail - z)));
		}
	}
}

drawvec from_tile_scale(drawvec const &geom, int z, int detail) {
	drawvec out;
	for (size_t i = 0; i < geom.size(); i++) {
		draw d = geom[i];
		d.x *= (1LL << (32 - detail - z));
		d.y *= (1LL << (32 - detail - z));
		out.push_back(d);
	}
	return out;
}

drawvec remove_noop(drawvec geom, int type, int shift) {
	// first pass: remove empty linetos

	long long ox = 0, oy = 0;
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		long long nx = std::round((double) geom[i].x / (1LL << shift));
		long long ny = std::round((double) geom[i].y / (1LL << shift));

		if (geom[i].op == VT_LINETO && nx == ox && ny == oy) {
			continue;
		}

		if (geom[i].op == VT_CLOSEPATH) {
			out.push_back(geom[i]);
		} else { /* moveto or lineto */
			out.push_back(geom[i]);
			ox = nx;
			oy = ny;
		}
	}

	// second pass: remove unused movetos

	if (type != VT_POINT) {
		geom = out;
		out.resize(0);

		for (size_t i = 0; i < geom.size(); i++) {
			if (geom[i].op == VT_MOVETO) {
				if (i + 1 >= geom.size()) {
					// followed by end-of-geometry: not needed
					continue;
				}

				if (geom[i + 1].op == VT_MOVETO) {
					// followed by another moveto: not needed
					continue;
				}

				if (geom[i + 1].op == VT_CLOSEPATH) {
					// followed by closepath: not possible
					fprintf(stderr, "Shouldn't happen\n");
					i++;  // also remove unused closepath
					continue;
				}
			}

			out.push_back(geom[i]);
		}
	}

	// second pass: remove empty movetos

	if (type == VT_LINE) {
		geom = out;
		out.resize(0);

		for (size_t i = 0; i < geom.size(); i++) {
			if (i > 1 && geom[i].op == VT_MOVETO) {
				if (geom[i - 1].op == VT_LINETO &&
				    std::round((double) geom[i - 1].x / (1LL << shift)) == std::round((double) geom[i].x / (1LL << shift)) &&
				    std::round((double) geom[i - 1].y / (1LL << shift)) == std::round((double) geom[i].y / (1LL << shift))) {
					continue;
				}
			}

			out.push_back(geom[i]);
		}
	}

	return out;
}

double get_area_scaled(const drawvec &geom, size_t i, size_t j) {
	const double max_exact_double = (double) ((1LL << 53) - 1);

	// keep scaling the geometry down until we can calculate its area without overflow
	for (long long scale = 2; scale < (1LL << 30); scale *= 2) {
		long long bx = geom[i].x;
		long long by = geom[i].y;
		bool again = false;

		// https://en.wikipedia.org/wiki/Shoelace_formula
		double area = 0;
		for (size_t k = i; k < j; k++) {
			area += (double) ((geom[k].x - bx) / scale) * (double) ((geom[i + ((k - i + 1) % (j - i))].y - by) / scale);
			if (std::fabs(area) >= max_exact_double) {
				again = true;
				break;
			}
			area -= (double) ((geom[k].y - by) / scale) * (double) ((geom[i + ((k - i + 1) % (j - i))].x - bx) / scale);
			if (std::fabs(area) >= max_exact_double) {
				again = true;
				break;
			}
		}

		if (again) {
			continue;
		} else {
			area /= 2;
			return area * scale * scale;
		}
	}

	fprintf(stderr, "get_area_scaled: can't happen\n");
	exit(EXIT_IMPOSSIBLE);
}

double get_area(const drawvec &geom, size_t i, size_t j) {
	const double max_exact_double = (double) ((1LL << 53) - 1);

	// Coordinates in `geom` are 40-bit integers, so there is no good way
	// to multiply them without possible precision loss. Since they probably
	// do not use the full precision, shift them nearer to the origin so
	// their product is more likely to be exactly representable as a double.
	//
	// (In practice they are actually 34-bit integers: 32 bits for the
	// Mercator world plane, plus another two bits so features can stick
	// off either the left or right side. But that is still too many bits
	// for the product to fit either in a 64-bit long long or in a
	// double where the largest exact integer is 2^53.)
	//
	// If the intermediate calculation still exceeds 2^53, start trying to
	// recalculate the area by scaling down the geometry. This will not
	// produce as precise an area, but it will still be close, and the
	// sign will be correct, which is more important, since the sign
	// determines the winding order of the rings. We can then use that
	// sign with this generally more precise area calculation.

	long long bx = geom[i].x;
	long long by = geom[i].y;

	// https://en.wikipedia.org/wiki/Shoelace_formula
	double area = 0;
	bool overflow = false;
	for (size_t k = i; k < j; k++) {
		area += (double) (geom[k].x - bx) * (double) (geom[i + ((k - i + 1) % (j - i))].y - by);
		if (std::fabs(area) >= max_exact_double) {
			overflow = true;
		}
		area -= (double) (geom[k].y - by) * (double) (geom[i + ((k - i + 1) % (j - i))].x - bx);
		if (std::fabs(area) >= max_exact_double) {
			overflow = true;
		}
	}
	area /= 2;

	if (overflow) {
		double scaled_area = get_area_scaled(geom, i, j);
		if ((area < 0 && scaled_area > 0) || (area > 0 && scaled_area < 0)) {
			area = -area;
		}
	}

	return area;
}

double get_mp_area(drawvec &geom) {
	double ret = 0;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;

			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			ret += get_area(geom, i, j);
			i = j - 1;
		}
	}

	return ret;
}

drawvec close_poly(drawvec &geom) {
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			if (j - 1 > i) {
				if (geom[j - 1].x != geom[i].x || geom[j - 1].y != geom[i].y) {
					fprintf(stderr, "Internal error: polygon not closed\n");
				}
			}

			for (size_t n = i; n < j - 1; n++) {
				out.push_back(geom[n]);
			}
			out.push_back(draw(VT_CLOSEPATH, 0, 0));

			i = j - 1;
		}
	}

	return out;
}

static bool inside(std::pair<double, double> d, int edge, long long minx, long long miny, long long maxx, long long maxy) {
	switch (edge) {
	case 0:	 // top
		return d.second > miny;

	case 1:	 // right
		return d.first < maxx;

	case 2:	 // bottom
		return d.second < maxy;

	case 3:	 // left
		return d.first > minx;
	}

	fprintf(stderr, "internal error inside\n");
	exit(EXIT_FAILURE);
}

static std::pair<double, double> intersect(std::pair<double, double> a, std::pair<double, double> b, int edge, long long minx, long long miny, long long maxx, long long maxy) {
	switch (edge) {
	case 0:	 // top
		return std::pair<double, double>((a.first + (double) (b.first - a.first) * (miny - a.second) / (b.second - a.second)), miny);

	case 1:	 // right
		return std::pair<double, double>(maxx, (a.second + (double) (b.second - a.second) * (maxx - a.first) / (b.first - a.first)));

	case 2:	 // bottom
		return std::pair<double, double>((a.first + (double) (b.first - a.first) * (maxy - a.second) / (b.second - a.second)), maxy);

	case 3:	 // left
		return std::pair<double, double>(minx, (a.second + (double) (b.second - a.second) * (minx - a.first) / (b.first - a.first)));
	}

	fprintf(stderr, "internal error intersecting\n");
	exit(EXIT_FAILURE);
}

// http://en.wikipedia.org/wiki/Sutherland%E2%80%93Hodgman_algorithm
static std::vector<std::pair<double, double>> clip_poly1(std::vector<std::pair<double, double>> &geom,
							 long long minx, long long miny, long long maxx, long long maxy,
							 long long ax, long long ay, long long bx, long long by, drawvec &edge_nodes,
							 bool prevent_simplify_shared_nodes) {
	std::vector<std::pair<double, double>> out = geom;

	for (int edge = 0; edge < 4; edge++) {
		if (out.size() > 0) {
			std::vector<std::pair<double, double>> in = out;
			out.resize(0);

			std::pair<double, double> S = in[in.size() - 1];

			for (size_t e = 0; e < in.size(); e++) {
				std::pair<double, double> E = in[e];

				if (!inside(S, edge, minx, miny, maxx, maxy)) {
					// was outside the buffer

					if (!inside(E, edge, minx, miny, maxx, maxy)) {
						// still outside the buffer
					} else if (!inside(E, edge, ax, ay, bx, by)) {
						// outside the tile but inside the buffer
						out.push_back(intersect(S, E, edge, minx, miny, maxx, maxy));  // on buffer edge
						out.push_back(E);
					} else {
						out.push_back(intersect(S, E, edge, minx, miny, maxx, maxy));  // on buffer edge
						if (prevent_simplify_shared_nodes) {
							out.push_back(intersect(S, E, edge, ax, ay, bx, by));  // on tile boundary
							edge_nodes.push_back(draw(VT_MOVETO, std::round(out.back().first), std::round(out.back().second)));
						}
						out.push_back(E);
					}
				} else if (!inside(S, edge, ax, ay, bx, by)) {
					// was inside the buffer but outside the tile edge

					if (!inside(E, edge, minx, miny, maxx, maxy)) {
						// now outside the buffer
						out.push_back(intersect(S, E, edge, minx, miny, maxx, maxy));  // on buffer edge
					} else if (!inside(E, edge, ax, ay, bx, by)) {
						// still outside the tile edge but inside the buffer
						out.push_back(E);
					} else {
						// now inside the tile
						if (prevent_simplify_shared_nodes) {
							out.push_back(intersect(S, E, edge, ax, ay, bx, by));  // on tile boundary
							edge_nodes.push_back(draw(VT_MOVETO, std::round(out.back().first), std::round(out.back().second)));
						}
						out.push_back(E);
					}
				} else {
					// was inside the tile

					if (!inside(E, edge, minx, miny, maxx, maxy)) {
						// now outside the buffer
						if (prevent_simplify_shared_nodes) {
							out.push_back(intersect(S, E, edge, ax, ay, bx, by));  // on tile boundary
							edge_nodes.push_back(draw(VT_MOVETO, std::round(out.back().first), std::round(out.back().second)));
						}
						out.push_back(intersect(S, E, edge, minx, miny, maxx, maxy));  // on buffer edge
					} else if (!inside(E, edge, ax, ay, bx, by)) {
						// now inside the buffer but outside the tile edge
						if (prevent_simplify_shared_nodes) {
							out.push_back(intersect(S, E, edge, ax, ay, bx, by));  // on tile boundary
							edge_nodes.push_back(draw(VT_MOVETO, std::round(out.back().first), std::round(out.back().second)));
						}
						out.push_back(E);
					} else {
						// still inside the tile
						out.push_back(E);
					}
				}

				S = E;
			}
		}
	}

	if (out.size() > 0) {
		// If the polygon begins and ends outside the edge,
		// the starting and ending points will be left as the
		// places where it intersects the edge. Need to add
		// another point to close the loop.

		if (out[0].first != out[out.size() - 1].first || out[0].second != out[out.size() - 1].second) {
			out.push_back(out[0]);
		}

		if (out.size() < 3) {
			// fprintf(stderr, "Polygon degenerated to a line segment\n");
			out.clear();
			return out;
		}
	}

	return out;
}

std::string overzoom(const std::string &s, int oz, int ox, int oy, int nz, int nx, int ny,
		     int detail, int buffer, std::set<std::string> const &keep, bool do_compress,
		     std::vector<std::pair<unsigned, unsigned>> *next_overzoomed_tiles,
		     bool demultiply, json_object *filter, bool preserve_input_order, std::unordered_map<std::string, attribute_op> const &attribute_accum, std::vector<std::string> const &unidecode_data) {
	mvt_tile tile;

	try {
		bool was_compressed;
		if (!tile.decode(s, was_compressed)) {
			fprintf(stderr, "Couldn't parse tile %d/%u/%u\n", oz, ox, oy);
			exit(EXIT_MVT);
		}
	} catch (std::exception const &e) {
		fprintf(stderr, "PBF decoding error in tile %d/%u/%u\n", oz, ox, oy);
		exit(EXIT_PROTOBUF);
	}

	return overzoom(tile, oz, ox, oy, nz, nx, ny, detail, buffer, keep, do_compress, next_overzoomed_tiles, demultiply, filter, preserve_input_order, attribute_accum, unidecode_data);
}

struct tile_feature {
	drawvec geom;
	int t;
	bool has_id;
	unsigned long long id;
	std::vector<unsigned> tags;
	mvt_layer const *layer;
	size_t seq = 0;
};

static void feature_out(std::vector<tile_feature> const &features, mvt_layer &outlayer, std::set<std::string> const &keep, std::unordered_map<std::string, attribute_op> const &attribute_accum, std::shared_ptr<std::string> const &tile_stringpool) {
	// Add geometry to output feature

	mvt_feature outfeature;
	outfeature.type = features[0].t;
	for (auto const &g : features[0].geom) {
		outfeature.geometry.emplace_back(g.op, g.x, g.y);
	}

	// ID and attributes, if it didn't get clipped away

	if (outfeature.geometry.size() > 0) {
		if (features[0].has_id) {
			outfeature.has_id = true;
			outfeature.id = features[0].id;
		}

		outfeature.seq = features[0].seq;

		if (attribute_accum.size() > 0) {
			// convert the attributes of the output feature
			// from mvt_value to serial_val so they can have
			// attributes from the other features of the
			// multiplier cluster accumulated onto them

			std::unordered_map<std::string, accum_state> attribute_accum_state;
			std::vector<std::string> full_keys;
			std::vector<serial_val> full_values;

			for (size_t i = 0; i + 1 < features[0].tags.size(); i += 2) {
				auto f = attribute_accum.find(features[0].layer->keys[features[0].tags[i]]);
				if (f != attribute_accum.end()) {
					// this attribute has an accumulator, so convert it
					full_keys.push_back(features[0].layer->keys[features[0].tags[i]]);
					full_values.push_back(mvt_value_to_serial_val(features[0].layer->values[features[0].tags[i + 1]]));
				} else {
					// otherwise just tag it directly onto the output feature
					if (keep.size() == 0 || keep.find(features[0].layer->keys[features[0].tags[i]]) != keep.end()) {
						outlayer.tag(outfeature, features[0].layer->keys[features[0].tags[i]], features[0].layer->values[features[0].tags[i + 1]]);
					}
				}
			}

			// accumulate whatever attributes are specified to be accumulated
			// onto the feature that will survive into the output, from the
			// features that will not

			for (size_t i = 1; i < features.size(); i++) {
				for (size_t j = 0; j + 1 < features[i].tags.size(); j += 2) {
					std::string key = features[i].layer->keys[features[i].tags[j]];

					auto f = attribute_accum.find(key);
					if (f != attribute_accum.end()) {
						serial_val val = mvt_value_to_serial_val(features[i].layer->values[features[i].tags[j + 1]]);
						preserve_attribute(f->second, key, val, full_keys, full_values, attribute_accum_state);
					}
				}
			}

			// convert the final attributes back to mvt_value
			// and tag them onto the output feature

			for (size_t i = 0; i < full_keys.size(); i++) {
				if (keep.size() == 0 || keep.find(full_keys[i]) != keep.end()) {
					outlayer.tag(outfeature, full_keys[i], stringified_to_mvt_value(full_values[i].type, full_values[i].s.c_str(), tile_stringpool));
				}
			}
		} else {
			for (size_t i = 0; i + 1 < features[0].tags.size(); i += 2) {
				if (keep.size() == 0 || keep.find(features[0].layer->keys[features[0].tags[i]]) != keep.end()) {
					outlayer.tag(outfeature, features[0].layer->keys[features[0].tags[i]], features[0].layer->values[features[0].tags[i + 1]]);
				}
			}
		}

		outlayer.features.push_back(std::move(outfeature));
	}
}

static struct preservecmp {
	bool operator()(const mvt_feature &a, const mvt_feature &b) {
		return a.seq < b.seq;
	}
} preservecmp;

std::string overzoom(const mvt_tile &tile, int oz, int ox, int oy, int nz, int nx, int ny,
		     int detail, int buffer, std::set<std::string> const &keep, bool do_compress,
		     std::vector<std::pair<unsigned, unsigned>> *next_overzoomed_tiles,
		     bool demultiply, json_object *filter, bool preserve_input_order, std::unordered_map<std::string, attribute_op> const &attribute_accum, std::vector<std::string> const &unidecode_data) {
	mvt_tile outtile;
	std::shared_ptr<std::string> tile_stringpool = std::make_shared<std::string>();

	for (auto const &layer : tile.layers) {
		mvt_layer outlayer = mvt_layer();

		int det = detail;
		if (det <= 0) {
			det = std::round(log(layer.extent) / log(2));
		}

		outlayer.name = layer.name;
		outlayer.version = layer.version;
		outlayer.extent = 1LL << det;

		std::vector<tile_feature> pending_tile_features;

		static const std::string retain_points_multiplier_first = "tippecanoe:retain_points_multiplier_first";
		static const std::string retain_points_multiplier_sequence = "tippecanoe:retain_points_multiplier_sequence";

		for (auto feature : layer.features) {
			bool flush_multiplier_cluster = false;
			if (demultiply) {
				for (ssize_t i = feature.tags.size() - 2; i >= 0; i -= 2) {
					if (layer.keys[feature.tags[i]] == retain_points_multiplier_first) {
						mvt_value v = layer.values[feature.tags[i + 1]];
						if (v.type == mvt_bool && v.numeric_value.bool_value) {
							flush_multiplier_cluster = true;
							feature.tags.erase(feature.tags.begin() + i, feature.tags.begin() + i + 2);
						}
					}

					if (layer.keys[feature.tags[i]] == retain_points_multiplier_sequence) {
						mvt_value v = layer.values[feature.tags[i + 1]];
						feature.seq = mvt_value_to_long_long(v);
						feature.tags.erase(feature.tags.begin() + i, feature.tags.begin() + i + 2);
					}
				}
			} else {
				flush_multiplier_cluster = true;
			}

			if (flush_multiplier_cluster) {
				if (pending_tile_features.size() > 0) {
					feature_out(pending_tile_features, outlayer, keep, attribute_accum, tile_stringpool);
					pending_tile_features.clear();
				}
			}

			std::set<std::string> exclude_attributes;
			if (filter != NULL && !evaluate(feature, layer, filter, exclude_attributes, nz, unidecode_data)) {
				continue;
			}

			drawvec geom;
			int t = feature.type;

			// Convert feature geometry to world coordinates

			long long tilesize = 1LL << (32 - oz);	// source tile size in world coordinates
			draw ring_closure(0, 0, 0);
			bool sametile = (nz == oz && nx == ox && ny == oy && outlayer.extent >= layer.extent);

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

			if (!sametile) {
				// Clip to output tile

				long long xmin = LLONG_MAX;
				long long ymin = LLONG_MAX;
				long long xmax = LLONG_MIN;
				long long ymax = LLONG_MIN;

				for (auto const &g : geom) {
					xmin = std::min(xmin, g.x);
					ymin = std::min(ymin, g.y);
					xmax = std::max(xmax, g.x);
					ymax = std::max(ymax, g.y);
				}

				long long b = outtilesize * buffer / 256;
				if (xmax < -b || ymax < -b || xmin > outtilesize + b || ymin > outtilesize + b) {
					continue;
				}

				if (t == VT_LINE) {
					geom = clip_lines(geom, nz, buffer);
				} else if (t == VT_POLYGON) {
					drawvec dv;
					geom = simple_clip_poly(geom, nz, buffer, dv, false);
				} else if (t == VT_POINT) {
					geom = clip_point(geom, nz, buffer);
				}
			}

			// Scale to output tile extent

			to_tile_scale(geom, nz, det);

			if (!sametile) {
				// Clean geometries

				geom = remove_noop(geom, t, 0);
				if (t == VT_POLYGON) {
					geom = clean_or_clip_poly(geom, 0, 0, false, false);
				}
			}

			if (t == VT_POLYGON) {
				geom = close_poly(geom);
			}

			tile_feature tf;
			tf.geom = std::move(geom);
			tf.t = t;
			tf.has_id = feature.has_id;
			tf.id = feature.id;
			tf.tags = std::move(feature.tags);
			tf.layer = &layer;
			tf.seq = feature.seq;

			pending_tile_features.push_back(tf);
		}

		if (pending_tile_features.size() > 0) {
			feature_out(pending_tile_features, outlayer, keep, attribute_accum, tile_stringpool);
			pending_tile_features.clear();
		}

		if (preserve_input_order) {
			std::sort(outlayer.features.begin(), outlayer.features.end(), preservecmp);
		}

		if (outlayer.features.size() > 0) {
			outtile.layers.push_back(std::move(outlayer));
		}
	}

	if (next_overzoomed_tiles != NULL) {
		// will any child tiles have features in them?
		// find out recursively from the tile we just made.
		//
		// (yes, we should keep them instead of remaking them
		// later, but that first requires figuring out where to
		// keep them.)

		if (outtile.layers.size() > 0) {
			for (size_t x = 0; x < 2; x++) {
				for (size_t y = 0; y < 2; y++) {
					std::string child = overzoom(outtile, nz, nx, ny,
								     nz + 1, nx * 2 + x, ny * 2 + y,
								     detail, buffer, keep, false, NULL,
								     demultiply, filter, preserve_input_order, attribute_accum, unidecode_data);
					if (child.size() > 0) {
						next_overzoomed_tiles->emplace_back(nx * 2 + x, ny * 2 + y);
					}
				}
			}
		}
	}

	if (outtile.layers.size() > 0) {
		std::string pbf = outtile.encode();

		std::string compressed;
		if (do_compress) {
			compress(pbf, compressed, true);
		} else {
			compressed = pbf;
		}

		return compressed;
	} else {
		return "";
	}
}
