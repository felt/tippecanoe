#include <stdio.h>
#include <algorithm>
#include <set>
#include <vector>
#include <cmath>
#include <limits>
#include "geometry.hpp"
#include "errors.hpp"

struct point {
	double x;
	double y;

	point(double x_, double y_)
	    : x(std::round(x_)), y(std::round(y_)) {
	}

	point() {
		x = 0;
		y = 0;
	}

	bool operator<(point const &s) const {
		if (y < s.y || (y == s.y && x < s.x)) {
			return true;
		} else {
			return false;
		}
	}

	bool operator==(point const &s) const {
		return y == s.y && x == s.x;
	}

	bool operator!=(point const &s) const {
		return !(*this == s);
	}
};

typedef std::pair<point, point> segment;

bool fix_opposites(std::vector<segment> &segs) {
	bool changed = false;
	std::multimap<segment, size_t> opposites;
	segment erased = std::make_pair(point(INT_MAX, INT_MAX), point(INT_MAX, INT_MAX));

	for (size_t i = 0; i < segs.size(); i++) {
		segment opposite = std::make_pair(segs[i].second, segs[i].first);
		opposites.emplace(opposite, i);
	}

	size_t found = 0;
	for (size_t i = 0; i < segs.size(); i++) {
		if (segs[i] == erased) {
			continue;
		}

		auto f = opposites.equal_range(segs[i]);
		for (; f.first != f.second; ++f.first) {
			if (segs[f.first->second] == erased) {
				continue;
			}

			found++;

			double dx = std::round(segs[i].second.x) - std::round(segs[i].first.x);
			double dy = std::round(segs[i].second.y) - std::round(segs[i].first.y);
			double dsq = dx * dx + dy * dy;
			if (false && dsq >= 5 * 5) {
				// alter the segments instead to keep it from collapsing away

				double ang = atan2(dy, dx) - M_PI / 2;
				double cx = std::round((std::round(segs[i].second.x) + std::round(segs[i].first.x)) / 2 + sqrt(2) / 2 * cos(ang));
				double cy = std::round((std::round(segs[i].second.y) + std::round(segs[i].first.y)) / 2 + sqrt(2) / 2 * sin(ang));

				segs.emplace_back(point(cx, cy), segs[i].second);
				segs[i] = std::make_pair(segs[i].first, point(cx, cy));
				changed = true;

				// segs[i] is not erased, so segs[f.first->second]
				// will still match against it and will be bowed out
				// in the opposite direction.
			} else {
				segs[i] = erased;
				segs[f.first->second] = erased;
			}

			opposites.erase(f.first);
			break;
		}
	}

	if (found > 0) {
		changed = true;
	}

	size_t out = 0;
	for (size_t i = 0; i < segs.size(); i++) {
		if (segs[i] != erased) {
			segs[out++] = segs[i];
		}
	}
	segs.resize(out);
	return changed;
}

const std::pair<double, double> SAME_SLOPE = std::make_pair(-INT_MAX, INT_MAX);

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

	return SAME_SLOPE;
}

bool vertical(std::vector<segment> &segs, size_t s, double y) {
	if ((y > std::round(segs[s].first.y) && y < std::round(segs[s].second.y)) ||
	    (y > std::round(segs[s].second.y) && y < std::round(segs[s].first.y))) {
		segs.push_back(std::make_pair(point(segs[s].first.x, y), segs[s].second));
		segs[s] = std::make_pair(segs[s].first, point(segs[s].first.x, y));
		return true;
	}

	return false;
}

bool horizontal(std::vector<segment> &segs, size_t s, double x) {
	if ((x > std::round(segs[s].first.x) && x < std::round(segs[s].second.x)) ||
	    (x > std::round(segs[s].second.x) && x < std::round(segs[s].first.x))) {
		double slope = (std::round(segs[s].second.y) - std::round(segs[s].first.y)) /
			       (std::round(segs[s].second.x) - std::round(segs[s].first.x));
		double y = std::round(std::round(segs[s].first.y) + slope * (x - std::round(segs[s].first.x)));
		segs.push_back(std::make_pair(point(x, y), segs[s].second));
		segs[s] = std::make_pair(segs[s].first, point(x, y));
		return true;
	}

	return false;
}

