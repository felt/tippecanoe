#ifdef __APPLE__
#define _DARWIN_UNLIMITED_STREAMS
#endif

#include <iostream>
#include <fstream>
#include <string>
#include <stack>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <zlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <cmath>
#include <sqlite3.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <zlib.h>
#include <sys/wait.h>
#include "mvt.hpp"
#include "mbtiles.hpp"
#include "dirtiles.hpp"
#include "geometry.hpp"
#include "tile.hpp"
#include "pool.hpp"
#include "projection.hpp"
#include "serial.hpp"
#include "options.hpp"
#include "main.hpp"
#include "write_json.hpp"
#include "milo/dtoa_milo.h"
#include "evaluator.hpp"
#include "errors.hpp"
#include "compression.hpp"
#include "protozero/varint.hpp"
#include "attribute.hpp"
#include "thread.hpp"

extern "C" {
#include "jsonpull/jsonpull.h"
}

#include "plugin.hpp"

#define CMD_BITS 3

// Offset coordinates to keep them positive
#define COORD_OFFSET (4LL << 32)
#define SHIFT_RIGHT(a) ((long long) std::round((double) (a) / (1LL << geometry_scale)))

#define XSTRINGIFY(s) STRINGIFY(s)
#define STRINGIFY(s) #s

pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t var_lock = PTHREAD_MUTEX_INITIALIZER;

// convert serial feature geometry (drawvec) to output tile geometry (mvt_geometry)
static std::vector<mvt_geometry> to_feature(drawvec const &geom) {
	std::vector<mvt_geometry> out;

	for (size_t i = 0; i < geom.size(); i++) {
		out.emplace_back(geom[i].op, geom[i].x, geom[i].y);
	}

	return out;
}

// does this geometry have any non-zero-length linetos?
static bool draws_something(drawvec const &geom) {
	for (size_t i = 1; i < geom.size(); i++) {
		if (geom[i].op == VT_LINETO && (geom[i].x != geom[i - 1].x || geom[i].y != geom[i - 1].y)) {
			return true;
		}
	}

	return false;
}

static int metacmp(const std::vector<long long> &keys1, const std::vector<long long> &values1, char *stringpool1, const std::vector<long long> &keys2, const std::vector<long long> &values2, char *stringpool2, std::shared_ptr<std::string> const &tile_stringpool);

// comparator for --preserve-input-order, to reorder features back to their original input sequence
static struct preservecmp {
	bool operator()(const std::vector<serial_feature> &a, const std::vector<serial_feature> &b) {
		return operator()(a[0], b[0]);
	}

	bool operator()(const serial_feature &a, const serial_feature &b) {
		return a.seq < b.seq;
	}
} preservecmp;

// comparator for --coalesce and --reorder:
// two features can be coalesced if they have
// * the same type
// * the same id, if any
// * the same attributes, according to metacmp
// * the same full_keys and full_values attributes
//
// The tile_stringpool is used to construct temporary mvt_values for comparison.
static int coalcmp(const void *v1, const void *v2, std::shared_ptr<std::string> const &tile_stringpool) {
	const serial_feature *c1 = (const serial_feature *) v1;
	const serial_feature *c2 = (const serial_feature *) v2;

	int cmp = c1->t - c2->t;
	if (cmp != 0) {
		return cmp;
	}

	if (c1->has_id != c2->has_id) {
		return (int) c1->has_id - (int) c2->has_id;
	}

	if (c1->has_id && c2->has_id) {
		if (c1->id < c2->id) {
			return -1;
		}
		if (c1->id > c2->id) {
			return 1;
		}
	}

	cmp = metacmp(c1->keys, c1->values, c1->stringpool, c2->keys, c2->values, c2->stringpool, tile_stringpool);
	if (cmp != 0) {
		return cmp;
	}

	if (c1->full_keys.size() < c2->full_keys.size()) {
		return -1;
	} else if (c1->full_keys.size() > c2->full_keys.size()) {
		return 1;
	}

	for (size_t i = 0; i < c1->full_keys.size(); i++) {
		if (c1->full_keys[i] < c2->full_keys[i]) {
			return -1;
		} else if (c1->full_keys[i] > c2->full_keys[i]) {
			return 1;
		}

		if (c1->full_values[i].type < c2->full_values[i].type) {
			return -1;
		} else if (c1->full_values[i].type > c2->full_values[i].type) {
			return 1;
		}

		if (c1->full_values[i].s < c2->full_values[i].s) {
			return -1;
		} else if (c1->full_values[i].s > c2->full_values[i].s) {
			return 1;
		}
	}

	return 0;
}

// comparator for --reorder:
// features are ordered first by their attributes (according to coalcmp above)
// and then, if they are identical from that perspective, by their index (centroid)
// and geometry
struct coalindexcmp_comparator {
	int coalindexcmp(const serial_feature *c1, const serial_feature *c2, std::shared_ptr<std::string> const &tile_stringpool) const {
		int cmp = coalcmp((const void *) c1, (const void *) c2, tile_stringpool);

		if (cmp == 0) {
			if (c1->index < c2->index) {
				return -1;
			} else if (c1->index > c2->index) {
				return 1;
			}

			if (c1->geometry < c2->geometry) {
				return -1;
			} else if (c1->geometry > c2->geometry) {
				return 1;
			}
		}

		return cmp;
	}

	bool operator()(const serial_feature &a, const serial_feature &o) const {
		int cmp = coalindexcmp(&a, &o, a.tile_stringpool);
		if (cmp < 0) {
			return true;
		} else {
			return false;
		}
	}
};

// retrieve an attribute key or value from the string pool and return it as mvt_value
static mvt_value retrieve_string(long long off, const char *stringpool, std::shared_ptr<std::string> const &tile_stringpool) {
	int type = stringpool[off];
	const char *s = stringpool + off + 1;

	return stringified_to_mvt_value(type, s, tile_stringpool);
}

// retrieve an attribute key from the string pool and return it as std::string
static std::string retrieve_std_string(long long off, const char *stringpool) {
	return std::string(stringpool + off + 1);
}

// retrieve the keys and values of a feature from the string pool
// and tag them onto an mvt_feature and mvt_layer
static void decode_meta(std::vector<long long> const &metakeys, std::vector<long long> const &metavals, char *stringpool, mvt_layer &layer, mvt_feature &feature, std::shared_ptr<std::string> const &tile_stringpool) {
	size_t i;
	for (i = 0; i < metakeys.size(); i++) {
		std::string key = retrieve_std_string(metakeys[i], stringpool);
		mvt_value value = retrieve_string(metavals[i], stringpool, tile_stringpool);

		layer.tag(feature, key, value);
	}
}

// comparator used to check whether two features have identical keys and values,
// as determined by retrieving them from the string pool. The order of keys,
// not just the content of their values, must also be identical for them to compare equal.
//
// The tile_string_pool is used to construct temporary mvt_values for comparison.
// This should probably actually do direct lookups into the string pool instead of constructing.
static int metacmp(const std::vector<long long> &keys1, const std::vector<long long> &values1, char *stringpool1, const std::vector<long long> &keys2, const std::vector<long long> &values2, char *stringpool2, std::shared_ptr<std::string> const &tile_stringpool) {
	if (keys1.size() < keys2.size()) {
		return -1;
	} else if (keys1.size() > keys2.size()) {
		return 1;
	}

	size_t i;
	for (i = 0; i < keys1.size() && i < keys2.size(); i++) {
		mvt_value key1 = retrieve_string(keys1[i], stringpool1, tile_stringpool);
		mvt_value key2 = retrieve_string(keys2[i], stringpool2, tile_stringpool);

		if (key1.get_string_view() < key2.get_string_view()) {
			return -1;
		} else if (key1.get_string_view() > key2.get_string_view()) {
			return 1;
		}

		long long off1 = values1[i];
		int type1 = stringpool1[off1];
		char *s1 = stringpool1 + off1 + 1;

		long long off2 = values2[i];
		int type2 = stringpool2[off2];
		char *s2 = stringpool2 + off2 + 1;

		if (type1 != type2) {
			return type1 - type2;
		}
		int cmp = strcmp(s1, s2);
		if (cmp != 0) {
			return cmp;
		}
	}

	return 0;
}

// Retrieve the value of an attribute or pseudo-attribute (ORDER_BY_SIZE) for --order purposes.
// The tile_stringpool argument is used to construct the mvt_values.
static mvt_value find_attribute_value(const serial_feature *c1, std::string const &key, std::shared_ptr<std::string> &tile_stringpool) {
	if (key == ORDER_BY_SIZE) {
		mvt_value v;
		v.type = mvt_double;
		v.numeric_value.double_value = c1->extent;
		return v;
	}

	const std::vector<long long> &keys1 = c1->keys;
	const std::vector<long long> &values1 = c1->values;
	const char *stringpool1 = c1->stringpool;

	for (size_t i = 0; i < keys1.size(); i++) {
		mvt_value key1 = retrieve_string(keys1[i], stringpool1, tile_stringpool);
		if (key == key1.get_string_value()) {
			return retrieve_string(values1[i], stringpool1, tile_stringpool);
		}
	}

	for (size_t i = 0; i < c1->full_keys.size(); i++) {
		if (c1->full_keys[i] == key) {
			return stringified_to_mvt_value(c1->full_values[i].type, c1->full_values[i].s.c_str(), tile_stringpool);
		}
	}

	mvt_value v;
	v.type = mvt_null;
	v.numeric_value.null_value = 0;
	return v;
}

// Ensure that two mvt_values can be compared numerically by converting other numeric types to mvt_double
static mvt_value coerce_double(mvt_value v) {
	if (v.type == mvt_int) {
		v.type = mvt_double;
		v.numeric_value.double_value = v.numeric_value.int_value;
	} else if (v.type == mvt_uint) {
		v.type = mvt_double;
		v.numeric_value.double_value = v.numeric_value.uint_value;
	} else if (v.type == mvt_sint) {
		v.type = mvt_double;
		v.numeric_value.double_value = v.numeric_value.sint_value;
	} else if (v.type == mvt_float) {
		v.type = mvt_double;
		v.numeric_value.double_value = v.numeric_value.float_value;
	}

	return v;
}

// comparator for ordering features for --order: for each sort key that the user has specified,
// compare features numerically according to that sort key until the keys are exhausted.
// If there is a tie, the feature with the earlier index (centroid) comes first.
struct ordercmp {
	std::shared_ptr<std::string> tile_stringpool = std::make_shared<std::string>();

	bool operator()(const std::vector<serial_feature> &a, const std::vector<serial_feature> &b) {
		return operator()(a[0], b[0]);
	}

	bool operator()(const serial_feature &a, const serial_feature &b) {
		for (size_t i = 0; i < order_by.size(); i++) {
			mvt_value v1 = coerce_double(find_attribute_value(&a, order_by[i].name, tile_stringpool));
			mvt_value v2 = coerce_double(find_attribute_value(&b, order_by[i].name, tile_stringpool));

			if (order_by[i].descending) {
				if (v2 < v1) {
					return true;
				} else if (v1 < v2) {
					return false;
				}  // else they are equal, so continue to the next attribute
			} else {
				if (v1 < v2) {
					return true;
				} else if (v2 < v1) {
					return false;
				}  // else they are equal, so continue to the next attribute
			}
		}

		if (a.index < b.index) {
			return true;
		}

		return false;  // greater than or equal
	}
};

// For --retain-points-multiplier: Go through a list of features and return a list of clusters of features,
// creating a new cluster whenever the tippecanoe:retain_points_multiplier_first attribute is seen.
static std::vector<std::vector<serial_feature>> assemble_multiplier_clusters(std::vector<serial_feature> const &features) {
	std::vector<std::vector<serial_feature>> clusters;

	if (retain_points_multiplier == 1) {
		for (auto const &feature : features) {
			std::vector<serial_feature> cluster;
			cluster.push_back(feature);
			clusters.push_back(cluster);
		}
	} else {
		for (auto const &feature : features) {
			bool is_cluster_start = false;

			for (size_t i = 0; i < feature.full_keys.size(); i++) {
				if (feature.full_keys[i] == "tippecanoe:retain_points_multiplier_first") {
					is_cluster_start = true;
					break;
				}
			}

			if (is_cluster_start || clusters.size() == 0) {
				clusters.push_back(std::vector<serial_feature>());
			}

			clusters.back().push_back(feature);
		}
	}

	return clusters;
}

// For --retain-points-multiplier: Flatten a list of clusters of features back into a list of features,
// moving the "tippecanoe:retain_points_multiplier_first" attribute onto the first feature of each cluster
// if it is not already there.
static std::vector<serial_feature> disassemble_multiplier_clusters(std::vector<std::vector<serial_feature>> &clusters) {
	std::vector<serial_feature> out;

	for (auto &cluster : clusters) {
		// fix up the attributes so the first feature of the multiplier cluster
		// gets the marker attribute
		for (size_t i = 0; i < cluster.size(); i++) {
			for (size_t j = 0; j < cluster[i].full_keys.size(); j++) {
				if (cluster[i].full_keys[j] == "tippecanoe:retain_points_multiplier_first") {
					cluster[0].full_keys.push_back(cluster[i].full_keys[j]);
					cluster[0].full_values.push_back(cluster[i].full_values[j]);

					cluster[i].full_keys.erase(cluster[i].full_keys.begin() + j);
					cluster[i].full_values.erase(cluster[i].full_values.begin() + j);

					i = cluster.size();  // break outer
					break;
				}
			}
		}

		for (auto const &feature : cluster) {
			out.push_back(feature);
		}
	}

	return out;
}

