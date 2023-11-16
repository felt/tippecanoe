#include <stdio.h>
#include <algorithm>
#include <set>
#include <vector>
#include <cmath>
#include <climits>
#include <limits>
#include "geometry.hpp"
#include "errors.hpp"

struct point {
	long long x;
	long long y;

	point(long long x_, long long y_)
	    : x(x_), y(y_) {
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

bool spindle_visible(segment const &seg, long long extent) {
	if (extent == 0) {
		// extent of 0 means no spindle revival
		return false;
	}

	long long minx = std::min(seg.first.x, seg.second.x);
	long long miny = std::min(seg.first.y, seg.second.y);
	long long maxx = std::max(seg.first.x, seg.second.x);
	long long maxy = std::max(seg.first.y, seg.second.y);

	if (maxx <= 0 || maxy <= 0 || minx >= extent || miny >= extent) {
		return false;
	} else {
		return true;
	}
}

bool fix_opposites(std::vector<segment> &segs, std::set<segment> &affected, long long extent) {
	bool changed = false;
	std::multimap<segment, size_t> opposites;
	segment erased = std::make_pair(point(INT_MAX, INT_MAX), point(INT_MAX, INT_MAX));

	for (size_t i = 0; i < segs.size(); i++) {
		segment opposite = std::make_pair(segs[i].second, segs[i].first);
		opposites.emplace(opposite, i);
	}

	for (size_t i = 0; i < segs.size(); i++) {
		if (segs[i] == erased) {
			continue;
		}

		auto f = opposites.equal_range(segs[i]);
		for (; f.first != f.second; ++f.first) {
			if (segs[f.first->second] == erased) {
				continue;
			}

			long long dx = segs[i].second.x - segs[i].first.x;
			long long dy = segs[i].second.y - segs[i].first.y;
			long long dsq = dx * dx + dy * dy;
			if (spindle_visible(segs[i], extent) && dsq >= 5 * 5) {
				// alter the segment instead to keep it from collapsing away

				double ang = atan2(dy, dx) - M_PI / 2;
				long long cx = std::llround((segs[i].second.x + segs[i].first.x) / 2.0 + sqrt(2) / 2.0 * cos(ang));
				long long cy = std::llround((segs[i].second.y + segs[i].first.y) / 2.0 + sqrt(2) / 2.0 * sin(ang));

				segs.emplace_back(point(cx, cy), segs[i].second);
				segs[i] = std::make_pair(segs[i].first, point(cx, cy));

				affected.insert(segs[i]);
				affected.insert(segs.back());
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
std::pair<double, double> get_line_intersection(long long p0_x, long long p0_y, long long p1_x, long long p1_y,
						long long p2_x, long long p2_y, long long p3_x, long long p3_y) {
	// bounding box reject, x
	long long min01x = std::min(p0_x, p1_x);
	long long max01x = std::max(p0_x, p1_x);
	long long min23x = std::min(p2_x, p3_x);
	long long max23x = std::max(p2_x, p3_x);
	if (max01x < min23x || max23x < min01x) {
		return std::make_pair(-1, -1);
	}

	// bounding box reject, y
	long long min01y = std::min(p0_y, p1_y);
	long long max01y = std::max(p0_y, p1_y);
	long long min23y = std::min(p2_y, p3_y);
	long long max23y = std::max(p2_y, p3_y);
	if (max01y < min23y || max23y < min01y) {
		return std::make_pair(-1, -1);
	}

	long long d01_x, d01_y, d23_x, d23_y;
	d01_x = p1_x - p0_x;
	d01_y = p1_y - p0_y;
	d23_x = p3_x - p2_x;
	d23_y = p3_y - p2_y;

	long long det = (-d23_x * d01_y + d01_x * d23_y);

	if (det != 0) {
		double t, s;
		t = (d23_x * (p0_y - p2_y) - d23_y * (p0_x - p2_x)) / (double) det;
		s = (-d01_y * (p0_x - p2_x) + d01_x * (p0_y - p2_y)) / (double) det;

		return std::make_pair(t, s);
	}

	return SAME_SLOPE;
}

bool vertical(std::vector<segment> &segs, size_t s, long long y, std::set<segment> &affected) {
	if ((y > segs[s].first.y && y < segs[s].second.y) ||
	    (y > segs[s].second.y && y < segs[s].first.y)) {
		segs.push_back(std::make_pair(point(segs[s].first.x, y), segs[s].second));
		segs[s] = std::make_pair(segs[s].first, point(segs[s].first.x, y));

		affected.insert(segs[s]);
		affected.insert(segs.back());
		return true;
	}

	return false;
}

bool horizontal(std::vector<segment> &segs, size_t s, long long x, std::set<segment> &affected) {
	if ((x > segs[s].first.x && x < segs[s].second.x) ||
	    (x > segs[s].second.x && x < segs[s].first.x)) {
		double slope = (segs[s].second.y - segs[s].first.y) /
			       (double) (segs[s].second.x - segs[s].first.x);
		long long y = std::llround(segs[s].first.y + slope * (x - segs[s].first.x));
		segs.push_back(std::make_pair(point(x, y), segs[s].second));
		segs[s] = std::make_pair(segs[s].first, point(x, y));

		affected.insert(segs[s]);
		affected.insert(segs.back());
		return true;
	}

	return false;
}

bool intersect_collinear(std::vector<segment> &segs, size_t s1, size_t s2, std::set<segment> &affected) {
	bool changed = false;

	if (segs[s1].first.x == segs[s1].second.x) {
		// vertical

		if (segs[s2].first.x == segs[s2].second.x) {
			// in which case the other one should also be vertical

			if (segs[s1].first.x == segs[s2].first.x) {
				// collinear, not parallel

				if (vertical(segs, s1, segs[s2].first.y, affected)) {
					changed = true;
				}
				if (vertical(segs, s1, segs[s2].second.y, affected)) {
					changed = true;
				}
				if (vertical(segs, s2, segs[s1].first.y, affected)) {
					changed = true;
				}
				if (vertical(segs, s2, segs[s1].second.y, affected)) {
					changed = true;
				}
			}
		} else {
			fprintf(stderr, "One segment is vertical and the other is not %lld,%lld to %lld,%lld; %lld,%lld to %lld,%lld.\n",
				segs[s1].first.x, segs[s1].first.y, segs[s1].second.x, segs[s1].second.y,
				segs[s2].first.x, segs[s2].first.y, segs[s2].second.x, segs[s2].second.y);
			exit(EXIT_IMPOSSIBLE);
		}
	} else {
		// horizontal or diagonal

		double slope1 = (segs[s1].second.y - segs[s1].first.y) /
				(double) (segs[s1].second.x - segs[s1].first.x);
		double slope2 = (segs[s2].second.y - segs[s2].first.y) /
				(double) (segs[s2].second.x - segs[s2].first.x);

		if (slope1 == slope2) {
			// they are parallel. do they have the same y intercept?

			long long y1 = std::llround(segs[s1].first.y + slope1 * (0 - segs[s1].first.x));
			long long y2 = std::llround(segs[s2].first.y + slope1 * (0 - segs[s2].first.x));

			if (y1 == y2) {
				// collinear, not parallel

				if (horizontal(segs, s1, segs[s2].first.x, affected)) {
					changed = true;
				}
				if (horizontal(segs, s1, segs[s2].second.x, affected)) {
					changed = true;
				}
				if (horizontal(segs, s2, segs[s1].first.x, affected)) {
					changed = true;
				}
				if (horizontal(segs, s2, segs[s1].second.x, affected)) {
					changed = true;
				}
			}
		} else {
			fprintf(stderr, "One segment has a slope of %f and the other %f: %lld,%lld to %lld,%lld; %lld,%lld to %lld,%lld.\n",
				slope1, slope2,
				segs[s1].first.x, segs[s1].first.y, segs[s1].second.x, segs[s1].second.y,
				segs[s2].first.x, segs[s2].first.y, segs[s2].second.x, segs[s2].second.y);
			exit(EXIT_IMPOSSIBLE);
		}
	}

	return changed;
}

bool intersect(std::vector<segment> &segs, size_t s1, size_t s2, std::set<segment> &affected) {
	auto intersections = get_line_intersection(segs[s1].first.x, segs[s1].first.y,
						   segs[s1].second.x, segs[s1].second.y,
						   segs[s2].first.x, segs[s2].first.y,
						   segs[s2].second.x, segs[s2].second.y);

	bool changed = false;
	if (intersections.first >= 0 && intersections.first <= 1 && intersections.second >= 0 && intersections.second <= 1) {
		long long x = std::llround(segs[s1].first.x + intersections.first * (segs[s1].second.x - segs[s1].first.x));
		long long y = std::llround(segs[s1].first.y + intersections.first * (segs[s1].second.y - segs[s1].first.y));

		if ((x == segs[s1].first.x && y == segs[s1].first.y) ||
		    (x == segs[s1].second.x && y == segs[s1].second.y)) {
			// at an endpoint in s1, so it doesn't need to be changed
		} else {
			// printf("introduce %f,%f in %f,%f to %f,%f (s1 %zu %zu)\n", x, y, segs[s1].first.x, segs[s1].first.y, segs[s1].second.x, segs[s1].second.y, s1, s2);
			segs.push_back(std::make_pair(point(x, y), segs[s1].second));
			segs[s1] = std::make_pair(segs[s1].first, point(x, y));

			changed = true;
			affected.insert(segs[s1]);
			affected.insert(segs.back());
		}

		if ((x == segs[s2].first.x && y == segs[s2].first.y) ||
		    (x == segs[s2].second.x && y == segs[s2].second.y)) {
			// at an endpoint in s2, so it doesn't need to be changed
		} else {
			// printf("introduce %f,%f in %f,%f to %f,%f (s2 %zu %zu)\n", x, y, segs[s2].first.x, segs[s2].first.y, segs[s2].second.x, segs[s2].second.y, s1, s2);
			// printf("introduce %lld,%lld in %lld,%lld to %lld,%lld (s2)\n", std::llround(x), std::llround(y), std::llround(segs[s2].first.x), std::llround(segs[s2].first.y), std::llround(segs[s2].second.x), std::llround(segs[s2].second.y));
			segs.push_back(std::make_pair(point(x, y), segs[s2].second));
			segs[s2] = std::make_pair(segs[s2].first, point(x, y));

			changed = true;
			affected.insert(segs[s2]);
			affected.insert(segs.back());
		}
	} else if (intersections == SAME_SLOPE) {
		if (intersect_collinear(segs, s1, s2, affected)) {
			changed = true;
		}
	} else {
		// could intersect, but does not
	}

	return changed;
}

struct scan_transition {
	long long y;
	size_t segment;

	scan_transition(long long y_, size_t segment_)
	    : y(y_), segment(segment_) {
	}

	bool operator<(scan_transition const &s) const {
		if (y < s.y) {
			return true;
		} else if (y == s.y) {
			if (segment < s.segment) {
				return true;
			}
		}

		return false;
	}
};

void snap_round(std::vector<segment> &segs, long long extent) {
	bool again = true;

	// affected is indexed by segment instead of just segment index
	// because fix_opposites() causes renumbering
	std::set<segment> affected;

	while (again) {
		again = false;
		std::set<segment> previously_affected = affected;
		affected.clear();

		// find identical opposite-winding segments and adjust for them
		//
		// this is in the same loop because we may introduce new self-intersections
		// in the course of trying to keep spindles alive, and will then need to
		// resolve those.

		if (fix_opposites(segs, affected, extent)) {
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

		// do the scan

		std::set<size_t> active;
		size_t bottom = 0;

		for (size_t i = 0; i < tops.size(); i++) {
			// activate anything that is coming into view.

			active.insert(tops[i].segment);
			// fprintf(stderr, "into scope: %zu at %lld\n", tops[i].segment, tops[i].y);

			// compare anything coming into view with the other
			// currently-active segments

			for (size_t s2 : active) {
				size_t s1 = tops[i].segment;

				if (s1 != s2) {
					if (previously_affected.size() == 0 ||	// first time
					    previously_affected.count(segs[s1]) > 0 ||
					    previously_affected.count(segs[s2]) > 0) {
						// fprintf(stderr, "check %zu vs %zu\n", s1, s2);
						if (intersect(segs, s1, s2, affected)) {
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
					// fprintf(stderr, "out of scope: %zu at %lld\n", bottoms[bottom].segment, bottoms[bottom].y);
					bottom++;
				}
			}
		}
	}
}

// https://www.cuemath.com/geometry/area-of-triangle-in-coordinate-geometry/
double triangle_area(drawvec const &geom, size_t base, size_t increment, size_t len) {
	double area = ((double) geom[base + (increment + 0) % len].x * (geom[base + (increment + 1) % len].y - geom[base + (increment + 2) % len].y) +
		       (double) geom[base + (increment + 1) % len].x * (geom[base + (increment + 2) % len].y - geom[base + (increment + 0) % len].y) +
		       (double) geom[base + (increment + 2) % len].x * (geom[base + (increment + 0) % len].y - geom[base + (increment + 1) % len].y)) /
		      2;

	return area;
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

			if (triangle_area(geom, 0, i, geom.size() - 1) != 0 && pnpoly(geom, 0, geom.size(), x, y)) {
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
		} else if (std::fabs(area) == std::fabs(s.area)) {
			if (geom < s.geom) {
				return true;
			}
		}

		return false;
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

		// rotate the ring for idempotence
		size_t first = 0;
		for (size_t i = 1; i < ring.size(); i++) {
			if (ring[i] < ring[first]) {
				first = i;
			}
		}

		{
			std::vector<point> ring2;
			for (size_t i = 0; i < ring.size(); i++) {
				ring2.push_back(ring[(i + first) % ring.size()]);
			}
			ring = std::move(ring2);
		}

		// these coordinates are scaled, so that `encloses` can always find
		// an interior point in each ring
		drawvec out;
		for (size_t i = 0; i < ring.size(); i++) {
			out.emplace_back(i == 0 ? VT_MOVETO : VT_LINETO, ring[i].x * SCALE, ring[i].y * SCALE);
		}
		out.emplace_back(VT_LINETO, ring[0].x * SCALE, ring[0].y * SCALE);

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
				fprintf(stderr, "%lld,%lld ", (long long) g.x, (long long) g.y);
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

	return pnpoly(parent.geom, 0, parent.geom.size(), child.ear_x, child.ear_y);
}

bool same_slope(draw d1, draw d2, draw d3) {
	long long dx12 = d2.x - d1.x;
	long long dy12 = d2.y - d1.y;

	long long dx23 = d3.x - d2.x;
	long long dy23 = d3.y - d2.y;

	if (dx12 == 0) {
		if (dx12 == dx23) {
			return true;
		} else {
			return false;
		}
	}

	if (dy12 / (double) dx12 == dy23 / (double) dx23) {
		return true;
	} else {
		return false;
	}
}

drawvec remove_collinear(drawvec const &geom) {
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		if (i > 0 && i + 1 < geom.size() &&
		    geom[i].op == VT_LINETO && geom[i + 1].op == VT_LINETO &&
		    same_slope(out.back(), geom[i], geom[i + 1])) {
			continue;
		}

		out.push_back(geom[i]);
	}

	return out;
}

drawvec scale_polygon(drawvec const &geom, int z, int detail) {
	double scale = 1LL << (32 - detail - z);
	drawvec out;

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO) {
			size_t j;

			for (j = i + 1; j < geom.size(); j++) {
				if (geom[j].op != VT_LINETO) {
					break;
				}
			}

			drawvec ring;
			// k + 1 to avoid copying duplicate last point for the moment
			for (size_t k = i; k + 1 < j; k++) {
				ring.emplace_back(geom[k].op, std::llround(geom[k].x / scale), std::llround(geom[k].y / scale));
			}

			for (size_t k = 0; k < ring.size(); k++) {
				double scaled_area_orig = triangle_area(geom, i, k, ring.size()) / scale / scale;
				double area_scaled = triangle_area(ring, 0, k, ring.size());

				// Was this ear's winding corrupted during scaling?
				// But ignore ears with area less than one pixel,
				// to avoid excessive fiddling with the geometry
				if ((scaled_area_orig > 1 && area_scaled < -1) ||
				    (scaled_area_orig < -1 && area_scaled > 1)) {
					// jitter one of the coordinates to try to fix it,
					// on the theory that a slightly-wrong ring is
					// better than an entirely missing ring.
					//
					// getting to an area of 0 is good enough because
					// that is addressed in fix_opposites()
					for (long long dx = -1; dx <= 1; dx++) {
						for (long long dy = -1; dy <= 1; dy++) {
							drawvec altered = ring;
							altered[0 + (k + 1) % ring.size()].x += dx;
							altered[0 + (k + 1) % ring.size()].y += dy;
							double area_altered = triangle_area(altered, 0, k, altered.size());

							if ((scaled_area_orig > 1 && area_altered >= 0) ||
							    (scaled_area_orig < -1 && area_altered <= 0)) {
								ring = altered;

								dx = dy = INT_MAX;  // break from both loops
								break;
							}
						}
					}
				}
			}

			for (auto const &g : ring) {
				out.push_back(g);
			}

			// close the ring
			out.push_back(draw(VT_LINETO, ring[0].x, ring[0].y));

			i = j - 1;
		}
	}

	return out;
}

drawvec clean_polygon(drawvec geom, long long extent) {
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
					point(geom[k].x, geom[k].y),
					point(geom[k + 1].x, geom[k + 1].y));

				if (seg.first.x != seg.second.x ||
				    seg.first.y != seg.second.y) {
					segments.push_back(seg);
				}
			}

			i = j - 1;
		}
	}

	// snap-round intersecting segments

	snap_round(segments, extent);

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
				} else if (rings[i].area < 0 && rings[j].area < 0) {
					// inner ring within inner ring
					rings[i].geom.clear();
				} else if (rings[i].area > 0 && rings[j].area > 0) {
					// outer ring within outer ring
					rings[i].geom.clear();
				} else {
					// outer ring within an inner ring;
					// this is fine, but it is treated as a new outer ring in the tile,
					// not output in a hierarchy with the enclosing rings
				}

				break;
			}
		}
	}

	drawvec ret;
	for (size_t i = 0; i < rings.size(); i++) {
		if (rings[i].area < 0 && rings[i].geom.size() != 0) {
			// drop top-level holes
			continue;
		}

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

	// remove collinear points

	return remove_collinear(ret);
}
