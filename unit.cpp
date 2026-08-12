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

	json_object *outer = nullptr;
	int arrays_seen = 0;

	json_object *j;
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
			// The completed outer array; the parser still owns it
			// via jp->root, so the borrowed pointer stays valid.
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
// with no parent, but the parser still owns it via jp->root.
// json_free must drop that parser reference too, otherwise the
// just-serialized feature would sit in memory until the next feature
// started parsing. Under the unique_ptr ownership model, the only
// owner is jp->root, so verifying that jp->root is empty after the
// json_free call is also a guarantee that the subtree itself has
// been destroyed.
TEST_CASE("json_free releases a top-level value held by the parser", "[jsonpull][memory]") {
	json_pull_ptr jp = json_begin_string(R"({"a": 1, "b": [2, 3]})");

	// json_read streams atoms first (1, 2, 3, [2,3], ...); the top-level
	// hash is returned by the final `}` token.
	json_object *top = nullptr;
	json_object *j;
	while ((j = json_read(jp)) != nullptr) {
		if (j->parent == nullptr) {
			top = j;
			break;
		}
	}

	REQUIRE(top != nullptr);
	REQUIRE(top->type == JSON_HASH);
	REQUIRE(jp->root.get() == top);

	json_free(top);
	// top is dangling now; do not dereference.

	REQUIRE(jp->root == nullptr);
}

// json_disconnect() is the documented way to splice a subtree out of the
// parser's tree and take ownership of it so that it can outlive the
// json_pull it came from. Nothing in tippecanoe calls it today -- the
// filter loaders get the same guarantee from json_read_tree, which clears
// back-pointers on the way out -- so cover it here rather than leave a
// documented ownership primitive untested.
TEST_CASE("json_disconnect hands a subtree to the caller", "[jsonpull][ownership]") {
	json_object_ptr taken;
	json_object *outer = nullptr;

	json_pull_ptr jp = json_begin_string("[[1, 2], [3, 4]]");

	int arrays_seen = 0;
	json_object *j;
	while ((j = json_read(jp)) != nullptr) {
		if (j->type != JSON_ARRAY) {
			continue;
		}
		arrays_seen++;
		if (arrays_seen == 2) {
			// This is [3, 4]; take it away from the enclosing array.
			taken = json_disconnect(j);
			REQUIRE(taken != nullptr);
			REQUIRE(taken.get() == j);
		} else if (j->parent == nullptr) {
			outer = j;
			break;
		}
	}

	// The outer array is left holding only the element we didn't take,
	// and the parser is still the owner of it.
	REQUIRE(outer != nullptr);
	REQUIRE(jp->root.get() == outer);
	REQUIRE(outer->array().size() == 1);
	REQUIRE(outer->array()[0]->array().size() == 2);
	REQUIRE(outer->array()[0]->array()[0]->number() == 1);
	REQUIRE(outer->array()[0]->array()[1]->number() == 2);

	// The detached subtree holds no pointers into the parser...
	REQUIRE(taken->parser == nullptr);
	REQUIRE(taken->array()[0]->parser == nullptr);
	REQUIRE(taken->array()[1]->parser == nullptr);

	// ...and its root no longer points out at the array it was spliced
	// from, but the parent links *within* it are left intact so the tree
	// stays navigable upwards.
	REQUIRE(taken->parent == nullptr);
	REQUIRE(taken->array()[0]->parent == taken.get());
	REQUIRE(taken->array()[1]->parent == taken.get());

	// So the subtree stays valid once the parser, and the tree the parser
	// still owns, are destroyed.
	jp.reset();
	// outer is dangling now; do not dereference.

	REQUIRE(taken->type == JSON_ARRAY);
	REQUIRE(taken->array().size() == 2);
	REQUIRE(taken->array()[0]->number() == 3);
	REQUIRE(taken->array()[1]->number() == 4);
}

// Preserving the parent links inside a detached tree is what lets
// json_free() keep working on its interior nodes: json_free() finds a
// node's owner through o->parent, so a detached tree whose parent links
// had been cleared would silently ignore the request and leave the node
// in place.
TEST_CASE("json_free prunes an interior node of a detached tree", "[jsonpull][ownership]") {
	json_pull_ptr jp = json_begin_string("[[1, 2], [3, 4], [5, 6]]");
	json_object_ptr tree = json_read_tree(jp);

	REQUIRE(tree != nullptr);
	REQUIRE(tree->type == JSON_ARRAY);
	REQUIRE(tree->array().size() == 3);

	// Destroy the parser first, so this is unambiguously operating on a
	// tree that no longer has one.
	jp.reset();
	REQUIRE(tree->parser == nullptr);

	json_object *drop = tree->array()[1].get();
	REQUIRE(drop->array()[0]->number() == 3);
	REQUIRE(drop->parent == tree.get());

	json_free(drop);
	// drop is dangling now; do not dereference.

	// Had the parent links been cleared on detach, json_free would not
	// have found an owner to splice the node out of, and the array would
	// still have three elements.
	REQUIRE(tree->array().size() == 2);
	REQUIRE(tree->array()[0]->array()[0]->number() == 1);
	REQUIRE(tree->array()[1]->array()[0]->number() == 5);
}