void rewrite(serial_feature const &osf, drawvec &geom, int z, int nextzoom, int maxzoom, long long *bbox, unsigned tx, unsigned ty, int buffer, int *within, std::atomic<long long> *geompos, compressor **geomfile, const char *fname, int child_shards, int max_zoom_increment, int segment, unsigned *initial_x, unsigned *initial_y) {
	if (geom.size() > 0 && (nextzoom <= maxzoom || additional[A_EXTEND_ZOOMS] || extend_zooms_max > 0)) {
		int xo, yo;
		int span = 1 << (nextzoom - z);

		// Get the feature bounding box in pixel (256) coordinates at the child zoom
		// in order to calculate which sub-tiles it can touch including the buffer.
		long long bbox2[4];
		int k;
		for (k = 0; k < 4; k++) {
			// Division instead of right-shift because coordinates can be negative
			bbox2[k] = bbox[k] / (1 << (32 - nextzoom - 8));
		}
		// Decrement the top and left edges so that any features that are
		// touching the edge can potentially be included in the adjacent tiles too.
		bbox2[0] -= buffer + 1;
		bbox2[1] -= buffer + 1;
		bbox2[2] += buffer;
		bbox2[3] += buffer;

		for (k = 0; k < 4; k++) {
			if (bbox2[k] < 0) {
				bbox2[k] = 0;
			}
			if (bbox2[k] >= 256 * span) {
				bbox2[k] = 256 * (span - 1);
			}

			bbox2[k] /= 256;
		}

		// Offset from tile coordinates back to world coordinates
		unsigned sx = 0, sy = 0;
		if (z != 0) {
			sx = tx << (32 - z);
			sy = ty << (32 - z);
		}

		drawvec geom2;
		for (size_t i = 0; i < geom.size(); i++) {
			geom2.push_back(draw(geom[i].op, SHIFT_RIGHT(geom[i].x + sx), SHIFT_RIGHT(geom[i].y + sy)));
		}

		for (xo = bbox2[0]; xo <= bbox2[2]; xo++) {
			for (yo = bbox2[1]; yo <= bbox2[3]; yo++) {
				unsigned jx = tx * span + xo;
				unsigned jy = ty * span + yo;

				// j is the shard that the child tile's data is being written to.
				//
				// Be careful: We can't jump more zoom levels than max_zoom_increment
				// because that could break the constraint that each of the children
				// of the current tile must have its own shard, because the data for
				// the child tile must be contiguous within the shard.
				//
				// But it's OK to spread children across all the shards, not just
				// the four that would normally result from splitting one tile,
				// because it will go through all the shards when it does the
				// next zoom.
				//
				// If child_shards is a power of 2 but not a power of 4, this will
				// shard X more widely than Y. XXX Is there a better way to do this
				// without causing collisions?

				int j = ((jx << max_zoom_increment) |
					 ((jy & ((1 << max_zoom_increment) - 1)))) &
					(child_shards - 1);

				{
					if (!within[j]) {
						serialize_int(geomfile[j]->fp, nextzoom, &geompos[j], fname);
						serialize_uint(geomfile[j]->fp, tx * span + xo, &geompos[j], fname);
						serialize_uint(geomfile[j]->fp, ty * span + yo, &geompos[j], fname);
						geomfile[j]->begin();
						within[j] = 1;
					}

					serial_feature sf;
					sf.layer = osf.layer;
					sf.segment = osf.segment;
					sf.seq = osf.seq;
					sf.t = osf.t;
					sf.has_id = osf.has_id;
					sf.id = osf.id;
					sf.has_tippecanoe_minzoom = osf.has_tippecanoe_minzoom;
					sf.tippecanoe_minzoom = osf.tippecanoe_minzoom;
					sf.has_tippecanoe_maxzoom = osf.has_tippecanoe_maxzoom;
					sf.tippecanoe_maxzoom = osf.tippecanoe_maxzoom;
					sf.geometry = geom2;
					sf.index = osf.index;
					sf.label_point = osf.label_point;
					sf.extent = osf.extent;
					sf.feature_minzoom = osf.feature_minzoom;

					sf.keys = osf.keys;
					sf.values = osf.values;

					std::string feature = serialize_feature(&sf, SHIFT_RIGHT(initial_x[segment]), SHIFT_RIGHT(initial_y[segment]));
					geomfile[j]->serialize_long_long(feature.size(), &geompos[j], fname);
					geomfile[j]->fwrite_check(feature.c_str(), sizeof(char), feature.size(), &geompos[j], fname);
				}
			}
		}
	}
}

struct simplification_worker_arg {
	std::vector<serial_feature> *features = NULL;
	int task = 0;
	int tasks = 0;

	drawvec *shared_nodes;
	node *shared_nodes_map;
	size_t nodepos;
};

drawvec revive_polygon(drawvec &geom, double area, int z, int detail) {
	// From area in world coordinates to area in tile coordinates
	long long divisor = 1LL << (32 - detail - z);
	area /= divisor * divisor;

	if (area == 0) {
		return drawvec();
	}

	int height = ceil(sqrt(area));
	int width = round(area / height);
	if (width == 0) {
		width = 1;
	}

	long long sx = 0, sy = 0, n = 0;
	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO || geom[i].op == VT_LINETO) {
			sx += geom[i].x;
			sy += geom[i].y;
			n++;
		}
	}

	if (n > 0) {
		sx /= n;
		sy /= n;

		drawvec out;
		out.push_back(draw(VT_MOVETO, sx - (width / 2), sy - (height / 2)));
		out.push_back(draw(VT_LINETO, sx - (width / 2) + width, sy - (height / 2)));
		out.push_back(draw(VT_LINETO, sx - (width / 2) + width, sy - (height / 2) + height));
		out.push_back(draw(VT_LINETO, sx - (width / 2), sy - (height / 2) + height));
		out.push_back(draw(VT_LINETO, sx - (width / 2), sy - (height / 2)));

		return out;
	} else {
		return drawvec();
	}
}

double simplify_serial_feature(serial_feature *p, drawvec const &shared_nodes, node *shared_nodes_map, size_t nodepos) {
	drawvec geom = p->geometry;
	signed char t = p->t;
	int z = p->z;
	int line_detail = p->line_detail;
	int maxzoom = p->maxzoom;

	if (additional[A_GRID_LOW_ZOOMS] && z < maxzoom) {
		geom = stairstep(geom, z, line_detail);
	}

	double area = 0;
	if (t == VT_POLYGON) {
		area = get_mp_area(geom);
	}

	if ((t == VT_LINE || t == VT_POLYGON) && !(prevent[P_SIMPLIFY] || (z == maxzoom && prevent[P_SIMPLIFY_LOW]) || (z < maxzoom && additional[A_GRID_LOW_ZOOMS]))) {
		// Now I finally remember why it doesn't simplify if the feature was reduced:
		// because it makes square placeholders look like weird triangular placeholders.
		// Only matters if simplification is set higher than the tiny polygon size.
		// Tiny polygons that are part of a tiny multipolygon will still get simplified.
		if (!p->reduced) {
			// These aren't necessarily actually no-ops until we scale down.
			// Don't do it if we are trying to preserve intersections, because
			// it might wipe out the intersection and spoil the matching even though
			// it would leave something else within the same tile pixel.
			if (t == VT_LINE && !prevent[P_SIMPLIFY_SHARED_NODES]) {
				// continues to deduplicate to line_detail even if we have extra detail
				geom = remove_noop(geom, t, 32 - z - line_detail);
			}

			bool already_marked = false;
			if (additional[A_DETECT_SHARED_BORDERS] && t == VT_POLYGON) {
				already_marked = true;
			}

			if (!already_marked) {
				if (p->coalesced && t == VT_POLYGON) {
					// clean coalesced polygons before simplification to avoid
					// introducing shards between shapes that otherwise would have
					// unioned exactly
					//
					// don't try to scale up because these are still world coordinates
					geom = clean_or_clip_poly(geom, 0, 0, false, false);
				}

				// continues to simplify to line_detail even if we have extra detail
				drawvec ngeom = simplify_lines(geom, z, p->tx, p->ty, line_detail, !(prevent[P_CLIPPING] || prevent[P_DUPLICATION]), p->simplification, t == VT_POLYGON ? 4 : 0, shared_nodes, shared_nodes_map, nodepos);

				if (t != VT_POLYGON || ngeom.size() >= 3) {
					geom = ngeom;
				}
			}
		}
	}

	if (t == VT_LINE && additional[A_REVERSE]) {
		geom = remove_noop(geom, t, 0);
		geom = reorder_lines(geom);
	}

	p->geometry = std::move(geom);
	return area;
}

void *simplification_worker(void *v) {
	simplification_worker_arg *a = (simplification_worker_arg *) v;
	std::vector<serial_feature> *features = a->features;

	for (size_t i = a->task; i < (*features).size(); i += a->tasks) {
		double area = simplify_serial_feature(&((*features)[i]), *(a->shared_nodes), a->shared_nodes_map, a->nodepos);

		signed char t = (*features)[i].t;
		int z = (*features)[i].z;
		int out_detail = (*features)[i].extra_detail;

		drawvec geom = (*features)[i].geometry;
		to_tile_scale(geom, z, out_detail);

		if (t == VT_POLYGON) {
			// Scaling may have made the polygon degenerate.
			// Give Clipper a chance to try to fix it.
			{
				drawvec before = geom;
				// we can try scaling up because this is now tile scale
				geom = clean_or_clip_poly(geom, 0, 0, false, true);
				if (additional[A_DEBUG_POLYGON]) {
					check_polygon(geom);
				}

				if (geom.size() < 3) {
					if (area > 0) {
						// area is in world coordinates, calculated before scaling down
						geom = revive_polygon(before, area, z, out_detail);
					} else {
						geom.clear();
					}
				}
			}
		}

		if (t == VT_POLYGON && additional[A_GENERATE_POLYGON_LABEL_POINTS]) {
			t = (*features)[i].t = VT_POINT;
			geom = checkerboard_anchors(from_tile_scale(geom, z, out_detail), (*features)[i].tx, (*features)[i].ty, z, (*features)[i].label_point);
			to_tile_scale(geom, z, out_detail);
		}

		(*features)[i].index = i;
		(*features)[i].geometry = std::move(geom);
	}

	return NULL;
}

int manage_gap(unsigned long long index, unsigned long long *previndex, double scale, double gamma, double *gap) {
	if (gamma > 0) {
		if (*gap > 0) {
			if (index == *previndex) {
				return 1;  // Exact duplicate: can't fulfil the gap requirement
			}

			if (index < *previndex || std::exp(std::log((index - *previndex) / scale) * gamma) >= *gap) {
				// Dot is further from the previous than the nth root of the gap,
				// so produce it, and choose a new gap at the next point.
				*gap = 0;
			} else {
				return 1;
			}
		} else if (index >= *previndex) {
			*gap = (index - *previndex) / scale;

			if (*gap == 0) {
				return 1;  // Exact duplicate: skip
			} else if (*gap < 1) {
				return 1;  // Narrow dot spacing: need to stretch out
			} else {
				*gap = 0;  // Wider spacing than minimum: so pass through unchanged
			}
		}

		*previndex = index;
	}

	return 0;
}

// Does not fix up moveto/lineto
static drawvec reverse_subring(drawvec const &dv) {
	drawvec out;

	for (size_t i = dv.size(); i > 0; i--) {
		out.push_back(dv[i - 1]);
	}

	return out;
}

struct edge {
	unsigned x1 = 0;
	unsigned y1 = 0;
	unsigned x2 = 0;
	unsigned y2 = 0;
	unsigned ring = 0;

	edge(unsigned _x1, unsigned _y1, unsigned _x2, unsigned _y2, unsigned _ring) {
		x1 = _x1;
		y1 = _y1;
		x2 = _x2;
		y2 = _y2;
		ring = _ring;
	}

	bool operator<(const edge &s) const {
		long long cmp = (long long) y1 - s.y1;
		if (cmp == 0) {
			cmp = (long long) x1 - s.x1;
		}
		if (cmp == 0) {
			cmp = (long long) y2 - s.y2;
		}
		if (cmp == 0) {
			cmp = (long long) x2 - s.x2;
		}
		return cmp < 0;
	}
};

struct edgecmp_ring {
	bool operator()(const edge &a, const edge &b) {
		long long cmp = (long long) a.y1 - b.y1;
		if (cmp == 0) {
			cmp = (long long) a.x1 - b.x1;
		}
		if (cmp == 0) {
			cmp = (long long) a.y2 - b.y2;
		}
		if (cmp == 0) {
			cmp = (long long) a.x2 - b.x2;
		}
		if (cmp == 0) {
			cmp = (long long) a.ring - b.ring;
		}
		return cmp < 0;
	}
} edgecmp_ring;

bool edges_same(std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e1, std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e2) {
	if ((e2.second - e2.first) != (e1.second - e1.first)) {
		return false;
	}

	while (e1.first != e1.second) {
		if (e1.first->ring != e2.first->ring) {
			return false;
		}

		++e1.first;
		++e2.first;
	}

	return true;
}

