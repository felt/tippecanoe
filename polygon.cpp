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

bool intersect(std::vector<segment> &segs, size_t s1, size_t s2) {
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
						if (intersect(segs, s1, s2)) {
							// if the segments intersected,
							// we need to do another scan,
							// because introducing a new node
							// may have caused new intersections
							again = true;
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
	double scale = 1LL << (-(32 - detail - z));

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
