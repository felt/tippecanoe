#include <stack>
#include <set>
#include <stdlib.h>
#include <mapbox/geometry/point.hpp>
#include <mapbox/geometry/multi_polygon.hpp>
#include <mapbox/geometry/wagyu/wagyu.hpp>
#include <clipper2/clipper.h>
#include <limits.h>
#include "geometry.hpp"
#include "errors.hpp"
#include "compression.hpp"
#include "mvt.hpp"
#include "evaluator.hpp"
#include "serial.hpp"
#include "attribute.hpp"
#include "projection.hpp"
#include "read_json.hpp"

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
							fprintf(f, "[%lld,%lld]", (long long) geom[k].x, (long long) geom[k].y);
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

drawvec clip_poly_poly(drawvec const &geom, drawvec const &bounds) {
	mapbox::geometry::multi_polygon<long long> result;

	{
		mapbox::geometry::wagyu::wagyu<long long> wagyu;

		for (size_t i = 0; i < geom.size(); i++) {
			if (geom[i].op == VT_MOVETO) {
				mapbox::geometry::linear_ring<long long> lr;
				lr.push_back(mapbox::geometry::point<long long>(geom[i].x, geom[i].y));

				size_t j;
				for (j = i + 1; j < geom.size(); j++) {
					if (geom[j].op != VT_LINETO) {
						break;
					}
					lr.push_back(mapbox::geometry::point<long long>(geom[j].x, geom[j].y));
				}

				if (lr.size() >= 4) {
					wagyu.add_ring(lr);
				}

				i = j - 1;
			}
		}

		for (size_t i = 0; i < bounds.size(); i++) {
			if (bounds[i].op == VT_MOVETO) {
				mapbox::geometry::linear_ring<long long> lr;
				lr.push_back(mapbox::geometry::point<long long>(bounds[i].x, bounds[i].y));

				size_t j;
				for (j = i + 1; j < bounds.size(); j++) {
					if (bounds[j].op != VT_LINETO) {
						break;
					}
					lr.push_back(mapbox::geometry::point<long long>(bounds[j].x, bounds[j].y));
				}

				if (lr.size() >= 4) {
					wagyu.add_ring(lr, mapbox::geometry::wagyu::polygon_type_clip);
				}

				i = j - 1;
			}
		}

		try {
			result.clear();
			wagyu.execute(mapbox::geometry::wagyu::clip_type_intersection, result, mapbox::geometry::wagyu::fill_type_positive, mapbox::geometry::wagyu::fill_type_positive);
		} catch (std::runtime_error &e) {
			fprintf(stderr, "Internal error: Polygon clipping failed\n");
			exit(EXIT_IMPOSSIBLE);
		}
	}

	drawvec ret;
	decode_clipped(result, ret, 1);
	return ret;
}

drawvec clip_point_poly(drawvec const &geom, drawvec const &bounds) {
	drawvec out;
	for (auto const &p : geom) {
		if (pnpoly_mp(bounds, p.x, p.y)) {
			out.push_back(p);
		}
	}
	return out;
}

static Clipper2Lib::Paths64 geom_to_clipper(drawvec const &geom) {
	Clipper2Lib::Paths64 subject;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			Clipper2Lib::Path64 path({{geom[i].x, geom[i].y}});
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
				path.emplace_back(geom[j].x, geom[j].y);
			}
			subject.push_back(path);
		}
	}

	return subject;
}

static void clipper_to_geom(Clipper2Lib::Paths64 const &geom, drawvec &out) {
	for (auto const &ring : geom) {
		for (size_t i = 0; i < ring.size(); i++) {
			out.emplace_back(i == 0 ? VT_MOVETO : VT_LINETO, ring[i].x, ring[i].y);
		}
	}
}

drawvec clip_lines_poly(drawvec const &geom, drawvec const &region) {
	Clipper2Lib::Paths64 subject = geom_to_clipper(geom);
	Clipper2Lib::Paths64 clip = geom_to_clipper(region);

	Clipper2Lib::Clipper64 clipper;
	clipper.AddOpenSubject(subject);
	clipper.AddClip(clip);

	Clipper2Lib::Paths64 solution, open_solution;
	clipper.Execute(Clipper2Lib::ClipType::Intersection, Clipper2Lib::FillRule::Positive, solution, open_solution);

	drawvec out;
	clipper_to_geom(solution, out);
	clipper_to_geom(open_solution, out);
	return out;
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
					// followed by closepath: only possible after close_poly()
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

double distance_from_line(long long point_x, long long point_y, long long segA_x, long long segA_y, long long segB_x, long long segB_y) {
	long long p2x = segB_x - segA_x;
	long long p2y = segB_y - segA_y;

	// These calculations must be made in integers instead of floating point
	// to make them consistent between x86 and arm floating point implementations.
	//
	// Coordinates may be up to 34 bits, so their product is up to 68 bits,
	// making their sum up to 69 bits. Downshift before multiplying to keep them in range.
	double something = ((p2x / 4) * (p2x / 8) + (p2y / 4) * (p2y / 8)) * 32.0;
	// likewise
	double u = (0 == something) ? 0 : ((point_x - segA_x) / 4 * (p2x / 8) + (point_y - segA_y) / 4 * (p2y / 8)) * 32.0 / (something);

	if (u >= 1) {
		u = 1;
	} else if (u <= 0) {
		u = 0;
	}

	double x = segA_x + u * p2x;
	double y = segA_y + u * p2y;

	double dx = x - point_x;
	double dy = y - point_y;

	double out = std::round(sqrt(dx * dx + dy * dy) * 16.0) / 16.0;
	return out;
}

// https://github.com/Project-OSRM/osrm-backend/blob/733d1384a40f/Algorithms/DouglasePeucker.cpp
void douglas_peucker(drawvec &geom, int start, int n, double e, size_t kept, size_t retain, bool prevent_simplify_shared_nodes) {
	std::stack<int> recursion_stack;

	if (!geom[start + 0].necessary || !geom[start + n - 1].necessary) {
		fprintf(stderr, "endpoints not marked necessary\n");
		exit(EXIT_IMPOSSIBLE);
	}

	int prev = 0;
	for (int here = 1; here < n; here++) {
		if (geom[start + here].necessary) {
			recursion_stack.push(prev);
			recursion_stack.push(here);
			prev = here;

			if (prevent_simplify_shared_nodes) {
				if (retain > 0) {
					retain--;
				}
			}
		}
	}
	// These segments are put on the stack from start to end,
	// independent of winding, so note that anything that uses
	// "retain" to force it to keep at least N points will
	// keep a different set of points when wound one way than
	// when wound the other way.

	while (!recursion_stack.empty()) {
		// pop next element
		int second = recursion_stack.top();
		recursion_stack.pop();
		int first = recursion_stack.top();
		recursion_stack.pop();

		double max_distance = -1;
		int farthest_element_index;

		// find index idx of element with max_distance
		int i;
		if (geom[start + first] < geom[start + second]) {
			farthest_element_index = first;
			for (i = first + 1; i < second; i++) {
				double temp_dist = distance_from_line(geom[start + i].x, geom[start + i].y, geom[start + first].x, geom[start + first].y, geom[start + second].x, geom[start + second].y);

				double distance = std::fabs(temp_dist);

				if ((distance > e || kept < retain) && (distance > max_distance || (distance == max_distance && geom[start + i] < geom[start + farthest_element_index]))) {
					farthest_element_index = i;
					max_distance = distance;
				}
			}
		} else {
			farthest_element_index = second;
			for (i = second - 1; i > first; i--) {
				double temp_dist = distance_from_line(geom[start + i].x, geom[start + i].y, geom[start + second].x, geom[start + second].y, geom[start + first].x, geom[start + first].y);

				double distance = std::fabs(temp_dist);

				if ((distance > e || kept < retain) && (distance > max_distance || (distance == max_distance && geom[start + i] < geom[start + farthest_element_index]))) {
					farthest_element_index = i;
					max_distance = distance;
				}
			}
		}

		if (max_distance >= 0) {
			// mark idx as necessary
			geom[start + farthest_element_index].necessary = 1;
			kept++;

			if (geom[start + first] < geom[start + second]) {
				if (1 < farthest_element_index - first) {
					recursion_stack.push(first);
					recursion_stack.push(farthest_element_index);
				}
				if (1 < second - farthest_element_index) {
					recursion_stack.push(farthest_element_index);
					recursion_stack.push(second);
				}
			} else {
				if (1 < second - farthest_element_index) {
					recursion_stack.push(farthest_element_index);
					recursion_stack.push(second);
				}
				if (1 < farthest_element_index - first) {
					recursion_stack.push(first);
					recursion_stack.push(farthest_element_index);
				}
			}
		}
	}
}

// cut-down version of simplify_lines(), not dealing with shared node preservation
static drawvec simplify_lines_basic(drawvec &geom, int z, int detail, double simplification, size_t retain) {
	int res = 1 << (32 - detail - z);

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			geom[i].necessary = 1;
		} else if (geom[i].op == VT_LINETO) {
			geom[i].necessary = 0;
			// if this is actually the endpoint, not an intermediate point,
			// it will be marked as necessary below
		} else {
			geom[i].necessary = 1;
		}
	}

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			geom[i].necessary = 1;
			geom[j - 1].necessary = 1;

			if (j - i > 1) {
				douglas_peucker(geom, i, j - i, res * simplification, 2, retain, false);
			}
			i = j - 1;
		}
	}

	size_t out = 0;
	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].necessary) {
			geom[out++] = geom[i];
		}
	}
	geom.resize(out);
	return geom;
}

