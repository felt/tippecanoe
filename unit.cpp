#define CATCH_CONFIG_MAIN
#include "catch/catch.hpp"
#include "text.hpp"
#include "sort.hpp"
#include "tile-cache.hpp"
#include "mvt.hpp"
#include "projection.hpp"
#include "geometry.hpp"
#include <unistd.h>
#include <limits.h>
#include <filesystem>

namespace fs = std::filesystem;

TEST_CASE("UTF-8 enforcement", "[utf8]") {
	REQUIRE(check_utf8("") == std::string(""));
	REQUIRE(check_utf8("hello world") == std::string(""));
	REQUIRE(check_utf8("Καλημέρα κόσμε") == std::string(""));
	REQUIRE(check_utf8("こんにちは 世界") == std::string(""));
	REQUIRE(check_utf8("👋🌏") == std::string(""));
	REQUIRE(check_utf8("Hola m\xF3n") == std::string("\"Hola m\xF3n\" is not valid UTF-8 (0xF3 0x6E)"));
}

TEST_CASE("UTF-8 truncation", "[trunc]") {
	REQUIRE(truncate16("0123456789abcdefghi", 16) == std::string("0123456789abcdef"));
	REQUIRE(truncate16("0123456789éîôüéîôüç", 16) == std::string("0123456789éîôüéî"));
	REQUIRE(truncate16("0123456789😀😬😁😂😃😄😅😆", 16) == std::string("0123456789😀😬😁"));
	REQUIRE(truncate16("0123456789😀😬😁😂😃😄😅😆", 17) == std::string("0123456789😀😬😁"));
	REQUIRE(truncate16("0123456789あいうえおかきくけこさ", 16) == std::string("0123456789あいうえおか"));

	REQUIRE(truncate_string("789éîôüéîôüç", 3) == std::string("789"));
	REQUIRE(truncate_string("789éîôüéîôüç", 4) == std::string("789"));
	REQUIRE(truncate_string("789éîôüéîôüç", 5) == std::string("789é"));
	REQUIRE(truncate_string("789éîôüéîôüç", 6) == std::string("789é"));
	REQUIRE(truncate_string("789éîôüéîôüç", 7) == std::string("789éî"));
	REQUIRE(truncate_string("789éîôüéîôüç", 8) == std::string("789éî"));

	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 10) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 11) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 12) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 13) == std::string("0123456789"));
	REQUIRE(truncate_string("0123456789😀😬😁😂😃😄😅😆", 14) == std::string("0123456789😀"));

	REQUIRE(truncate_string("😀", 4) == std::string("😀"));
	REQUIRE(truncate_string("😀", 3) == std::string(""));
	REQUIRE(truncate_string("😀", 2) == std::string(""));
	REQUIRE(truncate_string("😀", 1) == std::string(""));
	REQUIRE(truncate_string("😀", 0) == std::string(""));
}

int intcmp(const void *v1, const void *v2) {
	return *((int *) v1) - *((int *) v2);
}

TEST_CASE("External quicksort", "fqsort") {
	std::vector<FILE *> inputs;

	size_t written = 0;
	for (size_t i = 0; i < 5; i++) {
		std::string tmpname = "/tmp/in.XXXXXXX";
		int fd = mkstemp(tmpname.data());
		fs::remove(tmpname);
		FILE *f = fdopen(fd, "w+b");
		inputs.emplace_back(f);
		size_t iterations = 2000 + rand() % 200;
		for (size_t j = 0; j < iterations; j++) {
			int n = rand();
			fwrite((void *) &n, sizeof(int), 1, f);
			written++;
		}
		rewind(f);
	}

	std::string tmpname = "/tmp/out.XXXXXX";
	int fd = mkstemp(tmpname.data());
	fs::remove(tmpname);
	FILE *f = fdopen(fd, "w+b");

	fqsort(inputs, sizeof(int), intcmp, f, 256);
	rewind(f);

	int prev = INT_MIN;
	int here;
	size_t nread = 0;
	while (fread((void *) &here, sizeof(int), 1, f)) {
		REQUIRE(here >= prev);
		prev = here;
		nread++;
	}

	fclose(f);
	REQUIRE(nread == written);
}

