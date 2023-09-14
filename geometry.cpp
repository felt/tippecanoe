#include <iostream>
#include <fstream>
#include <string>
#include <stack>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <unistd.h>
#include <cmath>
#include <limits.h>
#include <sqlite3.h>
#include <mapbox/geometry/point.hpp>
#include <mapbox/geometry/multi_polygon.hpp>
#include <mapbox/geometry/snap_rounding.hpp>
#include "geometry.hpp"
#include "projection.hpp"
#include "serial.hpp"
#include "main.hpp"
#include "options.hpp"
#include "errors.hpp"
#include "projection.hpp"

drawvec decode_geometry(char **meta, int z, unsigned tx, unsigned ty, long long *bbox, unsigned initial_x, unsigned initial_y) {
	drawvec out;

	bbox[0] = LLONG_MAX;
	bbox[1] = LLONG_MAX;
	bbox[2] = LLONG_MIN;
	bbox[3] = LLONG_MIN;

	long long wx = initial_x, wy = initial_y;

	while (1) {
		draw d;

		deserialize_byte(meta, &d.op);
		if (d.op == VT_END) {
			break;
		}

		if (d.op == VT_MOVETO || d.op == VT_LINETO) {
			long long dx, dy;

			deserialize_long_long(meta, &dx);
			deserialize_long_long(meta, &dy);

			wx += dx * (1 << geometry_scale);
			wy += dy * (1 << geometry_scale);

			long long wwx = wx;
			long long wwy = wy;

			if (z != 0) {
				wwx -= tx << (32 - z);
				wwy -= ty << (32 - z);
			}

			if (wwx < bbox[0]) {
				bbox[0] = wwx;
			}
			if (wwy < bbox[1]) {
				bbox[1] = wwy;
			}
			if (wwx > bbox[2]) {
				bbox[2] = wwx;
			}
			if (wwy > bbox[3]) {
				bbox[3] = wwy;
			}

			d.x = wwx;
			d.y = wwy;
		}

		out.push_back(d);
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

void check_polygon(drawvec &geom) {
	geom = remove_noop(geom, VT_POLYGON, 0);

	mapbox::geometry::multi_polygon<long long> mp;
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
					lr.push_back(mapbox::geometry::point<long long>(geom[k].x, geom[k].y));
				}

				if (lr.size() >= 3) {
					mapbox::geometry::polygon<long long> p;
					p.push_back(lr);
					mp.push_back(p);
				}
			}

			i = j - 1;
		}
	}

	mapbox::geometry::multi_polygon<long long> mp2 = mapbox::geometry::snap_round(mp, true, true);
	if (mp != mp2) {
		fprintf(stderr, "Internal error: self-intersecting polygon\n");
	}

	size_t outer_start = -1;
	size_t outer_len = 0;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			double area = get_area(geom, i, j);

			if (area > 0) {
				outer_start = i;
				outer_len = j - i;
			} else {
				for (size_t k = i; k < j; k++) {
					if (!pnpoly(geom, outer_start, outer_len, geom[k].x, geom[k].y)) {
						bool on_edge = false;

						for (size_t l = outer_start; l < outer_start + outer_len; l++) {
							if (geom[k].x == geom[l].x || geom[k].y == geom[l].y) {
								on_edge = true;
								break;
							}
						}

						if (!on_edge) {
							fprintf(stderr, "%lld,%lld at %lld not in outer ring (%lld to %lld)\n", geom[k].x, geom[k].y, (long long) k, (long long) outer_start, (long long) (outer_start + outer_len));
						}
					}
				}
			}
		}
	}
}