drawvec reduce_tiny_poly(drawvec const &geom, int z, int detail, bool *still_needs_simplification, bool *reduced_away, double *accum_area, double tiny_polygon_size) {
	drawvec out;
	const double pixel = (1LL << (32 - detail - z)) * (double) tiny_polygon_size;

	bool included_last_outer = false;
	*still_needs_simplification = false;
	*reduced_away = false;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			double area = get_area(geom, i, j);

			// XXX There is an ambiguity here: If the area of a ring is 0 and it is followed by holes,
			// we don't know whether the area-0 ring was a hole too or whether it was the outer ring
			// that these subsequent holes are somehow being subtracted from. I hope that if a polygon
			// was simplified down to nothing, its holes also became nothing.

			if (area != 0) {
				// These are pixel coordinates, so area > 0 for the outer ring.
				// If the outer ring of a polygon was reduced to a pixel, its
				// inner rings must just have their area de-accumulated rather
				// than being drawn since we don't really know where they are.

				// i.e., this outer ring is small enough that we are including it
				// in a tiny polygon rather than letting it represent itself,
				// OR it is an inner ring and we haven't output an outer ring for it to be
				// cut out of, so we are just subtracting its area from the tiny polygon
				// rather than trying to deal with it geometrically
				if ((area > 0 && area <= pixel * pixel) || (area < 0 && !included_last_outer)) {
					*accum_area += area;
					*reduced_away = true;

					if (area > 0 && *accum_area > pixel * pixel) {
						// XXX use centroid;

						out.emplace_back(VT_MOVETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2);
						out.emplace_back(VT_LINETO, geom[i].x - pixel / 2 + pixel, geom[i].y - pixel / 2);
						out.emplace_back(VT_LINETO, geom[i].x - pixel / 2 + pixel, geom[i].y - pixel / 2 + pixel);
						out.emplace_back(VT_LINETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2 + pixel);
						out.emplace_back(VT_LINETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2);

						*accum_area -= pixel * pixel;
					}

					if (area > 0) {
						included_last_outer = false;
					}
				}
				// i.e., this ring is large enough that it gets to represent itself
				// or it is a tiny hole out of a real polygon, which we are still treating
				// as a real geometry because otherwise we can accumulate enough tiny holes
				// that we will drop the next several outer rings getting back up to 0.
				else {
					for (size_t k = i; k < j && k < geom.size(); k++) {
						out.push_back(geom[k]);
					}

					// which means that the overall polygon has a real geometry,
					// which means that it gets to be simplified.
					*still_needs_simplification = true;

					if (area > 0) {
						included_last_outer = true;
					}
				}
			} else {
				// area is 0: doesn't count as either having been reduced away,
				// since it was probably just degenerate from having been clipped,
				// or as needing simplification, since it produces no output.
			}

			i = j - 1;
		} else {
			fprintf(stderr, "how did we get here with %d in %d?\n", geom[i].op, (int) geom.size());

			for (size_t n = 0; n < geom.size(); n++) {
				fprintf(stderr, "%d/%lld/%lld ", geom[n].op, (long long) geom[n].x, (long long) geom[n].y);
			}
			fprintf(stderr, "\n");

			out.push_back(geom[i]);
		}
	}

	return out;
}

