#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <set>
#include <map>
#include <algorithm>
#include <limits.h>
#include <zlib.h>
#include "protozero/varint.hpp"
#include "geometry.hpp"
#include "mbtiles.hpp"
#include "mvt.hpp"
#include "tile.hpp"
#include "serial.hpp"
#include "options.hpp"
#include "main.hpp"
#include "pool.hpp"
#include "projection.hpp"
#include "evaluator.hpp"
#include "milo/dtoa_milo.h"
#include "errors.hpp"

// Offset coordinates to keep them positive
#define COORD_OFFSET (4LL << 32)
#define SHIFT_RIGHT(a) ((long long) std::round((double) (a) / (1LL << geometry_scale)))
#define SHIFT_LEFT(a) ((((a) + (COORD_OFFSET >> geometry_scale)) << geometry_scale) - COORD_OFFSET)

// write to file

size_t fwrite_check(const void *ptr, size_t size, size_t nitems, FILE *stream, std::atomic<long long> *fpos, const char *fname) {
	size_t w = fwrite(ptr, size, nitems, stream);
	if (w != nitems) {
		fprintf(stderr, "%s: Write to temporary file failed: %s\n", fname, strerror(errno));
		exit(EXIT_WRITE);
	}
	*fpos += size * nitems;
	return w;
}

void serialize_int(FILE *out, int n, std::atomic<long long> *fpos, const char *fname) {
	serialize_long_long(out, n, fpos, fname);
}

void serialize_long_long(FILE *out, long long n, std::atomic<long long> *fpos, const char *fname) {
	unsigned long long zigzag = protozero::encode_zigzag64(n);

	serialize_ulong_long(out, zigzag, fpos, fname);
}

void serialize_ulong_long(FILE *out, unsigned long long zigzag, std::atomic<long long> *fpos, const char *fname) {
	while (1) {
		unsigned char b = zigzag & 0x7F;
		if ((zigzag >> 7) != 0) {
			b |= 0x80;
			if (putc(b, out) == EOF) {
				fprintf(stderr, "%s: Write to temporary file failed: %s\n", fname, strerror(errno));
				exit(EXIT_WRITE);
			}
			*fpos += 1;
			zigzag >>= 7;
		} else {
			if (putc(b, out) == EOF) {
				fprintf(stderr, "%s: Write to temporary file failed: %s\n", fname, strerror(errno));
				exit(EXIT_WRITE);
			}
			*fpos += 1;
			break;
		}
	}
}

void serialize_byte(FILE *out, signed char n, std::atomic<long long> *fpos, const char *fname) {
	fwrite_check(&n, sizeof(signed char), 1, out, fpos, fname);
}

void serialize_uint(FILE *out, unsigned n, std::atomic<long long> *fpos, const char *fname) {
	serialize_ulong_long(out, n, fpos, fname);
}

// write to memory

size_t fwrite_check(const void *ptr, size_t size, size_t nitems, std::string &stream) {
	stream += std::string((char *) ptr, size * nitems);
	return nitems;
}

void serialize_ulong_long(std::string &out, unsigned long long zigzag) {
	while (1) {
		unsigned char b = zigzag & 0x7F;
		if ((zigzag >> 7) != 0) {
			b |= 0x80;
			out += b;
			zigzag >>= 7;
		} else {
			out += b;
			break;
		}
	}
}

void serialize_long_long(std::string &out, long long n) {
	unsigned long long zigzag = protozero::encode_zigzag64(n);

	serialize_ulong_long(out, zigzag);
}

void serialize_int(std::string &out, int n) {
	serialize_long_long(out, n);
}

void serialize_byte(std::string &out, signed char n) {
	out += n;
}

void serialize_uint(std::string &out, unsigned n) {
	serialize_ulong_long(out, n);
}

// read from memory

void deserialize_int(char **f, int *n) {
	long long ll;
	deserialize_long_long(f, &ll);
	*n = ll;
}

void deserialize_long_long(char **f, long long *n) {
	unsigned long long zigzag = 0;
	deserialize_ulong_long(f, &zigzag);
	*n = protozero::decode_zigzag64(zigzag);
}