drawvec reduce_tiny_poly(drawvec &geom, int z, int detail, bool *still_needs_simplification, bool *reduced_away, double *accum_area, serial_feature *this_feature, serial_feature *tiny_feature) {
	drawvec out;
	const double pixel = (1LL << (32 - detail - z)) * (double) tiny_polygon_size;
	bool includes_real = false;
	bool includes_dust = false;

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

						out.push_back(draw(VT_MOVETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2));
						out.push_back(draw(VT_LINETO, geom[i].x - pixel / 2 + pixel, geom[i].y - pixel / 2));
						out.push_back(draw(VT_LINETO, geom[i].x - pixel / 2 + pixel, geom[i].y - pixel / 2 + pixel));
						out.push_back(draw(VT_LINETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2 + pixel));
						out.push_back(draw(VT_LINETO, geom[i].x - pixel / 2, geom[i].y - pixel / 2));
						includes_dust = true;

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
					includes_real = true;

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
				fprintf(stderr, "%d/%lld/%lld ", geom[n].op, geom[n].x, geom[n].y);
			}
			fprintf(stderr, "\n");

			out.push_back(geom[i]);
			includes_real = true;
		}
	}

	if (!includes_real) {
		if (includes_dust) {
			// this geometry is just dust, so if there is another feature that
			// contributed to the dust that is larger than this feature,
			// keep its attributes instead of this one that just happened to be
			// the one that hit the threshold of survival.

			if (tiny_feature->extent > this_feature->extent) {
				*this_feature = *tiny_feature;
				tiny_feature->extent = 0;
			}
		} else {
			// this is a feature that we are throwing away, so hang on to it
			// attributes if it is bigger than the biggest one we threw away so far

			if (this_feature->extent > tiny_feature->extent) {
				*tiny_feature = *this_feature;
			}
		}
	}

	return out;
}

int quick_check(long long *bbox, int z, long long buffer) {
	long long min = 0;
	long long area = 1LL << (32 - z);

	// bbox entirely within the tile proper
	if (bbox[0] > min && bbox[1] > min && bbox[2] < area && bbox[3] < area) {
		return 1;
	}

	min -= buffer * area / 256;
	area += buffer * area / 256;

	// bbox entirely within the tile, including its buffer
	if (bbox[0] > min && bbox[1] > min && bbox[2] < area && bbox[3] < area) {
		return 3;
	}

	// bbox entirely outside the tile
	if (bbox[0] > area || bbox[1] > area) {
		return 0;
	}
	if (bbox[2] < min || bbox[3] < min) {
		return 0;
	}

	// some overlap of edge
	return 2;
}

bool point_within_tile(long long x, long long y, int z) {
	// No adjustment for buffer, because the point must be
	// strictly within the tile to appear exactly once

	long long area = 1LL << (32 - z);

	return x >= 0 && y >= 0 && x < area && y < area;
}

double distance_from_line(long long point_x, long long point_y, long long segA_x, long long segA_y, long long segB_x, long long segB_y) {
	long long p2x = segB_x - segA_x;
	long long p2y = segB_y - segA_y;
	double something = p2x * p2x + p2y * p2y;
	double u = (0 == something) ? 0 : ((point_x - segA_x) * p2x + (point_y - segA_y) * p2y) / (something);

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
static void douglas_peucker(drawvec &geom, int start, int n, double e, size_t kept, size_t retain) {
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

			if (prevent[P_SIMPLIFY_SHARED_NODES]) {
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

// If any line segment crosses a tile boundary, add a node there
// that cannot be simplified away, to prevent the edge of any
// feature from jumping abruptly at the tile boundary.
drawvec impose_tile_boundaries(drawvec &geom, long long extent) {
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		if (i > 0 && geom[i].op == VT_LINETO && (geom[i - 1].op == VT_MOVETO || geom[i - 1].op == VT_LINETO)) {
			long long x1 = geom[i - 1].x;
			long long y1 = geom[i - 1].y;

			long long x2 = geom[i - 0].x;
			long long y2 = geom[i - 0].y;

			int c = clip(&x1, &y1, &x2, &y2, 0, 0, extent, extent);

			if (c > 1) {  // clipped
				if (x1 != geom[i - 1].x || y1 != geom[i - 1].y) {
					out.push_back(draw(VT_LINETO, x1, y1));
					out[out.size() - 1].necessary = 1;
				}
				if (x2 != geom[i - 0].x || y2 != geom[i - 0].y) {
					out.push_back(draw(VT_LINETO, x2, y2));
					out[out.size() - 1].necessary = 1;
				}
			}
		}

		out.push_back(geom[i]);
	}

	return out;
}

drawvec simplify_lines(drawvec &geom, int z, int detail, bool mark_tile_bounds, double simplification, size_t retain, drawvec const &shared_nodes) {
	int res = 1 << (32 - detail - z);
	long long area = 1LL << (32 - z);

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

		if (prevent[P_SIMPLIFY_SHARED_NODES]) {
			auto pt = std::lower_bound(shared_nodes.begin(), shared_nodes.end(), geom[i]);
			if (pt != shared_nodes.end() && *pt == geom[i]) {
				geom[i].necessary = true;
			}
		}
	}

	if (mark_tile_bounds) {
		geom = impose_tile_boundaries(geom, area);
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

			// empirical mapping from douglas-peucker simplifications
			// to visvalingam simplifications that yield similar
			// output sizes
			double sim = simplification * (0.1596 * z + 0.878);
			double scale = (res * sim) * (res * sim);
			scale = exp(1.002 * log(scale) + 0.3043);

			if (j - i > 1) {
				if (additional[A_VISVALINGAM]) {
					visvalingam(geom, i, j, scale, retain);
				} else {
					douglas_peucker(geom, i, j - i, res * simplification, 2, retain);
				}
			}
			i = j - 1;
		}
	}

	drawvec out;
	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].necessary) {
			out.push_back(geom[i]);
		}
	}

	return out;
}