bool find_common_edges(std::vector<serial_feature> &features, int z, int line_detail, double simplification, int maxzoom, double merge_fraction) {
	size_t merge_count = ceil((1 - merge_fraction) * features.size());

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				drawvec &g = features[i].geometry;
				drawvec out;

				for (size_t k = 0; k < g.size(); k++) {
					if (g[k].op == VT_LINETO && k > 0 && g[k - 1] == g[k]) {
						;
					} else {
						out.push_back(g[k]);
					}
				}

				features[i].geometry = out;
			}
		}
	}

	// Construct a mapping from all polygon edges to the set of rings
	// that each edge appears in. (The ring number is across all polygons;
	// we don't need to look it back up, just to tell where it changes.)

	std::vector<edge> edges;
	size_t ring = 0;
	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				for (size_t k = 0; k + 1 < features[i].geometry.size(); k++) {
					if (features[i].geometry[k].op == VT_MOVETO) {
						ring++;
					}

					if (features[i].geometry[k + 1].op == VT_LINETO) {
						drawvec dv;
						if (features[i].geometry[k] < features[i].geometry[k + 1]) {
							dv.push_back(features[i].geometry[k]);
							dv.push_back(features[i].geometry[k + 1]);
						} else {
							dv.push_back(features[i].geometry[k + 1]);
							dv.push_back(features[i].geometry[k]);
						}

						edges.push_back(edge(dv[0].x, dv[0].y, dv[1].x, dv[1].y, ring));
					}
				}
			}
		}
	}

	std::sort(edges.begin(), edges.end(), edgecmp_ring);
	std::set<draw> necessaries;

	// Now mark all the points where the set of rings using the edge on one side
	// is not the same as the set of rings using the edge on the other side.

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				drawvec &g = features[i].geometry;

				for (size_t k = 0; k < g.size(); k++) {
					g[k].necessary = 0;
				}

				for (size_t a = 0; a < g.size(); a++) {
					if (g[a].op == VT_MOVETO) {
						size_t b;

						for (b = a + 1; b < g.size(); b++) {
							if (g[b].op != VT_LINETO) {
								break;
							}
						}

						// -1 because of duplication at the end
						size_t s = b - a - 1;

						if (s > 0) {
							drawvec left;
							if (g[a + (s - 1) % s] < g[a]) {
								left.push_back(g[a + (s - 1) % s]);
								left.push_back(g[a]);
							} else {
								left.push_back(g[a]);
								left.push_back(g[a + (s - 1) % s]);
							}
							if (left[1] < left[0]) {
								fprintf(stderr, "left misordered\n");
							}
							std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e1 = std::equal_range(edges.begin(), edges.end(), edge(left[0].x, left[0].y, left[1].x, left[1].y, 0));

							for (size_t k = 0; k < s; k++) {
								drawvec right;

								if (g[a + k] < g[a + k + 1]) {
									right.push_back(g[a + k]);
									right.push_back(g[a + k + 1]);
								} else {
									right.push_back(g[a + k + 1]);
									right.push_back(g[a + k]);
								}

								std::pair<std::vector<edge>::iterator, std::vector<edge>::iterator> e2 = std::equal_range(edges.begin(), edges.end(), edge(right[0].x, right[0].y, right[1].x, right[1].y, 0));

								if (right[1] < right[0]) {
									fprintf(stderr, "left misordered\n");
								}

								if (e1.first == e1.second || e2.first == e2.second) {
									fprintf(stderr, "Internal error: polygon edge lookup failed for %lld,%lld to %lld,%lld or %lld,%lld to %lld,%lld\n", left[0].x, left[0].y, left[1].x, left[1].y, right[0].x, right[0].y, right[1].x, right[1].y);
									exit(EXIT_IMPOSSIBLE);
								}

								if (!edges_same(e1, e2)) {
									g[a + k].necessary = 1;
									necessaries.insert(g[a + k]);
								}

								e1 = e2;
							}
						}

						a = b - 1;
					}
				}
			}
		}
	}

	edges.clear();
	std::map<drawvec, size_t> arcs;
	std::multimap<ssize_t, size_t> merge_candidates;  // from arc to serial_feature

	// Roll rings that include a necessary point around so they start at one

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			{
				drawvec &g = features[i].geometry;

				for (size_t k = 0; k < g.size(); k++) {
					if (necessaries.count(g[k]) != 0) {
						g[k].necessary = 1;
					}
				}

				for (size_t k = 0; k < g.size(); k++) {
					if (g[k].op == VT_MOVETO) {
						ssize_t necessary = -1;
						ssize_t lowest = k;
						size_t l;
						for (l = k + 1; l < g.size(); l++) {
							if (g[l].op != VT_LINETO) {
								break;
							}

							if (g[l].necessary) {
								necessary = l;
							}
							if (g[l] < g[lowest]) {
								lowest = l;
							}
						}

						if (necessary < 0) {
							necessary = lowest;
							// Add a necessary marker if there was none in the ring,
							// so the arc code below can find it.
							g[lowest].necessary = 1;
						}

						{
							drawvec tmp;

							// l - 1 because the endpoint is duplicated
							for (size_t m = necessary; m < l - 1; m++) {
								tmp.push_back(g[m]);
							}
							for (ssize_t m = k; m < necessary; m++) {
								tmp.push_back(g[m]);
							}

							// replace the endpoint
							tmp.push_back(g[necessary]);

							if (tmp.size() != l - k) {
								fprintf(stderr, "internal error shifting ring\n");
								exit(EXIT_IMPOSSIBLE);
							}

							for (size_t m = 0; m < tmp.size(); m++) {
								if (m == 0) {
									tmp[m].op = VT_MOVETO;
								} else {
									tmp[m].op = VT_LINETO;
								}

								g[k + m] = tmp[m];
							}
						}

						// Now peel off each set of segments from one necessary point to the next
						// into an "arc" as in TopoJSON

						for (size_t m = k; m < l; m++) {
							if (!g[m].necessary) {
								fprintf(stderr, "internal error in arc building\n");
								exit(EXIT_IMPOSSIBLE);
							}

							drawvec arc;
							size_t n;
							for (n = m; n < l; n++) {
								arc.push_back(g[n]);
								if (n > m && g[n].necessary) {
									break;
								}
							}

							auto f = arcs.find(arc);
							if (f == arcs.end()) {
								drawvec arc2 = reverse_subring(arc);

								auto f2 = arcs.find(arc2);
								if (f2 == arcs.end()) {
									// Add new arc
									size_t added = arcs.size() + 1;
									arcs.insert(std::pair<drawvec, size_t>(arc, added));
									features[i].arc_polygon.push_back(added);
									merge_candidates.insert(std::pair<ssize_t, size_t>(added, i));
								} else {
									features[i].arc_polygon.push_back(-(ssize_t) f2->second);
									merge_candidates.insert(std::pair<ssize_t, size_t>(-(ssize_t) f2->second, i));
								}
							} else {
								features[i].arc_polygon.push_back(f->second);
								merge_candidates.insert(std::pair<ssize_t, size_t>(f->second, i));
							}

							m = n - 1;
						}

						features[i].arc_polygon.push_back(0);

						k = l - 1;
					}
				}
			}
		}
	}

	// Simplify each arc

	std::vector<drawvec> simplified_arcs;

	for (auto ai = arcs.begin(); ai != arcs.end(); ++ai) {
		if (simplified_arcs.size() < ai->second + 1) {
			simplified_arcs.resize(ai->second + 1);
		}

		drawvec dv = ai->first;
		for (size_t i = 0; i < dv.size(); i++) {
			if (i == 0) {
				dv[i].op = VT_MOVETO;
			} else {
				dv[i].op = VT_LINETO;
			}
		}
		if (!(prevent[P_SIMPLIFY] || (z == maxzoom && prevent[P_SIMPLIFY_LOW]) || (z < maxzoom && additional[A_GRID_LOW_ZOOMS]))) {
			// tx and ty are 0 here because we aren't trying to do anything with the shared_nodes_map
			simplified_arcs[ai->second] = simplify_lines(dv, z, 0, 0, line_detail, !(prevent[P_CLIPPING] || prevent[P_DUPLICATION]), simplification, 4, drawvec(), NULL, 0);
		} else {
			simplified_arcs[ai->second] = dv;
		}
	}

	// If necessary, merge some adjacent polygons into some other polygons

	struct merge_order {
		ssize_t edge = 0;
		unsigned long long gap = 0;
		size_t p1 = 0;
		size_t p2 = 0;

		bool operator<(const merge_order &m) const {
			return gap < m.gap;
		}
	};
	std::vector<merge_order> order;

	for (ssize_t i = 0; i < (ssize_t) simplified_arcs.size(); i++) {
		auto r1 = merge_candidates.equal_range(i);
		for (auto r1i = r1.first; r1i != r1.second; ++r1i) {
			auto r2 = merge_candidates.equal_range(-i);
			for (auto r2i = r2.first; r2i != r2.second; ++r2i) {
				if (r1i->second != r2i->second) {
					merge_order mo;
					mo.edge = i;
					if (features[r1i->second].index > features[r2i->second].index) {
						mo.gap = features[r1i->second].index - features[r2i->second].index;
					} else {
						mo.gap = features[r2i->second].index - features[r1i->second].index;
					}
					mo.p1 = r1i->second;
					mo.p2 = r2i->second;
					order.push_back(mo);
				}
			}
		}
	}
	std::sort(order.begin(), order.end());

	size_t merged = 0;
	for (size_t o = 0; o < order.size(); o++) {
		if (merged >= merge_count) {
			break;
		}

		size_t i = order[o].p1;
		while (features[i].renamed >= 0) {
			i = features[i].renamed;
		}
		size_t i2 = order[o].p2;
		while (features[i2].renamed >= 0) {
			i2 = features[i2].renamed;
		}

		for (size_t j = 0; j < features[i].arc_polygon.size() && merged < merge_count; j++) {
			if (features[i].arc_polygon[j] == order[o].edge) {
				{
					// XXX snap links
					if (features[order[o].p2].arc_polygon.size() > 0) {
						// This has to merge the ring that contains the anti-arc to this arc
						// into the current ring, and then add whatever other rings were in
						// that feature on to the end.
						//
						// This can't be good for keeping parent-child relationships among
						// the rings in order, but Wagyu should sort that out later

						std::vector<ssize_t> additions;
						std::vector<ssize_t> &here = features[i].arc_polygon;
						std::vector<ssize_t> &other = features[i2].arc_polygon;

#if 0
						printf("seeking %zd\n", features[i].arc_polygon[j]);
						printf("before: ");
						for (size_t k = 0; k < here.size(); k++) {
							printf("%zd ", here[k]);
						}
						printf("\n");
						printf("other: ");
						for (size_t k = 0; k < other.size(); k++) {
							printf("%zd ", other[k]);
						}
						printf("\n");
#endif

						for (size_t k = 0; k < other.size(); k++) {
							size_t l;
							for (l = k; l < other.size(); l++) {
								if (other[l] == 0) {
									break;
								}
							}
							if (l >= other.size()) {
								l--;
							}

#if 0
							for (size_t m = k; m <= l; m++) {
								printf("%zd ", other[m]);
							}
							printf("\n");
#endif

							size_t m;
							for (m = k; m <= l; m++) {
								if (other[m] == -features[i].arc_polygon[j]) {
									break;
								}
							}

							if (m <= l) {
								// Found the shared arc

								here.erase(here.begin() + j);

								size_t off = 0;
								for (size_t n = m + 1; n < l; n++) {
									here.insert(here.begin() + j + off, other[n]);
									off++;
								}
								for (size_t n = k; n < m; n++) {
									here.insert(here.begin() + j + off, other[n]);
									off++;
								}
							} else {
								// Looking at some other ring

								for (size_t n = k; n <= l; n++) {
									additions.push_back(other[n]);
								}
							}

							k = l;
						}

						features[i2].arc_polygon.clear();
						features[i2].renamed = i;
						merged++;

						for (size_t k = 0; k < additions.size(); k++) {
							features[i].arc_polygon.push_back(additions[k]);
						}

#if 0
						printf("after: ");
						for (size_t k = 0; k < here.size(); k++) {
							printf("%zd ", here[k]);
						}
						printf("\n");
#endif

#if 0
						for (size_t k = 0; k + 1 < here.size(); k++) {
							if (here[k] != 0 && here[k + 1] != 0) {
								if (simplified_arcs[here[k + 1]][0] != simplified_arcs[here[k]][simplified_arcs[here[k]].size() - 1]) {
									printf("error from %zd to %zd\n", here[k], here[k + 1]);
								}
							}
						}
#endif
					}
				}
			}
		}
	}

	// Turn the arc representations of the polygons back into standard polygon geometries

	for (size_t i = 0; i < features.size(); i++) {
		if (features[i].t == VT_POLYGON) {
			features[i].geometry.clear();
			bool at_start = true;
			draw first(-1, 0, 0);

			for (size_t j = 0; j < features[i].arc_polygon.size(); j++) {
				ssize_t p = features[i].arc_polygon[j];

				if (p == 0) {
					if (first.op >= 0) {
						features[i].geometry.push_back(first);
						first = draw(-1, 0, 0);
					}
					at_start = true;
				} else if (p > 0) {
					for (size_t k = 0; k + 1 < simplified_arcs[p].size(); k++) {
						if (at_start) {
							features[i].geometry.push_back(draw(VT_MOVETO, simplified_arcs[p][k].x, simplified_arcs[p][k].y));
							first = draw(VT_LINETO, simplified_arcs[p][k].x, simplified_arcs[p][k].y);
						} else {
							features[i].geometry.push_back(draw(VT_LINETO, simplified_arcs[p][k].x, simplified_arcs[p][k].y));
						}
						at_start = 0;
					}
				} else { /* p < 0 */
					for (ssize_t k = simplified_arcs[-p].size() - 1; k > 0; k--) {
						if (at_start) {
							features[i].geometry.push_back(draw(VT_MOVETO, simplified_arcs[-p][k].x, simplified_arcs[-p][k].y));
							first = draw(VT_LINETO, simplified_arcs[-p][k].x, simplified_arcs[-p][k].y);
						} else {
							features[i].geometry.push_back(draw(VT_LINETO, simplified_arcs[-p][k].x, simplified_arcs[-p][k].y));
						}
						at_start = 0;
					}
				}
			}
		}
	}

	if (merged >= merge_count) {
		return true;
	} else {
		return false;
	}
}

unsigned long long choose_mingap(std::vector<unsigned long long> const &indices, double f) {
	unsigned long long bot = ULLONG_MAX;
	unsigned long long top = 0;

	for (size_t i = 0; i < indices.size(); i++) {
		if (i > 0 && indices[i] >= indices[i - 1]) {
			if (indices[i] - indices[i - 1] > top) {
				top = indices[i] - indices[i - 1];
			}
			if (indices[i] - indices[i - 1] < bot) {
				bot = indices[i] - indices[i - 1];
			}
		}
	}

	size_t want = indices.size() * f;
	while (top - bot > 2) {
		unsigned long long guess = bot / 2 + top / 2;
		size_t count = 0;
		unsigned long long prev = 0;

		for (size_t i = 0; i < indices.size(); i++) {
			if (indices[i] - prev >= guess) {
				count++;
				prev = indices[i];
			}
		}

		if (count > want) {
			bot = guess;
		} else if (count < want) {
			top = guess;
		} else {
			return guess;
		}
	}

	return top;
}

// This function is called to choose the new "extent" threshold to try when a tile exceeds the
// tile size limit or feature limit and `--drop-smallest-as-needed` or `--coalesce-smallest-as-needed`
// has been set.
//
// The "extents" are the areas of the polygon features or the pseudo-areas associated with the
// linestring or point features that were examined for inclusion in the most recent
// iteration of this tile. (This includes features that were dropped because they were below
// the previous size threshold, but not features that were dropped by fractional point dropping).
// The extents are placed in order by the sort, from smallest to largest.
//
// The `fraction` is the proportion of these features that tippecanoe thinks should be retained to
// to make the tile small enough now. Because the extents are sorted from smallest to largest,
// the smallest extent threshold that will retain that fraction of features is found `fraction`
// distance from the end of the list, or at element `(1 - fraction) * (size() - 1)`.
//
// However, the extent found there may be the same extent that was used in the last iteration!
//
// (The "existing_extent" is the extent threshold that selected these features in the recent
// iteration. It is 0 the first time a tile is attempted, and gets higher on successive iterations
// as tippecanoe restricts the features to be kept to larger and larger features.)
//
// The features that are kept are those with a size >= the existing_extent, so if there are a large
// number of features with identical small areas, the new guess may not exclude enough features
// to actually choose a new threshold larger than the previous threshold.
//
// To address this, the array index `ix` of the new chosen extent is incremented toward the end
// of the list, until the possibilities run out or something higher than the old extent is found.
// If there are no higher extents available, the tile has already been reduced as much as possible
// and tippecanoe will exit with an error.

long long choose_minextent(std::vector<long long> &extents, double f, long long existing_extent) {
	std::sort(extents.begin(), extents.end());

	size_t ix = (extents.size() - 1) * (1 - f);
	while (ix + 1 < extents.size() && extents[ix] == existing_extent) {
		ix++;
	}

	return extents[ix];
}

struct write_tile_args {
	struct task *tasks = NULL;
	char *stringpool = NULL;
	int min_detail = 0;
	sqlite3 *outdb = NULL;
	const char *outdir = NULL;
	int buffer = 0;
	const char *fname = NULL;
	compressor **geomfile = NULL;
	double todo = 0;
	std::atomic<long long> *along = NULL;
	double gamma = 0;
	double gamma_out = 0;
	int child_shards = 0;
	int *geomfd = NULL;
	off_t *geom_size = NULL;
	std::atomic<unsigned> *midx = NULL;
	std::atomic<unsigned> *midy = NULL;
	int maxzoom = 0;
	int minzoom = 0;
	int basezoom = 0;
	double droprate = 0;
	int full_detail = 0;
	int low_detail = 0;
	double simplification = 0;
	std::atomic<long long> *most = NULL;
	long long *pool_off = NULL;
	unsigned *initial_x = NULL;
	unsigned *initial_y = NULL;
	std::atomic<int> *running = NULL;
	int err = 0;
	std::vector<std::map<std::string, layermap_entry>> *layermaps = NULL;
	std::vector<std::vector<std::string>> *layer_unmaps = NULL;
	size_t pass = 0;
	unsigned long long mingap = 0;
	unsigned long long mingap_out = 0;
	long long minextent = 0;
	long long minextent_out = 0;
	double fraction = 0;
	double fraction_out = 0;
	size_t tile_size_out = 0;
	size_t feature_count_out = 0;
	const char *prefilter = NULL;
	const char *postfilter = NULL;
	std::unordered_map<std::string, attribute_op> const *attribute_accum = NULL;
	bool still_dropping = false;
	int wrote_zoom = 0;
	size_t tiling_seg = 0;
	json_object *filter = NULL;
	std::atomic<size_t> *dropped_count = NULL;
	atomic_strategy *strategy = NULL;
	int zoom = -1;
	bool compressed;
	node *shared_nodes_map;
	size_t nodepos;
};

