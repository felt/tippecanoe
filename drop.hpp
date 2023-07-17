#ifndef DROP_HPP
#define DROP_HPP

// As features are read during the input phase, each one is represented by
// an index entry giving its geometry type, its spatial index, and the location
// in the geometry file of the rest of its data.
//
// Note that the fields are in a specific order so that `segment` and `t` will
// packed together with `seq` so that the total structure size will be only 32 bytes
// instead of 40. (Could we save a few more, perhaps, by tracking `len` instead of
// `end` and limiting the size of individual features to 32 bits?)

struct index {
	// first and last+1 byte of the feature in the geometry temp file
	long long start = 0;
	long long end = 0;

	// z-index or hilbert index of the feature
	unsigned long long ix = 0;

	// which thread's geometry temp file this feature is in
	short segment = 0;

	// geometry type
	unsigned short t : 2;

	// sequence number (sometimes with gaps in numbering) of the feature in the original input file
	unsigned long long seq : (64 - 18);  // pack with segment and t to stay in 32 bytes

	index()
	    : t(0),
	      seq(0) {
	}
};

struct drop_state {
	double gap;
	unsigned long long previndex;
	double interval;
	double seq;  // floating point because interval is
};

extern unsigned long long preserve_point_density_threshold;

int calc_feature_minzoom(struct index *ix, struct drop_state *ds, int maxzoom, double gamma);

#endif