drawvec reorder_lines(drawvec &geom) {
	// Only reorder simple linestrings with a single moveto

	if (geom.size() == 0) {
		return geom;
	}

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			if (i != 0) {
				// moveto is not at the start, so it is not simple
				return geom;
			}
		} else if (geom[i].op == VT_LINETO) {
			if (i == 0) {
				// lineto is at the start: can't happen
				return geom;
			}
		} else {
			// something other than moveto or lineto: can't happen
			return geom;
		}
	}

	// Reorder anything that goes up and to the left
	// instead of down and to the right
	// so that it will coalesce better

	unsigned long long l1 = encode_index(geom[0].x, geom[0].y);
	unsigned long long l2 = encode_index(geom[geom.size() - 1].x, geom[geom.size() - 1].y);

	if (l1 > l2) {
		drawvec out;
		for (size_t i = 0; i < geom.size(); i++) {
			out.push_back(geom[geom.size() - 1 - i]);
		}
		out[0].op = VT_MOVETO;
		if (out.size() > 1) {
			out[out.size() - 1].op = VT_LINETO;
		}
		return out;
	}

	return geom;
}

drawvec fix_polygon(drawvec &geom) {
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
			// not have enough, avoid a division by zero trying to
			// calculate the centroid below.
			if (j - i < 3) {
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

			// Reverse ring if winding order doesn't match
			// inner/outer expectation

			bool reverse_ring = false;
			if (prevent[P_USE_SOURCE_POLYGON_WINDING]) {
				// GeoJSON winding is reversed from vector winding
				reverse_ring = true;
			} else if (prevent[P_REVERSE_SOURCE_POLYGON_WINDING]) {
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

#if 0
std::vector<drawvec> chop_polygon(std::vector<drawvec> &geoms) {
	while (1) {
		bool again = false;
		std::vector<drawvec> out;

		for (size_t i = 0; i < geoms.size(); i++) {
			if (geoms[i].size() > 700) {
				static bool warned = false;
				if (!warned) {
					fprintf(stderr, "Warning: splitting up polygon with more than 700 sides\n");
					warned = true;
				}

				long long midx = 0, midy = 0, count = 0;
				long long maxx = LLONG_MIN, maxy = LLONG_MIN, minx = LLONG_MAX, miny = LLONG_MAX;

				for (size_t j = 0; j < geoms[i].size(); j++) {
					if (geoms[i][j].op == VT_MOVETO || geoms[i][j].op == VT_LINETO) {
						midx += geoms[i][j].x;
						midy += geoms[i][j].y;
						count++;

						if (geoms[i][j].x > maxx) {
							maxx = geoms[i][j].x;
						}
						if (geoms[i][j].y > maxy) {
							maxy = geoms[i][j].y;
						}
						if (geoms[i][j].x < minx) {
							minx = geoms[i][j].x;
						}
						if (geoms[i][j].y < miny) {
							miny = geoms[i][j].y;
						}
					}
				}

				midx /= count;
				midy /= count;

				drawvec c1, c2;

				if (maxy - miny > maxx - minx) {
					c1 = simple_clip_poly(geoms[i], minx, miny, maxx, midy, prevent[P_SIMPLIFY_EDGE_NODES]);
					c2 = simple_clip_poly(geoms[i], minx, midy, maxx, maxy, prevent[P_SIMPLIFY_EDGE_NODES]);
				} else {
					c1 = simple_clip_poly(geoms[i], minx, miny, midx, maxy, prevent[P_SIMPLIFY_EDGE_NODES]);
					c2 = simple_clip_poly(geoms[i], midx, miny, maxx, maxy, prevent[P_SIMPLIFY_EDGE_NODES]);
				}

				if (c1.size() >= geoms[i].size()) {
					fprintf(stderr, "Subdividing complex polygon failed\n");
				} else {
					out.push_back(c1);
				}
				if (c2.size() >= geoms[i].size()) {
					fprintf(stderr, "Subdividing complex polygon failed\n");
				} else {
					out.push_back(c2);
				}

				again = true;
			} else {
				out.push_back(geoms[i]);
			}
		}

		if (!again) {
			return out;
		}

		geoms = out;
	}
}
#endif

drawvec stairstep(drawvec &geom, int z, int detail) {
	drawvec out;
	double scale = 1 << (32 - detail - z);

	for (size_t i = 0; i < geom.size(); i++) {
		geom[i].x = std::round(geom[i].x / scale);
		geom[i].y = std::round(geom[i].y / scale);
	}

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			out.push_back(geom[i]);
		} else if (out.size() > 0) {
			long long x0 = out[out.size() - 1].x;
			long long y0 = out[out.size() - 1].y;
			long long x1 = geom[i].x;
			long long y1 = geom[i].y;
			bool swap = false;

			if (y0 < y1) {
				swap = true;
				std::swap(x0, x1);
				std::swap(y0, y1);
			}

			long long xx = x0, yy = y0;
			long long dx = std::abs(x1 - x0);
			long long sx = (x0 < x1) ? 1 : -1;
			long long dy = std::abs(y1 - y0);
			long long sy = (y0 < y1) ? 1 : -1;
			long long err = ((dx > dy) ? dx : -dy) / 2;
			int last = -1;

			drawvec tmp;
			tmp.push_back(draw(VT_LINETO, xx, yy));

			while (xx != x1 || yy != y1) {
				long long e2 = err;

				if (e2 > -dx) {
					err -= dy;
					xx += sx;
					if (last == 1) {
						tmp[tmp.size() - 1] = draw(VT_LINETO, xx, yy);
					} else {
						tmp.push_back(draw(VT_LINETO, xx, yy));
					}
					last = 1;
				}
				if (e2 < dy) {
					err += dx;
					yy += sy;
					if (last == 2) {
						tmp[tmp.size() - 1] = draw(VT_LINETO, xx, yy);
					} else {
						tmp.push_back(draw(VT_LINETO, xx, yy));
					}
					last = 2;
				}
			}

			if (swap) {
				for (size_t j = tmp.size(); j > 0; j--) {
					out.push_back(tmp[j - 1]);
				}
			} else {
				for (size_t j = 0; j < tmp.size(); j++) {
					out.push_back(tmp[j]);
				}
			}

			// out.push_back(draw(VT_LINETO, xx, yy));
		} else {
			fprintf(stderr, "Can't happen: stairstepping lineto with no moveto\n");
			exit(EXIT_IMPOSSIBLE);
		}
	}

	for (size_t i = 0; i < out.size(); i++) {
		out[i].x *= 1 << (32 - detail - z);
		out[i].y *= 1 << (32 - detail - z);
	}

	return out;
}

// https://github.com/Turfjs/turf/blob/master/packages/turf-center-of-mass/index.ts
//
// The MIT License (MIT)
//
// Copyright (c) 2019 Morgan Herlocker
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
draw centerOfMass(const drawvec &dv, size_t start, size_t end, draw centre) {
	std::vector<draw> coords;
	for (size_t i = start; i < end; i++) {
		coords.push_back(dv[i]);
	}

	// First, we neutralize the feature (set it around coordinates [0,0]) to prevent rounding errors
	// We take any point to translate all the points around 0
	draw translation = centre;
	double sx = 0;
	double sy = 0;
	double sArea = 0;
	draw pi, pj;
	double xi, xj, yi, yj, a;

	std::vector<draw> neutralizedPoints;
	for (size_t i = 0; i < coords.size(); i++) {
		neutralizedPoints.push_back(draw(coords[i].op, coords[i].x - translation.x, coords[i].y - translation.y));
	}

	for (size_t i = 0; i < coords.size() - 1; i++) {
		// pi is the current point
		pi = neutralizedPoints[i];
		xi = pi.x;
		yi = pi.y;

		// pj is the next point (pi+1)
		pj = neutralizedPoints[i + 1];
		xj = pj.x;
		yj = pj.y;

		// a is the common factor to compute the signed area and the final coordinates
		a = xi * yj - xj * yi;

		// sArea is the sum used to compute the signed area
		sArea += a;

		// sx and sy are the sums used to compute the final coordinates
		sx += (xi + xj) * a;
		sy += (yi + yj) * a;
	}

	// Shape has no area: fallback on turf.centroid
	if (sArea == 0) {
		return centre;
	} else {
		// Compute the signed area, and factorize 1/6A
		double area = sArea * 0.5;
		double areaFactor = 1 / (6 * area);

		// Compute the final coordinates, adding back the values that have been neutralized
		return draw(VT_MOVETO, translation.x + areaFactor * sx, translation.y + areaFactor * sy);
	}
}

double label_goodness(const drawvec &dv, long long x, long long y) {
	int nesting = 0;

	for (size_t i = 0; i < dv.size(); i++) {
		if (dv[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < dv.size(); j++) {
				if (dv[j].op != VT_LINETO) {
					break;
				}
			}

			// if it's inside the ring, and it's an outer ring,
			// we are nested more; if it's an inner ring, we are
			// nested less.
			if (pnpoly(dv, i, j - i, x, y)) {
				if (get_area(dv, i, j) >= 0) {
					nesting++;
				} else {
					nesting--;
				}
			}

			i = j - 1;
		}
	}

	if (nesting < 1) {
		return 0;  // outside the polygon is as bad as it gets
	}

	double closest = INFINITY;  // closest distance to the border

	for (size_t i = 0; i < dv.size(); i++) {
		double dx = dv[i].x - x;
		double dy = dv[i].y - y;
		double dist = sqrt(dx * dx + dy * dy);
		if (dist < closest) {
			closest = dist;
		}

		if (i > 0 && dv[i].op == VT_LINETO) {
			dist = distance_from_line(x, y, dv[i - 1].x, dv[i - 1].y, dv[i].x, dv[i].y);
			if (dist < closest) {
				closest = dist;
			}
		}
	}

	return closest;
}

struct sorty {
	long long x;
	long long y;
};

struct sorty_sorter {
	int kind;
	sorty_sorter(int k)
	    : kind(k){};

	bool operator()(const sorty &a, const sorty &b) const {
		long long xa, ya, xb, yb;

		if (kind == 0) {  // Y first
			xa = a.x;
			ya = a.y;

			xb = b.x;
			yb = b.y;
		} else if (kind == 1) {	 // X first
			xa = a.y;
			ya = a.x;

			xb = b.y;
			yb = b.x;
		} else if (kind == 2) {	 // diagonal
			xa = a.x + a.y;
			ya = a.x - a.y;

			xb = b.x + b.y;
			yb = b.x - b.y;
		} else {  // other diagonal
			xa = a.x - a.y;
			ya = a.x + a.y;

			xb = b.x - b.y;
			yb = b.x + b.y;
		}

		if (ya < yb) {
			return true;
		} else if (ya == yb && xa < xb) {
			return true;
		} else {
			return false;
		}
	};
};

struct candidate {
	long long x;
	long long y;
	double dist;

	bool operator<(const candidate &c) const {
		// largest distance sorts first
		return dist > c.dist;
	};
};

// Generate a label point for a polygon feature.
//
// A good label point will be near the center of the feature and far from any border.
//
// Polylabel is supposed to be able to do this optimally, but can be quite slow
// and sometimes still produces some odd results.
//
// The centroid is often off-center because edges with many curves will be
// weighted higher than edges with straight lines.
//
// Turf's center-of-mass algorithm generally does a good job, but can sometimes
// find a point that is outside the bounds of the polygon or quite close to the edge.
//
// So prefer the center of mass, but if it produces something too close to the border
// or outside the polygon, try a series of gridded points within the feature's bounding box
// until something works well, or if nothing does after several iterations, use the
// least-bad option.

drawvec polygon_to_anchor(const drawvec &geom) {
	size_t start = 0, end = 0;
	size_t best_area = 0;
	std::vector<sorty> points;

	// find the largest outer ring, which will be the best thing
	// to label if we can do it.

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;
			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}

				sorty sy;
				sy.x = geom[j].x;
				sy.y = geom[j].y;
				points.push_back(sy);
			}

			double area = get_area(geom, i, j);
			if (area > best_area) {
				start = i;
				end = j;
				best_area = area;
			}

			i = j - 1;
		}
	}

	// If there are no outer rings, don't generate a label point

	if (best_area > 0) {
		long long xsum = 0;
		long long ysum = 0;
		size_t count = 0;
		long long xmin = LLONG_MAX, ymin = LLONG_MAX, xmax = LLONG_MIN, ymax = LLONG_MIN;

		// Calculate centroid and bounding box of biggest ring.
		// start + 1 to exclude the first point, which is duplicated as the last
		for (size_t k = start + 1; k < end; k++) {
			xsum += geom[k].x;
			ysum += geom[k].y;
			count++;

			xmin = std::min(xmin, geom[k].x);
			ymin = std::min(ymin, geom[k].y);
			xmax = std::max(xmax, geom[k].x);
			ymax = std::max(ymax, geom[k].y);
		}

		if (count > 0) {
			// We want label points that are at least a moderate distance from
			// the edge of the feature. The threshold for what is too close
			// is derived from the area of the feature.

			double radius = sqrt(best_area / M_PI);
			double goodness_threshold = radius / 5;

			// First choice: Turf's center of mass.

			draw centroid(VT_MOVETO, xsum / count, ysum / count);
			draw d = centerOfMass(geom, start, end, centroid);
			double goodness = label_goodness(geom, d.x, d.y);
			const char *kind = "mass";

			if (goodness < goodness_threshold) {
				// Label is too close to the border or outside it,
				// so try some other possible points. Sort the vertices
				// both by Y and X coordinate and then by diagonals,
				// and walk through each set
				// in sorted order. Adjacent pairs of coordinates should
				// tend to bounce back and forth between rings, so the
				// midpoint of each pair will hopefully be somewhere in the
				// interior of the polygon.

				std::vector<candidate> candidates;

				for (size_t pass = 0; pass < 4; pass++) {
					std::sort(points.begin(), points.end(), sorty_sorter(pass));

					for (size_t i = 1; i < points.size(); i++) {
						double dx = points[i].x - points[i - 1].x;
						double dy = points[i].y - points[i - 1].y;

						double dist = sqrt(dx * dx + dy * dy);
						if (dist > 2 * goodness_threshold) {
							candidate c;

							c.x = (points[i].x + points[i - 1].x) / 2;
							c.y = (points[i].y + points[i - 1].y) / 2;
							c.dist = dist;

							candidates.push_back(c);
						}
					}
				}

				// Now sort the accumulate list of segment midpoints by the lengths
				// of the segments. Starting from the longest
				// segment, if we find one whose midpoint is inside the polygon and
				// far enough from any edge to be good enough, stop looking.

				std::sort(candidates.begin(), candidates.end());
				// only check the top 50 stride midpoints, since this list can be quite large
				for (size_t i = 0; i < candidates.size() && i < 50; i++) {
					double maybe_goodness = label_goodness(geom, candidates[i].x, candidates[i].y);

					if (maybe_goodness > goodness) {
						d.x = candidates[i].x;
						d.y = candidates[i].y;

						goodness = maybe_goodness;
						kind = "diagonal";
						if (goodness > goodness_threshold) {
							break;
						}
					}
				}
			}

			// We may still not have anything decent, so the next thing to look at
			// is points from gridding the bounding box of the largest ring.

			if (goodness < goodness_threshold) {
				for (long long sub = 2;
				     sub < 32 && (xmax - xmin) > 2 * sub && (ymax - ymin) > 2 * sub;
				     sub *= 2) {
					for (long long x = 1; x < sub; x++) {
						for (long long y = 1; y < sub; y++) {
							draw maybe(VT_MOVETO,
								   xmin + x * (xmax - xmin) / sub,
								   ymin + y * (ymax - ymin) / sub);

							double maybe_goodness = label_goodness(geom, maybe.x, maybe.y);
							if (maybe_goodness > goodness) {
								// better than the previous
								d = maybe;
								goodness = maybe_goodness;
								kind = "grid";
							}
						}
					}

					if (goodness > goodness_threshold) {
						break;
					}
				}

				// There is nothing really good. Is the centroid maybe better?
				// If not, we're stuck with whatever the best we found was.
				double maybe_goodness = label_goodness(geom, centroid.x, centroid.y);
				if (maybe_goodness > goodness) {
					d = centroid;
					goodness = maybe_goodness;
					kind = "centroid";
				}

				if (goodness <= 0) {
					double lon, lat;
					tile2lonlat(d.x, d.y, 32, &lon, &lat);

					static std::atomic<long long> warned(0);
					if (warned++ < 10) {
						fprintf(stderr, "could not find good label point: %s %f,%f\n", kind, lat, lon);
					}
				}
			}

			drawvec dv;
			dv.push_back(d);
			return dv;
		}
	}

	return drawvec();
}