bool clip_to_tile(serial_feature &sf, int z, long long buffer) {
	int quick = quick_check(sf.bbox, z, buffer);

	if (z == 0) {
		if (sf.bbox[0] <= (1LL << 32) * buffer / 256 || sf.bbox[2] >= (1LL << 32) - ((1LL << 32) * buffer / 256)) {
			// If the geometry extends off the edge of the world, concatenate on another copy
			// shifted by 360 degrees, and then make sure both copies get clipped down to size.

			size_t n = sf.geometry.size();

			if (sf.bbox[0] <= (1LL << 32) * buffer / 256) {
				for (size_t i = 0; i < n; i++) {
					sf.geometry.push_back(draw(sf.geometry[i].op, sf.geometry[i].x + (1LL << 32), sf.geometry[i].y));
				}
			}

			if (sf.bbox[2] >= (1LL << 32) - ((1LL << 32) * buffer / 256)) {
				for (size_t i = 0; i < n; i++) {
					sf.geometry.push_back(draw(sf.geometry[i].op, sf.geometry[i].x - (1LL << 32), sf.geometry[i].y));
				}
			}

			sf.bbox[0] = 0;
			sf.bbox[2] = 1LL << 32;

			quick = -1;
		}
	}

	if (quick == 0) {  // entirely outside the tile
		return true;
	}

	// if quick == 3 the feature touches the buffer, not just the tile proper,
	// so we need to clip to add intersection points at the tile edge.

	// if quick == 2 it touches the buffer and beyond, so likewise

	// if quick == 1 we should be able to get away without clipping, because
	// the feature is entirely within the tile proper.

	// Can't accept the quick check if guaranteeing no duplication, since the
	// overlap might have been in the buffer.
	if (quick != 1 || prevent[P_DUPLICATION]) {
		drawvec clipped;

		// Do the clipping, even if we are going to include the whole feature,
		// so that we can know whether the feature itself, or only the feature's
		// bounding box, touches the tile.

		if (sf.t == VT_LINE) {
			clipped = clip_lines(sf.geometry, z, buffer);
		}
		if (sf.t == VT_POLYGON) {
			clipped = simple_clip_poly(sf.geometry, z, buffer, sf.edge_nodes, prevent[P_SIMPLIFY_SHARED_NODES]);
		}
		if (sf.t == VT_POINT) {
			clipped = clip_point(sf.geometry, z, buffer);
		}

		clipped = remove_noop(clipped, sf.t, 0);

		// Must clip at z0 even if we don't want clipping, to handle features
		// that are duplicated across the date line

		if (prevent[P_DUPLICATION] && z != 0) {
			if (point_within_tile((sf.bbox[0] + sf.bbox[2]) / 2, (sf.bbox[1] + sf.bbox[3]) / 2, z)) {
				// sf.geometry is unchanged
			} else {
				sf.geometry.clear();
			}
		} else if (prevent[P_CLIPPING] && z != 0) {
			if (clipped.size() == 0) {
				sf.geometry.clear();
			} else {
				// sf.geometry is unchanged
			}
		} else {
			sf.geometry = clipped;
		}
	}

	return false;
}

void remove_attributes(serial_feature &sf, std::set<std::string> const &exclude_attributes, const char *stringpool, long long *pool_off) {
	for (ssize_t i = sf.keys.size() - 1; i >= 0; i--) {
		std::string key = stringpool + pool_off[sf.segment] + sf.keys[i] + 1;
		if (exclude_attributes.count(key) > 0) {
			sf.keys.erase(sf.keys.begin() + i);
			sf.values.erase(sf.values.begin() + i);
		}
	}

	for (ssize_t i = sf.full_keys.size() - 1; i >= 0; i--) {
		std::string key = sf.full_keys[i];
		if (exclude_attributes.count(key) > 0) {
			sf.full_keys.erase(sf.full_keys.begin() + i);
			sf.full_values.erase(sf.full_values.begin() + i);
		}
	}
}

struct multiplier_state {
	std::map<std::string, size_t> count;
};

serial_feature next_feature(decompressor *geoms, std::atomic<long long> *geompos_in, int z, unsigned tx, unsigned ty, unsigned *initial_x, unsigned *initial_y, long long *original_features, long long *unclipped_features, int nextzoom, int maxzoom, int minzoom, int max_zoom_increment, size_t pass, std::atomic<long long> *along, long long alongminus, int buffer, int *within, compressor **geomfile, std::atomic<long long> *geompos, std::atomic<double> *oprogress, double todo, const char *fname, int child_shards, json_object *filter, const char *stringpool, long long *pool_off, std::vector<std::vector<std::string>> *layer_unmaps, bool first_time, bool compressed, multiplier_state *multiplier_state, std::shared_ptr<std::string> &tile_stringpool) {
	while (1) {
		serial_feature sf;
		std::string s;
		long long len;

		if (geoms->deserialize_long_long(&len, geompos_in) == 0) {
			fprintf(stderr, "Unexpected physical EOF in feature stream\n");
			exit(EXIT_READ);
		}
		if (len == 0) {
			if (compressed) {
				geoms->end(geompos_in);
			}

			sf.t = -2;
			return sf;
		}

		s.resize(std::abs(len));
		size_t n = geoms->fread((void *) s.c_str(), sizeof(char), s.size(), geompos_in);
		if (n != s.size()) {
			fprintf(stderr, "Short read (%zu for %zu) from geometry\n", n, s.size());
			exit(EXIT_READ);
		}

		sf = deserialize_feature(s, z, tx, ty, initial_x, initial_y);

		size_t passes = pass + 1;
		double progress = floor(((((*geompos_in + *along - alongminus) / (double) todo) + pass) / passes + z) / (maxzoom + 1) * 1000) / 10;
		if (progress >= *oprogress + 0.1) {
			if (!quiet && !quiet_progress && progress_time()) {
				fprintf(stderr, "  %3.1f%%  %d/%u/%u  \r", progress, z, tx, ty);
				fflush(stderr);
			}
			if (logger.json_enabled && progress_time()) {
				logger.progress_tile(progress);
			}
			*oprogress = progress;
		}

		(*original_features)++;

		if (clip_to_tile(sf, z, buffer)) {
			continue;
		}

		if (sf.geometry.size() > 0) {
			(*unclipped_features)++;
		}

		if (first_time && pass == 0) { /* only write out the next zoom once, even if we retry */
			if (sf.tippecanoe_maxzoom == -1 || sf.tippecanoe_maxzoom >= nextzoom) {
				rewrite(sf, sf.geometry, z, nextzoom, maxzoom, sf.bbox, tx, ty, buffer, within, geompos, geomfile, fname, child_shards, max_zoom_increment, sf.segment, initial_x, initial_y);
			}
		}

		if (z < minzoom) {
			continue;
		}

		if (sf.tippecanoe_minzoom != -1 && z < sf.tippecanoe_minzoom) {
			continue;
		}
		if (sf.tippecanoe_maxzoom != -1 && z > sf.tippecanoe_maxzoom) {
			continue;
		}

		if (filter != NULL) {
			std::unordered_map<std::string, mvt_value> attributes;
			std::string layername = (*layer_unmaps)[sf.segment][sf.layer];
			std::set<std::string> exclude_attributes;

			for (size_t i = 0; i < sf.keys.size(); i++) {
				std::string key = stringpool + pool_off[sf.segment] + sf.keys[i] + 1;

				serial_val sv;
				sv.type = (stringpool + pool_off[sf.segment])[sf.values[i]];
				sv.s = stringpool + pool_off[sf.segment] + sf.values[i] + 1;

				mvt_value val = stringified_to_mvt_value(sv.type, sv.s.c_str(), tile_stringpool);
				attributes.insert(std::pair<std::string, mvt_value>(key, val));
			}

			for (size_t i = 0; i < sf.full_keys.size(); i++) {
				std::string key = sf.full_keys[i];
				mvt_value val = stringified_to_mvt_value(sf.full_values[i].type, sf.full_values[i].s.c_str(), tile_stringpool);

				attributes.insert(std::pair<std::string, mvt_value>(key, val));
			}

			if (sf.has_id) {
				mvt_value v;
				v.type = mvt_uint;
				v.numeric_value.uint_value = sf.id;

				attributes.insert(std::pair<std::string, mvt_value>("$id", v));
			}

			mvt_value v;
			v.type = mvt_string;

			if (sf.t == mvt_point) {
				v.set_string_value("Point");
			} else if (sf.t == mvt_linestring) {
				v.set_string_value("LineString");
			} else if (sf.t == mvt_polygon) {
				v.set_string_value("Polygon");
			}

			attributes.insert(std::pair<std::string, mvt_value>("$type", v));

			mvt_value v2;
			v2.type = mvt_uint;
			v2.numeric_value.uint_value = z;

			attributes.insert(std::pair<std::string, mvt_value>("$zoom", v2));

			if (!evaluate(attributes, layername, filter, exclude_attributes)) {
				continue;
			}

			if (exclude_attributes.size() > 0) {
				remove_attributes(sf, exclude_attributes, stringpool, pool_off);
			}
		}

		if (sf.tippecanoe_minzoom == -1) {
			bool keep = false;

			std::string layername = (*layer_unmaps)[sf.segment][sf.layer];
			auto count = multiplier_state->count.find(layername);
			if (count == multiplier_state->count.end()) {
				multiplier_state->count.emplace(layername, 0);
				count = multiplier_state->count.find(layername);
				keep = true;  // the first feature in each layer in each tile is always kept
			}

			sf.dropped = true;

			if (z >= sf.feature_minzoom || keep) {
				count->second = retain_points_multiplier;

				if (retain_points_multiplier > 1) {
					serial_val val;
					val.type = mvt_bool;
					val.s = "true";

					sf.full_keys.push_back("tippecanoe:retain_points_multiplier_first");
					sf.full_values.push_back(val);
				}
			}

			if (count->second > 0) {
				sf.dropped = false;
				count->second -= 1;
			}
		}

		// Remove nulls, now that the expression evaluation filter has run

		for (ssize_t i = (ssize_t) sf.keys.size() - 1; i >= 0; i--) {
			int type = (stringpool + pool_off[sf.segment])[sf.values[i]];

			if (type == mvt_null) {
				sf.keys.erase(sf.keys.begin() + i);
				sf.values.erase(sf.values.begin() + i);
			}
		}

		for (ssize_t i = (ssize_t) sf.full_keys.size() - 1; i >= 0; i--) {
			if (sf.full_values[i].type == mvt_null) {
				sf.full_keys.erase(sf.full_keys.begin() + i);
				sf.full_values.erase(sf.full_values.begin() + i);
			}
		}

		return sf;
	}
}

struct run_prefilter_args {
	decompressor *geoms = NULL;
	std::atomic<long long> *geompos_in = NULL;
	int z = 0;
	unsigned tx = 0;
	unsigned ty = 0;
	unsigned *initial_x = 0;
	unsigned *initial_y = 0;
	long long *original_features = 0;
	long long *unclipped_features = 0;
	int nextzoom = 0;
	int maxzoom = 0;
	int minzoom = 0;
	int max_zoom_increment = 0;
	size_t pass = 0;
	std::atomic<long long> *along = 0;
	long long alongminus = 0;
	int buffer = 0;
	int *within = NULL;
	compressor **geomfile = NULL;
	std::atomic<long long> *geompos = NULL;
	std::atomic<double> *oprogress = NULL;
	double todo = 0;
	const char *fname = 0;
	int child_shards = 0;
	std::vector<std::vector<std::string>> *layer_unmaps = NULL;
	char *stringpool = NULL;
	long long *pool_off = NULL;
	FILE *prefilter_fp = NULL;
	json_object *filter = NULL;
	bool first_time = false;
	bool compressed = false;
};

void *run_prefilter(void *v) {
	run_prefilter_args *rpa = (run_prefilter_args *) v;
	json_writer state(rpa->prefilter_fp);
	struct multiplier_state multiplier_state;
	std::shared_ptr<std::string> tile_stringpool = std::make_shared<std::string>();

	while (1) {
		serial_feature sf = next_feature(rpa->geoms, rpa->geompos_in, rpa->z, rpa->tx, rpa->ty, rpa->initial_x, rpa->initial_y, rpa->original_features, rpa->unclipped_features, rpa->nextzoom, rpa->maxzoom, rpa->minzoom, rpa->max_zoom_increment, rpa->pass, rpa->along, rpa->alongminus, rpa->buffer, rpa->within, rpa->geomfile, rpa->geompos, rpa->oprogress, rpa->todo, rpa->fname, rpa->child_shards, rpa->filter, rpa->stringpool, rpa->pool_off, rpa->layer_unmaps, rpa->first_time, rpa->compressed, &multiplier_state, tile_stringpool);
		if (sf.t < 0) {
			break;
		}

		mvt_layer tmp_layer;
		tmp_layer.extent = 1LL << 32;
		tmp_layer.name = (*(rpa->layer_unmaps))[sf.segment][sf.layer];

		if (sf.t == VT_POLYGON) {
			sf.geometry = close_poly(sf.geometry);
		}

		mvt_feature tmp_feature;
		tmp_feature.type = sf.t;
		tmp_feature.geometry = to_feature(sf.geometry);
		tmp_feature.id = sf.id;
		tmp_feature.has_id = sf.has_id;
		tmp_feature.dropped = sf.dropped;

		// Offset from tile coordinates back to world coordinates
		unsigned sx = 0, sy = 0;
		if (rpa->z != 0) {
			sx = rpa->tx << (32 - rpa->z);
			sy = rpa->ty << (32 - rpa->z);
		}
		for (size_t i = 0; i < tmp_feature.geometry.size(); i++) {
			tmp_feature.geometry[i].x += sx;
			tmp_feature.geometry[i].y += sy;
		}

		decode_meta(sf.keys, sf.values, rpa->stringpool + rpa->pool_off[sf.segment], tmp_layer, tmp_feature, tile_stringpool);
		tmp_layer.features.push_back(tmp_feature);

		layer_to_geojson(tmp_layer, 0, 0, 0, false, true, false, true, sf.index, sf.seq, sf.extent, true, state, 0);
	}

	if (fclose(rpa->prefilter_fp) != 0) {
		if (errno == EPIPE) {
			static bool warned = false;
			if (!warned) {
				fprintf(stderr, "Warning: broken pipe in prefilter\n");
				warned = true;
			}
		} else {
			perror("fclose output to prefilter");
			exit(EXIT_CLOSE);
		}
	}
	return NULL;
}

