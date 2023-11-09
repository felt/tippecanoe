#include <stdio.h>
#include <algorithm>
#include <set>
#include <vector>
#include <cmath>
#include "geometry.hpp"

struct point {
	double x;
	double y;

	point(double x_, double y_)
	    : x(x_), y(y_) {
	}
};

typedef std::pair<point, point> segment;

// https://stackoverflow.com/questions/563198/how-do-you-detect-where-two-line-segments-intersect
//
// beware of
// https://stackoverflow.com/questions/9043805/test-if-two-lines-intersect-javascript-function/16725715#16725715
// which does not seem to produce correct results.
std::pair<double, double> get_line_intersection(double p0_x, double p0_y, double p1_x, double p1_y,
						double p2_x, double p2_y, double p3_x, double p3_y) {
	double d01_x, d01_y, d23_x, d23_y;
	d01_x = p1_x - p0_x;
	d01_y = p1_y - p0_y;
	d23_x = p3_x - p2_x;
	d23_y = p3_y - p2_y;

	float det = (-d23_x * d01_y + d01_x * d23_y);

	if (det != 0) {
		double t, s;
		t = (d23_x * (p0_y - p2_y) - d23_y * (p0_x - p2_x)) / det;
		s = (-d01_y * (p0_x - p2_x) + d01_x * (p0_y - p2_y)) / det;

		return std::make_pair(t, s);

#if 0
		printf("%f,%f to %f,%f and %f,%f to %f,%f: %f and %f\n",
		       p0_x, p0_y, p1_x, p1_y, p2_x, p2_y, p3_x, p3_y, t, s);
		printf("%f,%f or %f,%f\n",
		       p0_x + t * d01_x, p0_y + t * d01_y,
		       p2_x + s * d23_x, p2_y + s * d23_y);
#endif
	}

	return std::make_pair(-1, -1);
}

bool intersect(std::vector<segment> &segs, size_t s1, size_t s2) {
	auto intersections = get_line_intersection(std::round(segs[s1].first.x), std::round(segs[s1].first.y),
						   std::round(segs[s1].second.x), std::round(segs[s1].second.y),
						   std::round(segs[s2].first.x), std::round(segs[s2].first.y),
						   std::round(segs[s2].second.x), std::round(segs[s2].second.y));

	bool changed = false;
	if (intersections.first >= 0 && intersections.first <= 1 && intersections.second >= 0 && intersections.second <= 1) {
		double x = (segs[s1].first.x + intersections.first * (segs[s1].second.x - segs[s1].first.x));
		double y = (segs[s1].first.y + intersections.first * (segs[s1].second.y - segs[s1].first.y));

#if 0
		// try intersecting the original segments without rounding,
		// since that intersection should be more true to the original
		// intent of the data.

		auto intersections2 = get_line_intersection((segs[s1].first.x), (segs[s1].first.y),
							    (segs[s1].second.x), (segs[s1].second.y),
							    (segs[s2].first.x), (segs[s2].first.y),
							    (segs[s2].second.x), (segs[s2].second.y));

		if (intersections2.first >= 0 && intersections2.first <= 1 && intersections2.second >= 0 && intersections2.second <= 1) {
			double x2 = (segs[s1].first.x + intersections2.first * (segs[s1].second.x - segs[s1].first.x));
			double y2 = (segs[s1].first.y + intersections2.first * (segs[s1].second.y - segs[s1].first.y));

			if (x != x2 || y != y2) {
				// printf("would intersect at %f,%f; from rounded chose %f,%f\n", x2, y2, x, y);
				x = x2;
				y = y2;
			}
		}
#endif

		if ((std::llround(x) == std::llround(segs[s1].first.x) && std::llround(y) == std::llround(segs[s1].first.y)) ||
		    (std::llround(x) == std::llround(segs[s1].second.x) && std::llround(y) == std::llround(segs[s1].second.y))) {
			// at an endpoint in s1, so it doesn't need to be changed
		} else {
			printf("introduce %f,%f in %f,%f to %f,%f (s1 %zu %zu)\n", x, y, segs[s1].first.x, segs[s1].first.y, segs[s1].second.x, segs[s1].second.y, s1, s2);
			segs.push_back(std::make_pair(point(x, y), segs[s1].second));
			segs[s1] = std::make_pair(segs[s1].first, point(x, y));
			changed = true;
		}

		if ((std::llround(x) == std::llround(segs[s2].first.x) && std::llround(y) == std::llround(segs[s2].first.y)) ||
		    (std::llround(x) == std::llround(segs[s2].second.x) && std::llround(y) == std::llround(segs[s2].second.y))) {
			// at an endpoint in s2, so it doesn't need to be changed
		} else {
			printf("introduce %f,%f in %f,%f to %f,%f (s2 %zu %zu)\n", x, y, segs[s2].first.x, segs[s2].first.y, segs[s2].second.x, segs[s2].second.y, s1, s2);
			// printf("introduce %lld,%lld in %lld,%lld to %lld,%lld (s2)\n", std::llround(x), std::llround(y), std::llround(segs[s2].first.x), std::llround(segs[s2].first.y), std::llround(segs[s2].second.x), std::llround(segs[s2].second.y));
			segs.push_back(std::make_pair(point(x, y), segs[s2].second));
			segs[s2] = std::make_pair(segs[s2].first, point(x, y));
			changed = true;
		}
	} else if (intersections.first == -1 && intersections.second == -1) {
		// XXX handle collinear
	} else {
		// could intersect, but does not
	}

	return changed;
}