/* pnpoly:
Copyright (c) 1970-2003, Wm. Randolph Franklin

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimers.
Redistributions in binary form must reproduce the above copyright notice in the documentation and/or other materials provided with the distribution.
The name of W. Randolph Franklin may not be used to endorse or promote products derived from this Software without specific prior written permission.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

int pnpoly(const drawvec &vert, size_t start, size_t nvert, long long testx, long long testy) {
	size_t i, j;
	bool c = false;
	for (i = 0, j = nvert - 1; i < nvert; j = i++) {
		if (((vert[i + start].y > testy) != (vert[j + start].y > testy)) &&
		    (testx < (vert[j + start].x - vert[i + start].x) * (testy - vert[i + start].y) / (double) (vert[j + start].y - vert[i + start].y) + vert[i + start].x))
			c = !c;
	}
	return c;
}

bool pnpoly(const std::vector<mvt_geometry> &vert, size_t start, size_t nvert, long long testx, long long testy) {
	size_t i, j;
	bool c = false;
	for (i = 0, j = nvert - 1; i < nvert; j = i++) {
		if (((vert[i + start].y > testy) != (vert[j + start].y > testy)) &&
		    (testx < (vert[j + start].x - vert[i + start].x) * (testy - vert[i + start].y) / (double) (vert[j + start].y - vert[i + start].y) + vert[i + start].x))
			c = !c;
	}
	return c;
}

bool pnpoly_mp(std::vector<mvt_geometry> const &geom, long long x, long long y) {
	// assumes rings are properly nested, so inside a hole matches twice
	bool found = false;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == mvt_moveto) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != mvt_lineto) {
					break;
				}
			}

			found ^= pnpoly(geom, i, j - i, x, y);
			i = j - 1;
		}
	}

	return found;
}

bool pnpoly_mp(drawvec const &geom, long long x, long long y) {
	// assumes rings are properly nested, so inside a hole matches twice
	bool found = false;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			found ^= pnpoly(geom, i, j - i, x, y);
			i = j - 1;
		}
	}

	return found;
}

clipbbox parse_clip_poly(std::string arg) {
	json_pull *jp = json_begin_string(arg.c_str());
	json_object *j = json_read_tree(jp);
	if (j == NULL) {
		fprintf(stderr, "Expected JSON object, not %s\n", arg.c_str());
		exit(EXIT_ARGS);
	}
	if (j->type != JSON_HASH) {
		fprintf(stderr, "Expected JSON geometry object, not %s\n", arg.c_str());
		exit(EXIT_ARGS);
	}

	std::pair<int, drawvec> parsed_geometry = parse_geometry(j, jp, j, 0, 0, 0, 1LL << 32, false, false);
	json_end(jp);

	clipbbox out;
	out.minx = LLONG_MAX;
	out.miny = LLONG_MAX;
	out.maxx = LLONG_MIN;
	out.maxy = LLONG_MIN;
	for (auto const &d : parsed_geometry.second) {
		if (d.op == VT_MOVETO || d.op == VT_LINETO) {
			if (d.x < out.minx) {
				out.minx = d.x;
			}
			if (d.y < out.miny) {
				out.miny = d.y;
			}
			if (d.x > out.maxx) {
				out.maxx = d.x;
			}
			if (d.y > out.maxy) {
				out.maxy = d.y;
			}
		}
	}
	out.dv = std::move(parsed_geometry.second);

	return out;
}

std::string overzoom(std::vector<input_tile> const &tiles, int nz, int nx, int ny,
		     int detail_or_unspecified, int buffer,
		     std::set<std::string> const &keep,
		     std::set<std::string> const &exclude,
		     std::vector<std::string> const &exclude_prefix,
		     bool do_compress,
		     std::vector<std::pair<unsigned, unsigned>> *next_overzoomed_tiles,
		     bool demultiply, json_object *filter, bool preserve_input_order,
		     std::unordered_map<std::string, attribute_op> const &attribute_accum,
		     std::vector<std::string> const &unidecode_data, double simplification,
		     double tiny_polygon_size,
		     std::vector<mvt_layer> const &bins, std::string const &bin_by_id_list,
		     std::string const &accumulate_numeric, size_t feature_limit,
		     std::vector<clipbbox> const &clipbboxes,
		     bool deduplicate_by_id) {
	std::vector<source_tile> decoded;

	for (auto const &t : tiles) {
		mvt_tile tile;

		try {
			bool was_compressed;
			if (!tile.decode(t.tile, was_compressed)) {
				fprintf(stderr, "Couldn't parse tile %d/%u/%u\n", t.z, t.x, t.y);
				exit(EXIT_MVT);
			}
		} catch (std::exception const &e) {
			fprintf(stderr, "PBF decoding error in tile %d/%u/%u\n", t.z, t.x, t.y);
			exit(EXIT_PROTOBUF);
		}

		source_tile out;
		out.tile = tile;
		out.z = t.z;
		out.x = t.x;
		out.y = t.y;

		decoded.push_back(out);
	}

	return overzoom(decoded, nz, nx, ny, detail_or_unspecified, buffer, keep, exclude, exclude_prefix, do_compress, next_overzoomed_tiles, demultiply, filter, preserve_input_order, attribute_accum, unidecode_data, simplification, tiny_polygon_size, bins, bin_by_id_list, accumulate_numeric, feature_limit, clipbboxes, deduplicate_by_id);
}

// like a minimal serial_feature, but with mvt_feature-style attributes
struct tile_feature {
	drawvec geom;
	int t;
	bool has_id;
	unsigned long long id;
	std::vector<unsigned> tags;
	mvt_layer const *layer;
	size_t seq = 0;
};

static bool should_keep(std::string const &key,
			std::set<std::string> const &keep,
			std::set<std::string> const &exclude,
			std::vector<std::string> const &exclude_prefix) {
	if (keep.size() == 0 || keep.find(key) != keep.end()) {
		if (exclude.find(key) != exclude.end()) {
			return false;
		}

		for (auto const &prefix : exclude_prefix) {
			if (starts_with(key, prefix)) {
				return false;
			}
		}

		return true;
	}

	return false;
}

static void add_mean(mvt_feature &feature, mvt_layer &layer, std::string const &accumulate_numeric,
		     std::set<std::string> const &keep, std::set<std::string> const &exclude,
		     std::vector<std::string> const &exclude_prefix) {
	std::string accumulate_numeric_colon = accumulate_numeric + ":";

	std::unordered_map<std::string, size_t> attributes;
	for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
		std::string const &key = layer.keys[feature.tags[i]];
		if (starts_with(key, accumulate_numeric_colon)) {
			attributes.emplace(key, i);
		}
	}

	for (size_t i = 0; i + 1 < feature.tags.size(); i += 2) {
		std::string accumulate_numeric_sum_colon = accumulate_numeric + ":sum:";

		std::string const &key = layer.keys[feature.tags[i]];
		if (starts_with(key, accumulate_numeric_sum_colon)) {
			std::string trunc = key.substr(accumulate_numeric_sum_colon.size());
			auto const f = attributes.find(accumulate_numeric + ":count:" + trunc);
			if (f != attributes.end()) {
				mvt_value const &sum = layer.values[feature.tags[i + 1]];
				mvt_value const &count = layer.values[feature.tags[f->second + 1]];
				double count_val = mvt_value_to_double(count);
				if (count_val <= 0) {
					fprintf(stderr, "can't happen: count is %s (type %d)\n", count.toString().c_str(), count.type);
					exit(EXIT_IMPOSSIBLE);
				}
				mvt_value mean;
				mean.type = mvt_double;
				mean.numeric_value.double_value = mvt_value_to_double(sum) / count_val;

				if (should_keep(key, keep, exclude, exclude_prefix)) {
					layer.tag(feature, accumulate_numeric + ":mean:" + trunc, mean);
				}
			}
		}
	}
};

// accumulate :sum:, :min:, :max:, and :count: versions of the specified attribute
static void preserve_numeric(const std::string &key, const mvt_value &val,	    // numeric attribute being accumulated
			     std::vector<std::shared_ptr<std::string>> &full_keys,  // keys of feature being accumulated onto
			     std::vector<mvt_value> &full_values,		    // values of features being accumulated onto
			     const std::string &accumulate_numeric,		    // prefix of accumulations
			     std::set<std::string> &keys,			    // key presence in the source feature
			     std::map<std::string, size_t> &numeric_out_field,	    // key index in the output feature
			     key_pool &key_pool,
			     std::set<std::string> const &keep, std::set<std::string> const &exclude,
			     std::vector<std::string> const &exclude_prefix) {
	// If this is a numeric attribute, but there is also a prefix:sum (etc.) for the
	// same attribute, we want to use that one instead of this one.

	for (auto const &op : numeric_operations) {
		std::string compound_key = accumulate_numeric + ":" + op.first + ":" + key;
		auto compound_found = keys.find(compound_key);
		if (compound_found != keys.end()) {
			// found, so skip this one
		} else {
			// not found, so accumulate this one

			// if this is already prefixed, strip off the prefix
			// if it is the right one, and skip the attribute if
			// it is the wrong one.

			std::string outkey = key;
			bool starting_from_accumulation;

			if (starts_with(outkey, accumulate_numeric + ":")) {
				std::string prefix = accumulate_numeric + ":" + op.first + ":";
				if (starts_with(outkey, prefix)) {
					outkey = outkey.substr(prefix.size());
					starting_from_accumulation = true;  // from a subaccumulation
				} else {
					continue;  // to next operation
				}
			} else {
				starting_from_accumulation = false;  // from a plain value
			}
			// and then put it back on for the output field
			std::string prefixed = accumulate_numeric + ":" + op.first + ":" + outkey;

			if (!should_keep(prefixed, keep, exclude, exclude_prefix)) {
				continue;
			}

			// Does it exist in the output feature already?

			auto prefixed_attr = numeric_out_field.find(prefixed);
			if (prefixed_attr == numeric_out_field.end()) {
				// No? Does it exist unprefixed in the output feature already?

				auto out_attr = numeric_out_field.find(outkey);
				if (out_attr == numeric_out_field.end()) {
					// not present at all, so copy our value to the prefixed output
					numeric_out_field.emplace(prefixed, full_keys.size());
					full_keys.push_back(key_pool.pool(prefixed));

					if (op.second == op_count) {
						if (starting_from_accumulation) {
							// copy our count
							full_values.push_back(val);
						} else {
							// new count of 1
							full_values.push_back(mvt_value(1));
						}
					} else {
						full_values.push_back(val);
					}
				} else {
					// exists unprefixed, so copy it, and then accumulate on our value
					numeric_out_field.emplace(prefixed, full_keys.size());
					full_keys.push_back(key_pool.pool(prefixed));

					if (op.second == op_count) {
						mvt_value v;
						if (starting_from_accumulation) {
							// sum our count onto the existing 1
							v = mvt_value(1 + mvt_value_to_long_long(val));
						} else {
							// sum our 1 onto the existing 1
							v = mvt_value(2);
						}
						full_values.push_back(v);
					} else {
						full_values.push_back(full_values[out_attr->second]);
						preserve_attribute(op.second, prefixed, val, full_keys, full_values, key_pool);
					}
				}
			} else {
				// exists, so accumulate on our value
				if (op.second == op_count) {
					if (starting_from_accumulation) {
						// sum our count onto the existing count
						full_values[prefixed_attr->second] = mvt_value(mvt_value_to_long_long(full_values[prefixed_attr->second]) + mvt_value_to_long_long(val));
					} else {
						full_values[prefixed_attr->second] = mvt_value(mvt_value_to_long_long(full_values[prefixed_attr->second]) + 1);
					}
				} else {
					preserve_attribute(op.second, prefixed, val, full_keys, full_values, key_pool);
				}
			}
		}
	}
}

static void handle_closepath_from_mvt(drawvec &geom) {
	// mvt geometries close polygons with a mvt_closepath operation
	// tippecanoe-internal geometries close polygons with a lineto to the initial point

	size_t last_open = 0;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == mvt_closepath) {
			geom[i] = draw(mvt_lineto, geom[last_open].x, geom[last_open].y);
		} else if (geom[i].op == mvt_moveto) {
			last_open = i;
		}
	}
}

// returns true if a feature was output; false if it was clipped away
static bool feature_out(std::vector<tile_feature> const &features, mvt_layer &outlayer,
			std::set<std::string> const &keep,
			std::set<std::string> const &exclude,
			std::vector<std::string> const &exclude_prefix,
			std::unordered_map<std::string, attribute_op> const &attribute_accum,
			std::string const &accumulate_numeric,
			key_pool &key_pool, int buffer, bool include_nonaggregate,
			std::vector<clipbbox> const &clipbboxes, int nz, int nx, int ny,
			std::set<unsigned long long> *deduplicate_ids) {
	// Add geometry to output feature

	drawvec geom = features[0].geom;
	int t = features[0].t;

	bool fix_polygons = false;
	if ((buffer >= 0 || clipbboxes.size() > 0) && t == VT_POLYGON) {
		fix_polygons = true;
	}

	if (fix_polygons) {
		handle_closepath_from_mvt(geom);
	}

	if (buffer >= 0) {
		if (t == VT_LINE) {
			geom = clip_lines(geom, 32 - outlayer.detail(), buffer);
		} else if (t == VT_POLYGON) {
			drawvec dv;
			geom = simple_clip_poly(geom, 32 - outlayer.detail(), buffer, dv, false);
		} else if (t == VT_POINT) {
			geom = clip_point(geom, 32 - outlayer.detail(), buffer);
		}

		geom = remove_noop(geom, t, 0);
	}

	if (clipbboxes.size() != 0) {
		// bounding box is in world coordinates at world scale
		// feature is in local coordinates at tile scale

		long long dx = (long long) nx << (32 - nz);
		long long dy = (long long) ny << (32 - nz);
		double scale = (double) outlayer.extent / (1LL << (32 - nz));

		for (auto const &c_world : clipbboxes) {
			clipbbox c = c_world;

			c.minx = std::llround((c_world.minx - dx) * scale);
			c.miny = std::llround((c_world.miny - dy) * scale);
			c.maxx = std::llround((c_world.maxx - dx) * scale);
			c.maxy = std::llround((c_world.maxy - dy) * scale);

			for (auto &p : c.dv) {
				p.x = std::llround((p.x - dx) * scale);
				p.y = std::llround((p.y - dy) * scale);
			}

			if (t == VT_POLYGON) {
				geom = simple_clip_poly(geom, c.minx, c.miny, c.maxx, c.maxy, false);
				if (c.dv.size() > 0 && geom.size() > 0) {
					geom = clip_poly_poly(geom, c.dv);
				}
			} else if (t == VT_LINE) {
				geom = clip_lines(geom, c.minx, c.miny, c.maxx, c.maxy);
				if (c.dv.size() > 0 && geom.size() > 0) {
					geom = clip_lines_poly(geom, c.dv);
				}
			} else if (t == VT_POINT) {
				geom = clip_point(geom, c.minx, c.miny, c.maxx, c.maxy);
				if (c.dv.size() > 0 && geom.size() > 0) {
					geom = clip_point_poly(geom, c.dv);
				}
			}
		}

		geom = remove_noop(geom, t, 0);
	}

	if (fix_polygons) {
		geom = clean_or_clip_poly(geom, 0, 0, false, false);
		geom = close_poly(geom);
	}

	mvt_feature outfeature;
	outfeature.type = features[0].t;
	for (auto const &g : geom) {
		outfeature.geometry.emplace_back(g.op, g.x, g.y);
	}

	// ID and attributes, if it didn't get clipped away

	if (features[0].has_id && deduplicate_ids != NULL) {
		if (deduplicate_ids->find(features[0].id) != deduplicate_ids->end()) {
			outfeature.geometry.clear();
		} else {
			deduplicate_ids->insert(features[0].id);
		}
	}

	if (outfeature.geometry.size() > 0) {
		if (features[0].has_id) {
			outfeature.has_id = true;
			outfeature.id = features[0].id;
		}

		outfeature.seq = features[0].seq;

		if (attribute_accum.size() > 0 || accumulate_numeric.size() > 0) {
			// convert the attributes of the output feature
			// from layer references to a vector so they can have
			// attributes from the other features of the
			// multiplier cluster accumulated onto them

			std::vector<std::shared_ptr<std::string>> full_keys;
			std::vector<mvt_value> full_values;
			std::map<std::string, size_t> numeric_out_field;

			auto const &f = features[0];
			for (size_t i = 0; i + 1 < f.tags.size(); i += 2) {
				const std::string &key = f.layer->keys[f.tags[i]];
				if (should_keep(key, keep, exclude, exclude_prefix)) {
					if (attribute_accum.find(key) != attribute_accum.end()) {
						// this attribute has an accumulator, so convert it
						full_keys.push_back(key_pool.pool(f.layer->keys[f.tags[i]]));
						full_values.push_back(f.layer->values[f.tags[i + 1]]);
					} else if (accumulate_numeric.size() > 0 && f.layer->values[f.tags[i + 1]].is_numeric()) {
						// convert numeric for accumulation
						numeric_out_field.emplace(key, full_keys.size());
						full_keys.push_back(key_pool.pool(key));
						full_values.push_back(f.layer->values[f.tags[i + 1]]);
					} else if (include_nonaggregate) {
						// otherwise just tag it directly onto the output feature
						outlayer.tag(outfeature, f.layer->keys[f.tags[i]], f.layer->values[f.tags[i + 1]]);
					}
				}
			}

			// accumulate whatever attributes are specified to be accumulated
			// onto the feature that will survive into the output, from the
			// features that will not

			for (size_t i = 1; i < features.size(); i++) {
				std::set<std::string> keys;

				for (size_t j = 0; j + 1 < features[i].tags.size(); j += 2) {
					const std::string &key = features[i].layer->keys[features[i].tags[j]];
					if (should_keep(key, keep, exclude, exclude_prefix)) {
						keys.insert(key);
					}
				}

				for (size_t j = 0; j + 1 < features[i].tags.size(); j += 2) {
					const std::string &key = features[i].layer->keys[features[i].tags[j]];
					if (should_keep(key, keep, exclude, exclude_prefix)) {
						auto found = attribute_accum.find(key);
						if (found != attribute_accum.end()) {
							mvt_value val = features[i].layer->values[features[i].tags[j + 1]];
							preserve_attribute(found->second, key, val, full_keys, full_values, key_pool);
						} else if (accumulate_numeric.size() > 0) {
							const mvt_value &val = features[i].layer->values[features[i].tags[j + 1]];
							if (val.is_numeric()) {
								preserve_numeric(key, val, full_keys, full_values,
										 accumulate_numeric,
										 keys, numeric_out_field, key_pool,
										 keep, exclude, exclude_prefix);
							}
						}
					}
				}
			}

			// convert the final attributes back to mvt_value
			// and tag them onto the output feature

			for (size_t i = 0; i < full_keys.size(); i++) {
				if (should_keep(*full_keys[i], keep, exclude, exclude_prefix)) {
					outlayer.tag(outfeature, *full_keys[i], full_values[i]);
				}
			}

			if (accumulate_numeric.size() > 0) {
				add_mean(outfeature, outlayer, accumulate_numeric, keep, exclude, exclude_prefix);
			}
		} else if (include_nonaggregate) {
			for (size_t i = 0; i + 1 < features[0].tags.size(); i += 2) {
				if (should_keep(features[0].layer->keys[features[0].tags[i]], keep, exclude, exclude_prefix)) {
					outlayer.tag(outfeature, features[0].layer->keys[features[0].tags[i]], features[0].layer->values[features[0].tags[i + 1]]);
				}
			}
		}

		outlayer.features.push_back(std::move(outfeature));
		return true;
	}

	return false;
}

static struct preservecmp {
	bool operator()(const mvt_feature &a, const mvt_feature &b) {
		return a.seq < b.seq;
	}
} preservecmp;

struct index_event {
	unsigned long long where;
	enum index_event_kind {
		ENTER = 0,  // new bin in is now active
		CHECK,	    // point needs to be checked against active bins
		EXIT	    // bin has ceased to be active
	} kind;
	size_t layer;
	size_t feature;
	long long xmin, ymin, xmax, ymax;

	index_event(unsigned long long where_, index_event_kind kind_, size_t layer_, size_t feature_,
		    long long xmin_, long long ymin_, long long xmax_, long long ymax_)
	    : where(where_), kind(kind_), layer(layer_), feature(feature_), xmin(xmin_), ymin(ymin_), xmax(xmax_), ymax(ymax_) {
	}

	bool operator<(const index_event &ie) const {
		if (where < ie.where) {
			return true;
		} else if (where == ie.where) {
			if (kind < ie.kind) {
				return true;
			} else if (kind == ie.kind) {
				if (layer < ie.layer) {
					return true;
				} else if (layer == ie.layer) {
					if (feature < ie.feature) {
						return true;
					}
				}
			}
		}

		return false;
	}
};

struct active_bin {
	size_t layer;
	size_t feature;

	active_bin(size_t layer_, size_t feature_)
	    : layer(layer_), feature(feature_) {
	}

	bool operator<(const active_bin &o) const {
		if (layer < o.layer) {
			return true;
		} else if (layer == o.layer) {
			if (feature < o.feature) {
				return true;
			}
		}

		return false;
	}

	size_t outfeature;
	long long xmin, ymin, xmax, ymax;
	size_t counter = 0;
};

void get_quadkey_bounds(long long xmin, long long ymin, long long xmax, long long ymax,
			unsigned long long *start, unsigned long long *end) {
	if (xmin < 0 || ymin < 0 || xmax >= 1LL << 32 || ymax >= 1LL << 32) {
		*start = 0;
		*end = ULLONG_MAX;
		return;
	}

	*start = encode_quadkey(xmin, ymin);
	*end = encode_quadkey(xmax, ymax);

	for (ssize_t i = 62; i >= 0; i -= 2) {
		if ((*start & (3LL << i)) != (*end & (3LL << i))) {
			for (; i >= 0; i -= 2) {
				*start &= ~(3LL << i);
				*end |= 3LL << i;
			}
			break;
		}
	}
}

static bool bbox_intersects(long long x1min, long long y1min, long long x1max, long long y1max,
			    long long x2min, long long y2min, long long x2max, long long y2max) {
	if (x1max < x2min) {
		return false;
	}
	if (x2max < x1min) {
		return false;
	}
	if (y1max < y2min) {
		return false;
	}
	if (y2max < y1min) {
		return false;
	}
	return true;
}

static std::vector<size_t> parse_ids_string(mvt_value const &v) {
	std::vector<size_t> out;
	std::string s = v.toString();

	for (size_t i = 0; i < s.size(); i++) {
		if (i == 0 || s[i - 1] == ',') {
			out.push_back(atoll(s.c_str() + i));
		}
	}

	return out;
}

mvt_tile assign_to_bins(mvt_tile &features,
			std::vector<mvt_layer> const &bins, std::string const &bin_by_id_list,
			int z, int x, int y,
			std::unordered_map<std::string, attribute_op> const &attribute_accum,
			std::string const &accumulate_numeric,
			std::set<std::string> keep,
			std::set<std::string> exclude,
			std::vector<std::string> exclude_prefix,
			int buffer, std::vector<clipbbox> const &clipbboxes) {
	std::vector<index_event> events;
	key_pool key_pool;

	if (bins.size() == 0) {
		return mvt_tile();
	}

	// Index bins
	for (size_t i = 0; i < bins.size(); i++) {
		for (size_t j = 0; j < bins[i].features.size(); j++) {
			long long xmin, ymin, xmax, ymax;
			unsigned long long start, end;

			get_bbox(bins[i].features[j].geometry, &xmin, &ymin, &xmax, &ymax, z, x, y, bins[i].detail());
			get_quadkey_bounds(xmin, ymin, xmax, ymax, &start, &end);
			events.emplace_back(start, index_event::ENTER, i, j, xmin, ymin, xmax, ymax);
			events.emplace_back(end, index_event::EXIT, i, j, xmin, ymin, xmax, ymax);
		}
	}

	std::map<unsigned long long, std::pair<size_t, size_t>> fid_to_feature;

	// Index points
	for (size_t i = 0; i < features.layers.size(); i++) {
		for (size_t j = 0; j < features.layers[i].features.size(); j++) {
			long long xmin, ymin, xmax, ymax;
			unsigned long long start, end;

			if (features.layers[i].features[j].geometry.size() > 0) {
				if (features.layers[i].features[j].has_id) {
					fid_to_feature.emplace(features.layers[i].features[j].id, std::make_pair(i, j));
				}

				get_bbox(features.layers[i].features[j].geometry, &xmin, &ymin, &xmax, &ymax, z, x, y, features.layers[i].detail());
				get_quadkey_bounds(xmin, ymin, xmax, ymax, &start, &end);
				events.emplace_back(start, index_event::CHECK, i, j, xmin, ymin, xmax, ymax);
			}
		}
	}

	std::sort(events.begin(), events.end());
	std::set<active_bin> active;

	mvt_layer outlayer;
	outlayer.extent = bins[0].extent;
	outlayer.version = 2;
	outlayer.name = features.layers[0].name;

	std::vector<std::vector<tile_feature>> outfeatures;

	for (auto &e : events) {
		if (e.kind == index_event::ENTER) {
			active_bin a(e.layer, e.feature);
			a.xmin = e.xmin;
			a.ymin = e.ymin;
			a.xmax = e.xmax;
			a.ymax = e.ymax;

			const mvt_feature &bin = bins[e.layer].features[e.feature];

			{
				tile_feature outfeature;
				for (auto const &g : bin.geometry) {
					outfeature.geom.emplace_back(g.op, g.x, g.y);
				}
				outfeature.t = bin.type;
				outfeature.has_id = bin.has_id;
				outfeature.id = bin.id;
				outfeature.tags = bin.tags;
				outfeature.layer = &bins[e.layer];
				outfeature.seq = e.feature;

				a.outfeature = outfeatures.size();
				outfeatures.push_back({std::move(outfeature)});
			}

			if (bin_by_id_list.size() > 0) {
				for (size_t k = 0; k < bin.tags.size(); k += 2) {
					if (bins[e.layer].keys[bin.tags[k]] == bin_by_id_list) {
						std::vector<size_t> ids = parse_ids_string(bins[e.layer].values[bin.tags[k + 1]]);
						for (auto &id : ids) {
							auto f = fid_to_feature.find(id);
							if (f != fid_to_feature.end()) {
								mvt_feature &feature = features.layers[f->second.first].features[f->second.second];
								if (feature.geometry.size() > 0) {
									tile_feature outfeature;
									for (auto const &g : feature.geometry) {
										outfeature.geom.emplace_back(g.op, g.x, g.y);
									}
									feature.geometry.clear();
									outfeature.t = feature.type;
									outfeature.has_id = feature.has_id;
									outfeature.id = feature.id;
									outfeature.tags = feature.tags;
									outfeature.layer = &features.layers[e.layer];
									outfeature.seq = e.feature;
									outfeatures.back().push_back(std::move(outfeature));
								}
							}
						}
						break;
					}
				}
			}

			active.insert(std::move(a));
		} else if (e.kind == index_event::CHECK) {
			if (bin_by_id_list.size() > 0) {
				continue;  // only bin by id, not geometrically
			}

			auto const &feature = features.layers[e.layer].features[e.feature];

			if (feature.geometry.size() == 0) {
				// already assigned by ID
				continue;
			}

			// if we can't find a real match,
			// assign points to the most nearby bin
			ssize_t which_outfeature = outfeatures.size() - 1;

			for (auto const &a : active) {
				auto const &bin = bins[a.layer].features[a.feature];

				if (bbox_intersects(e.xmin, e.ymin, e.xmax, e.ymax,
						    a.xmin, a.ymin, a.xmax, a.ymax)) {
					if (pnpoly_mp(bin.geometry, feature.geometry[0].x, feature.geometry[0].y)) {
						which_outfeature = a.outfeature;
						break;
					}
				}
			}

			if (which_outfeature >= 0) {
				tile_feature outfeature;
				for (auto const &g : feature.geometry) {
					outfeature.geom.emplace_back(g.op, g.x, g.y);
				}
				outfeature.t = feature.type;
				outfeature.has_id = feature.has_id;
				outfeature.id = feature.id;
				outfeature.tags = feature.tags;
				outfeature.layer = &features.layers[e.layer];
				outfeature.seq = e.feature;
				outfeatures[which_outfeature].push_back(std::move(outfeature));
			}
		} else /* EXIT */ {
			auto const &found = active.find({e.layer, e.feature});
			if (found != active.end()) {
				active.erase(found);
			} else {
				fprintf(stderr, "event mismatch: can't happen\n");
				exit(EXIT_IMPOSSIBLE);
			}
		}
	}

	for (size_t i = 0; i < outfeatures.size(); i++) {
		if (outfeatures[i].size() > 1) {
			if (feature_out(outfeatures[i], outlayer,
					keep, exclude, exclude_prefix, attribute_accum,
					accumulate_numeric, key_pool, buffer, true,
					clipbboxes, z, x, y, NULL)) {
				mvt_feature &nfeature = outlayer.features.back();
				mvt_value val;
				val.type = mvt_uint;
				val.numeric_value.uint_value = outfeatures[i].size() - 1;

				std::string attrname;
				if (accumulate_numeric.size() == 0) {
					attrname = "tippecanoe:count";
				} else {
					attrname = accumulate_numeric + ":count";
				}
				if (should_keep(attrname, keep, exclude, exclude_prefix)) {
					outlayer.tag(nfeature, attrname, val);
				}
			}
		}
	}

	mvt_tile ret;
	ret.layers.push_back(outlayer);
	return ret;
}

