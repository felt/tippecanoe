#include <sstream>
#include <vector>
#include <string>
#include <cctype>
#include <stdlib.h>
#include <algorithm>
#include "geocsv.hpp"
#include "mvt.hpp"
#include "serial.hpp"
#include "projection.hpp"
#include "main.hpp"
#include "text.hpp"
#include "csv.hpp"
#include "milo/dtoa_milo.h"
#include "options.hpp"
#include "errors.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <cctype>

void trim(std::string &s) {
	s.erase(0, s.find_first_not_of(' '));
	s.erase(s.find_last_not_of(' ') + 1);
}

void processRing(const std::string &ring, drawvec &dv) {
	std::stringstream coordStream(ring);
	std::string coord;
	bool first = true;
	while (std::getline(coordStream, coord, ',')) {
		coord.erase(0, coord.find_first_not_of(' '));
		coord.erase(coord.find_last_not_of(' ') + 1);
		std::stringstream pointStream(coord);
		std::string xStr, yStr;
		std::getline(pointStream, xStr, ' ');
		std::getline(pointStream, yStr);
		long long wx, wy;
		double x = std::stod(xStr);
		double y = std::stod(yStr);
		projection->project(x, y, 32, &wx, &wy);
		if (first) {
			dv.push_back(draw(VT_MOVETO, wx, wy));
			first = false;
		} else {
			dv.push_back(draw(VT_LINETO, wx, wy));
		}
	}
	dv.push_back(draw(VT_CLOSEPATH, 0, 0));
}

drawvec parse_wkt(const std::string &wkt, drawvec &dv, int &geometry_type) {
	std::string type, coordinates;
	std::stringstream ss(wkt);

	// Read geometry type
	std::getline(ss, type, '(');
	type.erase(0, type.find_first_not_of(' '));
	type.erase(type.find_last_not_of(' ') + 1);

	// Read coordinates
	std::getline(ss, coordinates);
	coordinates = coordinates.substr(0, coordinates.size() - 1);
	coordinates.erase(0, coordinates.find_first_not_of(' '));
	coordinates.erase(coordinates.find_last_not_of(' ') + 1);

	std::stringstream coordStream(coordinates);

	if (type == "POINT") {
		geometry_type = VT_POINT;
		std::string xStr, yStr;
		std::getline(coordStream, xStr, ' ');
		std::getline(coordStream, yStr);
		trim(xStr);
		trim(yStr);
		long long wx, wy;
		double x = std::stod(xStr);
		double y = std::stod(yStr);
		projection->project(x, y, 32, &wx, &wy);
		dv.push_back(draw(VT_MOVETO, wx, wy));
	} else if (type == "LINESTRING") {
		geometry_type = VT_LINE;
		processRing(coordinates, dv);
	} else if (type == "POLYGON") {
		geometry_type = VT_POLYGON;
		// Handle POLYGON type with multiple rings
		std::vector<std::string> rings;
		std::string ring;
		int level = 0;
		for (char c : coordinates) {
			if (c == '(') {
				if (level == 0) ring.clear();
				level++;
			} else if (c == ')') {
				level--;
				if (level == 0) {
					trim(ring);
					rings.push_back(ring);
				}
			}
			if (level > 0 && c != '(') {
				ring += c;
			}
		}
		for (const std::string &currentRing : rings) {
			processRing(currentRing, dv);
		}
	}

	return dv;
}

