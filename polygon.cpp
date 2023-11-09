#include <stdio.h>
#include "geometry.hpp"

struct point {
	double x;
	double y;

	point(double x_, double y_)
	    : x(x_), y(y_) {
	}
};

typedef std::pair<point, point> segment;

// https://stackoverflow.com/questions/9043805/test-if-two-lines-intersect-javascript-function/16725715#16725715
// this does not seem to be correct
bool intersects(double x1, double y1, double x2, double y2, double x3, double y3, double x4, double y4) {
	double det = (x2 - x1) * (y4 - y3) - (x4 - x3) * (y2 - y1);
	if (det == 0) {
		return false;
	} else {
		double lambda = ((y4 - y3) * (x4 - x1) + (x3 - x4) * (y4 - y1)) / det;
		double gamma = ((y1 - y2) * (x4 - x1) + (x2 - x1) * (y4 - y1)) / det;

		if (lambda > 0 && lambda < 1 && gamma > 0 && gamma < 1) {
			printf("%f,%f to %f,%f and %f,%f to %f,%f: %f and %f\n",
			       x1, y1, x2, y2, x3, y3, x4, y4, lambda, gamma);
			printf("%f,%f or %f,%f\n",
			       x1 + (x2 - x1) * lambda, y1 + (y2 - y1) * lambda,
			       x3 + (x4 - x3) * gamma, y3 + (y4 - y3) * gamma);
		}
		return (0 < lambda && lambda < 1) && (0 < gamma && gamma < 1);
	}
}

// https://stackoverflow.com/questions/563198/how-do-you-detect-where-two-line-segments-intersect
// this seems to produce the same point along both segments
bool get_line_intersection(double p0_x, double p0_y, double p1_x, double p1_y,
			   double p2_x, double p2_y, double p3_x, double p3_y) {
	double s1_x, s1_y, s2_x, s2_y;
	s1_x = p1_x - p0_x;
	s1_y = p1_y - p0_y;
	s2_x = p3_x - p2_x;
	s2_y = p3_y - p2_y;

	float det = (-s2_x * s1_y + s1_x * s2_y);

	if (det != 0) {
		double s, t;
		s = (-s1_y * (p0_x - p2_x) + s1_x * (p0_y - p2_y)) / det;
		t = (s2_x * (p0_y - p2_y) - s2_y * (p0_x - p2_x)) / det;

		if (s >= 0 && s <= 1 && t >= 0 && t <= 1) {
			printf("%f,%f to %f,%f and %f,%f to %f,%f: %f and %f\n",
			       p0_x, p0_y, p1_x, p1_y, p2_x, p2_y, p3_x, p3_y, t, s);
			printf("%f,%f or %f,%f\n",
			       p0_x + t * s1_x, p0_y + t * s1_y,
			       p2_x + s * s2_x, p2_y + s * s2_y);

			return 1;
		}
	}

	return 0;  // No collision
}

bool intersect(std::vector<segment> &segs, size_t s1, size_t s2) {
	get_line_intersection(std::round(segs[s1].first.x), std::round(segs[s1].first.y),
			      std::round(segs[s1].second.x), std::round(segs[s1].second.y),
			      std::round(segs[s2].first.x), std::round(segs[s2].first.y),
			      std::round(segs[s2].second.x), std::round(segs[s2].second.y));
	return false;
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

		std::set<std::pair<size_t, size_t>> already;
		std::set<size_t> active;
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
				std::pair<point, point> segment = std::make_pair(
					point(geom[k].x / scale, geom[k].y / scale),
					point(geom[k + 1].x / scale, geom[k + 1].y / scale));

				segments.push_back(segment);
			}

			i = j - 1;
		}
	}

	// snap-round intersecting segments

	segments = snap_round(segments);

	// remove duplicate segments with opposite windings

	// reassemble segments into rings

	// remove collinear points?

	// determine ring nesting

	return drawvec();
}