std::string overzoom(std::vector<source_tile> const &tiles, int nz, int nx, int ny,
		     int detail_or_unspecified, int buffer,
		     std::set<std::string> const &keep,
		     std::set<std::string> const &exclude,
		     std::vector<std::string> const &exclude_prefix,
		     bool do_compress,
		     std::vector<std::pair<unsigned, unsigned>> *next_overzoomed_tiles,
		     bool demultiply, json_object *filter, bool preserve_input_order,
		     std::unordered_map<std::string, attribute_op> const &attribute_accum,
		     std::vector<std::string> const &unidecode_data, double simplification,
		     double tiny_polygon_size,
		     std::vector<mvt_layer> const &bins, std::string const &bin_by_id_list,
		     std::string const &accumulate_numeric, size_t feature_limit,
		     std::vector<clipbbox> const &clipbboxes,
		     bool deduplicate_by_id) {
	mvt_tile outtile;
	key_pool key_pool;

	// map from layer name to ids used in that layer
	std::map<std::string, std::set<unsigned long long>> deduplicate_ids;

	for (auto const &tile : tiles) {
		for (auto const &layer : tile.tile.layers) {
			mvt_layer *outlayer = NULL;

			int det = detail_or_unspecified;
			if (det <= 0) {
				det = std::round(log(layer.extent) / log(2));
			}

			for (size_t i = 0; i < outtile.layers.size(); i++) {
				if (outtile.layers[i].name == layer.name) {
					outlayer = &outtile.layers[i];
				}
			}

			if (outlayer == NULL) {
				mvt_layer newlayer = mvt_layer();

				newlayer.name = layer.name;
				newlayer.version = layer.version;
				newlayer.extent = 1LL << det;

				outtile.layers.push_back(newlayer);
				outlayer = &outtile.layers.back();
			}

			std::set<unsigned long long> *deduplicate_by_id_set = NULL;
			if (deduplicate_by_id) {
				auto layer_deduplicate_ids = deduplicate_ids.find(layer.name);
				if (layer_deduplicate_ids == deduplicate_ids.end()) {
					deduplicate_ids.emplace(layer.name, std::set<unsigned long long>());
					layer_deduplicate_ids = deduplicate_ids.find(layer.name);
				}
				deduplicate_by_id_set = &layer_deduplicate_ids->second;
			}

			std::vector<tile_feature> pending_tile_features;
			double accum_area = 0;

			static const std::string retain_points_multiplier_first = "tippecanoe:retain_points_multiplier_first";
			static const std::string retain_points_multiplier_sequence = "tippecanoe:retain_points_multiplier_sequence";

			for (auto feature : layer.features) {
				drawvec geom;
				int t = feature.type;

				// Convert feature geometry to world coordinates

				long long tilesize = 1LL << (32 - tile.z);  // source tile size in world coordinates
				draw ring_closure(0, 0, 0);
				bool sametile = (nz == tile.z && nx == tile.x && ny == tile.y && outlayer->extent >= layer.extent);

				for (auto const &g : feature.geometry) {
					if (g.op == mvt_closepath) {
						geom.push_back(ring_closure);
					} else {
						geom.emplace_back(g.op,
								  g.x * tilesize / layer.extent + tile.x * tilesize,
								  g.y * tilesize / layer.extent + tile.y * tilesize);
						if (g.op == mvt_moveto) {
							ring_closure = geom.back();
							ring_closure.op = mvt_lineto;
						}
					}
				}

				// Clip to user-specified bounding boxes.
				// Bounding box clip first, to reduce complexity of the full clip.
				// But don't clip here if we are binning, because we need to bin points in the buffer
				if (bins.size() == 0) {
					for (auto &c : clipbboxes) {
						if (t == VT_POLYGON) {
							geom = simple_clip_poly(geom, c.minx, c.miny, c.maxx, c.maxy, false);
							if (c.dv.size() > 0 && geom.size() > 0) {
								geom = clip_poly_poly(geom, c.dv);
							}
						} else if (t == VT_LINE) {
							geom = clip_lines(geom, c.minx, c.miny, c.maxx, c.maxy);
							if (c.dv.size() > 0 && geom.size() > 0) {
								geom = clip_lines_poly(geom, c.dv);
							}
						} else if (t == VT_POINT) {
							geom = clip_point(geom, c.minx, c.miny, c.maxx, c.maxy);
							if (c.dv.size() > 0 && geom.size() > 0) {
								geom = clip_point_poly(geom, c.dv);
							}
						}
					}
				}

				// Now offset from world coordinates to output tile coordinates,
				// but retain world scale, because that is what tippecanoe zoom-oriented clipping expects

				long long outtilesize = 1LL << (32 - nz);  // destination tile size in world coordinates
				for (auto &g : geom) {
					g.x -= nx * outtilesize;
					g.y -= ny * outtilesize;
				}

				// Don't clip here if we are binning, because we need to bin points in the buffer
				if (bins.size() == 0) {
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
						// quick exclusion by bounding box
						if (deduplicate_by_id && feature.has_id) {
							deduplicate_by_id_set->insert(feature.id);
						}
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

				if (geom.size() == 0) {
					// clipped away
					if (deduplicate_by_id && feature.has_id) {
						deduplicate_by_id_set->insert(feature.id);
					}
					continue;
				}

				bool flush_multiplier_cluster = false;
				if (demultiply) {
					for (ssize_t i = feature.tags.size() - 2; i >= 0; i -= 2) {
						if (layer.keys[feature.tags[i]] == retain_points_multiplier_first) {
							mvt_value v = layer.values[feature.tags[i + 1]];
							if (v.type == mvt_bool && v.numeric_value.bool_value) {
								flush_multiplier_cluster = true;
								feature.tags.erase(feature.tags.begin() + i, feature.tags.begin() + i + 2);
							}
						} else if (i < (ssize_t) feature.tags.size() && layer.keys[feature.tags[i]] == retain_points_multiplier_sequence) {
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
						feature_out(pending_tile_features, *outlayer, keep, exclude, exclude_prefix, attribute_accum, accumulate_numeric, key_pool, -1, bins.size() == 0, std::vector<clipbbox>(), nz, nx, ny, deduplicate_by_id_set);
						if (outlayer->features.size() >= feature_limit) {
							break;
						}
						pending_tile_features.clear();
					}
				}

				std::set<std::string> exclude_attributes;
				if (filter != NULL && !evaluate(feature, layer, filter, exclude_attributes, nz, unidecode_data)) {
					// filtered away
					if (deduplicate_by_id && feature.has_id) {
						deduplicate_by_id_set->insert(feature.id);
					}
					continue;
				}

				bool still_need_simplification_after_reduction = false;
				if (t == VT_POLYGON && tiny_polygon_size > 0) {
					bool simplified_away_by_reduction = false;

					geom = reduce_tiny_poly(geom, nz, det, &still_need_simplification_after_reduction, &simplified_away_by_reduction, &accum_area, tiny_polygon_size);
				} else {
					still_need_simplification_after_reduction = true;
				}

				if (simplification > 0 && still_need_simplification_after_reduction) {
					if (t == VT_POLYGON) {
						geom = simplify_lines_basic(geom, nz, det, simplification, 4);
					} else if (t == VT_LINE) {
						geom = simplify_lines_basic(geom, nz, det, simplification, 0);
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

				if (geom.size() == 0) {
					// simplified away
					if (deduplicate_by_id && feature.has_id) {
						deduplicate_by_id_set->insert(feature.id);
					}
					continue;
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
				feature_out(pending_tile_features, *outlayer, keep, exclude, exclude_prefix, attribute_accum, accumulate_numeric, key_pool, -1, bins.size() == 0, std::vector<clipbbox>(), nz, nx, ny, deduplicate_by_id_set);
				pending_tile_features.clear();
				if (outlayer->features.size() >= feature_limit) {
					break;
				}
			}

			if (preserve_input_order) {
				std::stable_sort(outlayer->features.begin(), outlayer->features.end(), preservecmp);
			}
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
					source_tile st;
					st.tile = outtile;
					st.z = nz;
					st.x = nx;
					st.y = ny;

					std::vector<source_tile> sts;
					sts.push_back(st);

					// feature_limit arg is 1, because we just care whether there are any overzoomed features
					// left after clipping to the child tile, not about their actual content
					std::string child = overzoom(sts,
								     nz + 1, nx * 2 + x, ny * 2 + y,
								     detail_or_unspecified, buffer, keep, exclude, exclude_prefix, false, NULL,
								     demultiply, filter, preserve_input_order, attribute_accum, unidecode_data,
								     simplification, tiny_polygon_size, bins, bin_by_id_list, accumulate_numeric,
								     1, clipbboxes, deduplicate_by_id);
					if (child.size() > 0) {
						next_overzoomed_tiles->emplace_back(nx * 2 + x, ny * 2 + y);
					}
				}
			}
		}
	}

	if (bins.size() > 0) {
		outtile = assign_to_bins(outtile, bins, bin_by_id_list, nz, nx, ny,
					 attribute_accum, accumulate_numeric,
					 keep, exclude, exclude_prefix, buffer, clipbboxes);
	}

	for (ssize_t i = outtile.layers.size() - 1; i >= 0; i--) {
		if (outtile.layers[i].features.size() == 0) {
			outtile.layers.erase(outtile.layers.begin() + i);
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

drawvec fix_polygon(const drawvec &geom, bool use_winding, bool reverse_winding) {
	int outer = 1;
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_CLOSEPATH) {
			outer = 1;
		} else if (geom[i].op == VT_MOVETO) {
			// Find the end of the ring

			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			// A polygon ring must contain at least three points
			// (and really should contain four). If this one does
			// not have any, avoid a division by zero trying to
			// calculate the centroid below.
			if (j - i < 1) {
				i = j - 1;
				outer = 0;
				continue;
			}

			// Make a temporary copy of the ring.
			// Close it if it isn't closed.

			drawvec ring;
			for (size_t a = i; a < j; a++) {
				ring.push_back(geom[a]);
			}
			if (j - i != 0 && (ring[0].x != ring[j - i - 1].x || ring[0].y != ring[j - i - 1].y)) {
				ring.push_back(ring[0]);
			}

			// A polygon ring at this point should contain at least four points.
			// Flesh it out with some vertex copies if it doesn't.

			while (ring.size() < 4) {
				ring.push_back(ring[0]);
			}

			// Reverse ring if winding order doesn't match
			// inner/outer expectation

			bool reverse_ring = false;
			if (use_winding) {
				// GeoJSON winding is reversed from vector winding
				reverse_ring = true;
			} else if (reverse_winding) {
				// GeoJSON winding is reversed from vector winding
				reverse_ring = false;
			} else {
				double area = get_area(ring, 0, ring.size());
				if ((area > 0) != outer) {
					reverse_ring = true;
				}
			}

			if (reverse_ring) {
				drawvec tmp;
				for (int a = ring.size() - 1; a >= 0; a--) {
					tmp.push_back(ring[a]);
				}
				ring = tmp;
			}

			// Now we are rotating the ring to make the first/last point
			// one that would be unlikely to be simplified away.

			// calculate centroid
			// a + 1 < size() because point 0 is duplicated at the end
			long long xtotal = 0;
			long long ytotal = 0;
			long long count = 0;
			for (size_t a = 0; a + 1 < ring.size(); a++) {
				xtotal += ring[a].x;
				ytotal += ring[a].y;
				count++;
			}
			xtotal /= count;
			ytotal /= count;

			// figure out which point is furthest from the centroid
			long long dist2 = 0;
			long long furthest = 0;
			for (size_t a = 0; a + 1 < ring.size(); a++) {
				// division by 16 because these are z0 coordinates and we need to avoid overflow
				long long xd = (ring[a].x - xtotal) / 16;
				long long yd = (ring[a].y - ytotal) / 16;
				long long d2 = xd * xd + yd * yd;
				if (d2 > dist2 || (d2 == dist2 && ring[a] < ring[furthest])) {
					dist2 = d2;
					furthest = a;
				}
			}

			// then figure out which point is furthest from *that*,
			// which will hopefully be a good origin point since it should be
			// at a far edge of the shape.
			long long dist2b = 0;
			long long furthestb = 0;
			for (size_t a = 0; a + 1 < ring.size(); a++) {
				// division by 16 because these are z0 coordinates and we need to avoid overflow
				long long xd = (ring[a].x - ring[furthest].x) / 16;
				long long yd = (ring[a].y - ring[furthest].y) / 16;
				long long d2 = xd * xd + yd * yd;
				if (d2 > dist2b || (d2 == dist2b && ring[a] < ring[furthestb])) {
					dist2b = d2;
					furthestb = a;
				}
			}

			// rotate ring so the furthest point is the duplicated one.
			// the idea is that simplification will then be more efficient,
			// never wasting the start and end points, which are always retained,
			// on a point that has little impact on the shape.

			// Copy ring into output, fixing the moveto/lineto ops if necessary because of
			// reversal or closing

			for (size_t a = 0; a < ring.size(); a++) {
				size_t a2 = (a + furthestb) % (ring.size() - 1);

				if (a == 0) {
					out.push_back(draw(VT_MOVETO, ring[a2].x, ring[a2].y));
				} else {
					out.push_back(draw(VT_LINETO, ring[a2].x, ring[a2].y));
				}
			}

			// Next ring or polygon begins on the non-lineto that ended this one
			// and is not an outer ring unless there is a terminator first

			i = j - 1;
			outer = 0;
		} else {
			fprintf(stderr, "Internal error: polygon ring begins with %d, not moveto\n", geom[i].op);
			exit(EXIT_IMPOSSIBLE);
		}
	}

	return out;
}

bool line_is_too_small(drawvec const &geometry, int z, int detail) {
	if (geometry.size() == 0) {
		return true;
	}

	long long x = 0, y = 0;
	for (auto &g : geometry) {
		if (g.op == VT_MOVETO) {
			x = std::llround((double) g.x / (1LL << (32 - detail - z)));
			y = std::llround((double) g.y / (1LL << (32 - detail - z)));
		} else {
			long long xx = std::llround((double) g.x / (1LL << (32 - detail - z)));
			long long yy = std::llround((double) g.y / (1LL << (32 - detail - z)));

			if (xx != x || yy != y) {
				return false;
			}
		}
	}

	return true;
}

void coalesce_polygon(drawvec &geom, bool scale_up) {
	// wagyu should be able to straightforwardly handle
	// anything under a few hundred thousand vertices
	if (geom.size() < 100000) {
		geom = clean_or_clip_poly(geom, 0, 0, false, scale_up);
		return;
	}

	// These geometries were assembled in geometric order,
	// so sub-batches of them should hopefully union into
	// reasonable sets.
	//
	// Find the first outer ring after halfway point.

	for (size_t i = geom.size() / 2; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			if (get_area(geom, i, j) > 0) {
				// If we have an outer ring, split there
				// and coalesce the two halves

				// Copy second half to new vector
				std::vector<draw> geom2;
				geom2.resize(geom.size() - i);
				for (size_t k = i; k < geom.size(); k++) {
					geom2[k - i] = geom[k];
				}

				// Resize vector to include only first half
				geom.resize(i);

				// Clean each half individually
				coalesce_polygon(geom, scale_up);
				coalesce_polygon(geom2, scale_up);

				// Copy second half back with first
				size_t brk = geom.size();
				geom.resize(brk + geom2.size());
				for (size_t k = 0; k < geom2.size(); k++) {
					geom[brk + k] = geom2[k];
				}
				geom2.clear();

				// Clean the combined geometry
				geom = clean_or_clip_poly(geom, 0, 0, false, scale_up);
				return;
			}

			i = j - 1;
		}
	}

	// Can't find a breakpoint; take what we can get.
	geom = clean_or_clip_poly(geom, 0, 0, false, scale_up);
}