drawvec checkerboard_anchors(drawvec const &geom, int tx, int ty, int z, unsigned long long label_point) {
	drawvec out;

	// anchor point in world coordinates
	unsigned wx, wy;
	decode_index(label_point, &wx, &wy);

	// upper left of tile in world coordinates
	long long tx1 = 0, ty1 = 0;
	// lower right of tile in world coordinates;
	long long tx2 = 1LL << 32;  // , ty2 = 1LL << 32;
	if (z != 0) {
		tx1 = (long long) tx << (32 - z);
		ty1 = (long long) ty << (32 - z);

		tx2 = (long long) (tx + 1) << (32 - z);
		// ty2 = (long long) (ty + 1) << (32 - z);
	}

	// upper left of feature in world coordinates
	long long bx1 = LLONG_MAX, by1 = LLONG_MAX;
	// lower right of feature in world coordinates;
	long long bx2 = LLONG_MIN, by2 = LLONG_MIN;

	for (auto const &g : geom) {
		bx1 = std::min(bx1, g.x + tx1);
		by1 = std::min(by1, g.y + ty1);

		bx2 = std::max(bx2, g.x + tx1);
		by2 = std::max(by2, g.y + ty1);
	}

	if (bx1 > bx2 || by1 > by2) {
		return out;
	}

	// labels repeat every 0.3 tiles at z0
	double spiral_dist = 0.3;
	if (z > 0) {
		// only every ~6 tiles by the time we get to z15
		spiral_dist = spiral_dist * exp(log(z) * 1.2);
	}

	const long long label_spacing = spiral_dist * (tx2 - tx1);

	long long x1 = floor(std::min(bx1 - wx, bx2 - wx) / label_spacing);
	long long x2 = ceil(std::max(bx1 - wx, bx2 - wx) / label_spacing);

	long long y1 = floor(std::min(by1 - wy, by2 - wy) / label_spacing - 0.5);
	long long y2 = ceil(std::max(by1 - wy, by2 - wy) / label_spacing);

	for (long long lx = x1; lx <= x2; lx++) {
		for (long long ly = y1; ly <= y2; ly++) {
			long long x = lx * label_spacing + wx;
			long long y = ly * label_spacing + wy;

			if (((unsigned long long) lx & 1) == 1) {
				y += label_spacing / 2;
			}

			if (x < bx1 || x > bx2 || y < by1 || y > by2) {
				continue;
			}

			// If it's the central label, it's the best we've got,
			// so accept it in any case. If it's from the outer spiral,
			// don't use it if it's too close to a border.
			if (lx == 0 && ly == 0) {
				out.push_back(draw(VT_MOVETO, x - tx1, y - ty1));
				break;
			} else {
				double tilesize = 1LL << (32 - z);
				double goodness_threshold = tilesize / 100;
				if (label_goodness(geom, x - tx1, y - ty1) > goodness_threshold) {
					out.push_back(draw(VT_MOVETO, x - tx1, y - ty1));
					break;
				}
			}
		}
	}

	return out;
}
