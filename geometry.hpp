#ifndef GEOMETRY_HPP
#define GEOMETRY_HPP

#include <vector>
#include <atomic>
#include <set>
#include <string>
#include <sqlite3.h>
#include <stdio.h>
#include <mvt.hpp>
#include "jsonpull/jsonpull.h"
#include "attribute.hpp"

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

	bool operator>(draw const &s) const {
		return s < *this;
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

drawvec decode_geometry(const char **meta, int z, unsigned tx, unsigned ty, long long *bbox, unsigned initial_x, unsigned initial_y);
void to_tile_scale(drawvec &geom, int z, int detail);
drawvec from_tile_scale(drawvec const &geom, int z, int detail);
drawvec remove_noop(drawvec geom, int type, int shift);
drawvec clip_point(drawvec &geom, int z, long long buffer);
drawvec clean_or_clip_poly(drawvec &geom, int z, int buffer, bool clip, bool try_scaling);
drawvec close_poly(drawvec &geom);
drawvec reduce_tiny_poly(const drawvec &geom, int z, int detail, bool *still_needs_simplification, bool *reduced_away, double *accum_area, double tiny_polygon_size);
int clip(long long *x0, long long *y0, long long *x1, long long *y1, long long xmin, long long ymin, long long xmax, long long ymax);
drawvec clip_lines(drawvec &geom, int z, long long buffer);
drawvec stairstep(drawvec &geom, int z, int detail);
bool point_within_tile(long long x, long long y, int z);
int quick_check(const long long *bbox, int z, long long buffer);
void douglas_peucker(drawvec &geom, int start, int n, double e, size_t kept, size_t retain, bool prevent_simplify_shared_nodes);
drawvec simplify_lines(drawvec &geom, int z, int tx, int ty, int detail, bool mark_tile_bounds, double simplification, size_t retain, drawvec const &shared_nodes, struct node *shared_nodes_map, size_t nodepos, std::string const &shared_nodes_bloom);
drawvec reorder_lines(const drawvec &geom);
drawvec fix_polygon(const drawvec &geom, bool use_winding, bool reverse_winding);
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
bool pnpoly_mp(drawvec const &geom, long long x, long long y);
double distance_from_line(long long point_x, long long point_y, long long segA_x, long long segA_y, long long segB_x, long long segB_y);

drawvec clip_poly_poly(drawvec const &geom, drawvec const &bounds);
drawvec clip_lines_poly(drawvec const &geom, drawvec const &bounds);
drawvec clip_point_poly(drawvec const &geom, drawvec const &bounds);

struct input_tile {
	std::string tile;
	int z;
	int x;
	int y;
};

struct source_tile {
	mvt_tile tile;
	int z;
	int x;
	int y;
};

struct clipbbox {
	double lon1;
	double lat1;
	double lon2;
	double lat2;

	long long minx;
	long long miny;
	long long maxx;
	long long maxy;

	drawvec dv;  // empty, or arbitrary clipping polygon
};

std::string overzoom(std::vector<source_tile> const &tiles, int nz, int nx, int ny,
		     int detail, int buffer,
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
		     bool deduplicate_by_id);

std::string overzoom(std::vector<input_tile> const &tiles, int nz, int nx, int ny,
		     int detail, int buffer,
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
		     bool deduplicate_by_id);

draw center_of_mass_mp(const drawvec &dv);

void get_quadkey_bounds(long long xmin, long long ymin, long long xmax, long long ymax,
			unsigned long long *start, unsigned long long *end);

clipbbox parse_clip_poly(std::string arg);

bool line_is_too_small(drawvec const &geometry, int z, int detail);
void coalesce_polygon(drawvec &geom, bool scale_up);

#endif
