#define CATCH_CONFIG_MAIN
#include "catch/catch.hpp"
#include "text.hpp"
#include "drop.hpp"
#include "geometry.hpp"
#include "projection.hpp"

unsigned int additional[256] = {0};

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
}

TEST_CASE("index structure packing", "[index]") {
	REQUIRE(sizeof(struct index) == 32);
}

TEST_CASE("prep drop states", "[prep_drop_state]") {
	struct drop_state ds[25];

	prep_drop_states(ds, 24, 16, 2);
	REQUIRE(ds[24].interval == 1);
	REQUIRE(ds[17].interval == 1);
	REQUIRE(ds[16].interval == 1);
	REQUIRE(ds[15].interval == 2);
	REQUIRE(ds[14].interval == 4);
	REQUIRE(ds[0].interval == 65536);
}

TEST_CASE("select minzoom", "[calc_feature_minzoom]") {
    const int maxzoom = 3;

    struct drop_state ds[maxzoom + 1];
    struct index ix;
    long long wx, wy;
    int minzoom;

    prep_drop_states(ds, maxzoom, maxzoom, 2);
    ix.t = VT_POINT;
    
    lonlat2tile(-87.635236553223379, 41.84796128336411, 32, &wx, &wy); // Chicago
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Chicago %d\n", ix.ix, minzoom);

    lonlat2tile(-122.31532231603904, 47.600423319460475, 32, &wx, &wy); // Seattle
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Seattle %d\n", ix.ix, minzoom);

    lonlat2tile(-122.6819359, 45.5219697, 32, &wx, &wy); // Portland
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Portland %d\n", ix.ix, minzoom);

    lonlat2tile(-87.635236553223379, 41.84796128336411, 32, &wx, &wy); // Chicago
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Chicago %d\n", ix.ix, minzoom);

    lonlat2tile(-122.31532231603904, 47.600423319460475, 32, &wx, &wy); // Seattle
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Seattle %d\n", ix.ix, minzoom);

    lonlat2tile(-122.6819359, 45.5219697, 32, &wx, &wy); // Portland
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Portland %d\n", ix.ix, minzoom);

    lonlat2tile(-87.635236553223379, 41.84796128336411, 32, &wx, &wy); // Chicago
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Chicago %d\n", ix.ix, minzoom);

    lonlat2tile(-122.31532231603904, 47.600423319460475, 32, &wx, &wy); // Seattle
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Seattle %d\n", ix.ix, minzoom);

    lonlat2tile(-122.6819359, 45.5219697, 32, &wx, &wy); // Portland
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Portland %d\n", ix.ix, minzoom);

    lonlat2tile(-87.635236553223379, 41.84796128336411, 32, &wx, &wy); // Chicago
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Chicago %d\n", ix.ix, minzoom);

    lonlat2tile(-122.31532231603904, 47.600423319460475, 32, &wx, &wy); // Seattle
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Seattle %d\n", ix.ix, minzoom);

    lonlat2tile(-122.6819359, 45.5219697, 32, &wx, &wy); // Portland
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Portland %d\n", ix.ix, minzoom);

    lonlat2tile(-87.635236553223379, 41.84796128336411, 32, &wx, &wy); // Chicago
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Chicago %d\n", ix.ix, minzoom);

    lonlat2tile(-122.31532231603904, 47.600423319460475, 32, &wx, &wy); // Seattle
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Seattle %d\n", ix.ix, minzoom);

    lonlat2tile(-122.6819359, 45.5219697, 32, &wx, &wy); // Portland
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Portland %d\n", ix.ix, minzoom);

    lonlat2tile(-87.635236553223379, 41.84796128336411, 32, &wx, &wy); // Chicago
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Chicago %d\n", ix.ix, minzoom);

    lonlat2tile(-122.31532231603904, 47.600423319460475, 32, &wx, &wy); // Seattle
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Seattle %d\n", ix.ix, minzoom);

    lonlat2tile(-122.6819359, 45.5219697, 32, &wx, &wy); // Portland
    ix.ix = encode_hilbert(wx, wy);
    minzoom = calc_feature_minzoom(&ix, ds, maxzoom, 1);
    printf("%llu Portland %d\n", ix.ix, minzoom);
}