bool intersect_collinear(std::vector<segment> &segs, size_t s1, size_t s2) {
	bool changed = false;

	if (std::round(segs[s1].first.x) == std::round(segs[s1].second.x)) {
		// vertical

		if (std::round(segs[s2].first.x) == std::round(segs[s2].second.x)) {
			// in which case the other one should also be vertical

			if (std::round(segs[s1].first.x) == std::round(segs[s2].first.x)) {
				// collinear, not parallel

				if (vertical(segs, s1, std::round(segs[s2].first.y))) {
					changed = true;
				}
				if (vertical(segs, s1, std::round(segs[s2].second.y))) {
					changed = true;
				}
				if (vertical(segs, s2, std::round(segs[s1].first.y))) {
					changed = true;
				}
				if (vertical(segs, s2, std::round(segs[s1].second.y))) {
					changed = true;
				}
			}
		} else {
			fprintf(stderr, "One segment is vertical and the other is not %f,%f to %f,%f; %f,%f to %f,%f.\n",
				segs[s1].first.x, segs[s1].first.y, segs[s1].second.x, segs[s1].second.y,
				segs[s2].first.x, segs[s2].first.y, segs[s2].second.x, segs[s2].second.y);
			exit(EXIT_IMPOSSIBLE);
		}
	} else {
		// horizontal or diagonal

		double slope1 = (std::round(segs[s1].second.y) - std::round(segs[s1].first.y)) /
				(std::round(segs[s1].second.x) - std::round(segs[s1].first.x));
		double slope2 = (std::round(segs[s2].second.y) - std::round(segs[s2].first.y)) /
				(std::round(segs[s2].second.x) - std::round(segs[s2].first.x));

		if (slope1 == slope2) {
			// they are parallel. do they have the same y intercept?

			double y1 = std::round(std::round(segs[s1].first.y) + slope1 * (0 - std::round(segs[s1].first.x)));
			double y2 = std::round(std::round(segs[s2].first.y) + slope1 * (0 - std::round(segs[s2].first.x)));

			if (y1 == y2) {
				// collinear, not parallel

				if (horizontal(segs, s1, std::round(segs[s2].first.x))) {
					changed = true;
				}
				if (horizontal(segs, s1, std::round(segs[s2].second.x))) {
					changed = true;
				}
				if (horizontal(segs, s2, std::round(segs[s1].first.x))) {
					changed = true;
				}
				if (horizontal(segs, s2, std::round(segs[s1].second.x))) {
					changed = true;
				}
			}
		} else {
			fprintf(stderr, "One segment has a slope of %f and the other %f: %f,%f to %f,%f; %f,%f to %f,%f.\n",
				slope1, slope2,
				segs[s1].first.x, segs[s1].first.y, segs[s1].second.x, segs[s1].second.y,
				segs[s2].first.x, segs[s2].first.y, segs[s2].second.x, segs[s2].second.y);
			exit(EXIT_IMPOSSIBLE);
		}
	}

	return changed;
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
			// printf("introduce %f,%f in %f,%f to %f,%f (s1 %zu %zu)\n", x, y, segs[s1].first.x, segs[s1].first.y, segs[s1].second.x, segs[s1].second.y, s1, s2);
			segs.push_back(std::make_pair(point(std::round(x), std::round(y)), segs[s1].second));
			segs[s1] = std::make_pair(segs[s1].first, point(std::round(x), std::round(y)));
			changed = true;
		}

		if ((std::llround(x) == std::llround(segs[s2].first.x) && std::llround(y) == std::llround(segs[s2].first.y)) ||
		    (std::llround(x) == std::llround(segs[s2].second.x) && std::llround(y) == std::llround(segs[s2].second.y))) {
			// at an endpoint in s2, so it doesn't need to be changed
		} else {
			// printf("introduce %f,%f in %f,%f to %f,%f (s2 %zu %zu)\n", x, y, segs[s2].first.x, segs[s2].first.y, segs[s2].second.x, segs[s2].second.y, s1, s2);
			// printf("introduce %lld,%lld in %lld,%lld to %lld,%lld (s2)\n", std::llround(x), std::llround(y), std::llround(segs[s2].first.x), std::llround(segs[s2].first.y), std::llround(segs[s2].second.x), std::llround(segs[s2].second.y));
			segs.push_back(std::make_pair(point(std::round(x), std::round(y)), segs[s2].second));
			segs[s2] = std::make_pair(segs[s2].first, point(std::round(x), std::round(y)));
			changed = true;
		}
	} else if (intersections == SAME_SLOPE) {
		if (intersect_collinear(segs, s1, s2)) {
			changed = true;
		}
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

void snap_round(std::vector<segment> &segs) {
	bool again = true;

	while (again) {
		again = false;

		// find identical opposite-winding segments and adjust for them
		//
		// this is in the same loop because we may introduce new self-intersections
		// in the course of trying to keep spindles alive, and will then need to
		// resolve those.

		if (fix_opposites(segs)) {
			again = true;
		}

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
	}

	for (auto &s : segs) {
		s.first.x = std::round(s.first.x);
		s.first.y = std::round(s.first.y);
		s.second.x = std::round(s.second.x);
		s.second.y = std::round(s.second.y);
	}
}

struct ring_area {
	drawvec geom;
	double area;
	std::vector<size_t> children;
	long long ear_x;
	long long ear_y;

	ring_area(drawvec geom_, double area_) {
		geom = geom_;
		area = area_;

		// search polygon ears to find an interior point
		for (size_t i = 0; i + 2 < geom.size(); i++) {
			long long x = (geom[i].x + geom[i + 1].x + geom[i + 2].x) / 3;
			long long y = (geom[i].y + geom[i + 1].y + geom[i + 2].y) / 3;

			if (pnpoly(geom, 0, geom.size(), x, y)) {
				ear_x = x;
				ear_y = y;
				return;
			}
		}

		fprintf(stderr, "Couldn't find an interior point\n");
		exit(EXIT_IMPOSSIBLE);
	}

	bool operator<(ring_area const &s) const {
		// this sorts backwards, so the ring with the largest area comes first
		if (std::fabs(area) > std::fabs(s.area)) {
			return true;
		} else {
			return false;
		}
	}
};

const int SCALE = 3;

std::vector<ring_area> reassemble(std::vector<segment> const &segs) {
	std::multimap<point, segment> connections;
	std::vector<ring_area> ret;

	for (auto const &seg : segs) {
		connections.emplace(seg.first, seg);
	}

	while (connections.size() > 0) {
		// arbitrarily choose a starting point,
		// and walk the connections from there until
		// we find a point that we have already visited.

		// make a copy of the connections so we can remove
		// segments from it as we walk, even though some of
		// the segments we remove will probably have to go
		// back in because they are really part of another ring
		std::multimap<point, segment> examining = connections;
		std::map<point, segment> examined;

		segment here = examining.begin()->second;
		examined.emplace(here.first, here);
		examining.erase(examining.begin());

		// go until the segment that we are looking at
		// points to a vertex we have seen before, which
		// will be the initial point of the ring
		while (examined.find(here.second) == examined.end()) {
			auto options = examining.equal_range(here.second);
			if (options.first == options.second) {
				fprintf(stderr, "can't happen: no connections in ring construction\n");
				exit(EXIT_IMPOSSIBLE);
			}

			// choose the sharpest possible left turn
			// (in tile coordinate space, so Y coordinates
			// increase toward the bottom) of the available
			// connections from this point, which should
			// lead around either the largest outer ring or
			// the smallest inner ring that includes this point.

			auto best = options.first;
			double bestang = 500;

			for (; options.first != options.second; ++options.first) {
				double ang1 = atan2(here.second.y - here.first.y, here.second.x - here.first.x);
				double ang2 = atan2(options.first->second.second.y - options.first->second.first.y,
						    options.first->second.second.x - options.first->second.first.x);
				double diff = ang1 - ang2;
				// normalize to -180° … 180°
				while (diff > M_PI) {
					diff -= 2 * M_PI;
				}
				while (diff < -M_PI) {
					diff += 2 * M_PI;
				}

#if 0
				printf("%f,%f to %f,%f to %f,%f, %f,%f: %f\n",
				       here.first.x, here.first.y,
				       here.second.x, here.second.y,
				       options.first->second.first.x, options.first->second.first.y,
				       options.first->second.second.x, options.first->second.second.y,
				       diff * 180 / M_PI);
#endif

				// closest to -180 is the best
				if (diff < bestang) {
					bestang = diff;
					best = options.first;
				}
			}

			here = best->second;
			examined.emplace(here.first, here);
			examining.erase(best);
		}

		here = examined.find(here.second)->second;  // the new initial segment, found above

		// now do a second walk, actually removing the connections
		// from the original copy, and saving the ring that we make.

		examining.clear();
		examined.clear();
		std::vector<point> ring;

		examined.emplace(here.first, here);
		ring.push_back(here.first);

		// find the initial segment in `connections` so we can remove it
		auto initial = connections.equal_range(here.first);
		bool found = false;
		for (; initial.first != initial.second; ++initial.first) {
			if (initial.first->second == here) {
				connections.erase(initial.first);
				found = true;
				break;
			}
		}
		if (!found) {
			fprintf(stderr, "can't happen: couldn't find initial point");
			exit(EXIT_IMPOSSIBLE);
		}

		while (here.second != ring[0]) {
			auto options = connections.equal_range(here.second);
			if (options.first == options.second) {
				fprintf(stderr, "can't happen: no connections in ring construction\n");
				exit(EXIT_IMPOSSIBLE);
			}

			// choose the sharpest possible left turn
			// (in tile coordinate space, so Y coordinates
			// increase toward the bottom) of the available
			// connections from this point, which should
			// lead around either the largest outer ring or
			// the smallest inner ring that includes this point.

			auto best = options.first;
			double bestang = 500;

			for (; options.first != options.second; ++options.first) {
				double ang1 = atan2(here.second.y - here.first.y, here.second.x - here.first.x);
				double ang2 = atan2(options.first->second.second.y - options.first->second.first.y,
						    options.first->second.second.x - options.first->second.first.x);
				double diff = ang1 - ang2;
				// normalize to -180° … 180°
				while (diff > M_PI) {
					diff -= 2 * M_PI;
				}
				while (diff < -M_PI) {
					diff += 2 * M_PI;
				}

#if 0
				printf("%f,%f to %f,%f to %f,%f, %f,%f: %f\n",
				       here.first.x, here.first.y,
				       here.second.x, here.second.y,
				       options.first->second.first.x, options.first->second.first.y,
				       options.first->second.second.x, options.first->second.second.y,
				       diff * 180 / M_PI);
#endif

				// closest to -180 is the best
				if (diff < bestang) {
					bestang = diff;
					best = options.first;
				}
			}

			here = best->second;
			examined.emplace(here.first, here);
			connections.erase(best);
			ring.push_back(here.first);
		}

		// these coordinates are doubled, so that `encloses` can always find
		// an interior point in each ring
		drawvec out;
		for (size_t i = 0; i < ring.size(); i++) {
			out.emplace_back(i == 0 ? VT_MOVETO : VT_LINETO, std::round(ring[i].x) * SCALE, std::round(ring[i].y) * SCALE);
		}
		out.emplace_back(VT_LINETO, std::round(ring[0].x) * SCALE, std::round(ring[0].y) * SCALE);
		if (out[0] != out[out.size() - 1]) {
			fprintf(stderr, "Ring not closed???\n");
			exit(EXIT_IMPOSSIBLE);
		}
		double area = get_area(out, 0, out.size());
		if (area != 0) {
			ret.push_back(ring_area(out, area));
		} else {
			fprintf(stderr, "0-area ring: ");
			for (auto const &g : out) {
				fprintf(stderr, "%lld,%lld ", g.x, g.y);
			}
			fprintf(stderr, "\n");
		}
	}

	return ret;
}

bool encloses(ring_area const &parent, ring_area const &child) {
	if (std::fabs(child.area) > std::fabs(parent.area)) {
		fprintf(stderr, "child area %f is greater than parent area %f\n", child.area, parent.area);
		exit(EXIT_IMPOSSIBLE);
	}

	bool a = pnpoly(parent.geom, 0, parent.geom.size(), child.ear_x, child.ear_y);

#if 0
    if (a != b) {
        fprintf(stderr, "inconsistent pnpoly at %lld,%lld (%d) vs %lld,%lld (%d)\n", x, y, a, x2, y2, b);

        printf("0 setlinewidth ");
        for (auto const &g : parent.geom) {
            printf("%lld %lld %s ", g.x, g.y, g.op == VT_MOVETO ? "moveto" : "lineto");
        }
        printf("stroke\n");
        for (auto const &g : parent.geom) {
            printf("%lld %lld .05 0 360 arc fill ", g.x, g.y);
        }

        for (auto const &g : child.geom) {
            printf("%f %f %s ", g.x + 0.25, g.y + 0.25, g.op == VT_MOVETO ? "moveto" : "lineto");
        }
        printf("stroke\n");

        printf("%lld %lld .5 0 360 arc fill\n", x, y);
        printf("%lld %lld .5 0 360 arc fill\n", x2, y2);

        exit(EXIT_FAILURE);
    }
#endif

	return a;
}

void flatten_rings(std::vector<ring_area> &rings, size_t i, drawvec &out, ssize_t winding, ssize_t parent) {
	if (rings[i].geom.size() == 0) {
		return;
	}

	// only the transition from winding order 0 to 1 or from 1 to 0
	// is actually represented in the geometry.
	//
	// other transitions are outer rings nested inside other outer rings,
	// or inner rings nested inside other inner rings.
	if ((winding == 0 && rings[i].area > 0) ||
	    (winding == 1 && rings[i].area < 0)) {
		for (auto const &g : rings[i].geom) {
			out.emplace_back(g.op, g.x / SCALE, g.y / SCALE);
		}
		if (rings[i].geom.size() > 0 && rings[i].geom[0] != rings[i].geom[rings[i].geom.size() - 1]) {
			fprintf(stderr, "Ring not closed\n");
			exit(EXIT_IMPOSSIBLE);
		}
	} else {
		fprintf(stderr, "skipping ring %zu %f within winding %zd (%zd)\n", i, rings[i].area, winding, parent);
	}
	rings[i].geom.clear();

	if (rings[i].area > 0) {
		winding++;
	} else if (rings[i].area < 0) {
		winding--;
	}

	fprintf(stderr, "ring %zu contains rings:", i);
	for (size_t j = 0; j < rings[i].children.size(); j++) {
		fprintf(stderr, " %zu", rings[i].children[j]);
	}
	fprintf(stderr, "\n");

	for (size_t j = 0; j < rings[i].children.size(); j++) {
		flatten_rings(rings, rings[i].children[j], out, winding, i);
	}
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
					point(std::round(geom[k].x / scale), std::round(geom[k].y / scale)),
					point(std::round(geom[k + 1].x / scale), std::round(geom[k + 1].y / scale)));

				if (std::round(seg.first.x) != std::round(seg.second.x) ||
				    std::round(seg.first.y) != std::round(seg.second.y)) {
					segments.push_back(seg);
				}
			}

			i = j - 1;
		}
	}

	// snap-round intersecting segments

	snap_round(segments);

	// reassemble segments into rings

	std::vector<ring_area> rings = reassemble(segments);
	std::sort(rings.begin(), rings.end());

	// determine ring nesting

	for (size_t i = 0; i < rings.size(); i++) {	// from largest to smallest abs area
		for (ssize_t j = i - 1; j >= 0; j--) {	// from smallest to largest abs area of already examined
			if (encloses(rings[j], rings[i])) {
				if (rings[i].area < 0 && rings[j].area > 0) {
					// inner ring inside an outer ring;
					// attribute it to the outer ring
					rings[j].children.push_back(i);
#if 0
					fprintf(stderr, "inner within outer: ring %zd (%f) encloses ring %zu (%f) %s\n", j, rings[j].area, i, rings[i].area,
						signbit(rings[j].area) == signbit(rings[i].area) ? "!!!!" : "");
#endif
				} else if (rings[i].area < 0 && rings[j].area < 0) {
					fprintf(stderr, "inner within inner: ring %zd (%f) encloses ring %zu (%f) %s\n", j, rings[j].area, i, rings[i].area,
						signbit(rings[j].area) == signbit(rings[i].area) ? "!!!!" : "");
					rings[i].geom.clear();
				} else if (rings[i].area > 0 && rings[j].area > 0) {
					fprintf(stderr, "outer within outer: ring %zd (%f) encloses ring %zu (%f) %s\n", j, rings[j].area, i, rings[i].area,
						signbit(rings[j].area) == signbit(rings[i].area) ? "!!!!" : "");
					rings[i].geom.clear();
				} else {
					// outer ring within an inner ring;
					// this is OK, but it is treated as a new outer ring in the tile,
					// not output in a hierarchy
				}

				break;
			}
		}
	}

	drawvec ret;
	for (size_t i = 0; i < rings.size(); i++) {
		for (auto const &g : rings[i].geom) {
			ret.emplace_back(g.op, g.x / SCALE, g.y / SCALE);
		}
		for (auto child : rings[i].children) {
			for (auto const &g : rings[child].geom) {
				ret.emplace_back(g.op, g.x / SCALE, g.y / SCALE);
			}
			rings[child].geom.clear();
		}
	}

#if 0
	for (size_t i = 0; i < rings.size(); i++) {
		if (get_area(rings[i].geom, 0, rings[i].geom.size()) > 0) {
			for (auto const &g : rings[i].geom) {
				ret.emplace_back(g.op, g.x / SCALE, g.y / SCALE);
			}
		}
	}
#endif

	// remove collinear points?

#if 0
	drawvec ret;
	for (auto const &segment : segments) {
		ret.emplace_back(VT_MOVETO, segment.first.x, segment.first.y);
		ret.emplace_back(VT_LINETO, segment.second.x, segment.second.y);
	}
#endif

	return ret;
}