void deserialize_ulong_long(char **f, unsigned long long *zigzag) {
	*zigzag = 0;
	int shift = 0;

	while (1) {
		if ((**f & 0x80) == 0) {
			*zigzag |= ((unsigned long long) **f) << shift;
			*f += 1;
			shift += 7;
			break;
		} else {
			*zigzag |= ((unsigned long long) (**f & 0x7F)) << shift;
			*f += 1;
			shift += 7;
		}
	}
}

void deserialize_uint(char **f, unsigned *n) {
	unsigned long long v;
	deserialize_ulong_long(f, &v);
	*n = v;
}

void deserialize_byte(char **f, signed char *n) {
	memcpy(n, *f, sizeof(signed char));
	*f += sizeof(signed char);
}

static void write_geometry(drawvec const &dv, std::string &out, long long wx, long long wy) {
	for (size_t i = 0; i < dv.size(); i++) {
		if (dv[i].op == VT_MOVETO || dv[i].op == VT_LINETO) {
			serialize_byte(out, dv[i].op);
			serialize_long_long(out, dv[i].x - wx);
			serialize_long_long(out, dv[i].y - wy);
			wx = dv[i].x;
			wy = dv[i].y;
		} else {
			serialize_byte(out, dv[i].op);
		}
	}
	serialize_byte(out, VT_END);
}

// called from generating the next zoom level
std::string serialize_feature(serial_feature *sf, long long wx, long long wy) {
	std::string s;

	serialize_byte(s, sf->t);

#define FLAG_LAYER 7

#define FLAG_LABEL_POINT 6
#define FLAG_SEQ 5
#define FLAG_INDEX 4
#define FLAG_EXTENT 3
#define FLAG_ID 2
#define FLAG_MINZOOM 1
#define FLAG_MAXZOOM 0

	long long layer = 0;
	layer |= sf->layer << FLAG_LAYER;
	layer |= (sf->label_point != 0) << FLAG_LABEL_POINT;
	layer |= (sf->seq != 0) << FLAG_SEQ;
	layer |= (sf->index != 0) << FLAG_INDEX;
	layer |= (sf->extent != 0) << FLAG_EXTENT;
	layer |= sf->has_id << FLAG_ID;
	layer |= sf->has_tippecanoe_minzoom << FLAG_MINZOOM;
	layer |= sf->has_tippecanoe_maxzoom << FLAG_MAXZOOM;

	serialize_long_long(s, layer);
	if (sf->seq != 0) {
		serialize_long_long(s, sf->seq);
	}
	if (sf->has_tippecanoe_minzoom) {
		serialize_int(s, sf->tippecanoe_minzoom);
	}
	if (sf->has_tippecanoe_maxzoom) {
		serialize_int(s, sf->tippecanoe_maxzoom);
	}
	if (sf->has_id) {
		serialize_ulong_long(s, sf->id);
	}

	serialize_int(s, sf->segment);

	write_geometry(sf->geometry, s, wx, wy);

	if (sf->index != 0) {
		serialize_ulong_long(s, sf->index);
	}
	if (sf->label_point != 0) {
		serialize_ulong_long(s, sf->label_point);
	}
	if (sf->extent != 0) {
		serialize_long_long(s, sf->extent);
	}

	serialize_long_long(s, sf->keys.size());

	for (size_t i = 0; i < sf->keys.size(); i++) {
		serialize_long_long(s, sf->keys[i]);
		serialize_long_long(s, sf->values[i]);
	}

	// MAGIC: This knows that the feature minzoom is the last byte of the feature,
	serialize_byte(s, sf->feature_minzoom);
	return s;
}