void add_tilestats(std::string const &layername, int z, std::vector<std::map<std::string, layermap_entry>> *layermaps, size_t tiling_seg, std::vector<std::vector<std::string>> *layer_unmaps, std::string const &key, serial_val const &val) {
	std::map<std::string, layermap_entry> &layermap = (*layermaps)[tiling_seg];
	if (layermap.count(layername) == 0) {
		layermap_entry lme = layermap_entry(layermap.size());
		lme.minzoom = z;
		lme.maxzoom = z;
		lme.retain = 1;

		layermap.insert(std::pair<std::string, layermap_entry>(layername, lme));

		if (lme.id >= (*layer_unmaps)[tiling_seg].size()) {
			(*layer_unmaps)[tiling_seg].resize(lme.id + 1);
			(*layer_unmaps)[tiling_seg][lme.id] = layername;
		}
	}
	auto ts = layermap.find(layername);
	if (ts == layermap.end()) {
		fprintf(stderr, "Internal error: layer %s not found\n", layername.c_str());
		exit(EXIT_IMPOSSIBLE);
	}

	add_to_tilestats(ts->second.tilestats, key, val);
}

void promote_attribute(std::string const &key, serial_feature &p, char *stringpool, long long *pool_off) {
	if (p.need_tilestats.count(key) == 0) {
		p.need_tilestats.insert(key);
	}

	// If the feature being merged into has this key as a metadata reference,
	// promote it to a full_key so it can be modified

	for (size_t i = 0; i < p.keys.size(); i++) {
		if (strcmp(key.c_str(), stringpool + pool_off[p.segment] + p.keys[i] + 1) == 0) {
			serial_val sv;
			sv.s = stringpool + pool_off[p.segment] + p.values[i] + 1;
			sv.type = (stringpool + pool_off[p.segment])[p.values[i]];

			p.full_keys.push_back(key);
			p.full_values.push_back(sv);

			p.keys.erase(p.keys.begin() + i);
			p.values.erase(p.values.begin() + i);

			break;
		}
	}
}

void preserve_attributes(std::unordered_map<std::string, attribute_op> const *attribute_accum, serial_feature &sf, char *stringpool, long long *pool_off, serial_feature &p) {
	for (size_t i = 0; i < sf.keys.size(); i++) {
		std::string key = stringpool + pool_off[sf.segment] + sf.keys[i] + 1;

		serial_val sv;
		sv.type = (stringpool + pool_off[sf.segment])[sf.values[i]];
		sv.s = stringpool + pool_off[sf.segment] + sf.values[i] + 1;

		auto f = attribute_accum->find(key);
		if (f != attribute_accum->end()) {
			promote_attribute(key, p, stringpool, pool_off);
			preserve_attribute(f->second, key, sv, p.full_keys, p.full_values, p.attribute_accum_state);
		}
	}
	for (size_t i = 0; i < sf.full_keys.size(); i++) {
		std::string key = sf.full_keys[i];
		serial_val sv = sf.full_values[i];

		auto f = attribute_accum->find(key);
		if (f != attribute_accum->end()) {
			promote_attribute(key, p, stringpool, pool_off);
			preserve_attribute(f->second, key, sv, p.full_keys, p.full_values, p.attribute_accum_state);
		}
	}
}

// This function finds the feature in `features` onto which the attributes or geometry
// of a feature that is being dropped (`sf`) will be accumulated or coalesced. It
// ordinarily returns the most recently-added feature from the same layer as the feature
// that is being dropped, but if there is an active multiplier, will walk multiple
// features backward so that the features being dropped will be accumulated round-robin
// onto the N features that are being kept. The caller increments the `multiplier_seq`
// mod N with each dropped feature to drive the round-robin decision.
//
bool find_feature_to_accumulate_onto(std::vector<serial_feature> &features, serial_feature &sf, ssize_t &out, std::vector<std::vector<std::string>> *layer_unmaps, long long maxextent, ssize_t multiplier_seq) {
	for (size_t i = features.size(); i > 0; i--) {
		if (features[i - 1].t == sf.t) {
			std::string &layername1 = (*layer_unmaps)[features[i - 1].segment][features[i - 1].layer];
			std::string &layername2 = (*layer_unmaps)[sf.segment][sf.layer];

			if (layername1 == layername2 && features[i - 1].extent <= maxextent) {
				if (multiplier_seq <= 0) {
					out = i - 1;
					return true;
				}

				multiplier_seq--;
			}
		}
	}

	return false;
}

static bool line_is_too_small(drawvec const &geometry, int z, int detail) {
	if (geometry.size() == 0) {
		return true;
	}

	long long x = std::round((double) geometry[0].x / (1LL << (32 - detail - z)));
	long long y = std::round((double) geometry[0].y / (1LL << (32 - detail - z)));

	for (auto &g : geometry) {
		long long xx = std::round((double) g.x / (1LL << (32 - detail - z)));
		long long yy = std::round((double) g.y / (1LL << (32 - detail - z)));

		if (xx != x || yy != y) {
			return false;
		}
	}

	return true;
}

// Keep only a sample of 100K extents for feature dropping,
// to avoid spending lots of memory on a complete list when there are
// hundreds of millions of features.
template <class T>
void add_sample_to(std::vector<T> &vals, T val, size_t &increment, size_t seq) {
	if (seq % increment == 0) {
		vals.push_back(val);

		if (vals.size() > 100000) {
			std::vector<T> tmp;

			for (size_t i = 0; i < vals.size(); i += 2) {
				tmp.push_back(vals[i]);
			}

			increment *= 2;
			vals = tmp;
		}
	}
}

void coalesce_geometry(serial_feature &p, serial_feature &sf) {
	// XXX need another way to deduplicate here
#if 0
	// if the geometry being coalesced on is an exact duplicate
	// of an existing geometry, just drop it

	for (size_t i = 0; i < p.geometries.size(); i++) {
		if (p.geometries[i] == sf.geometry) {
			return;
		}
	}
#endif

	size_t s = p.geometry.size();
	p.geometry.resize(s + sf.geometry.size());
	for (size_t i = 0; i < sf.geometry.size(); i++) {
		p.geometry[s + i] = sf.geometry[i];
	}
}