void parse_geocsv(std::vector<struct serialization_state> &sst, std::string fname, int layer, std::string layername) {
	FILE *f;

	if (fname.size() == 0) {
		f = stdin;
	} else {
		f = fopen(fname.c_str(), "r");
		if (f == NULL) {
			perror(fname.c_str());
			exit(EXIT_OPEN);
		}
	}

	std::string s;
	std::vector<std::string> header;
	ssize_t latcol = -1, loncol = -1, geometrycol = -1;

	if ((s = csv_getline(f)).size() > 0) {
		std::string err = check_utf8(s);
		if (err != "") {
			fprintf(stderr, "%s: %s\n", fname.c_str(), err.c_str());
			exit(EXIT_UTF8);
		}

		header = csv_split(s.c_str());

		for (size_t i = 0; i < header.size(); i++) {
			header[i] = csv_dequote(header[i]);

			std::string lower(header[i]);
			std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

			if (lower == "y" || lower == "lat" || (lower.find("latitude") != std::string::npos)) {
				latcol = i;
			}
			if (lower == "x" || lower == "lon" || lower == "lng" || lower == "long" || (lower.find("longitude") != std::string::npos)) {
				loncol = i;
			}
			if (lower == "geometry" || lower == "wkt") {
				geometrycol = i;
			}
		}
	}

	if ((latcol < 0 || loncol < 0) && geometrycol < 0) {
		fprintf(stderr, "%s: Can't find \"lat\" and \"lon\" or \"geometry\" columns\n", fname.c_str());
		exit(EXIT_CSV);
	}

	size_t seq = 0;
	key_pool key_pool;
	while ((s = csv_getline(f)).size() > 0) {
		std::string err = check_utf8(s);
		if (err != "") {
			fprintf(stderr, "%s: %s\n", fname.c_str(), err.c_str());
			exit(EXIT_UTF8);
		}

		seq++;
		std::vector<std::string> line = csv_split(s.c_str());

		if (line.size() != header.size()) {
			fprintf(stderr, "%s:%zu: Mismatched column count: %zu in line, %zu in header\n", fname.c_str(), seq + 1, line.size(), header.size());
			exit(EXIT_CSV);
		}

		if ((line[loncol].empty() || line[latcol].empty()) && line[geometrycol].empty()) {
			static int warned = 0;
			if (!warned) {
				fprintf(stderr, "%s:%zu: null geometry (additional not reported)\n", fname.c_str(), seq + 1);
				warned = 1;
			}
			continue;
		}
		drawvec dv;
		int geometry_type = -1;
		if (latcol >= 0 && loncol >= 0) {
			double lon = atof(line[loncol].c_str());
			double lat = atof(line[latcol].c_str());

			long long x, y;
			projection->project(lon, lat, 32, &x, &y);
			dv.push_back(draw(VT_MOVETO, x, y));
			geometry_type = VT_POINT;
		} else if (geometrycol >= 0) {
			parse_wkt(csv_dequote(line[geometrycol]), dv, geometry_type);
		}

		std::vector<std::shared_ptr<std::string>> full_keys;
		std::vector<serial_val> full_values;

		for (size_t i = 0; i < line.size(); i++) {
			if (i != (size_t) latcol && i != (size_t) loncol && i != (size_t) geometrycol) {
				line[i] = csv_dequote(line[i]);

				serial_val sv;
				if (is_number(line[i])) {
					sv.type = mvt_double;
				} else if (line[i].size() == 0 && prevent[P_EMPTY_CSV_COLUMNS]) {
					sv.type = mvt_null;
					line[i] = "null";
				} else {
					sv.type = mvt_string;
				}
				sv.s = line[i];

				full_keys.push_back(key_pool.pool(header[i]));
				full_values.push_back(sv);
			}
		}

		serial_feature sf;

		sf.layer = layer;
		sf.segment = sst[0].segment;
		sf.has_id = false;
		sf.id = seq;
		sf.tippecanoe_minzoom = -1;
		sf.tippecanoe_maxzoom = -1;
		sf.feature_minzoom = false;
		sf.seq = *(sst[0].layer_seq);
		sf.geometry = dv;
		sf.t = geometry_type;
		sf.full_keys = full_keys;
		sf.full_values = full_values;

		serialize_feature(&sst[0], sf, layername);
	}

	if (fname.size() != 0) {
		if (fclose(f) != 0) {
			perror("fclose");
			exit(EXIT_CLOSE);
		}
	}
}