serial_feature deserialize_feature(std::string &geoms, unsigned z, unsigned tx, unsigned ty, unsigned *initial_x, unsigned *initial_y) {
	serial_feature sf;
	char *cp = (char *) geoms.c_str();

	deserialize_byte(&cp, &sf.t);
	deserialize_long_long(&cp, &sf.layer);

	sf.seq = 0;
	if (sf.layer & (1 << FLAG_SEQ)) {
		deserialize_long_long(&cp, &sf.seq);
	}

	sf.tippecanoe_minzoom = -1;
	sf.tippecanoe_maxzoom = -1;
	sf.id = 0;
	sf.has_id = false;
	if (sf.layer & (1 << FLAG_MINZOOM)) {
		deserialize_int(&cp, &sf.tippecanoe_minzoom);
	}
	if (sf.layer & (1 << FLAG_MAXZOOM)) {
		deserialize_int(&cp, &sf.tippecanoe_maxzoom);
	}
	if (sf.layer & (1 << FLAG_ID)) {
		sf.has_id = true;
		deserialize_ulong_long(&cp, &sf.id);
	}

	deserialize_int(&cp, &sf.segment);

	sf.index = 0;
	sf.label_point = 0;
	sf.extent = 0;

	sf.geometry = decode_geometry(&cp, z, tx, ty, sf.bbox, initial_x[sf.segment], initial_y[sf.segment]);

	if (sf.layer & (1 << FLAG_INDEX)) {
		deserialize_ulong_long(&cp, &sf.index);
	}
	if (sf.layer & (1 << FLAG_LABEL_POINT)) {
		deserialize_ulong_long(&cp, &sf.label_point);
	}
	if (sf.layer & (1 << FLAG_EXTENT)) {
		deserialize_long_long(&cp, &sf.extent);
	}

	sf.layer >>= FLAG_LAYER;

	long long count;
	deserialize_long_long(&cp, &count);

	for (long long i = 0; i < count; i++) {
		long long k, v;
		deserialize_long_long(&cp, &k);
		deserialize_long_long(&cp, &v);
		sf.keys.push_back(k);
		sf.values.push_back(v);
	}

	// MAGIC: This knows that the feature minzoom is the last byte of the feature.
	deserialize_byte(&cp, &sf.feature_minzoom);

	if (cp != geoms.c_str() + geoms.size()) {
		fprintf(stderr, "wrong length decoding feature: used %zd, len is %zu\n", cp - geoms.c_str(), geoms.size());
		exit(EXIT_IMPOSSIBLE);
	}

	return sf;
}

static long long scale_geometry(struct serialization_state *sst, long long *bbox, drawvec &geom) {
	long long offset = 0;
	long long prev = 0;
	bool has_prev = false;
	double scale = 1.0 / (1 << geometry_scale);

	for (size_t i = 0; i < geom.size(); i++) {
		if (geom[i].op == VT_MOVETO || geom[i].op == VT_LINETO) {
			long long x = geom[i].x;
			long long y = geom[i].y;

			if (additional[A_DETECT_WRAPAROUND]) {
				x += offset;
				if (has_prev) {
					if (x - prev > (1LL << 31)) {
						offset -= 1LL << 32;
						x -= 1LL << 32;
					} else if (prev - x > (1LL << 31)) {
						offset += 1LL << 32;
						x += 1LL << 32;
					}
				}

				has_prev = true;
				prev = x;
			}

			if (x < bbox[0]) {
				bbox[0] = x;
			}
			if (y < bbox[1]) {
				bbox[1] = y;
			}
			if (x > bbox[2]) {
				bbox[2] = x;
			}
			if (y > bbox[3]) {
				bbox[3] = y;
			}

			if (!*(sst->initialized)) {
				if (x < 0 || x >= (1LL << 32) || y < 0 || y >= (1LL << 32)) {
					*(sst->initial_x) = 1LL << 31;
					*(sst->initial_y) = 1LL << 31;
				} else {
					*(sst->initial_x) = SHIFT_LEFT(SHIFT_RIGHT(x));
					*(sst->initial_y) = SHIFT_LEFT(SHIFT_RIGHT(y));
				}

				*(sst->initialized) = 1;
			}

			if (additional[A_GRID_LOW_ZOOMS]) {
				// If we are gridding, snap to the maxzoom grid in case the incoming data
				// is already supposed to be aligned to tile boundaries (but is not, exactly,
				// because of rounding error during projection).

				geom[i].x = std::round(x * scale);
				geom[i].y = std::round(y * scale);
			} else {
				geom[i].x = SHIFT_RIGHT(x);
				geom[i].y = SHIFT_RIGHT(y);
			}
		}
	}

	return geom.size();
}

static std::string strip_zeroes(std::string s) {
	// Doesn't do anything special with '-' followed by leading zeros
	// since integer IDs must be positive

	while (s.size() > 0 && s[0] == '0') {
		s.erase(s.begin());
	}

	return s;
}

