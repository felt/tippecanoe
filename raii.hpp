#ifndef RAII_HPP
#define RAII_HPP

#include <memory>
#include <stdio.h>
#include "jsonpull/jsonpull.h"

// FILE* → unique_file
struct file_closer {
	void operator()(FILE *f) {
		if (f) {
			fclose(f);
		}
	}
};
using unique_file = std::unique_ptr<FILE, file_closer>;

// json_pull* → unique_json_pull
struct json_pull_closer {
	void operator()(json_pull *jp) {
		if (jp) {
			json_end(jp);
		}
	}
};
using unique_json_pull = std::unique_ptr<json_pull, json_pull_closer>;

#endif
