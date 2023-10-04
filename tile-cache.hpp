#include <functional>
#include "mvt.hpp"
#include "dirtiles.hpp"	 // for zxy

struct mvt_tile_seq {
	mvt_tile tile;
	size_t seq;
};

struct tile_cache {
	std::map<zxy, mvt_tile_seq> overzoom_cache;
	std::atomic<size_t> seq;
	size_t capacity = 1000;

	mvt_tile get(zxy parent_tile, std::function<mvt_tile(zxy)> getter) {
		mvt_tile source;
		auto f = overzoom_cache.find(parent_tile);
		if (f == overzoom_cache.end()) {
			if (overzoom_cache.size() >= capacity) {
				// evict the oldest tile to make room

				auto to_erase = overzoom_cache.begin();
				for (auto here = overzoom_cache.begin(); here != overzoom_cache.end(); ++here) {
					if (here->second.seq < to_erase->second.seq) {
						to_erase = here;
					}
				}

				overzoom_cache.erase(to_erase);
			}

			source = getter(parent_tile);

			mvt_tile_seq to_cache;
			to_cache.tile = source;
			to_cache.seq = seq++;

			overzoom_cache.emplace(parent_tile, to_cache);
		} else {
			f->second.seq = seq++;
			source = f->second.tile;
		}

		return source;
	}
};