mvt_tile mock_get_tile(zxy tile) {
	mvt_layer l;
	l.name = std::to_string(tile.z) + "/" + std::to_string(tile.x) + "/" + std::to_string(tile.y);
	mvt_tile t;
	t.layers.push_back(l);
	return t;
}

TEST_CASE("Tile-join cache", "tile cache") {
	tile_cache tc;
	tc.capacity = 5;

	REQUIRE(tc.get(zxy(11, 327, 791), mock_get_tile).layers[0].name == "11/327/791");
	REQUIRE(tc.get(zxy(11, 5, 7), mock_get_tile).layers[0].name == "11/5/7");
	REQUIRE(tc.get(zxy(11, 5, 8), mock_get_tile).layers[0].name == "11/5/8");
	REQUIRE(tc.get(zxy(11, 5, 9), mock_get_tile).layers[0].name == "11/5/9");
	REQUIRE(tc.get(zxy(11, 5, 10), mock_get_tile).layers[0].name == "11/5/10");
	REQUIRE(tc.get(zxy(11, 327, 791), mock_get_tile).layers[0].name == "11/327/791");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 327, 791)) != tc.overzoom_cache.end());
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 7)) != tc.overzoom_cache.end());

	// verify that additional gets evict the least-recently-used elements

	REQUIRE(tc.get(zxy(11, 5, 11), mock_get_tile).layers[0].name == "11/5/11");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 7)) == tc.overzoom_cache.end());

	REQUIRE(tc.get(zxy(11, 5, 12), mock_get_tile).layers[0].name == "11/5/12");
	REQUIRE(tc.overzoom_cache.size() == 5);
	REQUIRE(tc.overzoom_cache.find(zxy(11, 5, 8)) == tc.overzoom_cache.end());
}

TEST_CASE("Bit reversal", "bit reversal") {
	REQUIRE(bit_reverse(1) == 0x8000000000000000);
	REQUIRE(bit_reverse(0x1234567812489BCF) == 0xF3D912481E6A2C48);
	REQUIRE(bit_reverse(0xF3D912481E6A2C48) == 0x1234567812489BCF);
}

TEST_CASE("mvt_geometry bbox") {
	std::vector<mvt_geometry> geom;

	geom.emplace_back(mvt_moveto, 128, 128);
	geom.emplace_back(mvt_lineto, 256, 256);

	long long xmin, ymin, xmax, ymax;
	get_bbox(geom, &xmin, &ymin, &xmax, &ymax, 11, 327, 791, 9);

	double lon, lat;
	tile2lonlat(xmin, ymin, 32, &lon, &lat);
	REQUIRE(std::to_string(lon) == "-122.475586");
	REQUIRE(std::to_string(lat) == "37.822802");

	tile2lonlat(xmax, ymax, 32, &lon, &lat);
	REQUIRE(std::to_string(lon) == "-122.431641");
	REQUIRE(std::to_string(lat) == "37.788081");

	unsigned long long start, end;
	get_quadkey_bounds(xmin, ymin, xmax, ymax, &start, &end);
	// 22 bits in common, for z11
	REQUIRE(start == 0x1c84fc0000000000);
	REQUIRE(end == 0x1c84ffffffffffff);
}

TEST_CASE("line_is_too_small") {
	drawvec dv;
	dv.emplace_back(VT_MOVETO, 4243099709, 2683872952);
	dv.emplace_back(VT_LINETO, 4243102487, 2683873977);
	dv.emplace_back(VT_MOVETO, -51867587, 2683872952);
	dv.emplace_back(VT_LINETO, -51864809, 2683873977);
	REQUIRE(line_is_too_small(dv, 0, 10));
}
