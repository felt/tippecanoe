#ifndef GEOMETRY_HPP
#define GEOMETRY_HPP

#include <vector>
#include <atomic>
#include <set>
#include <string>
#include <sqlite3.h>
#include <stdio.h>

#define VT_POINT 1
#define VT_LINE 2
#define VT_POLYGON 3

#define VT_END 0
#define VT_MOVETO 1
#define VT_LINETO 2
#define VT_CLOSEPATH 7

// The bitfield is to make sizeof(draw) be 16 instead of 24
// at the cost, apparently, of a 0.7% increase in running time
// for packing and unpacking.
struct draw {
	long long x : 40;
	signed char op;
	long long y : 40;
	signed char necessary;

	draw(int nop, long long nx, long long ny)
	    : x(nx),
	      op(nop),
	      y(ny),
	      necessary(0) {
	}

	draw()
	    : x(0),
	      op(0),
	      y(0),
	      necessary(0) {
	}

	bool operator<(draw const &s) const {
		if (y < s.y || (y == s.y && x < s.x)) {
			return true;
		} else {
			return false;
		}
	}

	bool operator==(draw const &s) const {
		return y == s.y && x == s.x;
	}

	bool operator!=(draw const &s) const {
		return y != s.y || x != s.x;
	}
};

typedef std::vector<draw> drawvec;
struct serial_feature;

drawvec decode_geometry(char **meta, int z, unsigned tx, unsigned ty, long long *bbox, unsigned initial_x, unsigned initial_y);
void to_tile_scale(drawvec &geom, int z, int detail);
drawvec from_tile_scale(drawvec const &geom, int z, int detail);
drawvec remove_noop(drawvec geom, int type, int shift);
drawvec clip_point(drawvec &geom, int z, long long buffer);
drawvec clean_or_clip_poly(drawvec &geom, int z, int buffer, bool clip, bool try_scaling);
drawvec close_poly(drawvec &geom);
drawvec reduce_tiny_poly(drawvec &geom, int z, int detail, bool *still_needs_simplification, bool *reduced_away, double *accum_area, serial_feature *this_feature, serial_feature *tiny_feature);
int clip(long long *x0, long long *y0, long long *x1, long long *y1, long long xmin, long long ymin, long long xmax, long long ymax);
drawvec clip_lines(drawvec &geom, int z, long long buffer);
drawvec stairstep(drawvec &geom, int z, int detail);
bool point_within_tile(long long x, long long y, int z);
int quick_check(long long *bbox, int z, long long buffer);
drawvec simplify_lines(drawvec &geom, int z, int detail, bool mark_tile_bounds, double simplification, size_t retain, drawvec const &shared_nodes);
drawvec reorder_lines(drawvec &geom);
drawvec fix_polygon(drawvec &geom);
std::vector<drawvec> chop_polygon(std::vector<drawvec> &geoms);
void check_polygon(drawvec &geom);
double get_area(const drawvec &geom, size_t i, size_t j);
double get_mp_area(drawvec &geom);
drawvec polygon_to_anchor(const drawvec &geom);
drawvec checkerboard_anchors(drawvec const &geom, int tx, int ty, int z, unsigned long long label_point);

drawvec simple_clip_poly(drawvec &geom, int z, int buffer, drawvec &shared_nodes, bool prevent_simplify_shared_nodes);
drawvec simple_clip_poly(drawvec &geom, long long x1, long long y1, long long x2, long long y2, bool prevent_simplify_shared_nodes);
drawvec simple_clip_poly(drawvec &geom, long long x1, long long y1, long long x2, long long y2,
			 long long ax, long long ay, long long bx, long long by, drawvec &shared_nodes, bool prevent_simplify_shared_nodes);
drawvec clip_lines(drawvec &geom, long long x1, long long y1, long long x2, long long y2);
drawvec clip_point(drawvec &geom, long long x1, long long y1, long long x2, long long y2);
void visvalingam(drawvec &ls, size_t start, size_t end, double threshold, size_t retain);
int pnpoly(const drawvec &vert, size_t start, size_t nvert, long long testx, long long testy);
double distance_from_line(long long point_x, long long point_y, long long segA_x, long long segA_y, long long segB_x, long long segB_y);

std::string overzoom(std::string s, int oz, int ox, int oy, int nz, int nx, int ny,
		     int detail, int buffer, std::set<std::string> const &keep);

#endif
