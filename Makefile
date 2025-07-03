PREFIX ?= /usr/local
MANDIR ?= $(PREFIX)/share/man/man1/
BUILDTYPE ?= Release
BUILD_INFO ?=
SHELL = /bin/sh


# inherit from env if set
CC := $(CC)
CXX := $(CXX)
CFLAGS := $(CFLAGS) -fPIE -DBUILD_INFO=$(BUILD_INFO)
CXXFLAGS := $(CXXFLAGS) -std=c++17 -fPIE -DBUILD_INFO=$(BUILD_INFO)
LDFLAGS := $(LDFLAGS)
WARNING_FLAGS := -Wall -Wshadow -Wsign-compare -Wextra -Wunreachable-code -Wuninitialized -Wshadow
RELEASE_FLAGS := -O3 -DNDEBUG
DEBUG_FLAGS := -O0 -DDEBUG -fno-inline-functions -fno-omit-frame-pointer


OS := $(shell uname -s)
ifeq ($(OS),FreeBSD)
ADVSHELL = /usr/local/bin/bash
else
ADVSHELL = /bin/bash
endif


ifeq ($(BUILDTYPE),Release)
	FINAL_FLAGS := -g $(WARNING_FLAGS) $(RELEASE_FLAGS)
else
	FINAL_FLAGS := -g $(WARNING_FLAGS) $(DEBUG_FLAGS)
endif

all: tippecanoe tippecanoe-enumerate tippecanoe-decode tile-join unit tippecanoe-json-tool tippecanoe-overzoom

docs: man/tippecanoe.1

install: tippecanoe tippecanoe-enumerate tippecanoe-decode tile-join tippecanoe-json-tool tippecanoe-overzoom
	mkdir -p $(PREFIX)/bin
	mkdir -p $(MANDIR)
	cp tippecanoe $(PREFIX)/bin/tippecanoe
	cp tippecanoe-enumerate $(PREFIX)/bin/tippecanoe-enumerate
	cp tippecanoe-decode $(PREFIX)/bin/tippecanoe-decode
	cp tippecanoe-json-tool $(PREFIX)/bin/tippecanoe-json-tool
	cp tippecanoe-overzoom $(PREFIX)/bin/tippecanoe-overzoom
	cp tile-join $(PREFIX)/bin/tile-join
	cp man/tippecanoe.1 $(MANDIR)/tippecanoe.1

uninstall:
	rm $(PREFIX)/bin/tippecanoe $(PREFIX)/bin/tippecanoe-enumerate $(PREFIX)/bin/tippecanoe-decode $(PREFIX)/bin/tile-join $(MANDIR)/tippecanoe.1 $(PREFIX)/bin/tippecanoe-json-tool

man/tippecanoe.1: README.md
	md2man-roff README.md > man/tippecanoe.1

PG=

H = $(wildcard *.h) $(wildcard *.hpp)
C = $(wildcard *.c) $(wildcard *.cpp)

INCLUDES = -I/usr/local/include -I. -Iclipper2/include
LIBS = -L/usr/local/lib

