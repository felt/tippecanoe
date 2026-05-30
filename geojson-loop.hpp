#include <string>
#include "jsonpull/jsonpull.h"

struct json_feature_action {
	std::string fname;

	virtual int add_feature(json_object_ptr geometry, bool geometrycollection, json_object_ptr properties, json_object_ptr id, json_object_ptr tippecanoe, json_object_ptr feature) = 0;
	virtual void check_crs(json_object_ptr j) = 0;
};

void parse_json(json_feature_action *action, json_pull_ptr jp);
