#ifndef PROJECTION_HPP
#define PROJECTION_HPP

#define GLOBAL_DETAIL 32
typedef unsigned long long index_t;

void lonlat2tile(double lon, double lat, int zoom, long long *x, long long *y);
void epsg3857totile(double ix, double iy, int zoom, long long *x, long long *y);
void tile2lonlat(long long x, long long y, int zoom, double *lon, double *lat);
void tiletoepsg3857(long long x, long long y, int zoom, double *ox, double *oy);
void set_projection_or_exit(const char *optarg);

struct projection {
	const char *name;
	void (*project)(double ix, double iy, int zoom, long long *ox, long long *oy);
	void (*unproject)(long long ix, long long iy, int zoom, double *ox, double *oy);
	const char *alias;
};

extern struct projection *projection;
extern struct projection projections[];

extern index_t (*encode_index)(unsigned int wx, unsigned int wy);
extern void (*decode_index)(index_t index, unsigned *wx, unsigned *wy);

index_t encode_quadkey(unsigned int wx, unsigned int wy);
void decode_quadkey(index_t index, unsigned *wx, unsigned *wy);

index_t encode_hilbert(unsigned int wx, unsigned int wy);
void decode_hilbert(index_t index, unsigned *wx, unsigned *wy);

unsigned coordinate_to_encodable(long long coord);
long long decoded_to_coordinate(unsigned coord);

#endif
