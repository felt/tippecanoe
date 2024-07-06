#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <cmath>
#include <atomic>
#include "projection.hpp"
#include "errors.hpp"

#define UINT_BITS 32

index_t (*encode_index)(unsigned long long wx, unsigned long long wy) = NULL;
void (*decode_index)(index_t index, unsigned long long *wx, unsigned long long *wy) = NULL;

struct projection projections[] = {
	{"EPSG:4326", lonlat2tile, tile2lonlat, "urn:ogc:def:crs:OGC:1.3:CRS84"},
	{"EPSG:3857", epsg3857totile, tiletoepsg3857, "urn:ogc:def:crs:EPSG::3857"},
	{NULL, NULL, NULL, NULL},
};

struct projection *projection = &projections[0];

// http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void lonlat2tile(double lon, double lat, int zoom, long long *x, long long *y) {
	// Place infinite and NaN coordinates off the edge of the Mercator plane

	int lat_class = std::fpclassify(lat);
	int lon_class = std::fpclassify(lon);
	bool bad_lon = false;

	if (lat_class == FP_INFINITE || lat_class == FP_NAN) {
		lat = 89.9;
	}
	if (lon_class == FP_INFINITE || lon_class == FP_NAN) {
		// Keep these far enough from the plane that they don't get
		// moved back into it by 360-degree offsetting

		lon = 720;
		bad_lon = true;
	}

	// Must limit latitude somewhere to prevent overflow.
	// 89.9 degrees latitude is 0.621 worlds beyond the edge of the flat earth,
	// hopefully far enough out that there are few expectations about the shape.
	if (lat < -89.9) {
		lat = -89.9;
	}
	if (lat > 89.9) {
		lat = 89.9;
	}

	if (lon < -360 && !bad_lon) {
		lon = -360;
	}
	if (lon > 360 && !bad_lon) {
		lon = 360;
	}

	double lat_rad = lat * M_PI / 180;
	unsigned long long n = 1LL << zoom;

	long long llx = std::round(n * ((lon + 180) / 360));
	long long lly = std::round(n * (1 - (log(tan(lat_rad) + 1 / cos(lat_rad)) / M_PI)) / 2);

	*x = llx;
	*y = lly;
}

// http://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
void tile2lonlat(long long x, long long y, int zoom, double *lon, double *lat) {
	unsigned long long n = 1LL << zoom;
	*lon = 360.0 * x / n - 180.0;
	*lat = atan(sinh(M_PI * (1 - 2.0 * y / n))) * 180.0 / M_PI;
}

void epsg3857totile(double ix, double iy, int zoom, long long *x, long long *y) {
	// Place infinite and NaN coordinates off the edge of the Mercator plane

	int iy_class = std::fpclassify(iy);
	int ix_class = std::fpclassify(ix);

	if (iy_class == FP_INFINITE || iy_class == FP_NAN) {
		iy = 40000000.0;
	}
	if (ix_class == FP_INFINITE || ix_class == FP_NAN) {
		ix = 40000000.0;
	}

	*x = std::round(ix * (1LL << (GLOBAL_DETAIL - 1)) / 6378137.0 / M_PI + (1LL << (GLOBAL_DETAIL - 1)));
	*y = std::round(((1LL << GLOBAL_DETAIL) - 1) - (iy * (1LL << (GLOBAL_DETAIL - 1)) / 6378137.0 / M_PI + (1LL << (GLOBAL_DETAIL - 1))));

	if (zoom != 0) {
		*x = std::round((double) *x / (1LL << (GLOBAL_DETAIL - zoom)));
		*y = std::round((double) *y / (1LL << (GLOBAL_DETAIL - zoom)));
	}
}

void tiletoepsg3857(long long ix, long long iy, int zoom, double *ox, double *oy) {
	if (zoom != 0) {
		ix <<= (GLOBAL_DETAIL - zoom);
		iy <<= (GLOBAL_DETAIL - zoom);
	}

	*ox = (ix - (1LL << (GLOBAL_DETAIL - 1))) * M_PI * 6378137.0 / (1LL << (GLOBAL_DETAIL - 1));
	*oy = ((1LL << GLOBAL_DETAIL) - 1 - iy - (1LL << (GLOBAL_DETAIL - 1))) * M_PI * 6378137.0 / (1LL << (GLOBAL_DETAIL - 1));
}