long long write_tile(decompressor *geoms, std::atomic<long long> *geompos_in, char *stringpool, int z, const unsigned tx, const unsigned ty, const int detail, int min_detail, sqlite3 *outdb, const char *outdir, int buffer, const char *fname, compressor **geomfile, int minzoom, int maxzoom, double todo, std::atomic<long long> *along, long long alongminus, double gamma, int child_shards, long long *pool_off, unsigned *initial_x, unsigned *initial_y, std::atomic<int> *running, double simplification, std::vector<std::map<std::string, layermap_entry>> *layermaps, std::vector<std::vector<std::string>> *layer_unmaps, size_t tiling_seg, size_t pass, unsigned long long mingap, long long minextent, double fraction, const char *prefilter, const char *postfilter, json_object *filter, write_tile_args *arg, atomic_strategy *strategy, bool compressed_input, node *shared_nodes_map, size_t nodepos) {
	double merge_fraction = 1;
	double mingap_fraction = 1;
	double minextent_fraction = 1;

	// allow larger tile sizes at low zooms when the retain-points-multiplier
	// is intended to allow more points through. scale back down toward a
	// tile size multiple of 1 at basezoom and beyond
	size_t scaled_max_tile_size = max_tile_size;
	double regular_retention = 1 / exp(log(arg->droprate) * (arg->basezoom - z));
	if (regular_retention > 1) {
		regular_retention = 1;
	}
	double multiplier_retention = 1 / exp(log(arg->droprate) * (arg->basezoom - z)) * retain_points_multiplier;
	if (multiplier_retention > 1) {
		multiplier_retention = 1;
	}
	scaled_max_tile_size *= multiplier_retention / regular_retention;

	static std::atomic<double> oprogress(0);
	long long og = *geompos_in;

	// XXX is there a way to do this without floating point?
	int max_zoom_increment = std::log(child_shards) / std::log(4);
	if (child_shards < 4 || max_zoom_increment < 1) {
		fprintf(stderr, "Internal error: %d shards, max zoom increment %d\n", child_shards, max_zoom_increment);
		exit(EXIT_IMPOSSIBLE);
	}
	if ((((child_shards - 1) << 1) & child_shards) != child_shards) {
		fprintf(stderr, "Internal error: %d shards not a power of 2\n", child_shards);
		exit(EXIT_IMPOSSIBLE);
	}

	int nextzoom = z + 1;
	if (nextzoom < minzoom) {
		if (z + max_zoom_increment > minzoom) {
			nextzoom = minzoom;
		} else {
			nextzoom = z + max_zoom_increment;
		}
	}

	bool first_time = true;
	// This only loops if the tile data didn't fit, in which case the detail
	// goes down and the progress indicator goes backward for the next try.
	int line_detail;
	for (line_detail = detail; line_detail >= min_detail || line_detail == detail; line_detail--, oprogress = 0) {
		long long count = 0;
		double accum_area = 0;

		double fraction_accum = 0;

		unsigned long long previndex = 0, density_previndex = 0, merge_previndex = 0;
		unsigned long long extent_previndex = 0;
		double scale = (double) (1LL << (64 - 2 * (z + 8)));
		double gap = 0, density_gap = 0;
		double spacing = 0;

		long long original_features = 0;
		long long unclipped_features = 0;

		std::vector<serial_feature> features;
		std::map<std::string, std::vector<serial_feature>> layers;

		std::vector<unsigned long long> indices;
		std::vector<long long> extents;
		size_t extents_increment = 1;

		double coalesced_area = 0;
		drawvec shared_nodes;

		int tile_detail = line_detail;
		size_t skipped = 0;
		size_t kept = 0;

		size_t unsimplified_geometry_size = 0;
		size_t simplified_geometry_through = 0;

		int within[child_shards];
		std::atomic<long long> geompos[child_shards];
		for (size_t i = 0; i < (size_t) child_shards; i++) {
			geompos[i] = 0;
			within[i] = 0;
		}

		std::shared_ptr<std::string> tile_stringpool = std::make_shared<std::string>();

		if (*geompos_in != og) {
			if (compressed_input) {
				if (geoms->within) {
					geoms->end(geompos_in);
				}

				geoms->begin();
			}

			if (fseek(geoms->fp, og, SEEK_SET) != 0) {
				perror("fseek geom");
				exit(EXIT_SEEK);
			}

			*geompos_in = og;
			geoms->zs.avail_in = 0;
			geoms->zs.avail_out = 0;
		}

		int prefilter_write = -1, prefilter_read = -1;
		pid_t prefilter_pid = 0;
		FILE *prefilter_fp = NULL;
		pthread_t prefilter_writer;
		run_prefilter_args rpa;	 // here so it stays in scope until joined
		FILE *prefilter_read_fp = NULL;
		json_pull *prefilter_jp = NULL;

		serial_feature tiny_feature;  // used to track which feature currently represents the dust

		if (z < minzoom) {
			prefilter = NULL;
			postfilter = NULL;
		}

		if (prefilter != NULL) {
			setup_filter(prefilter, &prefilter_write, &prefilter_read, &prefilter_pid, z, tx, ty);
			prefilter_fp = fdopen(prefilter_write, "w");
			if (prefilter_fp == NULL) {
				perror("freopen prefilter");
				exit(EXIT_OPEN);
			}

			rpa.geoms = geoms;
			rpa.geompos_in = geompos_in;
			rpa.z = z;
			rpa.tx = tx;
			rpa.ty = ty;
			rpa.initial_x = initial_x;
			rpa.initial_y = initial_y;
			rpa.original_features = &original_features;
			rpa.unclipped_features = &unclipped_features;
			rpa.nextzoom = nextzoom;
			rpa.maxzoom = maxzoom;
			rpa.minzoom = minzoom;
			rpa.max_zoom_increment = max_zoom_increment;
			rpa.pass = pass;
			rpa.along = along;
			rpa.alongminus = alongminus;
			rpa.buffer = buffer;
			rpa.within = within;
			rpa.geomfile = geomfile;
			rpa.geompos = geompos;
			rpa.oprogress = &oprogress;
			rpa.todo = todo;
			rpa.fname = fname;
			rpa.child_shards = child_shards;
			rpa.prefilter_fp = prefilter_fp;
			rpa.layer_unmaps = layer_unmaps;
			rpa.stringpool = stringpool;
			rpa.pool_off = pool_off;
			rpa.filter = filter;
			rpa.first_time = first_time;
			rpa.compressed = compressed_input;

			if (thread_create(&prefilter_writer, NULL, run_prefilter, &rpa) != 0) {
				perror("pthread_create (prefilter writer)");
				exit(EXIT_PTHREAD);
			}

			prefilter_read_fp = fdopen(prefilter_read, "r");
			if (prefilter_read_fp == NULL) {
				perror("fdopen prefilter output");
				exit(EXIT_OPEN);
			}
			prefilter_jp = json_begin_file(prefilter_read_fp);
		}

		struct multiplier_state multiplier_state;
		size_t multiplier_seq = retain_points_multiplier - 1;
		for (size_t seq = 0;; seq++) {
			serial_feature sf;
			ssize_t which_serial_feature = -1;

			if (prefilter == NULL) {
				sf = next_feature(geoms, geompos_in, z, tx, ty, initial_x, initial_y, &original_features, &unclipped_features, nextzoom, maxzoom, minzoom, max_zoom_increment, pass, along, alongminus, buffer, within, geomfile, geompos, &oprogress, todo, fname, child_shards, filter, stringpool, pool_off, layer_unmaps, first_time, compressed_input, &multiplier_state, tile_stringpool);
			} else {
				sf = parse_feature(prefilter_jp, z, tx, ty, layermaps, tiling_seg, layer_unmaps, postfilter != NULL);
			}

			if (sf.t < 0) {
				break;
			}

			if (sf.t == VT_POINT) {
				if (extent_previndex >= sf.index) {
					sf.extent = 1;
				} else {
					double radius = sqrt(sf.index - extent_previndex) / 4.0;
					sf.extent = M_PI * radius * radius;
					if (sf.extent < 1) {
						sf.extent = 1;
					}
				}

				extent_previndex = sf.index;
			}

			if (sf.dropped) {
				multiplier_seq = (multiplier_seq + 1) % retain_points_multiplier;

				if (find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, LLONG_MAX, multiplier_seq)) {
					preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
					strategy->dropped_by_rate++;
					continue;
				}
			} else {
				multiplier_seq = retain_points_multiplier - 1;
			}

			if (gamma > 0) {
				if (manage_gap(sf.index, &previndex, scale, gamma, &gap) && find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, LLONG_MAX, multiplier_seq)) {
					preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
					strategy->dropped_by_gamma++;
					continue;
				}
			}

			// Cap the indices, rather than sampling them like extents (areas),
			// because choose_mingap cares about the distance between *surviving*
			// features, not between *original* features, so we can't just store
			// gaps rather than indices to be able to downsample them fairly.
			// Hopefully the first 100K features in the tile are reasonably
			// representative of the other features in the tile.
			const size_t MAX_INDICES = 100000;

			if (z <= cluster_maxzoom && (additional[A_CLUSTER_DENSEST_AS_NEEDED] || cluster_distance != 0)) {
				if (indices.size() < MAX_INDICES) {
					indices.push_back(sf.index);
				}
				if ((sf.index < merge_previndex || sf.index - merge_previndex < mingap) && find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, LLONG_MAX, multiplier_seq)) {
					features[which_serial_feature].clustered++;

					if (features[which_serial_feature].t == VT_POINT &&
					    features[which_serial_feature].geometry.size() == 1 &&
					    sf.geometry.size() == 1) {
						double x = (double) features[which_serial_feature].geometry[0].x * features[which_serial_feature].clustered;
						double y = (double) features[which_serial_feature].geometry[0].y * features[which_serial_feature].clustered;
						x += sf.geometry[0].x;
						y += sf.geometry[0].y;
						features[which_serial_feature].geometry[0].x = x / (features[which_serial_feature].clustered + 1);
						features[which_serial_feature].geometry[0].y = y / (features[which_serial_feature].clustered + 1);
					}

					preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
					strategy->coalesced_as_needed++;
					continue;
				}
			} else if (additional[A_DROP_DENSEST_AS_NEEDED]) {
				if (indices.size() < MAX_INDICES) {
					indices.push_back(sf.index);
				}
				if (sf.index - merge_previndex < mingap && find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, LLONG_MAX, multiplier_seq)) {
					preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
					strategy->dropped_as_needed++;
					continue;
				}
			} else if (additional[A_COALESCE_DENSEST_AS_NEEDED]) {
				if (indices.size() < MAX_INDICES) {
					indices.push_back(sf.index);
				}
				if (sf.index - merge_previndex < mingap && find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, LLONG_MAX, multiplier_seq)) {
					coalesce_geometry(features[which_serial_feature], sf);
					features[which_serial_feature].coalesced = true;
					coalesced_area += sf.extent;
					preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
					strategy->coalesced_as_needed++;
					continue;
				}
			} else if (additional[A_DROP_SMALLEST_AS_NEEDED]) {
				add_sample_to(extents, sf.extent, extents_increment, seq);
				// search here is for LLONG_MAX, not minextent, because we are dropping features, not coalescing them,
				// so we shouldn't expect to find anything small that we can related this feature to.
				if (minextent != 0 && sf.extent + coalesced_area <= minextent && find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, LLONG_MAX, multiplier_seq)) {
					preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
					strategy->dropped_as_needed++;
					continue;
				}
			} else if (additional[A_COALESCE_SMALLEST_AS_NEEDED]) {
				add_sample_to(extents, sf.extent, extents_increment, seq);
				if (minextent != 0 && sf.extent + coalesced_area <= minextent && find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, minextent, multiplier_seq)) {
					coalesce_geometry(features[which_serial_feature], sf);
					features[which_serial_feature].coalesced = true;
					coalesced_area += sf.extent;
					preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
					strategy->coalesced_as_needed++;
					continue;
				}
			}

			if (additional[A_CALCULATE_FEATURE_DENSITY]) {
				// Gamma is always 1 for this calculation so there is a reasonable
				// interpretation when no features are being dropped.
				// The spacing is only calculated if a feature would be retained by
				// that standard, so that duplicates aren't reported as infinitely dense.

				double o_density_previndex = density_previndex;
				if (!manage_gap(sf.index, &density_previndex, scale, 1, &density_gap)) {
					spacing = (sf.index - o_density_previndex) / scale;
				}
			}

			fraction_accum += fraction;
			if (fraction_accum < 1 && find_feature_to_accumulate_onto(features, sf, which_serial_feature, layer_unmaps, LLONG_MAX, multiplier_seq)) {
				if (additional[A_COALESCE_FRACTION_AS_NEEDED]) {
					coalesce_geometry(features[which_serial_feature], sf);
					features[which_serial_feature].coalesced = true;
					coalesced_area += sf.extent;
					strategy->coalesced_as_needed++;
				} else {
					strategy->dropped_as_needed++;
				}
				preserve_attributes(arg->attribute_accum, sf, stringpool, pool_off, features[which_serial_feature]);
				continue;
			}
			fraction_accum -= 1;

			bool still_need_simplification_after_reduction = false;
			if (sf.t == VT_POLYGON) {
				bool simplified_away_by_reduction = false;

				bool prevent_tiny = prevent[P_TINY_POLYGON_REDUCTION] ||
						    (prevent[P_TINY_POLYGON_REDUCTION_AT_MAXZOOM] && z == maxzoom);
				if (!prevent_tiny && !additional[A_GRID_LOW_ZOOMS]) {
					sf.geometry = reduce_tiny_poly(sf.geometry, z, line_detail, &still_need_simplification_after_reduction, &simplified_away_by_reduction, &accum_area, &sf, &tiny_feature);
					if (simplified_away_by_reduction) {
						strategy->tiny_polygons++;
					}
					if (sf.geometry.size() == 0) {
						continue;
					}
				} else {
					still_need_simplification_after_reduction = true;  // reduction skipped, so always simplify
				}
			} else {
				still_need_simplification_after_reduction = true;  // not a polygon, so simplify
			}

			if (sf.t == VT_POLYGON || sf.t == VT_LINE) {
				if (line_is_too_small(sf.geometry, z, line_detail)) {
					continue;
				}
			}

			if (sf.geometry.size() > 0) {
				if (features.size() > scaled_max_tile_size) {
					// Even being maximally conservative, each feature is still going to be
					// at least one byte in the output tile, so this can't possibly work.
					skipped++;
				} else {
					kept++;

					for (auto &p : sf.edge_nodes) {
						shared_nodes.push_back(p);
					}

					serial_feature p = std::move(sf);
					p.reduced = !still_need_simplification_after_reduction;
					p.coalesced = false;
					p.z = z;
					p.tx = tx;
					p.ty = ty;
					p.line_detail = line_detail;
					p.extra_detail = line_detail;
					p.maxzoom = maxzoom;
					p.spacing = spacing;
					p.simplification = simplification;
					p.renamed = -1;
					p.clustered = 0;
					p.stringpool = stringpool + pool_off[sf.segment];
					p.tile_stringpool = tile_stringpool;

					if (line_detail == detail && extra_detail >= 0 && z == maxzoom) {
						p.extra_detail = extra_detail;
						// maximum allowed coordinate delta in geometries is 2^31 - 1
						// so we need to stay under that, including the buffer
						if (p.extra_detail >= 30 - z) {
							p.extra_detail = 30 - z;
						}
						tile_detail = p.extra_detail;
					}

					features.push_back(p);

					unsimplified_geometry_size += sf.geometry.size() * sizeof(draw);
					if (unsimplified_geometry_size > 10 * 1024 * 1024 && !additional[A_DETECT_SHARED_BORDERS]) {
						// we should be safe to simplify here with P_SIMPLIFY_SHARED_NODES, since they will
						// have been assembled globally, although that also means that simplification
						// may not be very effective for reducing memory usage.

						drawvec dv;

						for (; simplified_geometry_through < features.size(); simplified_geometry_through++) {
							simplify_serial_feature(&features[simplified_geometry_through], dv, shared_nodes_map, nodepos);

							if (features[simplified_geometry_through].t == VT_POLYGON) {
								drawvec to_clean;

								for (auto &g : features[simplified_geometry_through].geometry) {
									to_clean.push_back(g);
								}

								// don't scale up because this is still world coordinates
								to_clean = clean_or_clip_poly(to_clean, 0, 0, false, false);
								features[simplified_geometry_through].geometry = to_clean;
							}
						}

						unsimplified_geometry_size = 0;
					}
				}
			}

			merge_previndex = sf.index;
			coalesced_area = 0;
		}

		if (retain_points_multiplier > 1) {
			// mapping from input sequence to current sequence within this tile
			std::vector<std::pair<size_t, size_t>> feature_sequences;

			for (size_t i = 0; i < features.size(); i++) {
				feature_sequences.emplace_back(features[i].seq, i);
			}

			// tag each feature with its sequence number within the tile
			// if the tile were sorted by input order
			//
			// these will be smaller numbers, and avoid the problem of the
			// original sequence number varying based on how many reader threads
			// there were reading the input
			std::sort(feature_sequences.begin(), feature_sequences.end());
			for (size_t i = 0; i < feature_sequences.size(); i++) {
				size_t j = feature_sequences[i].second;

				serial_val val;
				val.type = mvt_double;
				val.s = std::to_string(i);

				features[j].full_keys.push_back("tippecanoe:retain_points_multiplier_sequence");
				features[j].full_values.push_back(val);
			}
		}

		std::sort(shared_nodes.begin(), shared_nodes.end());

		for (size_t i = 0; i < features.size(); i++) {
			serial_feature &p = features[i];

			if (p.clustered > 0) {
				std::string layername = (*layer_unmaps)[p.segment][p.layer];
				serial_val sv, sv2, sv3, sv4;
				long long point_count = p.clustered + 1;
				char abbrev[17];  // to_string(LLONG_MAX).length() / 1000 + 1;

				p.full_keys.push_back("clustered");
				sv.type = mvt_bool;
				sv.s = "true";
				p.full_values.push_back(sv);

				add_tilestats(layername, z, layermaps, tiling_seg, layer_unmaps, "clustered", sv);

				p.full_keys.push_back("point_count");
				sv2.type = mvt_double;
				sv2.s = std::to_string(point_count);
				p.full_values.push_back(sv2);

				add_tilestats(layername, z, layermaps, tiling_seg, layer_unmaps, "point_count", sv2);

				p.full_keys.push_back("sqrt_point_count");
				sv3.type = mvt_double;
				sv3.s = std::to_string(round(100 * sqrt(point_count)) / 100.0);
				p.full_values.push_back(sv3);

				add_tilestats(layername, z, layermaps, tiling_seg, layer_unmaps, "sqrt_point_count", sv3);

				p.full_keys.push_back("point_count_abbreviated");
				sv4.type = mvt_string;
				if (point_count >= 10000) {
					snprintf(abbrev, sizeof(abbrev), "%.0fk", point_count / 1000.0);
				} else if (point_count >= 1000) {
					snprintf(abbrev, sizeof(abbrev), "%.1fk", point_count / 1000.0);
				} else {
					snprintf(abbrev, sizeof(abbrev), "%lld", point_count);
				}
				sv4.s = abbrev;
				p.full_values.push_back(sv4);

				add_tilestats(layername, z, layermaps, tiling_seg, layer_unmaps, "point_count_abbreviated", sv4);
			}

			if (p.need_tilestats.size() > 0) {
				std::string layername = (*layer_unmaps)[p.segment][p.layer];

				for (size_t j = 0; j < p.full_keys.size(); j++) {
					if (p.need_tilestats.count(p.full_keys[j]) > 0) {
						add_tilestats(layername, z, layermaps, tiling_seg, layer_unmaps, p.full_keys[j], p.full_values[j]);
					}
				}
			}
		}

		if (prefilter != NULL) {
			json_end(prefilter_jp);
			if (fclose(prefilter_read_fp) != 0) {
				perror("close output from prefilter");
				exit(EXIT_CLOSE);
			}
			while (1) {
				int stat_loc;
				if (waitpid(prefilter_pid, &stat_loc, 0) < 0) {
					perror("waitpid for prefilter\n");
					exit(EXIT_PTHREAD);
				}
				if (WIFEXITED(stat_loc) || WIFSIGNALED(stat_loc)) {
					break;
				}
			}
			void *ret;
			if (pthread_join(prefilter_writer, &ret) != 0) {
				perror("pthread_join prefilter writer");
				exit(EXIT_PTHREAD);
			}
		}

		first_time = false;

		if (additional[A_DETECT_SHARED_BORDERS]) {
			find_common_edges(features, z, line_detail, simplification, maxzoom, merge_fraction);
		}

		int tasks = ceil((double) CPUS / *running);
		if (tasks < 1) {
			tasks = 1;
		}

		pthread_t pthreads[tasks];
		std::vector<simplification_worker_arg> args;
		args.resize(tasks);
		for (int i = 0; i < tasks; i++) {
			args[i].task = i;
			args[i].tasks = tasks;
			args[i].features = &features;
			args[i].shared_nodes = &shared_nodes;
			args[i].shared_nodes_map = shared_nodes_map;
			args[i].nodepos = nodepos;

			if (tasks > 1) {
				if (thread_create(&pthreads[i], NULL, simplification_worker, &args[i]) != 0) {
					perror("pthread_create");
					exit(EXIT_PTHREAD);
				}
			} else {
				simplification_worker(&args[i]);
			}
		}

		if (tasks > 1) {
			for (int i = 0; i < tasks; i++) {
				void *retval;

				if (pthread_join(pthreads[i], &retval) != 0) {
					perror("pthread_join");
				}
			}
		}

		std::reverse(features.begin(), features.end());
		for (ssize_t i = features.size() - 1; i >= 0; i--) {
			signed char t = features[i].t;

			{
				if (t == VT_POINT || draws_something(features[i].geometry)) {
					// printf("segment %d layer %lld is %s\n", features[i].segment, features[i].layer, (*layer_unmaps)[features[i].segment][features[i].layer].c_str());

					std::string layername = (*layer_unmaps)[features[i].segment][features[i].layer];
					if (layers.count(layername) == 0) {
						layers.insert(std::pair<std::string, std::vector<serial_feature>>(layername, std::vector<serial_feature>()));
					}

					auto l = layers.find(layername);
					if (l == layers.end()) {
						fprintf(stderr, "Internal error: couldn't find layer %s\n", layername.c_str());
						fprintf(stderr, "segment %d\n", features[i].segment);
						fprintf(stderr, "layer %lld\n", features[i].layer);
						exit(EXIT_IMPOSSIBLE);
					}
					features[i].coalesced = false;
					l->second.push_back(std::move(features[i]));
				}
			}
		}

		features.clear();

		int j;
		for (j = 0; j < child_shards; j++) {
			if (within[j]) {
				geomfile[j]->serialize_long_long(0, &geompos[j], fname);  // EOF
				geomfile[j]->end(&geompos[j], fname);
				within[j] = 0;
			}
		}

		for (auto layer_iterator = layers.begin(); layer_iterator != layers.end(); ++layer_iterator) {
			std::vector<serial_feature> &layer_features = layer_iterator->second;

			if (additional[A_REORDER]) {
				std::sort(layer_features.begin(), layer_features.end(), coalindexcmp_comparator());
			}

			if (additional[A_COALESCE]) {
				// coalesce adjacent identical features if requested

				size_t out = 0;
				if (layer_features.size() > 0) {
					out++;
				}

				for (size_t x = 1; x < layer_features.size(); x++) {
					size_t y = out - 1;

					if (out > 0 && coalcmp(&layer_features[x], &layer_features[y], tile_stringpool) == 0) {
						for (size_t g = 0; g < layer_features[x].geometry.size(); g++) {
							layer_features[y].geometry.push_back(layer_features[x].geometry[g]);
						}
						layer_features[y].coalesced = true;
					} else {
						layer_features[out++] = layer_features[x];
					}
				}

				layer_features.resize(out);
			}

			{
				// clean up coalesced linestrings by simplification
				// and coalesced polygons by cleaning
				//
				// then close polygons

				size_t out = 0;

				for (size_t x = 0; x < layer_features.size(); x++) {
					if (layer_features[x].coalesced && layer_features[x].t == VT_LINE) {
						layer_features[x].geometry = remove_noop(layer_features[x].geometry, layer_features[x].t, 0);
						if (!(prevent[P_SIMPLIFY] || (z == maxzoom && prevent[P_SIMPLIFY_LOW]))) {
							// XXX revisit: why does this not take zoom into account?
							layer_features[x].geometry = simplify_lines(layer_features[x].geometry, 32, 0, 0, 0,
												    !(prevent[P_CLIPPING] || prevent[P_DUPLICATION]), simplification, layer_features[x].t == VT_POLYGON ? 4 : 0, shared_nodes, NULL, 0);
						}
					}

					if (layer_features[x].t == VT_POLYGON) {
						if (layer_features[x].coalesced) {
							// we can try scaling up because this is tile coordinates
							layer_features[x].geometry = clean_or_clip_poly(layer_features[x].geometry, 0, 0, false, true);
						}

						layer_features[x].geometry = close_poly(layer_features[x].geometry);
					}

					if (layer_features[x].geometry.size() > 0) {
						layer_features[out++] = layer_features[x];
					}
				}

				layer_features.resize(out);
			}

			if (prevent[P_INPUT_ORDER]) {
				auto clustered = assemble_multiplier_clusters(layer_features);
				std::sort(clustered.begin(), clustered.end(), preservecmp);
				layer_features = disassemble_multiplier_clusters(clustered);
			}

			if (order_by.size() != 0) {
				auto clustered = assemble_multiplier_clusters(layer_features);
				std::sort(clustered.begin(), clustered.end(), ordercmp());
				layer_features = disassemble_multiplier_clusters(clustered);
			}

			if (z == maxzoom && limit_tile_feature_count_at_maxzoom != 0) {
				if (layer_features.size() > limit_tile_feature_count_at_maxzoom) {
					layer_features.resize(limit_tile_feature_count_at_maxzoom);
				}
			} else if (limit_tile_feature_count != 0) {
				if (layer_features.size() > limit_tile_feature_count) {
					layer_features.resize(limit_tile_feature_count);
				}
			}
		}

		mvt_tile tile;
		size_t totalsize = 0;

		for (auto layer_iterator = layers.begin(); layer_iterator != layers.end(); ++layer_iterator) {
			std::vector<serial_feature> &layer_features = layer_iterator->second;
			totalsize += layer_features.size();

			mvt_layer layer;
			layer.name = layer_iterator->first;
			layer.version = 2;
			layer.extent = 1 << tile_detail;

			std::reverse(layer_features.begin(), layer_features.end());
			for (ssize_t x = layer_features.size() - 1; x >= 0; x--) {
				mvt_feature feature;

				if (layer_features[x].t == VT_LINE || layer_features[x].t == VT_POLYGON) {
					layer_features[x].geometry = remove_noop(layer_features[x].geometry, layer_features[x].t, 0);
				}

				if (layer_features[x].geometry.size() == 0) {
					continue;
				}

				feature.type = layer_features[x].t;
				feature.geometry = to_feature(layer_features[x].geometry);
				count += layer_features[x].geometry.size();
				layer_features[x].geometry.clear();

				feature.id = layer_features[x].id;
				feature.has_id = layer_features[x].has_id;

				decode_meta(layer_features[x].keys, layer_features[x].values, layer_features[x].stringpool, layer, feature, tile_stringpool);
				for (size_t a = 0; a < layer_features[x].full_keys.size(); a++) {
					serial_val sv = layer_features[x].full_values[a];
					mvt_value v = stringified_to_mvt_value(sv.type, sv.s.c_str(), tile_stringpool);
					layer.tag(feature, layer_features[x].full_keys[a], v);
				}

				if (additional[A_CALCULATE_FEATURE_DENSITY]) {
					int glow = 255;
					if (layer_features[x].spacing > 0) {
						glow = (1 / layer_features[x].spacing);
						if (glow > 255) {
							glow = 255;
						}
					}

					mvt_value v;
					v.type = mvt_sint;
					v.numeric_value.sint_value = glow;
					layer.tag(feature, "tippecanoe_feature_density", v);

					serial_val sv;
					sv.type = mvt_double;
					sv.s = std::to_string(glow);

					add_tilestats(layer.name, z, layermaps, tiling_seg, layer_unmaps, "tippecanoe_feature_density", sv);
				}

				layer.features.push_back(feature);
				layer_features.erase(layer_features.begin() + x);
			}

			if (layer.features.size() > 0) {
				tile.layers.push_back(layer);
			}
		}

		if (postfilter != NULL) {
			tile.layers = filter_layers(postfilter, tile.layers, z, tx, ty, layermaps, tiling_seg, layer_unmaps, 1 << tile_detail);
		}

		if (z == 0 && unclipped_features < original_features / 2 && clipbboxes.size() == 0) {
			fprintf(stderr, "\n\nMore than half the features were clipped away at zoom level 0.\n");
			fprintf(stderr, "Is your data in the wrong projection? It should be in WGS84/EPSG:4326.\n");
		}

		size_t passes = pass + 1;
		double progress = floor(((((*geompos_in + *along - alongminus) / (double) todo) + pass) / passes + z) / (maxzoom + 1) * 1000) / 10;
		if (progress >= oprogress + 0.1) {
			if (!quiet && !quiet_progress && progress_time()) {
				fprintf(stderr, "  %3.1f%%  %d/%u/%u  \r", progress, z, tx, ty);
				fflush(stderr);
			}
			if (logger.json_enabled && progress_time()) {
				logger.progress_tile(progress);
			}
			oprogress = progress;
		}

		if (totalsize > 0 && tile.layers.size() > 0) {
			if (totalsize > max_tile_features && !prevent[P_FEATURE_LIMIT]) {
				if (totalsize > arg->feature_count_out) {
					arg->feature_count_out = totalsize;
				}

				if (!quiet) {
					fprintf(stderr, "tile %d/%u/%u has %zu features, >%zu    \n", z, tx, ty, totalsize, max_tile_features);
				}

				if (additional[A_INCREASE_GAMMA_AS_NEEDED] && gamma < 10) {
					if (gamma < 1) {
						gamma = 1;
					} else {
						gamma = gamma * 1.25;
					}

					if (gamma > arg->gamma_out) {
						arg->gamma_out = gamma;
						arg->still_dropping = true;
					}

					if (!quiet) {
						fprintf(stderr, "Going to try gamma of %0.3f to make it fit\n", gamma);
					}
					line_detail++;	// to keep it the same when the loop decrements it
					continue;
				} else if (mingap < ULONG_MAX && (additional[A_DROP_DENSEST_AS_NEEDED] || additional[A_COALESCE_DENSEST_AS_NEEDED] || additional[A_CLUSTER_DENSEST_AS_NEEDED])) {
					mingap_fraction = mingap_fraction * max_tile_features / totalsize * 0.90;
					unsigned long long mg = choose_mingap(indices, mingap_fraction);
					if (mg <= mingap) {
						mg = (mingap + 1) * 1.5;

						if (mg <= mingap) {
							mg = ULONG_MAX;
						}
					}
					mingap = mg;
					if (mingap > arg->mingap_out) {
						arg->mingap_out = mingap;
						arg->still_dropping = true;
					}
					if (!quiet) {
						fprintf(stderr, "Going to try keeping the sparsest %0.2f%% of the features to make it fit\n", mingap_fraction * 100.0);
					}
					line_detail++;
					continue;
				} else if (additional[A_DROP_SMALLEST_AS_NEEDED] || additional[A_COALESCE_SMALLEST_AS_NEEDED]) {
					minextent_fraction = minextent_fraction * max_tile_features / totalsize * 0.75;
					long long m = choose_minextent(extents, minextent_fraction, minextent);
					if (m != minextent) {
						minextent = m;
						if (minextent > arg->minextent_out) {
							arg->minextent_out = minextent;
							arg->still_dropping = true;
						}
						if (!quiet) {
							fprintf(stderr, "Going to try keeping the biggest %0.2f%% of the features to make it fit\n", minextent_fraction * 100.0);
						}
						line_detail++;
						continue;
					}
				} else if (totalsize > layers.size() && (prevent[P_DYNAMIC_DROP] || additional[A_DROP_FRACTION_AS_NEEDED] || additional[A_COALESCE_FRACTION_AS_NEEDED])) {
					// The 95% is a guess to avoid too many retries
					// and probably actually varies based on how much duplicated metadata there is

					fraction = fraction * max_tile_features / totalsize * 0.95;
					if (!quiet) {
						fprintf(stderr, "Going to try keeping %0.2f%% of the features to make it fit\n", fraction * 100);
					}
					if ((additional[A_DROP_FRACTION_AS_NEEDED] || additional[A_COALESCE_FRACTION_AS_NEEDED]) && fraction < arg->fraction_out) {
						arg->fraction_out = fraction;
						arg->still_dropping = true;
					} else if (prevent[P_DYNAMIC_DROP]) {
						arg->still_dropping = true;
					}
					line_detail++;	// to keep it the same when the loop decrements it
					continue;
				} else {
					fprintf(stderr, "Try using --drop-fraction-as-needed or --drop-densest-as-needed.\n");
					return -1;
				}
			}

			std::string compressed;
			std::string pbf = tile.encode();

			tile.layers.clear();

			if (!prevent[P_TILE_COMPRESSION]) {
				compress(pbf, compressed, true);
			} else {
				compressed = pbf;
			}

			if (compressed.size() > scaled_max_tile_size && !prevent[P_KILOBYTE_LIMIT]) {
				// Estimate how big it really should have been compressed
				// from how many features were kept vs skipped for already being
				// over the threshold

				double kept_adjust = (skipped + kept) / (double) kept;

				if (compressed.size() > arg->tile_size_out) {
					arg->tile_size_out = compressed.size() * kept_adjust;
				}

				if (!quiet) {
					if (skipped > 0) {
						fprintf(stderr, "tile %d/%u/%u size is %lld (probably really %lld) with detail %d, >%zu    \n", z, tx, ty, (long long) compressed.size(), (long long) (compressed.size() * kept_adjust), line_detail, scaled_max_tile_size);
					} else {
						fprintf(stderr, "tile %d/%u/%u size is %lld with detail %d, >%zu    \n", z, tx, ty, (long long) compressed.size(), line_detail, scaled_max_tile_size);
					}
				}

				if (additional[A_INCREASE_GAMMA_AS_NEEDED] && gamma < 10) {
					if (gamma < 1) {
						gamma = 1;
					} else {
						gamma = gamma * 1.25;
					}

					if (gamma > arg->gamma_out) {
						arg->gamma_out = gamma;
						arg->still_dropping = true;
					}

					if (!quiet) {
						fprintf(stderr, "Going to try gamma of %0.3f to make it fit\n", gamma);
					}
					line_detail++;	// to keep it the same when the loop decrements it
				} else if (mingap < ULONG_MAX && (additional[A_DROP_DENSEST_AS_NEEDED] || additional[A_COALESCE_DENSEST_AS_NEEDED] || additional[A_CLUSTER_DENSEST_AS_NEEDED])) {
					mingap_fraction = mingap_fraction * scaled_max_tile_size / (kept_adjust * compressed.size()) * 0.90;
					unsigned long long mg = choose_mingap(indices, mingap_fraction);
					if (mg <= mingap) {
						double nmg = (mingap + 1) * 1.5;

						if (nmg <= mingap || nmg > ULONG_MAX) {
							mg = ULONG_MAX;
						} else {
							mg = nmg;

							if (mg <= mingap) {
								mg = ULONG_MAX;
							}
						}
					}
					mingap = mg;
					if (mingap > arg->mingap_out) {
						arg->mingap_out = mingap;
						arg->still_dropping = true;
					}
					if (!quiet) {
						fprintf(stderr, "Going to try keeping the sparsest %0.2f%% of the features to make it fit\n", mingap_fraction * 100.0);
					}
					line_detail++;
				} else if (additional[A_DROP_SMALLEST_AS_NEEDED] || additional[A_COALESCE_SMALLEST_AS_NEEDED]) {
					minextent_fraction = minextent_fraction * scaled_max_tile_size / (kept_adjust * compressed.size()) * 0.75;
					long long m = choose_minextent(extents, minextent_fraction, minextent);
					if (m != minextent) {
						minextent = m;
						if (minextent > arg->minextent_out) {
							arg->minextent_out = minextent;
							arg->still_dropping = true;
						}
						if (!quiet) {
							fprintf(stderr, "Going to try keeping the biggest %0.2f%% of the features to make it fit\n", minextent_fraction * 100.0);
						}
						line_detail++;
						continue;
					}
				} else if (totalsize > layers.size() && (prevent[P_DYNAMIC_DROP] || additional[A_DROP_FRACTION_AS_NEEDED] || additional[A_COALESCE_FRACTION_AS_NEEDED])) {
					// The 95% is a guess to avoid too many retries
					// and probably actually varies based on how much duplicated metadata there is

					fraction = fraction * scaled_max_tile_size / (kept_adjust * compressed.size()) * 0.95;
					if (!quiet) {
						fprintf(stderr, "Going to try keeping %0.2f%% of the features to make it fit\n", fraction * 100);
					}
					if ((additional[A_DROP_FRACTION_AS_NEEDED] || additional[A_COALESCE_FRACTION_AS_NEEDED]) && fraction < arg->fraction_out) {
						arg->fraction_out = fraction;
						arg->still_dropping = true;
					} else if (prevent[P_DYNAMIC_DROP]) {
						arg->still_dropping = true;
					}
					line_detail++;	// to keep it the same when the loop decrements it
				} else {
					strategy->detail_reduced++;
				}
			} else {
				if (pthread_mutex_lock(&db_lock) != 0) {
					perror("pthread_mutex_lock");
					exit(EXIT_PTHREAD);
				}

				if (outdb != NULL) {
					mbtiles_write_tile(outdb, z, tx, ty, compressed.data(), compressed.size());
				} else if (outdir != NULL) {
					dir_write_tile(outdir, z, tx, ty, compressed);
				}

				if (pthread_mutex_unlock(&db_lock) != 0) {
					perror("pthread_mutex_unlock");
					exit(EXIT_PTHREAD);
				}

				return count;
			}
		} else {
			return count;
		}
	}

	fprintf(stderr, "could not make tile %d/%u/%u small enough\n", z, tx, ty);
	return -1;
}