// called from frontends
int serialize_feature(struct serialization_state *sst, serial_feature &sf) {
	struct reader *r = &(*sst->readers)[sst->segment];

	sf.bbox[0] = LLONG_MAX;
	sf.bbox[1] = LLONG_MAX;
	sf.bbox[2] = LLONG_MIN;
	sf.bbox[3] = LLONG_MIN;

	for (size_t i = 0; i < sf.geometry.size(); i++) {
		if (sf.geometry[i].op == VT_MOVETO || sf.geometry[i].op == VT_LINETO) {
			if (sf.geometry[i].y > 0 && sf.geometry[i].y < 0xFFFFFFFF) {
				// standard -180 to 180 world plane

				long long x = sf.geometry[i].x & 0xFFFFFFFF;
				long long y = sf.geometry[i].y & 0xFFFFFFFF;

				r->file_bbox1[0] = std::min(r->file_bbox1[0], x);
				r->file_bbox1[1] = std::min(r->file_bbox1[1], y);
				r->file_bbox1[2] = std::max(r->file_bbox1[2], x);
				r->file_bbox1[3] = std::max(r->file_bbox1[3], y);

				// printf("%llx,%llx  %llx,%llx %llx,%llx  ", x, y, r->file_bbox1[0], r->file_bbox1[1], r->file_bbox1[2], r->file_bbox1[3]);

				// shift the western hemisphere 360 degrees to the east
				if (x < 0x80000000) {  // prime meridian
					x += 0x100000000;
				}

				r->file_bbox2[0] = std::min(r->file_bbox2[0], x);
				r->file_bbox2[1] = std::min(r->file_bbox2[1], y);
				r->file_bbox2[2] = std::max(r->file_bbox2[2], x);
				r->file_bbox2[3] = std::max(r->file_bbox2[3], y);
			}
		}
	}

	// try to remind myself that the geometry in this function is in SCALED COORDINATES
	drawvec scaled_geometry = sf.geometry;
	sf.geometry.clear();
	scale_geometry(sst, sf.bbox, scaled_geometry);

	// This has to happen after scaling so that the wraparound detection has happened first.
	// Otherwise the inner/outer calculation will be confused by bad geometries.
	if (sf.t == VT_POLYGON) {
		scaled_geometry = fix_polygon(scaled_geometry);
	}

	for (auto &c : clipbboxes) {
		if (sf.t == VT_POLYGON) {
			scaled_geometry = simple_clip_poly(scaled_geometry, SHIFT_RIGHT(c.minx), SHIFT_RIGHT(c.miny), SHIFT_RIGHT(c.maxx), SHIFT_RIGHT(c.maxy), prevent[P_SIMPLIFY_SHARED_NODES]);
		} else if (sf.t == VT_LINE) {
			scaled_geometry = clip_lines(scaled_geometry, SHIFT_RIGHT(c.minx), SHIFT_RIGHT(c.miny), SHIFT_RIGHT(c.maxx), SHIFT_RIGHT(c.maxy));
			scaled_geometry = remove_noop(scaled_geometry, sf.t, 0);
		} else if (sf.t == VT_POINT) {
			scaled_geometry = clip_point(scaled_geometry, SHIFT_RIGHT(c.minx), SHIFT_RIGHT(c.miny), SHIFT_RIGHT(c.maxx), SHIFT_RIGHT(c.maxy));
		}

		sf.bbox[0] = LLONG_MAX;
		sf.bbox[1] = LLONG_MAX;
		sf.bbox[2] = LLONG_MIN;
		sf.bbox[3] = LLONG_MIN;

		for (auto &g : scaled_geometry) {
			long long x = SHIFT_LEFT(g.x);
			long long y = SHIFT_LEFT(g.y);

			if (x < sf.bbox[0]) {
				sf.bbox[0] = x;
			}
			if (y < sf.bbox[1]) {
				sf.bbox[1] = y;
			}
			if (x > sf.bbox[2]) {
				sf.bbox[2] = x;
			}
			if (y > sf.bbox[3]) {
				sf.bbox[3] = y;
			}
		}
	}

	if (scaled_geometry.size() == 0) {
		// Feature was clipped away
		return 1;
	}

	if (!sf.has_id) {
		if (additional[A_GENERATE_IDS]) {
			sf.has_id = true;
			sf.id = sf.seq + 1;
		}
	}

	if (sst->want_dist) {
		std::vector<unsigned long long> locs;
		for (size_t i = 0; i < scaled_geometry.size(); i++) {
			if (scaled_geometry[i].op == VT_MOVETO || scaled_geometry[i].op == VT_LINETO) {
				locs.push_back(encode_index(SHIFT_LEFT(scaled_geometry[i].x), SHIFT_LEFT(scaled_geometry[i].y)));
			}
		}
		std::sort(locs.begin(), locs.end());
		size_t n = 0;
		double sum = 0;
		for (size_t i = 1; i < locs.size(); i++) {
			if (locs[i - 1] != locs[i]) {
				sum += log(locs[i] - locs[i - 1]);
				n++;
			}
		}
		if (n > 0) {
			double avg = exp(sum / n);
			// Convert approximately from tile units to feet
			// See comment about empirical data in main.cpp
			double dist_ft = sqrt(avg) / 33;

			*(sst->dist_sum) += log(dist_ft) * n;
			*(sst->dist_count) += n;
		}
		locs.clear();
	}

	double extent = 0;
	if (additional[A_DROP_SMALLEST_AS_NEEDED] || additional[A_COALESCE_SMALLEST_AS_NEEDED] || order_by_size || sst->want_dist) {
		if (sf.t == VT_POLYGON) {
			for (size_t i = 0; i < scaled_geometry.size(); i++) {
				if (scaled_geometry[i].op == VT_MOVETO) {
					size_t j;
					for (j = i + 1; j < scaled_geometry.size(); j++) {
						if (scaled_geometry[j].op != VT_LINETO) {
							break;
						}
					}

					extent += SHIFT_LEFT(SHIFT_LEFT(1LL)) * get_area(scaled_geometry, i, j);
					i = j - 1;
				}
			}
		} else if (sf.t == VT_LINE) {
			double dist = 0;
			for (size_t i = 1; i < scaled_geometry.size(); i++) {
				if (scaled_geometry[i].op == VT_LINETO) {
					double xd = SHIFT_LEFT(scaled_geometry[i].x - scaled_geometry[i - 1].x);
					double yd = SHIFT_LEFT(scaled_geometry[i].y - scaled_geometry[i - 1].y);
					dist += sqrt(xd * xd + yd * yd);
				}
			}
			// treat lines as having the area of a circle with the line as diameter
			extent = M_PI * (dist / 2) * (dist / 2);
		}

		// VT_POINT extent will be calculated in write_tile from the distance between adjacent features.
	}

	if (extent <= LLONG_MAX) {
		sf.extent = (long long) extent;
	} else {
		sf.extent = LLONG_MAX;
	}

	if (sst->want_dist && sf.t == VT_POLYGON) {
		*(sst->area_sum) += extent;
	}

	if (!prevent[P_INPUT_ORDER]) {
		sf.seq = 0;
	}

	unsigned long long bbox_index;
	long long midx, midy;

	if (sf.t == VT_POINT) {
		// keep old behavior, which loses one bit of precision at the bottom
		midx = (sf.bbox[0] / 2 + sf.bbox[2] / 2) & ((1LL << 32) - 1);
		midy = (sf.bbox[1] / 2 + sf.bbox[3] / 2) & ((1LL << 32) - 1);
	} else {
		// To reduce the chances of giving multiple polygons or linestrings
		// the same index, use an arbitrary but predictable point from the
		// geometry as the index point rather than the bounding box center
		// as was previously used. The index point chosen comes from a hash
		// of the overall geometry, so features with the same geometry will
		// still have the same index. Specifically this avoids guessing
		// too high a maxzoom for a data source that has a large number of
		// LineStrings that map essentially the same route but with slight
		// jitter between them, even though the geometries themselves are
		// not very detailed.
		size_t ix = 0;
		for (size_t i = 0; i < scaled_geometry.size(); i++) {
			ix += scaled_geometry[i].x + scaled_geometry[i].y;
		}
		ix = ix % scaled_geometry.size();

		// If off the edge of the plane, mask to bring it back into the addressable area
		midx = SHIFT_LEFT(scaled_geometry[ix].x) & ((1LL << 32) - 1);
		midy = SHIFT_LEFT(scaled_geometry[ix].y) & ((1LL << 32) - 1);
	}

	bbox_index = encode_index(midx, midy);

	if (sf.t == VT_POLYGON && additional[A_GENERATE_POLYGON_LABEL_POINTS]) {
		drawvec dv = polygon_to_anchor(scaled_geometry);
		if (dv.size() > 0) {
			dv[0].x = SHIFT_LEFT(dv[0].x) & ((1LL << 32) - 1);
			dv[0].y = SHIFT_LEFT(dv[0].y) & ((1LL << 32) - 1);
			sf.label_point = encode_index(dv[0].x, dv[0].y);
		}
	}

	if (additional[A_DROP_DENSEST_AS_NEEDED] || additional[A_COALESCE_DENSEST_AS_NEEDED] || additional[A_CLUSTER_DENSEST_AS_NEEDED] || additional[A_CALCULATE_FEATURE_DENSITY] || additional[A_DROP_SMALLEST_AS_NEEDED] || additional[A_COALESCE_SMALLEST_AS_NEEDED] || additional[A_INCREASE_GAMMA_AS_NEEDED] || additional[A_GENERATE_POLYGON_LABEL_POINTS] || sst->uses_gamma || cluster_distance != 0) {
		sf.index = bbox_index;
	} else {
		sf.index = 0;
	}

	if (sst->layermap->count(sf.layername) == 0) {
		sst->layermap->insert(std::pair<std::string, layermap_entry>(sf.layername, layermap_entry(sst->layermap->size())));
	}

	auto ai = sst->layermap->find(sf.layername);
	if (ai != sst->layermap->end()) {
		sf.layer = ai->second.id;

		if (!sst->filters) {
			if (sf.t == VT_POINT) {
				ai->second.points++;
			} else if (sf.t == VT_LINE) {
				ai->second.lines++;
			} else if (sf.t == VT_POLYGON) {
				ai->second.polygons++;
			}
		}
	} else {
		fprintf(stderr, "Internal error: can't find layer name %s\n", sf.layername.c_str());
		exit(EXIT_IMPOSSIBLE);
	}

	for (auto &kv : set_attributes) {
		bool found = false;
		for (size_t i = 0; i < sf.full_keys.size(); i++) {
			if (sf.full_keys[i] == kv.first) {
				sf.full_values[i] = kv.second;
				found = true;
				break;
			}
		}

		if (!found) {
			sf.full_keys.push_back(kv.first);
			sf.full_values.push_back(kv.second);
		}
	}

	for (ssize_t i = (ssize_t) sf.full_keys.size() - 1; i >= 0; i--) {
		coerce_value(sf.full_keys[i], sf.full_values[i].type, sf.full_values[i].s, sst->attribute_types);

		if (prevent[P_SINGLE_PRECISION]) {
			if (sf.full_values[i].type == mvt_double) {
				// don't coerce integers to floats, since that is counterproductive
				if (sf.full_values[i].s.find('.') != std::string::npos) {
					sf.full_values[i].s = milo::dtoa_milo((float) atof(sf.full_values[i].s.c_str()));
				}
			}
		}

		if (sf.full_keys[i] == attribute_for_id) {
			if (sf.full_values[i].type != mvt_double && !additional[A_CONVERT_NUMERIC_IDS]) {
				static bool warned = false;

				if (!warned) {
					fprintf(stderr, "Warning: Attribute \"%s\"=\"%s\" as feature ID is not a number\n", sf.full_keys[i].c_str(), sf.full_values[i].s.c_str());
					warned = true;
				}
			} else {
				char *err;
				long long id_value = strtoull(sf.full_values[i].s.c_str(), &err, 10);

				if (err != NULL && *err != '\0') {
					static bool warned_frac = false;

					if (!warned_frac) {
						fprintf(stderr, "Warning: Can't represent non-integer feature ID %s\n", sf.full_values[i].s.c_str());
						warned_frac = true;
					}
				} else if (std::to_string(id_value) != strip_zeroes(sf.full_values[i].s)) {
					static bool warned = false;

					if (!warned) {
						fprintf(stderr, "Warning: Can't represent too-large feature ID %s\n", sf.full_values[i].s.c_str());
						warned = true;
					}
				} else {
					sf.id = id_value;
					sf.has_id = true;

					sf.full_keys.erase(sf.full_keys.begin() + i);
					sf.full_values.erase(sf.full_values.begin() + i);
					continue;
				}
			}
		}

		if (sst->exclude_all) {
			if (sst->include->count(sf.full_keys[i]) == 0) {
				sf.full_keys.erase(sf.full_keys.begin() + i);
				sf.full_values.erase(sf.full_values.begin() + i);
				continue;
			}
		} else if (sst->exclude->count(sf.full_keys[i]) != 0) {
			sf.full_keys.erase(sf.full_keys.begin() + i);
			sf.full_values.erase(sf.full_values.begin() + i);
			continue;
		}
	}

	if (!sst->filters) {
		for (size_t i = 0; i < sf.full_keys.size(); i++) {
			type_and_string attrib;
			attrib.type = sf.full_values[i].type;
			attrib.string = sf.full_values[i].s;

			auto fk = sst->layermap->find(sf.layername);
			add_to_file_keys(fk->second.file_keys, sf.full_keys[i], attrib);
		}
	}

	for (size_t i = 0; i < sf.full_keys.size(); i++) {
		sf.keys.push_back(addpool(r->poolfile, r->treefile, sf.full_keys[i].c_str(), mvt_string));
		sf.values.push_back(addpool(r->poolfile, r->treefile, sf.full_values[i].s.c_str(), sf.full_values[i].type));
	}

	long long geomstart = r->geompos;
	sf.geometry = scaled_geometry;

	std::string feature = serialize_feature(&sf, SHIFT_RIGHT(*(sst->initial_x)), SHIFT_RIGHT(*(sst->initial_y)));
	serialize_long_long(r->geomfile, feature.size(), &r->geompos, sst->fname);
	fwrite_check(feature.c_str(), sizeof(char), feature.size(), r->geomfile, &r->geompos, sst->fname);

	struct index index;
	index.start = geomstart;
	index.end = r->geompos;
	index.segment = sst->segment;
	index.seq = *(sst->layer_seq);
	index.t = sf.t;
	index.ix = bbox_index;

	fwrite_check(&index, sizeof(struct index), 1, r->indexfile, &r->indexpos, sst->fname);

	for (size_t i = 0; i < 2; i++) {
		if (sf.bbox[i] < r->file_bbox[i]) {
			r->file_bbox[i] = sf.bbox[i];
		}
	}
	for (size_t i = 2; i < 4; i++) {
		if (sf.bbox[i] > r->file_bbox[i]) {
			r->file_bbox[i] = sf.bbox[i];
		}
	}

	if (*(sst->progress_seq) % 10000 == 0) {
		checkdisk(sst->readers);
		if (!quiet && !quiet_progress && progress_time()) {
			fprintf(stderr, "Read %.2f million features\r", *sst->progress_seq / 1000000.0);
			fflush(stderr);
		}
	}
	(*(sst->progress_seq))++;
	(*(sst->layer_seq))++;

	return 1;
}

void coerce_value(std::string const &key, int &vt, std::string &val, std::map<std::string, int> const *attribute_types) {
	auto a = (*attribute_types).find(key);
	if (a != attribute_types->end()) {
		if (a->second == mvt_string) {
			vt = mvt_string;
		} else if (a->second == mvt_float) {
			vt = mvt_double;
			val = milo::dtoa_milo(atof(val.c_str()));
		} else if (a->second == mvt_int) {
			vt = mvt_double;
			if (val.size() == 0) {
				val = "0";
			}

			for (size_t ii = 0; ii < val.size(); ii++) {
				char c = val[ii];
				if (c < '0' || c > '9') {
					val = std::to_string(round(atof(val.c_str())));
					break;
				}
			}
		} else if (a->second == mvt_bool) {
			if (val == "false" || val == "0" || val == "null" || val.size() == 0 || (vt == mvt_double && atof(val.c_str()) == 0)) {
				vt = mvt_bool;
				val = "false";
			} else {
				vt = mvt_bool;
				val = "true";
			}
		} else {
			fprintf(stderr, "Can't happen: attribute type %d\n", a->second);
			exit(EXIT_IMPOSSIBLE);
		}
	}
}
