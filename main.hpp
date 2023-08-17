#ifndef MAIN_HPP
#define MAIN_HPP

#include <stddef.h>
#include <atomic>
#include <string>
#include <vector>
#include <map>

#include "json_logger.hpp"
#include "serial.hpp"

struct index {
	long long start = 0;
	long long end = 0;
	unsigned long long ix = 0;
	short segment = 0;
	unsigned short t : 2;
	unsigned long long seq : (64 - 18);  // pack with segment and t to stay in 32 bytes

	index()
	    : t(0),
	      seq(0) {
	}
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
};

extern std::vector<clipbbox> clipbboxes;

void checkdisk(std::vector<struct reader> *r);

extern int geometry_scale;
extern int quiet;
extern int quiet_progress;
extern json_logger logger;
extern double progress_interval;
extern std::atomic<double> last_progress;
extern int extra_detail;

extern size_t CPUS;
extern size_t TEMP_FILES;
extern size_t memsize;

extern size_t max_tile_size;
extern size_t max_tile_features;
extern int cluster_distance;
extern int cluster_maxzoom;
extern std::string attribute_for_id;
extern int tiny_polygon_size;
extern size_t limit_tile_feature_count;
extern size_t limit_tile_feature_count_at_maxzoom;
extern std::map<std::string, serial_val> set_attributes;
extern long long extend_zooms_max;

struct order_field {
	std::string name;
	bool descending;

	order_field(std::string _name, bool _descending)
	    : name(_name),
	      descending(_descending) {
	}
};

extern std::vector<order_field> order_by;

// not legal UTF-8, so can't appear as a real attribute name
#define ORDER_BY_SIZE "\200size"
extern bool order_by_size;

int mkstemp_cloexec(char *name);
FILE *fopen_oflag(const char *name, const char *mode, int oflag);
bool progress_time();

#define MAX_ZOOM 24

#endif
