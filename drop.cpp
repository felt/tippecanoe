#include <cmath>
#include "drop.hpp"
#include "options.hpp"
#include "geometry.hpp"

unsigned long long preserve_point_density_threshold = 0;

int calc_feature_minzoom(struct index *ix, struct drop_state ds[], int maxzoom, double gamma) {
	int feature_minzoom = 0;

	if (gamma >= 0 && (ix->t == VT_POINT ||
			   (additional[A_LINE_DROP] && ix->t == VT_LINE) ||
			   (additional[A_POLYGON_DROP] && ix->t == VT_POLYGON))) {
		for (ssize_t i = 0; i <= maxzoom; i++) {
			// This zoom level is now lighter on features
			ds[i].error -= 1.0;
		}

		ssize_t chosen = maxzoom + 1;
		for (ssize_t i = 0; i <= maxzoom; i++) {
			if (ds[i].error < 0) {
				// this zoom level is too light, so it is time to emit a feature.
				feature_minzoom = i;

				// this feature now appears in this zoom level and all higher zoom levels,
				// so each of them has this feature as its last feature, and each of them
				// is now one feature (which has a weight of `interval`) heavier than before.
				for (ssize_t j = i; j <= maxzoom; j++) {
					ds[j].previndex = ix->ix;
					ds[j].error += ds[j].interval;
				}

				chosen = i;
				break;
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

					// this feature now appears in this zoom level and all higher zoom levels
					// (below `chosen`, beyond which were already credited with this feature)
					// so each of them has this feature as its last feature, and each of them
					// is now one feature (which has a weight of `interval`) heavier than before.
					for (ssize_t j = i; j < chosen; j++) {
						ds[j].previndex = ix->ix;
						ds[j].error += ds[j].interval;
					}

					break;
				}
			}
		}
	}

	return feature_minzoom;
}

void prep_drop_states(struct drop_state ds[], int maxzoom, int basezoom, double droprate) {
	if (basezoom < 0) {
		basezoom = maxzoom;
	}

	// Needs to be signed for interval calculation
	for (ssize_t i = 0; i <= maxzoom; i++) {
		ds[i].previndex = 0;
		ds[i].interval = 1;  // every feature appears in every zoom level at or above the basezoom

		if (i < basezoom) {
			// at zoom levels below the basezoom, the fraction of points that are dropped is
			// the drop rate to the power of the number of zooms this zoom is below the basezoom
			//
			// for example:
			// basezoom:       1    (droprate ^ 0)
			// basezoom - 1:   2.5  (droprate ^ 1)
			// basezoom - 2:   6.25 (droprate ^ 2)
			// ...
			// basezoom - n:        (droprate ^ n)
			ds[i].interval = std::powf(droprate, basezoom - i);
		}

		ds[i].error = 0;
	}
}