// https://en.wikipedia.org/wiki/Hilbert_curve

static void hilbert_rot(index_t n, unsigned long long *x, unsigned long long *y, index_t rx, index_t ry) {
	if (ry == 0) {
		if (rx == 1) {
			*x = n - 1 - *x;
			*y = n - 1 - *y;
		}

		unsigned long long t = *x;
		*x = *y;
		*y = t;
	}
}

static index_t hilbert_xy2d(index_t n, unsigned long long x, unsigned long long y) {
	index_t d = 0;
	index_t rx, ry;

	for (index_t s = n / 2; s > 0; s /= 2) {
		rx = (x & s) != 0;
		ry = (y & s) != 0;

		d += s * s * ((3 * rx) ^ ry);
		hilbert_rot(s, &x, &y, rx, ry);
	}

	return d;
}

static void hilbert_d2xy(index_t n, index_t d, unsigned long long *x, unsigned long long *y) {
	index_t rx, ry;
	index_t t = d;

	*x = *y = 0;
	for (index_t s = 1; s < n; s *= 2) {
		rx = 1 & (t / 2);
		ry = 1 & (t ^ rx);
		hilbert_rot(s, x, y, rx, ry);
		*x += s * rx;
		*y += s * ry;
		t /= 4;
	}
}

index_t encode_hilbert(unsigned long long wx, unsigned long long wy) {
	return hilbert_xy2d((index_t) 1 << 64, wx, wy);
}

void decode_hilbert(index_t index, unsigned long long *wx, unsigned long long *wy) {
	hilbert_d2xy((index_t) 1 << 64, index, wx, wy);
}

index_t encode_quadkey(unsigned long long wx, unsigned long long wy) {
	index_t out = 0;

	int i;
	for (i = 0; i < 64; i++) {
		index_t v = ((wx >> (64 - (i + 1))) & 1) << 1;
		v |= (wy >> (64 - (i + 1))) & 1;
		v = v << (128 - 2 * (i + 1));

		out |= v;
	}

	return out;
}

static std::atomic<unsigned char> decodex[256];
static std::atomic<unsigned char> decodey[256];

void decode_quadkey(index_t index, unsigned long long *wx, unsigned long long *wy) {
	static std::atomic<int> initialized(0);
	if (!initialized) {
		for (size_t ix = 0; ix < 256; ix++) {
			size_t xx = 0, yy = 0;

			for (size_t i = 0; i < UINT_BITS; i++) {
				xx |= ((ix >> (64 - 2 * (i + 1) + 1)) & 1) << (UINT_BITS - (i + 1));
				yy |= ((ix >> (64 - 2 * (i + 1) + 0)) & 1) << (UINT_BITS - (i + 1));
			}

			decodex[ix] = xx;
			decodey[ix] = yy;
		}

		initialized = 1;
	}

	*wx = *wy = 0;

	for (size_t i = 0; i < 16; i++) {
		*wx |= ((index_t) decodex[(index >> (8 * i)) & 0xFF]) << (4 * i);
		*wy |= ((index_t) decodey[(index >> (8 * i)) & 0xFF]) << (4 * i);
	}
}

unsigned coordinate_to_encodable(long long coord) {
	return (unsigned) (coord / (1LL << (GLOBAL_DETAIL - UINT_BITS)));
}

long long decoded_to_coordinate(unsigned coord) {
	return ((long long) coord) * (1LL << (GLOBAL_DETAIL - UINT_BITS));
}

void set_projection_or_exit(const char *optarg) {
	struct projection *p;
	for (p = projections; p->name != NULL; p++) {
		if (strcmp(p->name, optarg) == 0) {
			projection = p;
			break;
		}
		if (strcmp(p->alias, optarg) == 0) {
			projection = p;
			break;
		}
	}
	if (p->name == NULL) {
		fprintf(stderr, "Unknown projection (-s): %s\n", optarg);
		exit(EXIT_ARGS);
	}
}