struct task {
	int fileno = 0;
	struct task *next = NULL;
};

void *run_thread(void *vargs) {
	write_tile_args *arg = (write_tile_args *) vargs;
	struct task *task;
	int *err_or_null = NULL;

	for (task = arg->tasks; task != NULL; task = task->next) {
		int j = task->fileno;

		if (arg->geomfd[j] < 0) {
			// only one source file for zoom level 0
			continue;
		}
		if (arg->geom_size[j] == 0) {
			continue;
		}

		// If this is zoom level 0, the geomfd will be uncompressed data,
		// because (at least for now) it needs to stay uncompressed during
		// the sort and post-sort maxzoom calculation and fixup so that
		// the sort can rearrange individual features and the fixup can
		// then adjust their minzooms without decompressing and recompressing
		// each feature.
		//
		// In higher zooms, it will be compressed data written out during the
		// previous zoom.

		FILE *geom = fdopen(arg->geomfd[j], "rb");
		if (geom == NULL) {
			perror("open geom");
			exit(EXIT_OPEN);
		}

		decompressor dc(geom);

		std::atomic<long long> geompos(0);
		long long prevgeom = 0;

		while (1) {
			int z;
			unsigned x, y;

			// These z/x/y are uncompressed so we can seek to the start of the
			// compressed feature data that immediately follows.

			if (!dc.deserialize_int(&z, &geompos)) {
				break;
			}
			dc.deserialize_uint(&x, &geompos);
			dc.deserialize_uint(&y, &geompos);
#if 0
			// currently broken because also requires tracking nextzoom when skipping zooms
			if (z != arg->zoom) {
				fprintf(stderr, "Expected zoom %d, found zoom %d\n", arg->zoom, z);
				exit(EXIT_IMPOSSIBLE);
			}
#endif

			if (arg->compressed) {
				dc.begin();
			}

			arg->wrote_zoom = z;

			// fprintf(stderr, "%d/%u/%u\n", z, x, y);

			long long len = write_tile(&dc, &geompos, arg->stringpool, z, x, y, z == arg->maxzoom ? arg->full_detail : arg->low_detail, arg->min_detail, arg->outdb, arg->outdir, arg->buffer, arg->fname, arg->geomfile, arg->minzoom, arg->maxzoom, arg->todo, arg->along, geompos, arg->gamma, arg->child_shards, arg->pool_off, arg->initial_x, arg->initial_y, arg->running, arg->simplification, arg->layermaps, arg->layer_unmaps, arg->tiling_seg, arg->pass, arg->mingap, arg->minextent, arg->fraction, arg->prefilter, arg->postfilter, arg->filter, arg, arg->strategy, arg->compressed, arg->shared_nodes_map, arg->nodepos);

			if (pthread_mutex_lock(&var_lock) != 0) {
				perror("pthread_mutex_lock");
				exit(EXIT_PTHREAD);
			}

			if (z == arg->maxzoom) {
				if (len > *arg->most) {
					*arg->midx = x;
					*arg->midy = y;
					*arg->most = len;
				} else if (len == *arg->most) {
					unsigned long long a = (((unsigned long long) x) << 32) | y;
					unsigned long long b = (((unsigned long long) *arg->midx) << 32) | *arg->midy;

					if (a < b) {
						*arg->midx = x;
						*arg->midy = y;
						*arg->most = len;
					}
				}
			}

			*arg->along += geompos - prevgeom;
			prevgeom = geompos;

			if (pthread_mutex_unlock(&var_lock) != 0) {
				perror("pthread_mutex_unlock");
				exit(EXIT_PTHREAD);
			}

			if (len < 0) {
				err_or_null = &arg->err;
				*err_or_null = z - 1;
				break;
			}
		}

		if (arg->pass == 1) {
			// Since the fclose() has closed the underlying file descriptor
			arg->geomfd[j] = -1;
		} else {
			int newfd = dup(arg->geomfd[j]);
			if (newfd < 0) {
				perror("dup geometry");
				exit(EXIT_OPEN);
			}
			if (lseek(newfd, 0, SEEK_SET) < 0) {
				perror("lseek geometry");
				exit(EXIT_SEEK);
			}
			arg->geomfd[j] = newfd;
		}

		if (fclose(geom) != 0) {
			perror("close geom");
			exit(EXIT_CLOSE);
		}
	}

	arg->running--;
	return err_or_null;
}

