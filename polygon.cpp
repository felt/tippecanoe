#include "geometry.hpp"

struct point {
	double x;
	double y;

	point(double x_, double y_)
	    : x(x_), y(y_) {
	}
};

typedef std::pair<point, point> segment;

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

		std::vector<scan_transition> tops;
		std::vector<scan_transition> bottoms;

		for (size_t i = 0; i < segs.size(); i++) {
			if (segs[i].first.y < segs[i].second.y) {
				tops.emplace_back(segs[i].first.y, i);
				bottoms.emplace_back(segs[i].second.y, i);
			} else {
				tops.emplace_back(segs[i].second.y, i);
				bottoms.emplace_back(segs[i].first.y, i);
			}
		}

		std::sort(tops.begin(), tops.end());
		std::sort(bottoms.begin(), bottoms.end());
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
				if (geom[i].op != VT_LINETO) {
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
