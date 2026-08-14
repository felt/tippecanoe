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

// A high surrogate followed by a non-surrogate used to be combined into one
// code point, because the range check tested the outer-loop byte instead of
// the parsed code unit. The stale surrogate should come out as standalone
// CESU-8, then U+E000 encoded normally.
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

// What geojson-loop does: free each feature once serialized, so they do not
// accumulate while the rest of the document is parsed.
//
// This shape cannot catch the array-splicing bug, at any size: json_read hands
// back each container as it completes, so the node freed here is always the
// last-added element of its parent, which the old memmove got right by moving
// zero bytes. "json_free prunes a non-final element" below covers that.
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

// A top-level value has no parent, so json_free has to drop the parser's
// reference instead. jp->root being empty afterwards is also proof the subtree
// was destroyed, since jp->root was its only owner.
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

// Nothing in tippecanoe calls json_disconnect today -- the filter loaders get
// the same guarantee from json_read_tree -- so cover it here rather than leave
// a documented ownership primitive untested.
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

// json_free finds a node's owner through o->parent, so clearing the interior
// parent links on detach would make this a silent no-op.
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

// Not a removal: the key stays with a JSON_NULL stand-in so the surrounding
// pairs keep their alignment, and the entry goes only when both halves do.
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

// The array-splicing fix. The old code passed an element count to memmove
// where a byte count was wanted, so pruning element 0 of eight left
// arr[0] == arr[1] -- one node owned twice -- and dropped the last element.
// Asserting each survivor's identity is what discriminates; checking only the
// size would not.
TEST_CASE("json_free prunes a non-final element", "[jsonpull][ownership]") {
	json_pull_ptr jp = json_begin_string("[[1], [2], [3], [4], [5], [6], [7], [8]]");
	json_object_ptr tree = json_read_tree(jp);

	REQUIRE(tree != nullptr);
	REQUIRE(tree->type == JSON_ARRAY);
	REQUIRE(tree->array().size() == 8);

	json_free(tree->array()[0].get());

	// Every survivor keeps its identity, in order, and nothing is aliased.
	REQUIRE(tree->array().size() == 7);
	for (size_t i = 0; i < tree->array().size(); i++) {
		json_object *e = tree->array()[i].get();
		REQUIRE(e->type == JSON_ARRAY);
		REQUIRE(e->array().size() == 1);
		REQUIRE(e->array()[0]->number() == (double) (i + 2));

		if (i + 1 < tree->array().size()) {
			REQUIRE(e != tree->array()[i + 1].get());
		}
	}
}

// The mirror of the value case above.
TEST_CASE("json_free of a hash key, then of both halves", "[jsonpull][ownership]") {
	json_pull_ptr jp = json_begin_string(R"({"a": 1, "b": 2, "c": 3})");
	json_object_ptr tree = json_read_tree(jp);

	REQUIRE(tree != nullptr);
	REQUIRE(tree->entries().size() == 3);

	// Free the key of the middle pair. The entry stays, with a null key,
	// and its value is still reachable positionally.
	json_free(tree->entries()[1].key.get());

	REQUIRE(tree->entries().size() == 3);
	REQUIRE(tree->entries()[1].key->type == JSON_NULL);
	REQUIRE(tree->entries()[1].value->number() == 2);

	// The neighbours are untouched, and the now-keyless pair is no longer
	// findable by name.
	REQUIRE(json_hash_get(tree, "b") == nullptr);
	REQUIRE(json_hash_get(tree, "a")->number() == 1);
	REQUIRE(json_hash_get(tree, "c")->number() == 3);

	// Freeing the other half too retires the whole entry.
	json_free(tree->entries()[1].value.get());

	REQUIRE(tree->entries().size() == 2);
	REQUIRE(json_hash_get(tree, "a")->number() == 1);
	REQUIRE(json_hash_get(tree, "c")->number() == 3);
}

// What the filter loaders and -L / -E do. Each detached tree has to survive
// the next json_read_tree call and the parser's destruction.
TEST_CASE("repeated json_read_tree on a line-delimited stream", "[jsonpull][ownership]") {
	json_pull_ptr jp = json_begin_string("{\"n\": 1}\n{\"n\": 2}\n{\"n\": 3}\n");

	std::vector<json_object_ptr> trees;
	for (int i = 0; i < 3; i++) {
		json_object_ptr t = json_read_tree(jp);
		REQUIRE(t != nullptr);
		REQUIRE(t->type == JSON_HASH);
		// Reading the next tree must not disturb the ones already taken.
		REQUIRE(json_hash_get(t, "n")->number() == i + 1);
		trees.push_back(std::move(t));
	}

	REQUIRE(json_read_tree(jp) == nullptr);

	// All three outlive the parser they came from.
	jp.reset();
	for (int i = 0; i < 3; i++) {
		REQUIRE(trees[i]->parser == nullptr);
		REQUIRE(json_hash_get(trees[i], "n")->number() == i + 1);
	}
}

// What json_context() prints on the error paths: a hash whose last key has no
// value yet renders that slot as "...".
TEST_CASE("json_stringify of a partially-parsed tree", "[jsonpull][stringify]") {
	json_pull_ptr jp = json_begin_string("{\"a\": [1, 2], \"b\":");

	// Read until the parser runs out of input mid-hash.
	while (json_read(jp) != nullptr) {
		;
	}
	REQUIRE(jp->error != nullptr);
	REQUIRE(jp->root != nullptr);

	std::string s = json_stringify(jp->root.get());
	REQUIRE(s == "{\"a\":[1,2],\"b\":...}");
}

// Values are std::string now, so an embedded NUL is legal and stringify has to
// walk past it rather than stop the way a c_str() loop would.
TEST_CASE("json_stringify keeps text after an embedded NUL", "[jsonpull][stringify]") {
	json_pull_ptr jp = json_begin_string("\"a\\u0000b\"");
	json_object_ptr o = json_read_tree(jp);

	REQUIRE(o != nullptr);
	REQUIRE(o->type == JSON_STRING);
	REQUIRE(o->string().size() == 3);

	std::string s = json_stringify(o.get());
	REQUIRE(s == "\"a\\u0000b\"");
}

// U+FFFF used to fall through the `< 0xFFFF` test into the four-byte branch
// and come out as the overlong F0 8F BF BF, which check_utf8 does not catch.
TEST_CASE("jsonpull encodes U+FFFF as three bytes", "[jsonpull][utf8]") {
	json_pull_ptr jp = json_begin_string("\"a\\uFFFFb\"");
	json_object_ptr o = json_read_tree(jp);

	REQUIRE(jp->error == nullptr);
	REQUIRE(o != nullptr);
	REQUIRE(o->type == JSON_STRING);

	REQUIRE(o->string() ==
		"a\xEF\xBF\xBF"
		"b");
	REQUIRE(o->string() !=
		"a\xF0\x8F\xBF\xBF"
		"b");

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