struct scan_transition {
	double y;
	size_t segment;

	scan_transition(double y_, size_t segment_)
	    : y(y_), segment(segment_) {
	}

	bool operator<(scan_transition const &s) const {
		if (y < s.y) {
			return true;
		} else {
			return false;
		}
	}
};

std::vector<segment> snap_round(std::vector<segment> segs) {
	bool again = true;

	while (again) {
		again = false;

		// set up for a scanline traversal of the segments
		// to find the pairs that intersect
		// while not looking at pairs that can't possibly intersect

		// index by rounded y coordinates, since we will be
		// intersecting with rounded coordinates

		std::vector<scan_transition> tops;
		std::vector<scan_transition> bottoms;

		for (size_t i = 0; i < segs.size(); i++) {
			if (std::round(segs[i].first.y) < std::round(segs[i].second.y)) {
				tops.emplace_back(std::round(segs[i].first.y), i);
				bottoms.emplace_back(std::round(segs[i].second.y), i);
			} else {
				tops.emplace_back(std::round(segs[i].second.y), i);
				bottoms.emplace_back(std::round(segs[i].first.y), i);
			}
		}

		std::sort(tops.begin(), tops.end());
		std::sort(bottoms.begin(), bottoms.end());

		// do the scan

		std::set<size_t> active;
		std::set<std::pair<size_t, size_t>> already;
		size_t bottom = 0;

		for (size_t i = 0; i < tops.size(); i++) {
			// activate anything that is coming into view

			active.insert(tops[i].segment);
			if (i + 1 < tops.size() && tops[i + 1].y == tops[i].y) {
				continue;
			}

			// look at the active segments

			for (size_t s1 : active) {
				for (size_t s2 : active) {
					if (s1 < s2) {
						if (already.find(std::make_pair(s1, s2)) == already.end()) {
							if (intersect(segs, s1, s2)) {
								// if the segments intersected,
								// we need to do another scan,
								// because introducing a new node
								// may have caused new intersections
								again = true;
							}

							already.insert(std::make_pair(s1, s2));
						}
					}
				}
			}

			// deactivate anything that is going out of view

			if (i + 1 < tops.size()) {
				while (bottom < bottoms.size() && bottoms[bottom].y < tops[i + 1].y) {
					auto found = active.find(bottoms[bottom].segment);
					active.erase(found);
					bottom++;
				}
			}
		}

		if (again) {
			// let the intersections settle down before we start trying
			// to make additional changes
			continue;
		}

		// find identical opposite-winding segments and adjust for them
		//
		// this is in the same loop because we may introduce new self-intersections
		// in the course of trying to keep spindles alive, and will then need to
		// resolve those.
	}

	return segs;
}

drawvec clean_polygon(drawvec const &geom, int z, int detail) {
	double scale = 1LL << (32 - detail - z);

	// decompose polygon rings into segments

	std::vector<std::pair<point, point>> segments;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;

			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			for (size_t k = i; k + 1 < j; k++) {
				std::pair<point, point> seg = std::make_pair(
					point(geom[k].x / scale, geom[k].y / scale),
					point(geom[k + 1].x / scale, geom[k + 1].y / scale));

				if (std::round(seg.first.x) != std::round(seg.second.x) ||
				    std::round(seg.first.y) != std::round(seg.second.y)) {
					segments.push_back(seg);
				}
			}

			i = j - 1;
		}
	}

	// snap-round intersecting segments

	segments = snap_round(segments);

	// reassemble segments into rings

	// remove collinear points?

	// determine ring nesting

	drawvec ret;
	for (auto const &segment : segments) {
		ret.emplace_back(VT_MOVETO, segment.first.x, segment.first.y);
		ret.emplace_back(VT_LINETO, segment.second.x, segment.second.y);
	}

	return ret;
}