int traverse_zooms(int *geomfd, off_t *geom_size, char *stringpool, std::atomic<unsigned> *midx, std::atomic<unsigned> *midy, int &maxzoom, int minzoom, sqlite3 *outdb, const char *outdir, int buffer, const char *fname, const char *tmpdir, double gamma, int full_detail, int low_detail, int min_detail, long long *pool_off, unsigned *initial_x, unsigned *initial_y, double simplification, double maxzoom_simplification, std::vector<std::map<std::string, layermap_entry>> &layermaps, const char *prefilter, const char *postfilter, std::unordered_map<std::string, attribute_op> const *attribute_accum, json_object *filter, std::vector<strategy> &strategies, int iz, node *shared_nodes_map, size_t nodepos, int basezoom, double droprate) {
	last_progress = 0;

	// The existing layermaps are one table per input thread.
	// We need to add another one per *tiling* thread so that it can be
	// safely changed during tiling.
	size_t layermaps_off = layermaps.size();
	for (size_t i = 0; i < CPUS; i++) {
		layermaps.push_back(std::map<std::string, layermap_entry>());
	}

	// Table to map segment and layer number back to layer name
	std::vector<std::vector<std::string>> layer_unmaps;
	for (size_t seg = 0; seg < layermaps.size(); seg++) {
		layer_unmaps.push_back(std::vector<std::string>());

		for (auto a = layermaps[seg].begin(); a != layermaps[seg].end(); ++a) {
			if (a->second.id >= layer_unmaps[seg].size()) {
				layer_unmaps[seg].resize(a->second.id + 1);
			}
			layer_unmaps[seg][a->second.id] = a->first;
		}
	}

	int z;
	for (z = iz; z <= maxzoom; z++) {
		std::atomic<long long> most(0);

		compressor compressors[TEMP_FILES];
		compressor *sub[TEMP_FILES];
		int subfd[TEMP_FILES];
		for (size_t j = 0; j < TEMP_FILES; j++) {
			char geomname[strlen(tmpdir) + strlen("/geom.XXXXXXXX" XSTRINGIFY(INT_MAX)) + 1];
			snprintf(geomname, sizeof(geomname), "%s/geom%zu.XXXXXXXX", tmpdir, j);
			subfd[j] = mkstemp_cloexec(geomname);
			// printf("%s\n", geomname);
			if (subfd[j] < 0) {
				perror(geomname);
				exit(EXIT_OPEN);
			}
			FILE *fp = fopen_oflag(geomname, "wb", O_WRONLY | O_CLOEXEC);
			if (fp == NULL) {
				perror(geomname);
				exit(EXIT_OPEN);
			}
			compressors[j] = compressor(fp);
			sub[j] = &compressors[j];
			unlink(geomname);
		}

		size_t useful_threads = 0;
		long long todo = 0;
		for (size_t j = 0; j < TEMP_FILES; j++) {
			todo += geom_size[j];
			if (geom_size[j] > 0) {
				useful_threads++;
			}
		}

		size_t threads = CPUS;
		if (threads > TEMP_FILES / 4) {
			threads = TEMP_FILES / 4;
		}
		// XXX is it useful to divide further if we know we are skipping
		// some zoom levels? Is it faster to have fewer CPUs working on
		// sharding, but more deeply, or more CPUs, less deeply?
		if (threads > useful_threads) {
			threads = useful_threads;
		}

		// Round down to a power of 2
		for (int e = 0; e < 30; e++) {
			if (threads >= (1U << e) && threads < (1U << (e + 1))) {
				threads = 1U << e;
				break;
			}
		}
		if (threads >= (1U << 30)) {
			threads = 1U << 30;
		}
		if (threads < 1) {
			threads = 1;
		}

		// Assign temporary files to threads

		std::vector<task> tasks;
		tasks.resize(TEMP_FILES);

		struct dispatch {
			struct task *tasks = NULL;
			long long todo = 0;
			struct dispatch *next = NULL;
		};
		std::vector<dispatch> dispatches;
		dispatches.resize(threads);

		dispatch *dispatch_head = &dispatches[0];
		for (size_t j = 0; j < threads; j++) {
			dispatches[j].tasks = NULL;
			dispatches[j].todo = 0;
			if (j + 1 < threads) {
				dispatches[j].next = &dispatches[j + 1];
			} else {
				dispatches[j].next = NULL;
			}
		}

		for (size_t j = 0; j < TEMP_FILES; j++) {
			if (geom_size[j] == 0) {
				continue;
			}

			tasks[j].fileno = j;
			tasks[j].next = dispatch_head->tasks;
			dispatch_head->tasks = &tasks[j];
			dispatch_head->todo += geom_size[j];

			dispatch *here = dispatch_head;
			dispatch_head = dispatch_head->next;

			dispatch **d;
			for (d = &dispatch_head; *d != NULL; d = &((*d)->next)) {
				if (here->todo < (*d)->todo) {
					break;
				}
			}

			here->next = *d;
			*d = here;
		}

		int err = INT_MAX;

		double zoom_gamma = gamma;
		unsigned long long zoom_mingap = ((1LL << (32 - z)) / 256 * cluster_distance) * ((1LL << (32 - z)) / 256 * cluster_distance);
		long long zoom_minextent = 0;
		double zoom_fraction = 1;
		size_t zoom_tile_size = 0;
		size_t zoom_feature_count = 0;

		for (size_t pass = 0;; pass++) {
			pthread_t pthreads[threads];
			std::vector<write_tile_args> args;
			args.resize(threads);
			std::atomic<int> running(threads);
			std::atomic<long long> along(0);
			atomic_strategy strategy;

			for (size_t thread = 0; thread < threads; thread++) {
				args[thread].stringpool = stringpool;
				args[thread].min_detail = min_detail;
				args[thread].outdb = outdb;  // locked with db_lock
				args[thread].outdir = outdir;
				args[thread].buffer = buffer;
				args[thread].fname = fname;
				args[thread].geomfile = sub + thread * (TEMP_FILES / threads);
				args[thread].todo = todo;
				args[thread].along = &along;  // locked with var_lock
				args[thread].gamma = zoom_gamma;
				args[thread].gamma_out = zoom_gamma;
				args[thread].mingap = zoom_mingap;
				args[thread].mingap_out = zoom_mingap;
				args[thread].minextent = zoom_minextent;
				args[thread].minextent_out = zoom_minextent;
				args[thread].fraction = zoom_fraction;
				args[thread].fraction_out = zoom_fraction;
				args[thread].tile_size_out = 0;
				args[thread].feature_count_out = 0;
				args[thread].child_shards = TEMP_FILES / threads;

				if (z == maxzoom && maxzoom_simplification > 0) {
					args[thread].simplification = maxzoom_simplification;
				} else {
					args[thread].simplification = simplification;
				}

				args[thread].geomfd = geomfd;
				args[thread].geom_size = geom_size;
				args[thread].midx = midx;  // locked with var_lock
				args[thread].midy = midy;  // locked with var_lock
				args[thread].maxzoom = maxzoom;
				args[thread].minzoom = minzoom;
				args[thread].basezoom = basezoom;
				args[thread].droprate = droprate;
				args[thread].full_detail = full_detail;
				args[thread].low_detail = low_detail;
				args[thread].most = &most;  // locked with var_lock
				args[thread].pool_off = pool_off;
				args[thread].initial_x = initial_x;
				args[thread].initial_y = initial_y;
				args[thread].layermaps = &layermaps;
				args[thread].layer_unmaps = &layer_unmaps;
				args[thread].tiling_seg = thread + layermaps_off;
				args[thread].prefilter = prefilter;
				args[thread].postfilter = postfilter;
				args[thread].attribute_accum = attribute_accum;
				args[thread].filter = filter;

				args[thread].tasks = dispatches[thread].tasks;
				args[thread].running = &running;
				args[thread].pass = pass;
				args[thread].wrote_zoom = -1;
				args[thread].still_dropping = false;
				args[thread].strategy = &strategy;
				args[thread].zoom = z;
				args[thread].compressed = (z != iz);
				args[thread].shared_nodes_map = shared_nodes_map;
				args[thread].nodepos = nodepos;

				if (thread_create(&pthreads[thread], NULL, run_thread, &args[thread]) != 0) {
					perror("pthread_create");
					exit(EXIT_PTHREAD);
				}
			}

			bool again = false;
			bool extend_zooms = false;
			for (size_t thread = 0; thread < threads; thread++) {
				void *retval;

				if (pthread_join(pthreads[thread], &retval) != 0) {
					perror("pthread_join");
				}

				if (retval != NULL) {
					err = *((int *) retval);
				}

				if (args[thread].gamma_out > zoom_gamma) {
					zoom_gamma = args[thread].gamma_out;
					again = true;
				}
				if (args[thread].mingap_out > zoom_mingap) {
					zoom_mingap = args[thread].mingap_out;
					again = true;
				}
				if (args[thread].minextent_out > zoom_minextent) {
					zoom_minextent = args[thread].minextent_out;
					again = true;
				}
				if (args[thread].fraction_out < zoom_fraction) {
					zoom_fraction = args[thread].fraction_out;
					again = true;
				}
				if (args[thread].tile_size_out > zoom_tile_size) {
					zoom_tile_size = args[thread].tile_size_out;
				}
				if (args[thread].feature_count_out > zoom_feature_count) {
					zoom_feature_count = args[thread].feature_count_out;
				}

				// Zoom counter might be lower than reality if zooms are being skipped
				if (args[thread].wrote_zoom > z) {
					z = args[thread].wrote_zoom;
				}

				if (args[thread].still_dropping) {
					extend_zooms = true;
				}
			}

			if (extend_zooms && (additional[A_EXTEND_ZOOMS] || extend_zooms_max > 0) && z == maxzoom && maxzoom < MAX_ZOOM) {
				maxzoom++;
				if (extend_zooms_max > 0) {
					extend_zooms_max--;
				}
			}

			if ((size_t) z >= strategies.size()) {
				strategies.resize(z + 1);
			}

			struct strategy s(strategy, zoom_tile_size, zoom_feature_count);
			strategies[z] = s;

			if (again) {
				if (outdb != NULL) {
					mbtiles_erase_zoom(outdb, z);
				} else if (outdir != NULL) {
					dir_erase_zoom(outdir, z);
				}
			} else {
				break;
			}
		}

		for (size_t j = 0; j < TEMP_FILES; j++) {
			// Can be < 0 if there is only one source file, at z0
			if (geomfd[j] >= 0) {
				if (close(geomfd[j]) != 0) {
					perror("close geom");
					exit(EXIT_CLOSE);
				}
			}
			if (sub[j]->fclose() != 0) {
				perror("close subfile");
				exit(EXIT_CLOSE);
			}

			struct stat geomst;
			if (fstat(subfd[j], &geomst) != 0) {
				perror("stat geom\n");
				exit(EXIT_STAT);
			}

			geomfd[j] = subfd[j];
			geom_size[j] = geomst.st_size;
		}

		if (err != INT_MAX) {
			return err;
		}
	}

	for (size_t j = 0; j < TEMP_FILES; j++) {
		// Can be < 0 if there is only one source file, at z0
		if (geomfd[j] >= 0) {
			if (close(geomfd[j]) != 0) {
				perror("close geom");
				exit(EXIT_CLOSE);
			}
		}
	}

	if (!quiet) {
		fprintf(stderr, "\n");
	}
	return maxzoom;
}
