#define CATCH_CONFIG_MAIN
#include "catch/catch.hpp"
#include "text.hpp"
#include "sort.hpp"
#include "tile-cache.hpp"
#include "mvt.hpp"
#include "projection.hpp"
#include "geometry.hpp"
#include "jsonpull/jsonpull.h"
#include <unistd.h>
#include <limits.h>

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
		int fd = mkstemp((char *) tmpname.c_str());
		unlink(tmpname.c_str());
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
	int fd = mkstemp((char *) tmpname.c_str());
	unlink(tmpname.c_str());
	FILE *f = fdopen(fd, "w+b");

	fqsort(inputs, sizeof(int), intcmp, f, 256, "/tmp");
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

TEST_CASE("line_is_too_small") {
	drawvec dv;
	dv.emplace_back(VT_MOVETO, 4243099709, 2683872952);
	dv.emplace_back(VT_LINETO, 4243102487, 2683873977);
	dv.emplace_back(VT_MOVETO, -51867587, 2683872952);
	dv.emplace_back(VT_LINETO, -51864809, 2683873977);
	REQUIRE(line_is_too_small(dv, 0, 10));
}

// Regression test for the surrogate-decoding bug that compared the leftover
// outer-loop byte `c` against `0xdfff` instead of the parsed code unit `ch`.
// For a string like "\uD83D\uE000" (a valid high surrogate followed by a
// non-surrogate BMP code point) the buggy version would mis-classify
// U+E000 as a low surrogate and combine the two units into the four-byte
// UTF-8 sequence F0 9F 90 80 (U+1F400). The fixed version flushes the
// stale high surrogate as standalone CESU-8 (ED A0 BD) and then encodes
// U+E000 normally as EE 80 80.
TEST_CASE("jsonpull surrogate-pair regression", "[jsonpull][surrogate]") {
	json_pull_ptr jp = json_begin_string("\"\\uD83D\\uE000\"");
	json_object_ptr o = json_read_tree(jp);

	REQUIRE(jp->error == nullptr);
	REQUIRE(o != nullptr);
	REQUIRE(o->type == JSON_STRING);

	const std::string expected = "\xED\xA0\xBD\xEE\x80\x80";
	REQUIRE(o->string() == expected);

	// Sanity check: the buggy output (a single 4-byte UTF-8 sequence for
	// U+1F400) must not be what we got.
	const std::string buggy = "\xF0\x9F\x90\x80";
	REQUIRE(o->string() != buggy);
}

// geojson-loop.cpp calls json_free(j) after jfa->add_feature has
// serialized the feature, intending to drop the JSON subtree from the
// in-progress parse tree so that already-serialized features don't sit
// in memory while subsequent features are parsed. That intent was
// never tested; this test pins it down. The pre-fix behavior of
// json_free was a bare unique_ptr/shared_ptr reset that only dropped
// the caller's local reference; the parent container kept the subtree
// alive, so memory grew until the top-level parse completed.
TEST_CASE("json_free prunes a subtree from its parent", "[jsonpull][memory]") {
	json_pull_ptr jp = json_begin_string("[[1, 2], [3, 4], [5, 6]]");

	json_object_ptr outer;
	int arrays_seen = 0;

	json_object_ptr j;
	while ((j = json_read(jp)) != nullptr) {
		if (j->type != JSON_ARRAY) {
			continue;
		}
		arrays_seen++;
		if (arrays_seen == 2) {
			// This is [3, 4]; verify, then ask the parser to drop it.
			REQUIRE(j->array().size() == 2);
			REQUIRE(j->array()[0]->number() == 3);
			REQUIRE(j->array()[1]->number() == 4);
			json_free(j);
		} else if (j->parent == nullptr) {
			// The completed outer array.
			outer = j;
			break;
		}
	}

	REQUIRE(outer != nullptr);
	REQUIRE(outer->type == JSON_ARRAY);
	REQUIRE(outer->array().size() == 2);

	// First surviving element: [1, 2].
	REQUIRE(outer->array()[0]->type == JSON_ARRAY);
	REQUIRE(outer->array()[0]->array().size() == 2);
	REQUIRE(outer->array()[0]->array()[0]->number() == 1);
	REQUIRE(outer->array()[0]->array()[1]->number() == 2);

	// Second surviving element (previously third): [5, 6].
	REQUIRE(outer->array()[1]->type == JSON_ARRAY);
	REQUIRE(outer->array()[1]->array().size() == 2);
	REQUIRE(outer->array()[1]->array()[0]->number() == 5);
	REQUIRE(outer->array()[1]->array()[1]->number() == 6);
}

// The companion case to the pruning test above: in a line-delimited
// stream, each feature returned by json_read is a top-level value
// with no parent, but the parser still co-owns it via jp->root.
// json_free must drop that parser reference too, otherwise the
// just-serialized feature stays in memory until the next feature
// starts parsing.
TEST_CASE("json_free releases a top-level value held by the parser", "[jsonpull][memory]") {
	std::weak_ptr<json_object> observer;

	json_pull_ptr jp = json_begin_string(R"({"a": 1, "b": [2, 3]})");
	json_object_ptr j = json_read_tree(jp);
	REQUIRE(j != nullptr);
	REQUIRE(j->type == JSON_HASH);
	REQUIRE(j->parent == nullptr);
	observer = j;

	json_free(j);

	REQUIRE(observer.expired());
}
