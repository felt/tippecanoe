#include "geometry.hpp"

struct point {
	double x;
	double y;

	point(double x_, double y_)
	    : x(x_), y(y_) {
	}
};

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

	// remove duplicate segments with opposite windings

	// reassemble segments into rings

	// remove collinear points?

	// determine ring nesting

	return drawvec();
}
