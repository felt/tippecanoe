#ifndef DROP_HPP
#define DROP_HPP

// As features are read during the input phase, each one is represented by
// an index entry giving its geometry type, its spatial index, and the location
// in the geometry file of the rest of its data.
//
// Note that the fields are in a specific order so that `segment` and `t` will
// packed together with `seq` so that the total structure size will be only 32 bytes
// instead of 40. (Could we save a few more, perhaps, by tracking `len` instead of
// `end` and limiting the size of individual features to 2^32 bytes?)

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
	unsigned long long seq : (64 - 16 - 2);	 // pack with segment and t to stay in 32 bytes

	index()
	    : t(0),
	      seq(0) {
	}
};

// Each zoom level has a drop_state that is used to account for the fraction of
// point features that are supposed to be dropped in that zoom level. As it goes
// through the spatially-sorted features, it is basically doing a diffusion dither
// to keep the density of features in each vicinity at each zoom level
// approximately correct by including or excluding individual features
// to maintain the balance.

struct drop_state {
	// the z-index or hilbert index of the last feature that was placed in this zoom level
	unsigned long long previndex;

	// the preservation rate (1 or more) for features in this zoom level.
	// 1 would be to keep all the features; 2 would drop every other feature;
	// 4 every fourth feature, and so on.
	double interval;

	// the current accumulated error in this zoom level:
	// positive if too many features have been dropped;
	// negative if not enough features have been dropped.
	//
	// this is floating-point because the interval is.
	double error;
};

extern unsigned long long preserve_point_density_threshold;

int calc_feature_minzoom(struct index *ix, struct drop_state ds[], int maxzoom, double gamma);
void prep_drop_states(struct drop_state ds[], int maxzoom, int basezoom, double droprate);

#endif
