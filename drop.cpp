#include "drop.hpp"
#include "options.hpp"
#include "geometry.hpp"

unsigned long long preserve_point_density_threshold = 0;

int calc_feature_minzoom(struct index *ix, struct drop_state *ds, int maxzoom, double gamma) {
	int feature_minzoom = 0;

	if (gamma >= 0 && (ix->t == VT_POINT ||
			   (additional[A_LINE_DROP] && ix->t == VT_LINE) ||
			   (additional[A_POLYGON_DROP] && ix->t == VT_POLYGON))) {
		for (ssize_t i = maxzoom; i >= 0; i--) {
			ds[i].seq++;
		}
		ssize_t chosen = maxzoom + 1;
		for (ssize_t i = maxzoom; i >= 0; i--) {
			if (ds[i].seq < 0) {
				feature_minzoom = i + 1;

				// The feature we are pushing out
				// appears in zooms i + 1 through maxzoom,
				// so track where that was so we can make sure
				// not to cluster something else that is *too*
				// far away into it.
				for (ssize_t j = i + 1; j <= maxzoom; j++) {
					ds[j].previndex = ix->ix;
				}

				chosen = i + 1;
				break;
			} else {
				ds[i].seq -= ds[i].interval;
			}
		}

		// If this feature has been chosen only for a high zoom level,
		// check whether at a low zoom level it is nevertheless too far
		// from the last feature chosen for that low zoom, in which case
		// we will go ahead and push it out.

		if (preserve_point_density_threshold > 0) {
			for (ssize_t i = 0; i < chosen && i < maxzoom; i++) {
				if (ix->ix - ds[i].previndex > ((1LL << (32 - i)) / preserve_point_density_threshold) * ((1LL << (32 - i)) / preserve_point_density_threshold)) {
					feature_minzoom = i;

					for (ssize_t j = i; j <= maxzoom; j++) {
						ds[j].previndex = ix->ix;
					}

					break;
				}
			}
		}
	}

	return feature_minzoom;
}