// The hash case is not a removal: json_free() of a hash value leaves the
// key in place with a JSON_NULL stand-in, so that detaching one half of a
// pair cannot disturb the key/value pairing of the entries around it. The
// entry is only erased once both halves are gone.
TEST_CASE("json_free of a hash value leaves a null placeholder", "[jsonpull][ownership]") {
	json_pull_ptr jp = json_begin_string(R"({"keep": 1, "drop": [2, 3]})");
	json_object_ptr tree = json_read_tree(jp);

	REQUIRE(tree != nullptr);
	REQUIRE(tree->entries().size() == 2);

	json_object *drop = json_hash_get(tree, "drop");
	REQUIRE(drop != nullptr);
	REQUIRE(drop->type == JSON_ARRAY);

	json_free(drop);
	// drop is dangling now; do not dereference.

	// The key survives, now paired with a null rather than the array.
	REQUIRE(tree->entries().size() == 2);
	json_object *after = json_hash_get(tree, "drop");
	REQUIRE(after != nullptr);
	REQUIRE(after->type == JSON_NULL);

	// The neighbouring pair is untouched.
	REQUIRE(json_hash_get(tree, "keep") != nullptr);
	REQUIRE(json_hash_get(tree, "keep")->number() == 1);
}

// A \uXXXX escape can only name a code point up to U+FFFF, and U+FFFF
// itself used to fall through the `< 0xFFFF` test into the four-byte
// branch, which emitted the overlong sequence F0 8F BF BF. check_utf8()
// only validates continuation-byte structure, so that invalid UTF-8 was
// copied into tiles unnoticed.
TEST_CASE("jsonpull encodes U+FFFF as three bytes", "[jsonpull][utf8]") {
	json_pull_ptr jp = json_begin_string("\"a\\uFFFFb\"");
	json_object_ptr o = json_read_tree(jp);

	REQUIRE(jp->error == nullptr);
	REQUIRE(o != nullptr);
	REQUIRE(o->type == JSON_STRING);

	REQUIRE(o->string() == "a\xEF\xBF\xBF" "b");
	REQUIRE(o->string() != "a\xF0\x8F\xBF\xBF" "b");

	// The boundary below it, and a genuine supplementary code point built
	// from a surrogate pair, both keep their existing encodings.
	json_pull_ptr jp2 = json_begin_string("\"\\uFFFE\"");
	json_object_ptr o2 = json_read_tree(jp2);
	REQUIRE(o2 != nullptr);
	REQUIRE(o2->string() == "\xEF\xBF\xBE");

	json_pull_ptr jp3 = json_begin_string("\"\\uD83D\\uDC00\"");
	json_object_ptr o3 = json_read_tree(jp3);
	REQUIRE(o3 != nullptr);
	REQUIRE(o3->string() == "\xF0\x9F\x90\x80");
}

TEST_CASE("Polygon cleaning drops a hole that no ring can parent", "[wagyu]") {
	// Two mutually reversed self-intersecting rings whose union leaves a hole
	// that wagyu's topology correction cannot assign to any surviving parent
	// ring (found by fuzzing; same failure as mapbox/tippecanoe#761). Without
	// the fix in mapbox/geometry/wagyu/topology_correction.hpp, this exits
	// through the "Could not properly place hole to a parent." handler in
	// clean_or_clip_poly instead of returning.
	static const std::vector<std::vector<std::pair<long long, long long>>> rings = {
	    {{0, 5}, {5, 4}, {5, 1}, {4, 4}, {4, 2}, {7, 1}, {0, 5}},
	    {{0, 5}, {7, 1}, {4, 2}, {4, 4}, {5, 1}, {5, 4}, {0, 0}, {0, 5}},
	};

	drawvec geom;
	for (auto const &ring : rings) {
		for (size_t i = 0; i < ring.size(); i++) {
			geom.push_back(draw(i == 0 ? VT_MOVETO : VT_LINETO, ring[i].first, ring[i].second));
		}
	}

	drawvec out = clean_or_clip_poly(geom, 0, 0, false, false);

	// The regression signal is getting here at all: without the fix,
	// clean_or_clip_poly exits the process from its wagyu error handler.
	SUCCEED("clean_or_clip_poly returned");

	// Anything that survives must be sanely wound: first ring positive.
	if (out.size() > 0) {
		size_t j = 1;
		while (j < out.size() && out[j].op == VT_LINETO) j++;
		REQUIRE(get_area(out, 0, j) > 0);
	}
}