tippecanoe: geojson.o jsonpull/jsonpull.o tile.o pool.o mbtiles.o geometry.o projection.o memfile.o mvt.o serial.o main.o platform.o text.o dirtiles.o pmtiles_file.o plugin.o read_json.o write_json.o geobuf.o flatgeobuf.o evaluator.o geocsv.o csv.o geojson-loop.o json_logger.o visvalingam.o compression.o clip.o sort.o attribute.o thread.o shared_borders.o clipper2/src/clipper.engine.o
	$(CXX) $(PG) $(LIBS) $(FINAL_FLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lm -lz -lsqlite3 -lpthread

tippecanoe-enumerate: enumerate.o
	$(CXX) $(PG) $(LIBS) $(FINAL_FLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lsqlite3

tippecanoe-decode: decode.o projection.o mvt.o write_json.o text.o jsonpull/jsonpull.o dirtiles.o pmtiles_file.o
	$(CXX) $(PG) $(LIBS) $(FINAL_FLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lm -lz -lsqlite3

tile-join: tile-join.o platform.o projection.o mbtiles.o mvt.o memfile.o dirtiles.o jsonpull/jsonpull.o text.o evaluator.o csv.o write_json.o pmtiles_file.o clip.o attribute.o thread.o read_json.o clipper2/src/clipper.engine.o
	$(CXX) $(PG) $(LIBS) $(FINAL_FLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lm -lz -lsqlite3 -lpthread

tippecanoe-json-tool: jsontool.o jsonpull/jsonpull.o csv.o text.o geojson-loop.o
	$(CXX) $(PG) $(LIBS) $(FINAL_FLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lm -lz -lsqlite3 -lpthread

unit: unit.o text.o sort.o mvt.o projection.o clip.o attribute.o jsonpull/jsonpull.o evaluator.o read_json.o clipper2/src/clipper.engine.o
	$(CXX) $(PG) $(LIBS) $(FINAL_FLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lm -lz -lsqlite3 -lpthread

tippecanoe-overzoom: overzoom.o mvt.o clip.o evaluator.o jsonpull/jsonpull.o text.o attribute.o read_json.o projection.o read_json.o clipper2/src/clipper.engine.o
	$(CXX) $(PG) $(LIBS) $(FINAL_FLAGS) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lm -lz -lsqlite3 -lpthread

-include $(wildcard *.d)

%.o: %.c
	$(CC) -MMD $(PG) $(INCLUDES) $(FINAL_FLAGS) $(CFLAGS) -c -o $@ $<

%.o: %.cpp
	$(CXX) -MMD $(PG) $(INCLUDES) $(FINAL_FLAGS) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f ./tippecanoe ./tippecanoe-* ./tile-join ./unit *.o *.d */*.o */*.d tests/**/*.mbtiles tests/**/*.check

indent:
	clang-format -i -style="{BasedOnStyle: Google, IndentWidth: 8, UseTab: Always, AllowShortIfStatementsOnASingleLine: false, ColumnLimit: 0, ContinuationIndentWidth: 8, SpaceAfterCStyleCast: true, IndentCaseLabels: false, AllowShortBlocksOnASingleLine: false, AllowShortFunctionsOnASingleLine: false, SortIncludes: false}" $(filter-out flatgeobuf.cpp,$(C)) $(H) jsonpull/*.[ch]

TESTS = $(wildcard tests/*/out/*.json)
SPACE = $(NULL) $(NULL)

test: tippecanoe tippecanoe-decode $(addsuffix .check,$(TESTS)) raw-tiles-test parallel-test pbf-test join-test enumerate-test decode-test join-filter-test unit json-tool-test allow-existing-test csv-test layer-json-test pmtiles-test decode-pmtiles-test overzoom-test accumulate-test
	./unit

suffixes = json json.gz

# Work around Makefile and filename punctuation limits:
# _ for argument-separator space
# %20 for quoted space
# %22 for quoted quote
# %2f for /
# %3a for :
# %5f for _
# %7b for {

testargs = \
    $(subst %20,' ',\
        $(subst %22,'"',\
            $(subst %3a,:,\
                $(subst %2f,/,\
                    $(subst %7b,'{',\
                        $(subst %5f,'_',\
                            $(subst _, ,$(1))))))))

%.json.check:
	./tippecanoe -q -a@ -f -o $@.mbtiles $(call testargs,$(patsubst %.json.check,%,$(word 4,$(subst /, ,$@)))) $(foreach suffix,$(suffixes),$(sort $(wildcard $(subst $(SPACE),/,$(wordlist 1,2,$(subst /, ,$@)))/*.$(suffix)))) < /dev/null
	./tippecanoe-decode -x generator $@.mbtiles > $@.out
	cmp $@.out $(patsubst %.check,%,$@)
	rm $@.out $@.mbtiles

# Don't test overflow with geobuf, because it fails (https://github.com/mapbox/geobuf/issues/87)
# Don't test stringids with geobuf, because it fails
nogeobuf = tests/overflow/out/-z0.json $(wildcard tests/stringid/out/*.json)
geobuf-test: tippecanoe-json-tool $(addsuffix .checkbuf,$(filter-out $(nogeobuf),$(TESTS)))

# For quicker address sanitizer build, hope that regular JSON parsing is tested enough by parallel and join tests
fewer-tests: tippecanoe tippecanoe-decode geobuf-test raw-tiles-test parallel-test pbf-test join-test enumerate-test decode-test join-filter-test unit

# XXX Use proper makefile rules instead of a for loop
%.json.checkbuf:
	for i in $(wildcard $(subst $(SPACE),/,$(wordlist 1,2,$(subst /, ,$@)))/*.json); do ./tippecanoe-json-tool -w $$i | ./node_modules/geobuf/bin/json2geobuf > $$i.geobuf; done
	for i in $(wildcard $(subst $(SPACE),/,$(wordlist 1,2,$(subst /, ,$@)))/*.json.gz); do gzip -dc $$i | ./tippecanoe-json-tool -w | ./node_modules/geobuf/bin/json2geobuf > $$i.geobuf; done
	./tippecanoe -q -a@ -f -o $@.mbtiles $(call testargs,$(patsubst %.json.checkbuf,%,$(word 4,$(subst /, ,$@)))) $(foreach suffix,$(suffixes),$(addsuffix .geobuf,$(sort $(wildcard $(subst $(SPACE),/,$(wordlist 1,2,$(subst /, ,$@)))/*.$(suffix))))) < /dev/null
	./tippecanoe-decode -x generator $@.mbtiles | sed 's/checkbuf/check/g' | sed 's/\.geobuf//g' > $@.out
	cmp $@.out $(patsubst %.checkbuf,%,$@)
	rm $@.out $@.mbtiles

parallel-test: $(eval SHELL:=$(ADVSHELL))
	mkdir -p tests/parallel
	perl -e 'for ($$i = 0; $$i < 20; $$i++) { $$lon = rand(360) - 180; $$lat = rand(180) - 90; $$k = rand(1); $$v = rand(1); print "{ \"type\": \"Feature\", \"properties\": { \"yes\": \"no\", \"who\": 1, \"$$k\": \"$$v\" }, \"geometry\": { \"type\": \"Point\", \"coordinates\": [ $$lon, $$lat ] } }\n"; }' > tests/parallel/in1.json
	perl -e 'for ($$i = 0; $$i < 300000; $$i++) { $$lon = rand(360) - 180; $$lat = rand(180) - 90; print "{ \"type\": \"Feature\", \"properties\": { }, \"geometry\": { \"type\": \"Point\", \"coordinates\": [ $$lon, $$lat ] } }\n"; }' > tests/parallel/in2.json
	perl -e 'for ($$i = 0; $$i < 20; $$i++) { $$lon = rand(360) - 180; $$lat = rand(180) - 90; print "{ \"type\": \"Feature\", \"properties\": { }, \"geometry\": { \"type\": \"Point\", \"coordinates\": [ $$lon, $$lat ] } }\n"; }' > tests/parallel/in3.json
	perl -e 'for ($$i = 0; $$i < 20; $$i++) { $$lon = rand(360) - 180; $$lat = rand(180) - 90; $$v = rand(1); print "{ \"type\": \"Feature\", \"properties\": { }, \"tippecanoe\": { \"layer\": \"$$v\" }, \"geometry\": { \"type\": \"Point\", \"coordinates\": [ $$lon, $$lat ] } }\n"; }' > tests/parallel/in4.json
	echo -n "" > tests/parallel/empty1.json
	echo "" > tests/parallel/empty2.json
	./tippecanoe -q -z5 -f -pi -l test -n test -o tests/parallel/linear-file.mbtiles tests/parallel/in[1234].json tests/parallel/empty[12].json
	./tippecanoe -q -z5 -f -pi -l test -n test -P -o tests/parallel/parallel-file.mbtiles tests/parallel/in[1234].json tests/parallel/empty[12].json
	cat tests/parallel/in[1234].json | ./tippecanoe -q -z5 -f -pi -l test -n test -o tests/parallel/linear-pipe.mbtiles
	cat tests/parallel/in[1234].json | ./tippecanoe -q -z5 -f -pi -l test -n test -P -o tests/parallel/parallel-pipe.mbtiles
	cat tests/parallel/in[1234].json | sed 's/^/@/' | tr '@' '\036' | ./tippecanoe -q -z5 -f -pi -l test -n test -o tests/parallel/implicit-pipe.mbtiles
	./tippecanoe -q -z5 -f -pi -l test -n test -P -o tests/parallel/parallel-pipes.mbtiles <(cat tests/parallel/in1.json) <(cat tests/parallel/empty1.json) <(cat tests/parallel/empty2.json) <(cat tests/parallel/in2.json) /dev/null <(cat tests/parallel/in3.json) <(cat tests/parallel/in4.json)
	./tippecanoe-decode -x generator -x generator_options tests/parallel/linear-file.mbtiles > tests/parallel/linear-file.json
	./tippecanoe-decode -x generator -x generator_options tests/parallel/parallel-file.mbtiles > tests/parallel/parallel-file.json
	./tippecanoe-decode -x generator -x generator_options tests/parallel/linear-pipe.mbtiles > tests/parallel/linear-pipe.json
	./tippecanoe-decode -x generator -x generator_options tests/parallel/parallel-pipe.mbtiles > tests/parallel/parallel-pipe.json
	./tippecanoe-decode -x generator -x generator_options tests/parallel/implicit-pipe.mbtiles > tests/parallel/implicit-pipe.json
	./tippecanoe-decode -x generator -x generator_options tests/parallel/parallel-pipes.mbtiles > tests/parallel/parallel-pipes.json
	cmp tests/parallel/linear-file.json tests/parallel/parallel-file.json
	cmp tests/parallel/linear-file.json tests/parallel/linear-pipe.json
	cmp tests/parallel/linear-file.json tests/parallel/parallel-pipe.json
	cmp tests/parallel/linear-file.json tests/parallel/implicit-pipe.json
	cmp tests/parallel/linear-file.json tests/parallel/parallel-pipes.json
	rm tests/parallel/*.mbtiles tests/parallel/*.json

raw-tiles-test: tippecanoe tippecanoe-decode tile-join
	./tippecanoe -q -f -e tests/raw-tiles/raw-tiles -r1 -pC tests/raw-tiles/hackspots.geojson
	./tippecanoe-decode -x generator tests/raw-tiles/raw-tiles > tests/raw-tiles/raw-tiles.json.check
	cmp tests/raw-tiles/raw-tiles.json.check tests/raw-tiles/raw-tiles.json
	# Test that -z and -Z work in tippecanoe-decode
	./tippecanoe-decode -x generator -Z6 -z7 tests/raw-tiles/raw-tiles > tests/raw-tiles/raw-tiles-z67.json.check
	cmp tests/raw-tiles/raw-tiles-z67.json.check tests/raw-tiles/raw-tiles-z67.json
	# Test that -z and -Z work in tile-join
	./tile-join -q -f -Z6 -z7 -e tests/raw-tiles/raw-tiles-z67 tests/raw-tiles/raw-tiles
	./tippecanoe-decode -x generator tests/raw-tiles/raw-tiles-z67 > tests/raw-tiles/raw-tiles-z67-join.json.check
	cmp tests/raw-tiles/raw-tiles-z67-join.json.check tests/raw-tiles/raw-tiles-z67-join.json
	rm -rf tests/raw-tiles/raw-tiles tests/raw-tiles/raw-tiles-z67 tests/raw-tiles/raw-tiles.json.check raw-tiles-z67.json.check tests/raw-tiles/raw-tiles-z67-join.json.check
	# Test that metadata.json is created even if all features are clipped away
	./tippecanoe -q -f -e tests/raw-tiles/nothing tests/raw-tiles/nothing.geojson
	./tippecanoe-decode -x generator tests/raw-tiles/nothing > tests/raw-tiles/nothing.json.check
	cmp tests/raw-tiles/nothing.json.check tests/raw-tiles/nothing.json
	rm -r tests/raw-tiles/nothing tests/raw-tiles/nothing.json.check

pmtiles-test: tippecanoe tippecanoe-decode tile-join
	./tippecanoe -q -f -o tests/pmtiles/hackspots.pmtiles -r1 -pC tests/raw-tiles/hackspots.geojson
	./tippecanoe-decode -x generator tests/pmtiles/hackspots.pmtiles > tests/pmtiles/hackspots.json.check
	cmp tests/pmtiles/hackspots.json.check tests/pmtiles/hackspots.json
	# Test generating pmtiles first and then converting to mbtiles with tile-join.
	./tile-join -q -f -pC -o tests/pmtiles/joined.mbtiles tests/pmtiles/hackspots.pmtiles
	./tippecanoe-decode -x generator tests/pmtiles/joined.mbtiles > tests/pmtiles/joined.json.check
	cmp tests/pmtiles/joined.json.check tests/pmtiles/joined.json
	rm -r tests/pmtiles/hackspots.json.check tests/pmtiles/hackspots.pmtiles

	# Test generating mbtiles first and then converting to pmtiles with tile-join. (Changes bounds)
	./tippecanoe -q -f -o tests/pmtiles/hackspots.mbtiles -r1 -pC tests/raw-tiles/hackspots.geojson
	./tile-join -q -f -pC -o tests/pmtiles/joined.pmtiles tests/pmtiles/hackspots.mbtiles

	# decode changes order (ZXY vs TMS order)
	./tippecanoe-decode -x generator tests/pmtiles/joined.pmtiles > tests/pmtiles/joined_reordered.json.check
	cmp tests/pmtiles/joined_reordered.json.check tests/pmtiles/joined_reordered.json
	rm -r tests/pmtiles/joined_reordered.json.check tests/pmtiles/hackspots.mbtiles tests/pmtiles/joined.pmtiles

	# From raw-tiles-test:
	./tippecanoe -q -f -o tests/raw-tiles/raw-tiles.pmtiles -r1 -pC tests/raw-tiles/hackspots.geojson
	./tippecanoe-decode -x generator tests/raw-tiles/raw-tiles.pmtiles | sed 's/\.pmtiles//g' | sed 's/ -o / -e /g' > tests/raw-tiles/raw-tiles.json.check
	cmp tests/raw-tiles/raw-tiles.json.check tests/raw-tiles/raw-tiles.json
	# Test that -z and -Z work in tippecanoe-decode
	./tippecanoe-decode -x generator -Z6 -z7 tests/raw-tiles/raw-tiles.pmtiles | sed 's/\.pmtiles//g' | sed 's/ -o / -e /g' > tests/raw-tiles/raw-tiles-z67.json.check
	cmp tests/raw-tiles/raw-tiles-z67.json.check tests/raw-tiles/raw-tiles-z67.json
	# Test that -z and -Z work in tile-join
	./tile-join -q -f -Z6 -z7 -o tests/raw-tiles/raw-tiles-z67.pmtiles tests/raw-tiles/raw-tiles.pmtiles
	./tippecanoe-decode -x generator tests/raw-tiles/raw-tiles-z67.pmtiles | sed 's/\.pmtiles//g' | sed 's/ -o / -e /g' > tests/raw-tiles/raw-tiles-z67-join.json.check
	cmp tests/raw-tiles/raw-tiles-z67-join.json.check tests/raw-tiles/raw-tiles-z67-join.json
	rm -rf tests/raw-tiles/raw-tiles.pmtiles tests/raw-tiles/raw-tiles-z67.pmtiles tests/raw-tiles/raw-tiles.json.check raw-tiles-z67.json.check tests/raw-tiles/raw-tiles-z67-join.json.check
	# Test that metadata.json is created even if all features are clipped away
	./tippecanoe -q -f -o tests/raw-tiles/nothing.pmtiles tests/raw-tiles/nothing.geojson
	./tippecanoe-decode -x generator tests/raw-tiles/nothing.pmtiles | sed 's/\.pmtiles//g' | sed 's/ -o / -e /g' > tests/raw-tiles/nothing.json.check
	cmp tests/raw-tiles/nothing.json.check tests/raw-tiles/nothing.json
	rm -r tests/raw-tiles/nothing.pmtiles tests/raw-tiles/nothing.json.check

decode-test: tippecanoe tippecanoe-decode
	mkdir -p tests/muni/decode
	./tippecanoe -q -z11 -Z11 -f -o tests/muni/decode/multi.mbtiles tests/muni/*.json
	./tippecanoe-decode -x generator -l subway tests/muni/decode/multi.mbtiles > tests/muni/decode/multi.mbtiles.json.check
	./tippecanoe-decode -x generator -l subway --integer tests/muni/decode/multi.mbtiles > tests/muni/decode/multi.mbtiles.integer.json.check
	./tippecanoe-decode -x generator -l subway --fraction tests/muni/decode/multi.mbtiles > tests/muni/decode/multi.mbtiles.fraction.json.check
	./tippecanoe-decode -x generator -c tests/muni/decode/multi.mbtiles > tests/muni/decode/multi.mbtiles.pipeline.json.check
	./tippecanoe-decode -x generator tests/muni/decode/multi.mbtiles 11 327 791 > tests/muni/decode/multi.mbtiles.onetile.json.check
	./tippecanoe-decode -x generator --stats tests/muni/decode/multi.mbtiles > tests/muni/decode/multi.mbtiles.stats.json.check
	cmp tests/muni/decode/multi.mbtiles.json.check tests/muni/decode/multi.mbtiles.json
	cmp tests/muni/decode/multi.mbtiles.integer.json.check tests/muni/decode/multi.mbtiles.integer.json
	cmp tests/muni/decode/multi.mbtiles.fraction.json.check tests/muni/decode/multi.mbtiles.fraction.json
	cmp tests/muni/decode/multi.mbtiles.pipeline.json.check tests/muni/decode/multi.mbtiles.pipeline.json
	cmp tests/muni/decode/multi.mbtiles.onetile.json.check tests/muni/decode/multi.mbtiles.onetile.json
	cmp tests/muni/decode/multi.mbtiles.stats.json.check tests/muni/decode/multi.mbtiles.stats.json
	rm -f tests/muni/decode/multi.mbtiles.json.check tests/muni/decode/multi.mbtiles tests/muni/decode/multi.mbtiles.pipeline.json.check tests/muni/decode/multi.mbtiles.stats.json.check tests/muni/decode/multi.mbtiles.onetile.json.check

decode-pmtiles-test: tippecanoe tippecanoe-decode
	mkdir -p tests/muni/decode
	./tippecanoe -q -z11 -Z11 -f -o tests/muni/decode/multi.pmtiles tests/muni/*.json
	./tippecanoe-decode -x generator -l subway tests/muni/decode/multi.pmtiles | sed 's/pmtiles/mbtiles/g' > tests/muni/decode/multi.pmtiles.json.check
	./tippecanoe-decode -x generator -l subway --integer tests/muni/decode/multi.pmtiles | sed 's/pmtiles/mbtiles/g' > tests/muni/decode/multi.pmtiles.integer.json.check
	./tippecanoe-decode -x generator -l subway --fraction tests/muni/decode/multi.pmtiles | sed 's/pmtiles/mbtiles/g' > tests/muni/decode/multi.pmtiles.fraction.json.check
	./tippecanoe-decode -x generator -c tests/muni/decode/multi.pmtiles | sed 's/pmtiles/mbtiles/g' > tests/muni/decode/multi.pmtiles.pipeline.json.check
	./tippecanoe-decode -x generator tests/muni/decode/multi.pmtiles 11 327 791 | sed 's/pmtiles/mbtiles/g' > tests/muni/decode/multi.pmtiles.onetile.json.check
	./tippecanoe-decode -x generator --stats tests/muni/decode/multi.pmtiles | sed 's/pmtiles/mbtiles/g' > tests/muni/decode/multi.pmtiles.stats.json.check
	cmp tests/muni/decode/multi.pmtiles.json.check tests/muni/decode/multi.mbtiles.json
	cmp tests/muni/decode/multi.pmtiles.integer.json.check tests/muni/decode/multi.mbtiles.integer.json
	cmp tests/muni/decode/multi.pmtiles.fraction.json.check tests/muni/decode/multi.mbtiles.fraction.json
	cmp tests/muni/decode/multi.pmtiles.pipeline.json.check tests/muni/decode/multi.mbtiles.pipeline.json
	cmp tests/muni/decode/multi.pmtiles.onetile.json.check tests/muni/decode/multi.mbtiles.onetile.json
	cmp tests/muni/decode/multi.pmtiles.stats.json.check tests/muni/decode/multi.mbtiles.stats.json
	rm -f tests/muni/decode/multi.pmtiles.json.check tests/muni/decode/multi.pmtiles tests/muni/decode/multi.pmtiles.pipeline.json.check tests/muni/decode/multi.pmtiles.stats.json.check tests/muni/decode/multi.pmtiles.onetile.json.check

pbf-test: tippecanoe-decode
	./tippecanoe-decode -x generator tests/pbf/11-328-791.vector.pbf 11 328 791 > tests/pbf/11-328-791.vector.pbf.out
	cmp tests/pbf/11-328-791.json tests/pbf/11-328-791.vector.pbf.out
	rm tests/pbf/11-328-791.vector.pbf.out
	./tippecanoe-decode -x generator -s EPSG:3857 tests/pbf/11-328-791.vector.pbf 11 328 791 > tests/pbf/11-328-791.3857.vector.pbf.out
	cmp tests/pbf/11-328-791.3857.json tests/pbf/11-328-791.3857.vector.pbf.out
	rm tests/pbf/11-328-791.3857.vector.pbf.out

enumerate-test: tippecanoe tippecanoe-enumerate
	./tippecanoe -q -z5 -f -o tests/ne_110m_admin_0_countries/out/enum.mbtiles tests/ne_110m_admin_0_countries/in.json.gz
	./tippecanoe-enumerate tests/ne_110m_admin_0_countries/out/enum.mbtiles > tests/ne_110m_admin_0_countries/out/enum.check
	cmp tests/ne_110m_admin_0_countries/out/enum.check tests/ne_110m_admin_0_countries/out/enum
	rm tests/ne_110m_admin_0_countries/out/enum.mbtiles tests/ne_110m_admin_0_countries/out/enum.check

overzoom-test: tippecanoe-overzoom
	# Basic operation
	./tippecanoe-overzoom -o tests/pbf/13-1310-3166.pbf tests/pbf/11-327-791.pbf 11/327/791 13/1310/3166
	./tippecanoe-decode tests/pbf/13-1310-3166.pbf 13 1310 3166 > tests/pbf/13-1310-3166.pbf.json.check
	cmp tests/pbf/13-1310-3166.pbf.json.check tests/pbf/13-1310-3166.pbf.json
	rm tests/pbf/13-1310-3166.pbf tests/pbf/13-1310-3166.pbf.json.check
	# Make sure feature order is stable
	# Large -b20 tile buffer to prevent altering the geometries by clipping
	./tippecanoe-overzoom -b20 --preserve-input-order -o tests/pbf/11-327-791-out.pbf tests/pbf/11-327-791.pbf 11/327/791 11/327/791
	./tippecanoe-decode tests/pbf/11-327-791.pbf 11 327 791 > tests/pbf/11-327-791.json
	./tippecanoe-decode tests/pbf/11-327-791-out.pbf 11 327 791 > tests/pbf/11-327-791-out.json
	cmp tests/pbf/11-327-791.json tests/pbf/11-327-791-out.json
	rm tests/pbf/11-327-791.json tests/pbf/11-327-791-out.json tests/pbf/11-327-791-out.pbf
	# Basic operation, multiple input form
	./tippecanoe-overzoom -o tests/pbf/13-1310-3166.pbf -t 13/1310/3166 tests/pbf/11-327-791.pbf 11/327/791
	./tippecanoe-decode tests/pbf/13-1310-3166.pbf 13 1310 3166 > tests/pbf/13-1310-3166.pbf.json.check
	cmp tests/pbf/13-1310-3166.pbf.json.check tests/pbf/13-1310-3166.pbf.json
	rm tests/pbf/13-1310-3166.pbf tests/pbf/13-1310-3166.pbf.json.check
	# Multiple inputs, no compression
	./tippecanoe-overzoom --no-tile-compression -o tests/pbf/13-1310-3166-ne.pbf -t 13/1310/3166 tests/pbf/11-327-791.pbf 11/327/791 tests/pbf/0-0-0.pbf 0/0/0
	./tippecanoe-decode tests/pbf/13-1310-3166-ne.pbf 13 1310 3166 > tests/pbf/13-1310-3166-ne.pbf.json.check
	cmp tests/pbf/13-1310-3166-ne.pbf.json.check tests/pbf/13-1310-3166-ne.pbf.json
	rm tests/pbf/13-1310-3166-ne.pbf tests/pbf/13-1310-3166-ne.pbf.json.check
	# Different detail and buffer, and attribute stripping
	./tippecanoe-overzoom -d8 -b30 -y NAME -y name -y scalerank -o tests/pbf/13-1310-3166-8-30.pbf tests/pbf/11-327-791.pbf 11/327/791 13/1310/3166
	./tippecanoe-decode tests/pbf/13-1310-3166-8-30.pbf 13 1310 3166 > tests/pbf/13-1310-3166-8-30.pbf.json.check
	cmp tests/pbf/13-1310-3166-8-30.pbf.json.check tests/pbf/13-1310-3166-8-30.pbf.json
	rm tests/pbf/13-1310-3166-8-30.pbf tests/pbf/13-1310-3166-8-30.pbf.json.check
	# No features in child tile
	./tippecanoe-overzoom -o tests/pbf/14-2616-6331.pbf tests/pbf/11-327-791.pbf 11/327/791 14/2616/6331
	cmp tests/pbf/14-2616-6331.pbf /dev/null
	rm tests/pbf/14-2616-6331.pbf
	# Thinning
	# 243 features in the source tile tests/pbf/0-0-0-pop.pbf
	# 9 of them survive as the best of each cluster of 30
	# ./tippecanoe -z1 -r30 --retain-points-multiplier 30 -f -e out.dir tests/ne_110m_populated_places/in.json
	# cp out.dir/0/0/0.pbf tests/pbf/0-0-0-pop.pbf
	./tippecanoe-overzoom -y NAME -m -o tests/pbf/0-0-0-pop-filtered.pbf tests/pbf/0-0-0-pop.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/0-0-0-pop-filtered.pbf 0 0 0 > tests/pbf/0-0-0-pop-filtered.pbf.json.check
	cmp tests/pbf/0-0-0-pop-filtered.pbf.json.check tests/pbf/0-0-0-pop-filtered.pbf.json
	rm tests/pbf/0-0-0-pop-filtered.pbf tests/pbf/0-0-0-pop-filtered.pbf.json.check
	# Thinning with accumulation
	./tippecanoe-overzoom -y NAME -m --accumulate-attribute NAME:comma -o tests/pbf/0-0-0-pop-accum.pbf tests/pbf/0-0-0-pop.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/0-0-0-pop-accum.pbf 0 0 0 > tests/pbf/0-0-0-pop-accum.pbf.json.check
	cmp tests/pbf/0-0-0-pop-accum.pbf.json.check tests/pbf/0-0-0-pop-accum.pbf.json
	rm tests/pbf/0-0-0-pop-accum.pbf tests/pbf/0-0-0-pop-accum.pbf.json.check
	# Filtering
	# 243 features in the source tile tests/pbf/0-0-0-pop.pbf
	# 27 of them match the filter and are retained
	./tippecanoe-overzoom -y NAME -j'{"*":["SCALERANK","eq",0]}' -o tests/pbf/0-0-0-pop-expr.pbf tests/pbf/0-0-0-pop.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/0-0-0-pop-expr.pbf 0 0 0 > tests/pbf/0-0-0-pop-expr.pbf.json.check
	cmp tests/pbf/0-0-0-pop-expr.pbf.json.check tests/pbf/0-0-0-pop-expr.pbf.json
	rm tests/pbf/0-0-0-pop-expr.pbf tests/pbf/0-0-0-pop-expr.pbf.json.check
	# Same filter test, but reading the filter from a file
	./tippecanoe-overzoom -y NAME -J tests/pbf/scalerank-0-filter.json -o tests/pbf/0-0-0-pop-expr.pbf tests/pbf/0-0-0-pop.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/0-0-0-pop-expr.pbf 0 0 0 > tests/pbf/0-0-0-pop-expr.pbf.json.check
	cmp tests/pbf/0-0-0-pop-expr.pbf.json.check tests/pbf/0-0-0-pop-expr.pbf.json
	rm tests/pbf/0-0-0-pop-expr.pbf tests/pbf/0-0-0-pop-expr.pbf.json.check
	# Filtering with multiplier
	# 243 features in the source tile tests/pbf/0-0-0-pop.pbf
	# 8 features survive into the output, from 9 clusters of 30
	./tippecanoe-overzoom -y NAME -y SCALERANK -j'{"*":["SCALERANK","eq",0]}' -m -o tests/pbf/0-0-0-filter-mult.pbf tests/pbf/0-0-0-pop.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/0-0-0-filter-mult.pbf 0 0 0 > tests/pbf/0-0-0-filter-mult.pbf.json.check
	cmp tests/pbf/0-0-0-filter-mult.pbf.json.check tests/pbf/0-0-0-filter-mult.pbf.json
	rm tests/pbf/0-0-0-filter-mult.pbf tests/pbf/0-0-0-filter-mult.pbf.json.check
	# Filtering with multiplier and preserve-input-order
	# 243 features in the source tile tests/pbf/0-0-0-pop.pbf
	./tippecanoe-overzoom -y NAME -y SCALERANK -j'{"*":["NAME","cn","e"]}' -m --preserve-input-order -o tests/pbf/0-0-0-filter-mult-order.pbf tests/pbf/0-0-0-pop.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/0-0-0-filter-mult-order.pbf 0 0 0 > tests/pbf/0-0-0-filter-mult-order.pbf.json.check
	cmp tests/pbf/0-0-0-filter-mult-order.pbf.json.check tests/pbf/0-0-0-filter-mult-order.pbf.json
	rm tests/pbf/0-0-0-filter-mult-order.pbf tests/pbf/0-0-0-filter-mult-order.pbf.json.check
	# Test that overzooming with a multiplier exactly reverses the effect of tiling with a multiplier
	./tippecanoe -q -z5 --preserve-point-density-threshold 8 --retain-points-multiplier 3 -f -e tests/muni/out/out.dir tests/muni/muni.json
	./tippecanoe -q -z5 --preserve-point-density-threshold 8 -f -o tests/muni/out/out.mbtiles tests/muni/muni.json
	./tippecanoe-overzoom -m -o tests/muni/out/out.dir/000.pbf tests/muni/out/out.dir/0/0/0.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/muni/out/out.mbtiles 0 0 0 > tests/muni/out/out.dir/direct.json
	./tippecanoe-decode tests/muni/out/out.dir/000.pbf 0 0 0 > tests/muni/out/out.dir/overzoomed.json
	cmp tests/muni/out/out.dir/overzoomed.json tests/muni/out/out.dir/direct.json
	rm -rf tests/muni/out/out.dir tests/muni/out/out.mbtiles tests/muni/out/out.dir/overzoomed.json tests/muni/out/out.dir/direct.json
	# Test filter with null attribute
	./tippecanoe-overzoom -j '{"*":["name","ni",[1,5,6,9]]}' -o tests/pbf/12-2145-1391-filter1.pbf tests/pbf/12-2145-1391.pbf 12/2145/1391 12/2145/1391
	./tippecanoe-decode tests/pbf/12-2145-1391-filter1.pbf 12 2145 1391 > tests/pbf/12-2145-1391-filter1.pbf.json.check
	cmp tests/pbf/12-2145-1391-filter1.pbf.json.check tests/pbf/12-2145-1391-filter1.pbf.json
	rm tests/pbf/12-2145-1391-filter1.pbf.json.check tests/pbf/12-2145-1391-filter1.pbf
	# Test filter with null attribute in "ni" list
	./tippecanoe-overzoom -j '{"*":["name","ni",[1,5,6,9,null]]}' -o tests/pbf/12-2145-1391-filter2.pbf tests/pbf/12-2145-1391.pbf 12/2145/1391 12/2145/1391
	./tippecanoe-decode tests/pbf/12-2145-1391-filter2.pbf 12 2145 1391 > tests/pbf/12-2145-1391-filter2.pbf.json.check
	cmp tests/pbf/12-2145-1391-filter2.pbf.json.check tests/pbf/12-2145-1391-filter2.pbf.json
	rm tests/pbf/12-2145-1391-filter2.pbf.json.check tests/pbf/12-2145-1391-filter2.pbf
	# Tiny polygon reduction
	./tippecanoe-overzoom --line-simplification=5 --tiny-polygon-size=50 -o tests/pbf/countries-0-0-0.pbf.out tests/pbf/countries-0-0-0.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/countries-0-0-0.pbf.out 0 0 0 > tests/pbf/countries-0-0-0.pbf.out.json.check
	cmp tests/pbf/countries-0-0-0.pbf.out.json.check tests/pbf/countries-0-0-0.pbf.out.json
	rm tests/pbf/countries-0-0-0.pbf.out tests/pbf/countries-0-0-0.pbf.out.json.check
	# Clipping to bounding box
	./tippecanoe-overzoom --clip-bounding-box 5,5,25.7,50 -o tests/pbf/countries-0-0-0-clip.pbf.out tests/pbf/countries-0-0-0.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/countries-0-0-0-clip.pbf.out 0 0 0 > tests/pbf/countries-0-0-0-clip.pbf.out.json.check
	cmp tests/pbf/countries-0-0-0-clip.pbf.out.json.check tests/pbf/countries-0-0-0-clip.pbf.out.json
	rm tests/pbf/countries-0-0-0-clip.pbf.out tests/pbf/countries-0-0-0-clip.pbf.out.json.check
	# Binning
	./tippecanoe-overzoom -o tests/pbf/bin-11-327-791.pbf.out --assign-to-bins tests/pbf/sf-zips.json tests/pbf/muni-11-327-791.pbf 11/327/791 11/327/791
	./tippecanoe-decode tests/pbf/bin-11-327-791.pbf.out 11 327 791 > tests/pbf/bin-11-327-791.pbf.out.json.check
	cmp tests/pbf/bin-11-327-791.pbf.out.json.check tests/pbf/bin-11-327-791.pbf.out.json
	rm tests/pbf/bin-11-327-791.pbf.out.json.check tests/pbf/bin-11-327-791.pbf.out
	# Binning by id
	./tippecanoe-overzoom -o tests/pbf/bin-11-327-791-ids.pbf.out --assign-to-bins tests/pbf/sf-zips.json --bin-by-id-list bin-ids tests/pbf/yearbuilt.pbf 11/327/791 11/327/791
	./tippecanoe-decode tests/pbf/bin-11-327-791-ids.pbf.out 11 327 791 > tests/pbf/bin-11-327-791-ids.pbf.out.json.check
	cmp tests/pbf/bin-11-327-791-ids.pbf.out.json.check tests/pbf/bin-11-327-791-ids.pbf.out.json
	rm tests/pbf/bin-11-327-791-ids.pbf.out.json.check tests/pbf/bin-11-327-791-ids.pbf.out
	# Binning by id, clipping by polygon
	./tippecanoe-overzoom -o tests/pbf/bin-11-327-791-ids-clip.pbf.out --clip-polygon='{"coordinates":[[[-122.4527379,37.8128815],[-122.4598853,37.7834743],[-122.4280914,37.7959397],[-122.4527379,37.8128815]]],"type":"Polygon"}' --assign-to-bins tests/pbf/sf-zips.json --bin-by-id-list bin-ids tests/pbf/yearbuilt.pbf 11/327/791 11/327/791
	./tippecanoe-decode tests/pbf/bin-11-327-791-ids-clip.pbf.out 11 327 791 > tests/pbf/bin-11-327-791-ids-clip.pbf.out.json.check
	cmp tests/pbf/bin-11-327-791-ids-clip.pbf.out.json.check tests/pbf/bin-11-327-791-ids-clip.pbf.out.json
	rm tests/pbf/bin-11-327-791-ids-clip.pbf.out.json.check tests/pbf/bin-11-327-791-ids-clip.pbf.out
	# Binning by id, clipping by polygon from file
	./tippecanoe-overzoom -o tests/pbf/bin-11-327-791-ids-clip.pbf.out --clip-polygon-file=tests/pbf/clip-poly.json --assign-to-bins tests/pbf/sf-zips.json --bin-by-id-list bin-ids tests/pbf/yearbuilt.pbf 11/327/791 11/327/791
	./tippecanoe-decode tests/pbf/bin-11-327-791-ids-clip.pbf.out 11 327 791 > tests/pbf/bin-11-327-791-ids-clip.pbf.out.json.check
	cmp tests/pbf/bin-11-327-791-ids-clip.pbf.out.json.check tests/pbf/bin-11-327-791-ids-clip.pbf.out.json
	rm tests/pbf/bin-11-327-791-ids-clip.pbf.out.json.check tests/pbf/bin-11-327-791-ids-clip.pbf.out
	# Binning by id, attribute stripping
	# Note that it still works even if we exclude the ID that we are binning by
	./tippecanoe-overzoom -yZCTA5CE10 -ytippecanoe:count -o tests/pbf/bin-11-327-791-ids-zip.pbf.out --assign-to-bins tests/pbf/sf-zips.json --bin-by-id-list bin-ids tests/pbf/yearbuilt.pbf 11/327/791 11/327/791
	./tippecanoe-decode tests/pbf/bin-11-327-791-ids-zip.pbf.out 11 327 791 > tests/pbf/bin-11-327-791-ids-zip.pbf.out.json.check
	cmp tests/pbf/bin-11-327-791-ids-zip.pbf.out.json.check tests/pbf/bin-11-327-791-ids-zip.pbf.out.json
	rm tests/pbf/bin-11-327-791-ids-zip.pbf.out.json.check tests/pbf/bin-11-327-791-ids-zip.pbf.out
	# Binning with longitude wraparound problems
	./tippecanoe-overzoom -o tests/pbf/0-0-0-pop-2-0-1.pbf.out --accumulate-numeric-attributes=tippecanoe --assign-to-bins tests/pbf/h3-2-0-1.geojson tests/pbf/0-0-0.pbf 2/0/1 2/0/1
	./tippecanoe-decode tests/pbf/0-0-0-pop-2-0-1.pbf.out 2 0 1 > tests/pbf/0-0-0-pop-2-0-1.pbf.out.json.check
	cmp tests/pbf/0-0-0-pop-2-0-1.pbf.out.json.check tests/pbf/0-0-0-pop-2-0-1.pbf.out.json
	rm tests/pbf/0-0-0-pop-2-0-1.pbf.out tests/pbf/0-0-0-pop-2-0-1.pbf.out.json.check
	./tippecanoe-overzoom -o tests/pbf/0-0-0-pop-1-1-0.pbf.out --accumulate-numeric-attributes=tippecanoe --assign-to-bins tests/pbf/h3-1-1-0.geojson tests/pbf/0-0-0.pbf 1/1/0 1/1/0
	./tippecanoe-decode tests/pbf/0-0-0-pop-1-1-0.pbf.out 1 1 0 > tests/pbf/0-0-0-pop-1-1-0.pbf.out.json.check
	cmp tests/pbf/0-0-0-pop-1-1-0.pbf.out.json.check tests/pbf/0-0-0-pop-1-1-0.pbf.out.json
	rm tests/pbf/0-0-0-pop-1-1-0.pbf.out tests/pbf/0-0-0-pop-1-1-0.pbf.out.json.check
	./tippecanoe-overzoom -o tests/pbf/0-0-0-pop-0-0-0.pbf.out --accumulate-numeric-attributes=tippecanoe --assign-to-bins tests/pbf/h3-0-0-0.geojson tests/pbf/0-0-0.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/0-0-0-pop-0-0-0.pbf.out 0 0 0 > tests/pbf/0-0-0-pop-0-0-0.pbf.out.json.check
	cmp tests/pbf/0-0-0-pop-0-0-0.pbf.out.json.check tests/pbf/0-0-0-pop-0-0-0.pbf.out.json
	rm tests/pbf/0-0-0-pop-0-0-0.pbf.out tests/pbf/0-0-0-pop-0-0-0.pbf.out.json.check
	# Binning, clipping to bounding box
	./tippecanoe-overzoom --clip-bounding-box 88,67.5,138,78 -o tests/pbf/0-0-0-pop-1-1-0-clip.pbf.out --accumulate-numeric-attributes=tippecanoe --assign-to-bins tests/pbf/h3-1-1-0.geojson tests/pbf/0-0-0.pbf 1/1/0 1/1/0
	./tippecanoe-decode tests/pbf/0-0-0-pop-1-1-0-clip.pbf.out 1 1 0 > tests/pbf/0-0-0-pop-1-1-0-clip.pbf.out.json.check
	cmp tests/pbf/0-0-0-pop-1-1-0-clip.pbf.out.json.check tests/pbf/0-0-0-pop-1-1-0-clip.pbf.out.json
	rm tests/pbf/0-0-0-pop-1-1-0-clip.pbf.out tests/pbf/0-0-0-pop-1-1-0-clip.pbf.out.json.check
	# Verify fix for crash
	./tippecanoe-overzoom '-o' tests/10188-crash/out.pbf '-t' '3/2/2' '--assign-to-bins' 'tests/10188-crash/bins.json' '--bin-by-id-list' 'felt:bin_features' '-b5' 'tests/10188-crash/2-0-0.pbf' '2/0/0'
	rm tests/10188-crash/out.pbf
	# Polygon clipping
	./tippecanoe-overzoom -o tests/pbf/countries-1-1-0-clip.pbf --clip-polygon "`cat tests/pbf/region.json`" tests/pbf/countries-1-1-0.pbf 1/1/0 1/1/0
	./tippecanoe-decode tests/pbf/countries-1-1-0-clip.pbf 1 1 0 > tests/pbf/countries-1-1-0-clip.json.check
	cmp tests/pbf/countries-1-1-0-clip.json.check tests/pbf/countries-1-1-0-clip.json
	rm tests/pbf/countries-1-1-0-clip.pbf tests/pbf/countries-1-1-0-clip.json.check
	# LineString clipping
	./tippecanoe-overzoom -o tests/pbf/roads-1-1-0-clip.pbf --clip-polygon "`cat tests/pbf/region.json`" tests/pbf/roads-1-1-0.pbf 1/1/0 1/1/0
	./tippecanoe-decode tests/pbf/roads-1-1-0-clip.pbf 1 1 0 > tests/pbf/roads-1-1-0-clip.json.check
	cmp tests/pbf/roads-1-1-0-clip.json.check tests/pbf/roads-1-1-0-clip.json
	rm tests/pbf/roads-1-1-0-clip.pbf tests/pbf/roads-1-1-0-clip.json.check
	# Point clipping
	./tippecanoe-overzoom -o tests/pbf/places-1-1-0-clip.pbf --clip-polygon "`cat tests/pbf/region.json`" tests/pbf/places-1-1-0.pbf 1/1/0 1/1/0
	./tippecanoe-decode tests/pbf/places-1-1-0-clip.pbf 1 1 0 > tests/pbf/places-1-1-0-clip.json.check
	cmp tests/pbf/places-1-1-0-clip.json.check tests/pbf/places-1-1-0-clip.json
	rm tests/pbf/places-1-1-0-clip.pbf tests/pbf/places-1-1-0-clip.json.check
	# Polygon clipping, with excessively large clip region
	./tippecanoe-overzoom -b10 -o tests/pbf/countries-8-135-86-bigclip.pbf --clip-polygon "`cat tests/pbf/region.json`" tests/pbf/countries-1-1-0.pbf 1/1/0 8/135/86
	./tippecanoe-decode tests/pbf/countries-8-135-86-bigclip.pbf 8 135 86 > tests/pbf/countries-8-135-86-bigclip.json.check
	cmp tests/pbf/countries-8-135-86-bigclip.json.check tests/pbf/countries-8-135-86-bigclip.json
	rm tests/pbf/countries-8-135-86-bigclip.pbf tests/pbf/countries-8-135-86-bigclip.json.check
	# Clip region that does not intersect with the tile
	./tippecanoe-overzoom -o tests/pbf/squirrels-13-2413-3077-clip.pbf --clip-polygon-file tests/pbf/squirrels-clip.json tests/pbf/squirrels-13-2413-3077.pbf 13/2413/3077 13/2413/3077
	cmp tests/pbf/squirrels-13-2413-3077-clip.pbf /dev/null  # clipped away
	rm tests/pbf/squirrels-13-2413-3077-clip.pbf
	# Deduplication by feature ID
	./tippecanoe -z0 -f -e tests/pbf/1.json.dir -l layer tests/pbf/1.json
	./tippecanoe -z0 -f -e tests/pbf/2.json.dir -l layer tests/pbf/2.json
	./tippecanoe-overzoom -b0 -o tests/pbf/merged-nodedup.pbf -t 1/1/0 tests/pbf/1.json.dir/0/0/0.pbf 0/0/0 tests/pbf/2.json.dir/0/0/0.pbf 0/0/0
	./tippecanoe-decode tests/pbf/merged-nodedup.pbf 1 1 0 > tests/pbf/merged-nodedup.pbf.json.check
	cmp tests/pbf/merged-nodedup.pbf.json.check tests/pbf/merged-nodedup.pbf.json
	./tippecanoe-overzoom -b0 --deduplicate-by-id -o tests/pbf/merged-dedup.pbf -t 1/1/0 tests/pbf/1.json.dir/0/0/0.pbf 0/0/0 tests/pbf/2.json.dir/0/0/0.pbf 0/0/0
	./tippecanoe-decode tests/pbf/merged-dedup.pbf 1 1 0 > tests/pbf/merged-dedup.pbf.json.check
	cmp tests/pbf/merged-dedup.pbf.json.check tests/pbf/merged-dedup.pbf.json
	rm -r tests/pbf/1.json.dir tests/pbf/2.json.dir tests/pbf/merged-nodedup.pbf tests/pbf/merged-nodedup.pbf.json.check tests/pbf/merged-dedup.pbf.json.check

join-test: tippecanoe tippecanoe-decode tile-join
	./tippecanoe -q -f -z12 -o tests/join-population/tabblock_06001420.mbtiles -YALAND10:'Land area' -L'{"file": "tests/join-population/tabblock_06001420.json", "description": "population"}'
	./tippecanoe -q -f -Z5 -z10 -o tests/join-population/macarthur.mbtiles -l macarthur tests/join-population/macarthur.json
	./tile-join -q -f -Z6 -z9 -o tests/join-population/macarthur-6-9.mbtiles tests/join-population/macarthur.mbtiles
	./tippecanoe-decode -x generator tests/join-population/macarthur-6-9.mbtiles > tests/join-population/macarthur-6-9.mbtiles.json.check
	cmp tests/join-population/macarthur-6-9.mbtiles.json.check tests/join-population/macarthur-6-9.mbtiles.json
	./tile-join -q -f -Z6 -z9 -X -o tests/join-population/macarthur-6-9-exclude.mbtiles tests/join-population/macarthur.mbtiles
	./tippecanoe-decode -x generator tests/join-population/macarthur-6-9-exclude.mbtiles > tests/join-population/macarthur-6-9-exclude.mbtiles.json.check
	cmp tests/join-population/macarthur-6-9-exclude.mbtiles.json.check tests/join-population/macarthur-6-9-exclude.mbtiles.json
	rm -f tests/join-population/macarthur-6-9.mbtiles.json.check tests/join-population/macarthur-6-9.mbtiles tests/join-population/macarthur-6-9-exclude.mbtiles.json.check tests/join-population/macarthur-6-9-exclude.mbtiles
	./tippecanoe -q -f -d10 -D10 -Z9 -z11 -o tests/join-population/macarthur2.mbtiles -l macarthur tests/join-population/macarthur2.json
	./tile-join --quiet --force -o tests/join-population/joined.mbtiles -x GEOID10 -c tests/join-population/population.csv tests/join-population/tabblock_06001420.mbtiles
	./tile-join --quiet --force -o tests/join-population/joined-null.mbtiles --empty-csv-columns-are-null -x GEOID10 -c tests/join-population/population.csv tests/join-population/tabblock_06001420.mbtiles
	./tile-join --quiet --force --no-tile-stats -o tests/join-population/joined-no-tile-stats.mbtiles -x GEOID10 -c tests/join-population/population.csv tests/join-population/tabblock_06001420.mbtiles
	./tile-join --quiet --force --tile-stats-attributes-limit=1 -o tests/join-population/joined-tile-stats-attributes-limit.mbtiles -x GEOID10 -c tests/join-population/population.csv tests/join-population/tabblock_06001420.mbtiles
	./tile-join --quiet --force --tile-stats-sample-values-limit=1 -o tests/join-population/joined-tile-stats-sample-values-limit.mbtiles -x GEOID10 -c tests/join-population/population.csv tests/join-population/tabblock_06001420.mbtiles
	./tile-join --quiet --force --tile-stats-values-limit=1 -o tests/join-population/joined-tile-stats-values-limit.mbtiles -x GEOID10 -c tests/join-population/population.csv tests/join-population/tabblock_06001420.mbtiles
	./tile-join -q -f -i -o tests/join-population/joined-i.mbtiles -x GEOID10 -c tests/join-population/population.csv tests/join-population/tabblock_06001420.mbtiles
	./tile-join -q -f -o tests/join-population/merged.mbtiles tests/join-population/tabblock_06001420.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur2.mbtiles
	./tile-join -q -f -c tests/join-population/windows.csv -o tests/join-population/windows.mbtiles tests/join-population/macarthur.mbtiles
	./tippecanoe-decode -x generator --maximum-zoom=11 --minimum-zoom=4 tests/join-population/joined.mbtiles > tests/join-population/joined.mbtiles.json.check
	./tippecanoe-decode -x generator --maximum-zoom=11 --minimum-zoom=4 tests/join-population/joined-null.mbtiles > tests/join-population/joined-null.mbtiles.json.check
	./tippecanoe-decode -x generator --maximum-zoom=11 --minimum-zoom=4 tests/join-population/joined-no-tile-stats.mbtiles > tests/join-population/joined-no-tile-stats.mbtiles.json.check
	./tippecanoe-decode -x generator --maximum-zoom=11 --minimum-zoom=4 tests/join-population/joined-tile-stats-attributes-limit.mbtiles > tests/join-population/joined-tile-stats-attributes-limit.mbtiles.json.check
	./tippecanoe-decode -x generator --maximum-zoom=11 --minimum-zoom=4 tests/join-population/joined-tile-stats-values-limit.mbtiles > tests/join-population/joined-tile-stats-values-limit.mbtiles.json.check
	./tippecanoe-decode -x generator --maximum-zoom=11 --minimum-zoom=4 tests/join-population/joined-tile-stats-sample-values-limit.mbtiles > tests/join-population/joined-tile-stats-sample-values-limit.mbtiles.json.check
	./tippecanoe-decode -x generator tests/join-population/joined-i.mbtiles > tests/join-population/joined-i.mbtiles.json.check
	./tippecanoe-decode -x generator tests/join-population/merged.mbtiles > tests/join-population/merged.mbtiles.json.check
	./tippecanoe-decode -x generator tests/join-population/windows.mbtiles > tests/join-population/windows.mbtiles.json.check
	cmp tests/join-population/joined.mbtiles.json.check tests/join-population/joined.mbtiles.json
	cmp tests/join-population/joined-null.mbtiles.json.check tests/join-population/joined-null.mbtiles.json
	cmp tests/join-population/joined-no-tile-stats.mbtiles.json.check tests/join-population/joined-no-tile-stats.mbtiles.json
	cmp tests/join-population/joined-tile-stats-attributes-limit.mbtiles.json.check tests/join-population/joined-tile-stats-attributes-limit.mbtiles.json
	cmp tests/join-population/joined-tile-stats-sample-values-limit.mbtiles.json.check tests/join-population/joined-tile-stats-sample-values-limit.mbtiles.json
	cmp tests/join-population/joined-tile-stats-values-limit.mbtiles.json.check tests/join-population/joined-tile-stats-values-limit.mbtiles.json
	cmp tests/join-population/joined-i.mbtiles.json.check tests/join-population/joined-i.mbtiles.json
	cmp tests/join-population/merged.mbtiles.json.check tests/join-population/merged.mbtiles.json
	cmp tests/join-population/windows.mbtiles.json.check tests/join-population/windows.mbtiles.json
	rm -f tests/join-population/joined-null.mbtiles tests/join-population/joined-null.mbtiles.json.check
	./tile-join -q -f -l macarthur -n "macarthur name" -N "macarthur description" -A "macarthur's attribution" -o tests/join-population/just-macarthur.mbtiles tests/join-population/merged.mbtiles
	./tile-join -q -f -L macarthur -o tests/join-population/no-macarthur.mbtiles tests/join-population/merged.mbtiles
	./tippecanoe-decode -x generator tests/join-population/just-macarthur.mbtiles > tests/join-population/just-macarthur.mbtiles.json.check
	./tippecanoe-decode -x generator tests/join-population/no-macarthur.mbtiles > tests/join-population/no-macarthur.mbtiles.json.check
	cmp tests/join-population/just-macarthur.mbtiles.json.check tests/join-population/just-macarthur.mbtiles.json
	cmp tests/join-population/no-macarthur.mbtiles.json.check tests/join-population/no-macarthur.mbtiles.json
	./tile-join -q --no-tile-compression -f -e tests/join-population/raw-merged-folder tests/join-population/tabblock_06001420.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur2.mbtiles
	./tippecanoe-decode -x generator tests/join-population/raw-merged-folder > tests/join-population/raw-merged-folder.json.check
	cmp tests/join-population/raw-merged-folder.json.check tests/join-population/raw-merged-folder.json
	rm -f tests/join-population/raw-merged-folder.json.check
	./tippecanoe -q -z12 -f -e tests/join-population/tabblock_06001420-folder -YALAND10:'Land area' -L'{"file": "tests/join-population/tabblock_06001420.json", "description": "population"}'
	./tippecanoe -q -Z5 -z10 -f -e tests/join-population/macarthur-folder -l macarthur tests/join-population/macarthur.json
	./tippecanoe -q -d10 -D10 -Z9 -z11 -f -e tests/join-population/macarthur2-folder -l macarthur tests/join-population/macarthur2.json
	./tile-join -q -f -o tests/join-population/merged-folder.mbtiles tests/join-population/tabblock_06001420-folder tests/join-population/macarthur-folder tests/join-population/macarthur2-folder
	./tippecanoe-decode -x generator tests/join-population/merged-folder.mbtiles > tests/join-population/merged-folder.mbtiles.json.check
	cmp tests/join-population/merged-folder.mbtiles.json.check tests/join-population/merged-folder.mbtiles.json
	./tile-join -q -n "merged name" -N "merged description" -f -e tests/join-population/merged-mbtiles-to-folder tests/join-population/tabblock_06001420.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur2.mbtiles
	./tile-join -q -n "merged name" -N "merged description" -f -e tests/join-population/merged-folders-to-folder tests/join-population/tabblock_06001420-folder tests/join-population/macarthur-folder tests/join-population/macarthur2-folder
	./tippecanoe-decode -x generator -x generator_options tests/join-population/merged-mbtiles-to-folder > tests/join-population/merged-mbtiles-to-folder.json.check
	./tippecanoe-decode -x generator -x generator_options tests/join-population/merged-folders-to-folder > tests/join-population/merged-folders-to-folder.json.check
	cmp tests/join-population/merged-mbtiles-to-folder.json.check tests/join-population/merged-folders-to-folder.json.check
	rm -f tests/join-population/merged-mbtiles-to-folder.json.check tests/join-population/merged-folders-to-folder.json.check
	./tile-join -q -f -c tests/join-population/windows.csv -o tests/join-population/windows-merged.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur2-folder
	./tile-join -q -c tests/join-population/windows.csv -f -e tests/join-population/windows-merged-folder tests/join-population/macarthur.mbtiles tests/join-population/macarthur2-folder
	./tile-join -q -f -o tests/join-population/windows-merged2.mbtiles tests/join-population/windows-merged-folder
	./tippecanoe-decode -x generator -x generator_options tests/join-population/windows-merged.mbtiles > tests/join-population/windows-merged.mbtiles.json.check
	./tippecanoe-decode -x generator -x generator_options tests/join-population/windows-merged2.mbtiles > tests/join-population/windows-merged2.mbtiles.json.check
	cmp tests/join-population/windows-merged.mbtiles.json.check tests/join-population/windows-merged2.mbtiles.json.check
	./tile-join -q -f -o tests/join-population/macarthur-and-macarthur2-merged.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur2-folder
	./tile-join -q -f -e tests/join-population/macarthur-and-macarthur2-folder tests/join-population/macarthur.mbtiles tests/join-population/macarthur2-folder
	./tile-join -q -f -o tests/join-population/macarthur-and-macarthur2-merged2.mbtiles tests/join-population/macarthur-and-macarthur2-folder
	./tippecanoe-decode -x generator -x generator_options tests/join-population/macarthur-and-macarthur2-merged.mbtiles > tests/join-population/macarthur-and-macarthur2-merged.mbtiles.json.check
	./tippecanoe-decode -x generator -x generator_options tests/join-population/macarthur-and-macarthur2-merged2.mbtiles > tests/join-population/macarthur-and-macarthur2-merged2.mbtiles.json.check
	cmp tests/join-population/macarthur-and-macarthur2-merged.mbtiles.json.check tests/join-population/macarthur-and-macarthur2-merged2.mbtiles.json.check
	rm tests/join-population/tabblock_06001420.mbtiles tests/join-population/joined.mbtiles tests/join-population/joined-i.mbtiles tests/join-population/joined.mbtiles.json.check tests/join-population/joined-i.mbtiles.json.check tests/join-population/macarthur.mbtiles tests/join-population/merged.mbtiles tests/join-population/merged.mbtiles.json.check  tests/join-population/merged-folder.mbtiles tests/join-population/macarthur2.mbtiles tests/join-population/windows.mbtiles tests/join-population/windows-merged.mbtiles tests/join-population/windows-merged2.mbtiles tests/join-population/windows.mbtiles.json.check tests/join-population/just-macarthur.mbtiles tests/join-population/no-macarthur.mbtiles tests/join-population/just-macarthur.mbtiles.json.check tests/join-population/no-macarthur.mbtiles.json.check tests/join-population/merged-folder.mbtiles.json.check tests/join-population/windows-merged.mbtiles.json.check tests/join-population/windows-merged2.mbtiles.json.check tests/join-population/macarthur-and-macarthur2-merged.mbtiles tests/join-population/macarthur-and-macarthur2-merged2.mbtiles tests/join-population/macarthur-and-macarthur2-merged.mbtiles.json.check tests/join-population/macarthur-and-macarthur2-merged2.mbtiles.json.check
	rm -rf tests/join-population/raw-merged-folder tests/join-population/tabblock_06001420-folder tests/join-population/macarthur-folder tests/join-population/macarthur2-folder tests/join-population/merged-mbtiles-to-folder tests/join-population/merged-folders-to-folder tests/join-population/windows-merged-folder tests/join-population/macarthur-and-macarthur2-folder
	# Test renaming of layers
	./tippecanoe -q -f -Z5 -z10 -o tests/join-population/macarthur.mbtiles -l macarthur1 tests/join-population/macarthur.json
	./tippecanoe -q -f -Z5 -z10 -o tests/join-population/macarthur2.mbtiles -l macarthur2 tests/join-population/macarthur2.json
	./tile-join -q -R macarthur1:one --rename-layer=macarthur2:two -f -o tests/join-population/renamed.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur2.mbtiles
	./tippecanoe-decode -x generator tests/join-population/renamed.mbtiles > tests/join-population/renamed.mbtiles.json.check
	cmp tests/join-population/renamed.mbtiles.json.check tests/join-population/renamed.mbtiles.json
	rm -f tests/join-population/renamed.mbtiles.json.check tests/join-population/renamed.mbtiles.json.check tests/join-population/macarthur.mbtiles tests/join-population/macarthur2.mbtiles
	# Make sure the concatenated name isn't too long
	./tippecanoe -q -f -z0 -n 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' -o tests/join-population/macarthur.mbtiles tests/join-population/macarthur.json
	./tile-join -f -o tests/join-population/concat.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur.mbtiles tests/join-population/macarthur.mbtiles
	./tippecanoe-decode -x generator tests/join-population/concat.mbtiles > tests/join-population/concat.mbtiles.json.check
	cmp tests/join-population/concat.mbtiles.json.check tests/join-population/concat.mbtiles.json
	rm tests/join-population/concat.mbtiles.json.check tests/join-population/concat.mbtiles tests/join-population/macarthur.mbtiles
	# Test reading list of input files from file
	./tippecanoe -q -f -Z5 -z10 -o tests/readfile/macarthur.mbtiles -l macarthur1 tests/join-population/macarthur.json
	./tippecanoe -q -f -Z5 -z10 -o tests/readfile/macarthur2.mbtiles -l macarthur2 tests/join-population/macarthur2.json
	./tile-join -q -R macarthur1:one --rename-layer=macarthur2:two -f -o tests/readfile/renamed.mbtiles tests/readfile/macarthur.mbtiles tests/readfile/macarthur2.mbtiles
	./tippecanoe-decode -x generator -x generator_options tests/readfile/renamed.mbtiles > tests/readfile/renamed.mbtiles.json.check
	./tile-join -q -R macarthur1:one --rename-layer=macarthur2:two -f -r tests/readfile/readfile.list -o tests/readfile/readfile.mbtiles
	./tippecanoe-decode -x generator -x generator_options tests/readfile/readfile.mbtiles > tests/readfile/readfile.mbtiles.json.check
	cmp tests/readfile/renamed.mbtiles.json.check tests/readfile/readfile.mbtiles.json.check
	rm tests/readfile/renamed.mbtiles.json.check tests/readfile/readfile.mbtiles.json.check  tests/readfile/readfile.mbtiles tests/readfile/renamed.mbtiles
	#`
	# Make sure empty tilesets work
	#
	# mbtiles:
	./tippecanoe -q -z0 -f -o tests/join-population/empty.mbtiles tests/join-population/empty.json
	./tile-join -f -o tests/join-population/empty.out.mbtiles tests/join-population/empty.mbtiles
	./tippecanoe-decode -x generator -x generator_options -x name -x description tests/join-population/empty.mbtiles > tests/join-population/empty.out.json.check
	cmp tests/join-population/empty.out.json.check tests/join-population/empty.out.json
	rm -f tests/join-population/empty.mbtiles tests/join-population/empty.out.mbtiles tests/join-population/empty.out.json.check
	# pmtiles:
	./tippecanoe -q -z0 -f -o tests/join-population/empty.pmtiles tests/join-population/empty.json
	./tile-join -f -o tests/join-population/empty.out.pmtiles tests/join-population/empty.pmtiles
	./tippecanoe-decode -x generator -x generator_options -x name -x description tests/join-population/empty.pmtiles > tests/join-population/empty.out.json.check
	cmp tests/join-population/empty.out.json.check tests/join-population/empty.out.json
	rm -f tests/join-population/empty.pmtiles tests/join-population/empty.out.pmtiles tests/join-population/empty.out.json.check
	# pmtiles again, with --overzoom
	./tippecanoe -q -z0 -f -o tests/join-population/empty.pmtiles tests/join-population/empty.json
	./tile-join --overzoom -f -o tests/join-population/empty.out.pmtiles tests/join-population/empty.pmtiles
	./tippecanoe-decode -x generator -x generator_options -x name -x description tests/join-population/empty.pmtiles > tests/join-population/empty.out.json.check
	cmp tests/join-population/empty.out.json.check tests/join-population/empty.out.json
	rm -f tests/join-population/empty.pmtiles tests/join-population/empty.out.pmtiles tests/join-population/empty.out.json.check
	# dirtiles:
	./tippecanoe -q -z0 -f -e tests/join-population/empty.dirtiles tests/join-population/empty.json
	./tile-join -f -e tests/join-population/empty.out.dirtiles tests/join-population/empty.dirtiles
	./tippecanoe-decode -x generator -x generator_options -x name -x description tests/join-population/empty.dirtiles > tests/join-population/empty.out.json.check
	cmp tests/join-population/empty.out.json.check tests/join-population/empty.out.json
	rm -rf tests/join-population/empty.dirtiles tests/join-population/empty.out.dirtiles tests/join-population/empty.out.json.check
	#
	# Test overzooming of tilesets with different maxzooms
	#
	mkdir -p tests/ne_110m_ocean/join
	./tippecanoe -q -z2 -f -o tests/ne_110m_ocean/join/ocean.mbtiles tests/ne_110m_ocean/in.json
	./tippecanoe -q -z4 -d8 -y name -f -o tests/ne_110m_ocean/join/countries.mbtiles tests/ne_110m_admin_0_countries/in.json.gz
	./tile-join --overzoom -f -o tests/ne_110m_ocean/join/joined.mbtiles tests/ne_110m_ocean/join/ocean.mbtiles tests/ne_110m_ocean/join/countries.mbtiles
	./tippecanoe-decode -x generator tests/ne_110m_ocean/join/joined.mbtiles > tests/ne_110m_ocean/join/joined.mbtiles.json.check
	cmp tests/ne_110m_ocean/join/joined.mbtiles.json.check tests/ne_110m_ocean/join/joined.mbtiles.json
	rm -f tests/ne_110m_ocean/join/ocean.mbtiles tests/ne_110m_ocean/join/countries.mbtiles tests/ne_110m_ocean/join/joined.mbtiles tests/ne_110m_ocean/join/joined.mbtiles.json.check
	#
	# Test sql join
	#
	./tile-join -i -f -o tests/join-sql/countries.pmtiles --join-sqlite tests/join-sql/countries.gpkg --join-table countries --join-tile-attribute ne10-admin0:name_en --join-table-expression 'lower(country)' tests/join-sql/bboxes.pmtiles
	./tippecanoe-decode -x generator tests/join-sql/countries.pmtiles > tests/join-sql/countries.pmtiles.json.check
	cmp tests/join-sql/countries.pmtiles.json.check tests/join-sql/countries.pmtiles.json
	rm -f tests/join-sql/countries.pmtiles tests/join-sql/countries.pmtiles.json.check
	#
	# Test sql join with limit
	#
	./tile-join --join-count-limit 3 -i -f -o tests/join-sql/countries-limit3.pmtiles --join-sqlite tests/join-sql/countries.gpkg --join-table countries --join-tile-attribute ne10-admin0:name_en --join-table-expression 'lower(country)' tests/join-sql/bboxes.pmtiles
	./tippecanoe-decode -x generator tests/join-sql/countries-limit3.pmtiles > tests/join-sql/countries-limit3.pmtiles.json.check
	cmp tests/join-sql/countries-limit3.pmtiles.json.check tests/join-sql/countries-limit3.pmtiles.json
	rm -f tests/join-sql/countries-limit3.pmtiles tests/join-sql/countries-limit3.pmtiles.json.check

accumulate-test:
	# there are 144 features with POP1950 in the original dataset
	test `grep '"POP1950": [0-9]' tests/ne_110m_populated_places_nulls/in.json | wc -l` == 144
	# and 99 without it
	test `grep '"POP1950": null' tests/ne_110m_populated_places_nulls/in.json | wc -l` == 99
	./tippecanoe -yNAME -yPOP1950 -yclustered:cluster_size -yclustered:unrelated -q -z3 -r1.75 -b0 -f -e tests/pbf/accum.dir --accumulate-numeric-attributes=clustered --set-attribute '{"clustered:cluster_size":1}' --accumulate-attribute '{"clustered:cluster_size":"sum"}' --retain-points-multiplier 3 tests/ne_110m_populated_places_nulls/in.json
	# at this drop rate, there are 61 points at z0 that have no POP1950s clustered onto them....
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep -v 'clustered:count:POP1950' | wc -l` == 61
	# 26 of which have no POP1950 at all
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep -v 'POP1950' | wc -l` == 26
	# 35 of which do have POP1950
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep -v 'clustered:count:POP1950' | grep 'POP1950' | wc -l` == 35
	# plus 60 that are clustered
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep 'clustered:count:POP1950' | wc -l` == 60
	# the 60 clustered POP1950s have a total count of 109
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep 'clustered:count:POP1950' | sed 's/.*"clustered:count:POP1950": //' | awk '{sum += $$1} END {print sum}'` == 109
	# we have already established that there are 36 bare POP1950s
	# which makes a total of 144, which is the total count expected
	#
	# meanwhile, regular attribute accumulation.
	# there are 121 features in the z0 tile, and they all have clustered:cluster_size
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep 'clustered:cluster_size' | wc -l` == 121
	# there are no features that lack it.
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep -v 'clustered:cluster_size' | wc -l` == 0
	# they add up to the 243 original features
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | sed 's/.*clustered:cluster_size": //' | awk '{sum += $$1} END {print sum}'` == 243
	# Make sure we do *not* accumulate a numeric attribute that already has the magic prefix:
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep sum:clustered:unrelated | wc -l` == 0
	# But that we *do* preserve those attributes into the output features:
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep clustered:unrelated | wc -l` == 61
	#
	# on to the sums:
	# in the original data set, the POP1950s that are present add up to 161590
	test `grep '"POP1950": [0-9]' tests/ne_110m_populated_places_nulls/in.json | sed 's/.*"POP1950": //' | awk '{sum += $$1} END {print sum}' ` == 161590
	# in the z0 tile, the clustered POP1950s add up to 116967
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep 'clustered:sum:POP1950' | sed 's/.*"clustered:sum:POP1950": //' | awk '{sum += $$1} END {print sum}'` == 116967
	# and the non-clustered ones add up to 44623
	test `./tippecanoe-decode -c tests/pbf/accum.dir/0/0/0.pbf 0 0 0 | grep -v 'clustered:sum:POP1950' | grep POP1950 | sed 's/.*"POP1950": //' | awk '{sum += $$1} END {print sum}'` == 44623
	# which is the correct 161590
	#
	# OK, so do these still hold after megatile filtering?
	./tippecanoe-overzoom --accumulate-numeric-attributes=clustered --accumulate-attribute '{"clustered:cluster_size":"sum"}' -m -o tests/pbf/accum-0-0-0.pbf tests/pbf/accum.dir/0/0/0.pbf 0/0/0 0/0/0
	# Now there are 40 features with POP1950 clusters
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep 'clustered:count:POP1950' | wc -l` == 40
	# There are 4 with bare POP1950
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep -v 'clustered:count:POP1950' | grep 'POP1950' | wc -l` == 4
	# And 2 with no POP1950 at all
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep -v 'POP1950' | wc -l` == 2
	# (which is the same as you get if you don't use -retain-points-multiplier when creating the tileset)
	#
	# the clustered and megatile-filtered POP1950s add up to 146370
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep 'clustered:sum:POP1950' | sed 's/.*"clustered:sum:POP1950": //' | awk '{sum += $$1} END {print sum}'` == 146370
	# the non-clustered but megatile-filtered POP1950s add up to 15220
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep -v 'clustered:sum:POP1950' | grep POP1950 | sed 's/.*"POP1950": //' | awk '{sum += $$1} END {print sum}'` == 15220
	# which add up to 161590 so we have the right global total
	# Make sure we do *not* accumulate a numeric attribute that already has the magic prefix:
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep sum:clustered:unrelated | wc -l` == 0
	# But that we *do* preserve those attributes into the output features:
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep clustered:unrelated | wc -l` == 22
	# the cluster sizes still add up to the 243 original features
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | sed 's/.*clustered:cluster_size": //' | awk '{sum += $$1} END {print sum}'` == 243
	#
	# We actually want to serve point tiles without the numeric accumulations,
	# but with cluster size, so test that combination:
	./tippecanoe-overzoom --accumulate-attribute '{"clustered:cluster_size":"sum"}' --exclude-prefix clustered:sum --exclude-prefix clustered:count --exclude-prefix clustered:min --exclude-prefix clustered:max --exclude-prefix clustered:mean -m -o tests/pbf/accum-0-0-0.pbf tests/pbf/accum.dir/0/0/0.pbf 0/0/0 0/0/0
	# There are no POP1950 clusters
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep 'clustered:count:POP1950' | wc -l` == 0
	# But there are still 28 with bare POP1950
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep -v 'clustered:count:POP1950' | grep 'POP1950' | wc -l` == 28
	# And 18 with no POP1950 at all
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | grep -v 'POP1950' | wc -l` == 18
    # which matches the 46 features that you get if you tile without --retain-points-multiplier.
	# the cluster sizes still add up to the 243 original features
	test `./tippecanoe-decode -c tests/pbf/accum-0-0-0.pbf 0 0 0 | sed 's/.*clustered:cluster_size": //' | awk '{sum += $$1} END {print sum}'` == 243
	#
	# Now on to binning!
	./tippecanoe-overzoom --assign-to-bins tests/pbf/h3-0-0-0.geojson --accumulate-numeric-attributes=clustered --accumulate-attribute '{"clustered:cluster_size":"sum"}' -o tests/pbf/bins-0-0-0.pbf tests/pbf/accum.dir/0/0/0.pbf 0/0/0 0/0/0
	# Now there are 30 bins with POP1950 clusters
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep 'clustered:count:POP1950' | wc -l` == 41
	# There are none with bare POP1950 (which is expected; we should only have summary statistics)
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep -v 'clustered:count:POP1950' | grep 'POP1950' | wc -l` == 0
	# And 4 with no POP1950 at all
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep -v 'POP1950' | wc -l` == 3
	#
	# the clustered and megatile-filtered and binned POP1950s add up to 161590
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep 'clustered:sum:POP1950' | sed 's/.*"clustered:sum:POP1950": //' | awk '{sum += $$1} END {print sum}'` == 161590
	# which is the right global total
	# Make sure we do *not* accumulate a numeric attribute that already has the magic prefix:
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep sum:clustered:unrelated | wc -l` == 0
	# And those attributes do *not* make it onto the bins
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep clustered:unrelated | wc -l` == 0
	# the cluster sizes still add up to the 243 original features
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | sed 's/.*clustered:cluster_size": //' | awk '{sum += $$1} END {print sum}'` == 243
	#
	# Binning with attribute stripping
	./tippecanoe-overzoom -y clustered:count:POP1950 -y clustered:sum:POP1950 -y POP1950 -y clustered:cluster_size --assign-to-bins tests/pbf/h3-0-0-0.geojson --accumulate-numeric-attributes=clustered --accumulate-attribute '{"clustered:cluster_size":"sum"}' -o tests/pbf/bins-0-0-0.pbf tests/pbf/accum.dir/0/0/0.pbf 0/0/0 0/0/0
	# Now there are 30 bins with POP1950 clusters
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep 'clustered:count:POP1950' | wc -l` == 41
	# There are none with bare POP1950 (which is expected; we should only have summary statistics)
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep -v 'clustered:count:POP1950' | grep 'POP1950' | wc -l` == 0
	# And 4 with no POP1950 at all
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep -v 'POP1950' | wc -l` == 3
	#
	# the clustered and megatile-filtered and binned POP1950s add up to 161590
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep 'clustered:sum:POP1950' | sed 's/.*"clustered:sum:POP1950": //' | awk '{sum += $$1} END {print sum}'` == 161590
	# which is the right global total
	# Make sure we do *not* accumulate a numeric attribute that already has the magic prefix:
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep sum:clustered:unrelated | wc -l` == 0
	# And those attributes do *not* make it onto the bins
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | grep clustered:unrelated | wc -l` == 0
	# the cluster sizes still add up to the 243 original features
	test `./tippecanoe-decode -c tests/pbf/bins-0-0-0.pbf 0 0 0 | sed 's/.*clustered:cluster_size": //' | awk '{sum += $$1} END {print sum}'` == 243
	#
	#
	# A tile where the counts and means were previously wrong:
	./tippecanoe-overzoom --accumulate-numeric-attributes=felt -m -o tests/pbf/yearbuilt-accum.pbf tests/pbf/yearbuilt.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/yearbuilt-accum.pbf 0 0 0 > tests/pbf/yearbuilt-accum.pbf.json.check
	cmp tests/pbf/yearbuilt-accum.pbf.json.check tests/pbf/yearbuilt-accum.pbf.json
	rm tests/pbf/yearbuilt-accum.pbf tests/pbf/yearbuilt-accum.pbf.json.check
	# Same tile, with attribute stripping
	./tippecanoe-overzoom --accumulate-numeric-attributes=felt -y bldgsqft -y felt:sum:bldgsqft -m -o tests/pbf/yearbuilt-accum-bldgsqft.pbf tests/pbf/yearbuilt.pbf 0/0/0 0/0/0
	./tippecanoe-decode tests/pbf/yearbuilt-accum-bldgsqft.pbf 0 0 0 > tests/pbf/yearbuilt-accum-bldgsqft.pbf.json.check
	cmp tests/pbf/yearbuilt-accum-bldgsqft.pbf.json.check tests/pbf/yearbuilt-accum-bldgsqft.pbf.json
	rm tests/pbf/yearbuilt-accum-bldgsqft.pbf tests/pbf/yearbuilt-accum-bldgsqft.pbf.json.check

join-filter-test: tippecanoe tippecanoe-decode tile-join
	# Comes out different from the direct tippecanoe run because null attributes are lost
	./tippecanoe -q -z0 -f -o tests/feature-filter/out/all.mbtiles tests/feature-filter/in.json
	./tile-join -q -J tests/feature-filter/filter -f -o tests/feature-filter/out/filtered.mbtiles tests/feature-filter/out/all.mbtiles
	./tippecanoe-decode -x generator tests/feature-filter/out/filtered.mbtiles > tests/feature-filter/out/filtered.json.check
	cmp tests/feature-filter/out/filtered.json.check tests/feature-filter/out/filtered.json.standard
	rm -f tests/feature-filter/out/filtered.json.check tests/feature-filter/out/filtered.mbtiles tests/feature-filter/out/all.mbtiles
	# Test zoom level filtering
	./tippecanoe -q -r1 -z8 -f -o tests/feature-filter/out/places.mbtiles tests/ne_110m_populated_places/in.json
	./tile-join -q -J tests/feature-filter/places-filter -f -o tests/feature-filter/out/places-filter.mbtiles tests/feature-filter/out/places.mbtiles
	./tippecanoe-decode -x generator tests/feature-filter/out/places-filter.mbtiles > tests/feature-filter/out/places-filter.mbtiles.json.check
	cmp tests/feature-filter/out/places-filter.mbtiles.json.check tests/feature-filter/out/places-filter.mbtiles.json.standard
	rm -f tests/feature-filter/out/places.mbtiles tests/feature-filter/out/places-filter.mbtiles tests/feature-filter/out/places-filter.mbtiles.json.check

json-tool-test: tippecanoe-json-tool
	./tippecanoe-json-tool -e GEOID10 tests/join-population/tabblock_06001420.json | sort > tests/join-population/tabblock_06001420.json.sort
	./tippecanoe-json-tool -c tests/join-population/population.csv tests/join-population/tabblock_06001420.json.sort > tests/join-population/tabblock_06001420.json.sort.joined
	./tippecanoe-json-tool --empty-csv-columns-are-null -c tests/join-population/population.csv tests/join-population/tabblock_06001420.json.sort > tests/join-population/tabblock_06001420-null.json.sort.joined
	cmp tests/join-population/tabblock_06001420.json.sort.joined tests/join-population/tabblock_06001420.json.sort.joined.standard
	cmp tests/join-population/tabblock_06001420-null.json.sort.joined tests/join-population/tabblock_06001420-null.json.sort.joined.standard
	rm -f tests/join-population/tabblock_06001420.json.sort tests/join-population/tabblock_06001420.json.sort.joined
	rm -f tests/join-population/tabblock_06001420-null.json.sort.joined

allow-existing-test: tippecanoe
	# Make a tileset
	./tippecanoe -q -Z0 -z0 -f -o tests/allow-existing/both.mbtiles tests/coalesce-tract/tl_2010_06001_tract10.json
	# Writing to existing should fail
	if ./tippecanoe -q -Z1 -z1 -o tests/allow-existing/both.mbtiles tests/coalesce-tract/tl_2010_06001_tract10.json; then exit 1; else exit 0; fi
	# Replace existing
	./tippecanoe -q -Z8 -z9 -f -o tests/allow-existing/both.mbtiles tests/coalesce-tract/tl_2010_06001_tract10.json
	./tippecanoe -q -Z10 -z11 -F -o tests/allow-existing/both.mbtiles tests/coalesce-tract/tl_2010_06001_tract10.json
	./tippecanoe-decode -x generator -x generator_options tests/allow-existing/both.mbtiles > tests/allow-existing/both.mbtiles.json.check
	cmp tests/allow-existing/both.mbtiles.json.check tests/allow-existing/both.mbtiles.json
	# Make a tileset
	./tippecanoe -q -Z0 -z0 -f -e tests/allow-existing/both.dir tests/coalesce-tract/tl_2010_06001_tract10.json
	# Writing to existing should fail
	if ./tippecanoe -q -Z1 -z1 -e tests/allow-existing/both.dir tests/coalesce-tract/tl_2010_06001_tract10.json; then exit 1; else exit 0; fi
	# Replace existing
	./tippecanoe -q -Z8 -z9 -f -e tests/allow-existing/both.dir tests/coalesce-tract/tl_2010_06001_tract10.json
	./tippecanoe -q -Z10 -z11 -F -e tests/allow-existing/both.dir tests/coalesce-tract/tl_2010_06001_tract10.json
	./tippecanoe-decode -x generator -x generator_options tests/allow-existing/both.dir | sed 's/both\.dir/both.mbtiles/g' > tests/allow-existing/both.dir.json.check
	cmp tests/allow-existing/both.dir.json.check tests/allow-existing/both.mbtiles.json
	# Make a tileset
	./tippecanoe -q -Z0 -z0 -f -o tests/allow-existing/both.pmtiles tests/coalesce-tract/tl_2010_06001_tract10.json
	# Writing to existing should fail
	if ./tippecanoe -q -Z1 -z1 -o tests/allow-existing/both.pmtiles tests/coalesce-tract/tl_2010_06001_tract10.json; then exit 1; else exit 0; fi
	# Replace existing
	./tippecanoe -q -Z8 -z9 -f -o tests/allow-existing/both.pmtiles tests/coalesce-tract/tl_2010_06001_tract10.json
	# Allow-existing is not supported for pmtiles
	if ./tippecanoe -q -Z10 -z11 -F -o tests/allow-existing/both.pmtiles tests/coalesce-tract/tl_2010_06001_tract10.json; then exit 1; else exit 0; fi
	rm -r tests/allow-existing/both.pmtiles tests/allow-existing/both.dir.json.check tests/allow-existing/both.dir tests/allow-existing/both.mbtiles.json.check tests/allow-existing/both.mbtiles

csv-test: tippecanoe tippecanoe-decode
	# Reading from named CSV
	./tippecanoe -q -zg -f -o tests/csv/out.mbtiles tests/csv/ne_110m_populated_places_simple.csv
	./tippecanoe-decode -x generator -x generator_options tests/csv/out.mbtiles > tests/csv/out.mbtiles.json.check
	cmp tests/csv/out.mbtiles.json.check tests/csv/out.mbtiles.json
	rm -f tests/csv/out.mbtiles.json.check tests/csv/out.mbtiles
	# Reading from named CSV, with nulls
	./tippecanoe -q --empty-csv-columns-are-null -zg -f -o tests/csv/out-null.mbtiles tests/csv/ne_110m_populated_places_simple.csv
	./tippecanoe-decode -x generator tests/csv/out-null.mbtiles > tests/csv/out-null.mbtiles.json.check
	cmp tests/csv/out-null.mbtiles.json.check tests/csv/out-null.mbtiles.json
	rm -f tests/csv/out-null.mbtiles.json.check tests/csv/out-null.mbtiles
	# Same, but specifying csv with -L format
	./tippecanoe -q -zg -f -o tests/csv/out.mbtiles -L'{"file":"", "format":"csv", "layer":"ne_110m_populated_places_simple"}' < tests/csv/ne_110m_populated_places_simple.csv
	./tippecanoe-decode -x generator -x generator_options tests/csv/out.mbtiles > tests/csv/out.mbtiles.json.check
	cmp tests/csv/out.mbtiles.json.check tests/csv/out.mbtiles.json
	rm -f tests/csv/out.mbtiles.json.check tests/csv/out.mbtiles

layer-json-test: tippecanoe tippecanoe-decode
	# GeoJSON with description and named layer
	./tippecanoe -q -z0 -r1 -yNAME -f -o tests/layer-json/out.mbtiles -L'{"file":"tests/ne_110m_populated_places/in.json", "description":"World cities", "layer":"places"}'
	./tippecanoe-decode -x generator -x generator_options tests/layer-json/out.mbtiles > tests/layer-json/out.mbtiles.json.check
	cmp tests/layer-json/out.mbtiles.json.check tests/layer-json/out.mbtiles.json
	rm -f tests/layer-json/out.mbtiles.json.check tests/layer-json/out.mbtiles
	# Same, but reading from the standard input
	./tippecanoe -q -z0 -r1 -yNAME -f -o tests/layer-json/out.mbtiles -L'{"file":"", "description":"World cities", "layer":"places"}' < tests/ne_110m_populated_places/in.json
	./tippecanoe-decode -x generator -x generator_options tests/layer-json/out.mbtiles > tests/layer-json/out.mbtiles.json.check
	cmp tests/layer-json/out.mbtiles.json.check tests/layer-json/out.mbtiles.json
	rm -f tests/layer-json/out.mbtiles.json.check tests/layer-json/out.mbtiles

# Use this target to regenerate the standards that the tests are compared against
# after making a change that legitimately changes their output

prep-test: $(TESTS)

tests/%.json: Makefile tippecanoe tippecanoe-decode
	./tippecanoe -q -a@ -f -o $@.check.mbtiles $(call testargs,$(patsubst %.json,%,$(word 4,$(subst /, ,$@)))) $(foreach suffix,$(suffixes),$(sort $(wildcard $(subst $(SPACE),/,$(wordlist 1,2,$(subst /, ,$@)))/*.$(suffix))))
	./tippecanoe-decode -x generator $@.check.mbtiles > $@
	cmp $(patsubst %.check,%,$@) $@
	rm $@.check.mbtiles
