A tour of Tippecanoe
====================

Tippecanoe has what sounds like a simple job: It takes geographic objects in one file format ([GeoJSON](https://geojson.org/)) and copies them into a different file format ([Mapbox Vector Tiles](https://github.com/mapbox/vector-tile-spec)). But it takes a lot of code to do that. What's really going on?

Even at the surface it is not quite that simple. The input can also be [CSV](https://datatracker.ietf.org/doc/html/rfc4180), [Geobuf](https://github.com/mapbox/geobuf), or [FlatGeobuf](https://flatgeobuf.org/), and the output can be an [mbtiles](https://github.com/mapbox/mbtiles-spec) file, a directory of tiles, or a [PMTiles](https://github.com/protomaps/PMTiles) archive. And Tippecanoe is a family of programs, not one: `tile-join`, `tippecanoe-overzoom`, `tippecanoe-decode`, `tippecanoe-json-tool`, and `tippecanoe-enumerate` all operate on the tiles after the fact. But the core of it is this: read a lot of features, put them in an order that makes them easy to thin out, and then divide and conquer the world into tiles.

All the links below point at [`63fcac72`](https://github.com/felt/tippecanoe/tree/63fcac725abeb841a101abfc64189edd66d1cf14) (version 2.81.0), so the line numbers stay meaningful even as the code moves.

Starting up
-----------

[The main function](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L3168) has the job of processing the list of options and files that the user provides. I'll go into more detail later about what all those options actually do, but the list of them is [spelled out here](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2979) in a single table.

Everything about the options comes from that one table, so that no two descriptions of them can disagree. The entries whose `val` is 0 and whose `flag` is null are not options at all but headings for the usage message, and [`strip_usage_headings()`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/usage.hpp#L29) removes them before the table is handed to `getopt_long()`. [`getopt_string()`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/usage.hpp#L23) derives the short-option string from it, and [`print_usage()`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/usage.cpp#L88) prints the usage message from it. The other programs in the family use the same mechanism.

Most of the options set an entry in the `additional[]` or `prevent[]` arrays, indexed by the single-character codes listed in [`options.hpp`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/options.hpp), which is why so much of the code below reads like `if (additional[A_DROP_DENSEST_AS_NEEDED])`.

After the options are processed we reach the core operations: [create the output tileset](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L3794), [read input into it](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L3841), [close it](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L3847), and then, if the output name ended in `.pmtiles`, [convert what was written into a PMTiles archive](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L3851).

Creating the output tileset
---------------------------

[Creating an mbtiles file](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L29) is a series of sqlite operations, most of which I copied from [mbutil](https://github.com/mapbox/mbutil). There is an option (`-F`) to keep going even if these operations fail, to support a geocoding use case that needed to add new, non-conflicting, tiles to an existing mbtiles file.

The schema is a `map` table that maps z/x/y to a content hash, an `images` table that maps a content hash (within a zoom level) to the tile data, and a `tiles` *view* that joins the two back together into what a consumer expects. The point of the indirection is that identical tiles — which are extremely common at low zooms, and in any tileset with a lot of empty ocean — are stored only once. [`mbtiles_write_tile`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L104) hashes each tile's contents with [fnv1a](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/text.cpp#L260) to find the key.

There are two other output forms. With `-e`, the output is [a directory of tiles](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/dirtiles.cpp#L28), `z/x/y.pbf`, with a `metadata.json` alongside. If the output name ends in `.pmtiles`, Tippecanoe writes an mbtiles file first and then [repacks it](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pmtiles_file.cpp#L139) into the single-file PMTiles format at the end, which is described in [its own section](#converting-to-pmtiles) below. From the tiling code's point of view all three are the same: there is an `outdb` or an `outdir`, and it writes tiles to whichever one it has.

Temporary files to read input into
----------------------------------

[Reading input](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1240) is deceptively named because it also ultimately invokes the whole tiling process.

Its first job is to [create as many sets of temporary files](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1245) as your computer has CPUs, so that multiple threads can read input into these files at the same time without interfering with each other. At the end of the actual input stage, the per-thread temporary files will be merged or concatenated together so that tiling will operate on the combined results.

The temporary files are:

 * `geom`, which, as the name suggests, contains the feature geometry, plus [other characteristics of each feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.hpp#L105).
 * `index`, which is an index of the features in `geom` [by their byte offset within the file and their quadkey-encoded location](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.hpp#L13). It will be used later to merge the per-thread `geom` files back together.
 * `pool`, which contains one deduplicated instance of each property key or value that appears in any feature.
 * `tree`, which is a binary tree of `pool` entries, so that it can look up the existing pooled copy of any keys or values that are duplicated across multiple features, or add a new entry to the pool when a key or value is used for the first time.
 * `vertex`, which records, for `--no-simplification-of-shared-nodes`, each vertex of each line or ring together with the two vertices on either side of it, so that the points where features diverge from each other can be found globally.
 * `node`, which records the individual points that must not be simplified away.

Features reference their pooled keys and values directly, by their offsets into `pool`; there is no second level of indirection for features that have a lot of properties.

All of this data is in temporary files instead of in memory because it can be very large, and putting it on disk makes it possible to tile data that is too big to fit in memory. Processing will be much faster if the data does fit in memory, though. The `pool` and `tree` in particular use a hybrid: [`memfile`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/memfile.cpp#L11) keeps them as an in-memory `std::string` until they get too big, at which point [`memfile_full`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/memfile.cpp#L71) switches to appending to the real file. Note that data is explicitly copied in and out rather than accessed through a writable memory map, which has bad performance problems in containers.

What is straightforwardly in memory, not on disk, because it is small, is [the "layermap"](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1417) for each CPU, which is a list (by name) of the tile layers and all the property keys that have been used for any features in that layer, plus the sample values and ranges that will become the tileset's [tilestats](https://github.com/mapbox/mbtiles-spec/blob/master/1.3/spec.md). This will eventually end up in the tileset metadata.

Input formats
-------------

Before reading anything, Tippecanoe [makes up a layer name](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1358) for each source that doesn't have one, from the last component of its filename with the recognized suffixes and any characters that can't appear in a selector trimmed off.

Then, for each source in turn, the format is chosen from the file suffix, or from the `format` given in a `-L{"format":"…"}` JSON layer specification:

 * [FlatGeobuf](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1469), which is memory-mapped and parsed by [`parse_flatgeobuf`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/flatgeobuf.cpp#L349). Its features are queued up and [handed out to a pool of parser threads](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/flatgeobuf.cpp#L331).
 * [Geobuf](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1542), likewise memory-mapped, parsed by [`parse_geobuf`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geobuf.cpp#L535) with the same queue-and-threads arrangement.
 * [CSV](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1615), parsed by [`parse_geocsv`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geocsv.cpp#L14), which finds the latitude and longitude columns by name and makes a point feature out of each row.
 * GeoJSON, which is everything else, and is the interesting case.

GeoJSON input can also be gzipped: [`streamfdopen`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L599) transparently wraps the file in a `gzdopen` if its name ends in `.gz`.

Whichever frontend reads the features, they all converge on the same place: they fill in a [`serial_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.hpp#L105) and call [`serialize_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L416) to write it to the temporary files. Everything downstream of that point is format-independent.

Three ways to read GeoJSON
--------------------------

Now that the temporary files are in place, it is time to start reading input, either from `stdin` or from the files named on the command line. There are three ways that input files may be read, depending on whether the input is from a file or a stream and whether the user has asked for parallel input processing.

1. If the `--read-parallel` option was specified, Tippecanoe first tries to map the file into memory. If this was successful, it [creates several threads to parse parts of it in parallel](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1703) as will be described below.

2. If the file can't be mapped into memory (which means that it is an input stream, not a file on disk), Tippecanoe [opens it as a stream](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1718). If the user has nevertheless asked for parallel processing of input, Tippecanoe [starts reading the stream into another temporary file](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1733) that it *can* map into memory. Once it has accumulated enough to be worth [starting up several parser threads](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1782), it does that, and the main thread goes back to reading more streaming input into the next temporary file. This back-and-forth continues until the whole input has been consumed. It also [waits for the parsers](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1772) rather than reading indefinitely far ahead of them.

3. If the user didn't ask for parallel input, it just [runs a single JSON parser](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1834) on the streaming input file.

Parallel reading is not strictly opt-in, though: if the stream [begins with an ASCII record separator](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1728) (0x1E), the input is [GeoJSON Text Sequences](https://datatracker.ietf.org/doc/html/rfc8142), which *does* guarantee that features are separated, so Tippecanoe turns parallel parsing on by itself and splits on the separator instead of on newlines.

Parsing GeoJSON in parallel
---------------------------

To avoid massive digression I will first talk about parsing GeoJSON in parallel. The [`do_read_parallel` function](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L451) starts with the file already mapped into memory and knows how many CPUs can work on it simultaneously. It starts by [making a guess](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L456) that each CPU should have an equal-sized fraction of the file to work on, and then adjusts this guess by moving the divisions forward until each of them begins with a separator character (a newline, or a 0x1E if this is a GeoJSON text sequence).

(Note that GeoJSON itself makes no guarantee that feature objects are separated by newlines, or that newlines will never appear in the middle of a feature. This lack of a guarantee is why parallel parsing only happens if the user asks for it, or if the input announces itself as a text sequence, because it will misinterpret many correct GeoJSON inputs that are not designed for line-delimited use.)

It [sets up a parameter block for each CPU](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L485) pointing it to all the global settings and to the per-CPU temporary files and semi-global properties (bounding box, layermaps) and a JSON parser pointed to the portion of the file that was allocated to that CPU, and creates a thread to run the single-stream parsing described immediately below, from that JSON parser. Then it waits for all the threads to finish and is done.

Parsing a single GeoJSON stream
-------------------------------

To parse GeoJSON, whether from a fraction of a memory-mapped file as described immediately above, or from a regular file stream, Tippecanoe uses [a generic JSON pull-parser](https://github.com/felt/tippecanoe/tree/63fcac725abeb841a101abfc64189edd66d1cf14/jsonpull). By pull-parser, I mean that the parser returns JSON tokens and objects one at a time as they are encountered, rather than building up a full JSON object structure in memory for the entire input file and then presenting that to the caller.

[The loop over those tokens](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson-loop.cpp#L39) lives in `geojson-loop.cpp` rather than in `geojson.cpp`, because `tippecanoe-json-tool` needs the same thing. It runs until it encounters either a [bare geometry](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson-loop.cpp#L76) (an object whose `type` is `Point`, `LineString`, `Polygon`, `MultiPoint`, `MultiLineString`, or `MultiPolygon` and is not contained within something that could be a feature) or [a feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson-loop.cpp#L129) (an object whose `type` is `Feature`). In either case it calls the `add_feature` method of the `json_feature_action` it was given, which for Tippecanoe proper is [the one that calls `serialize_geojson_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson.cpp#L244).

The checks for whether something that looks like a geometry or a feature really is one are fussier than they first appear: an object is not a feature or geometry of its own if it [appears inside another object's `properties`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson-loop.cpp#L103), and a `GeometryCollection` inside a feature is [expanded into one feature per geometry](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson.cpp#L246) sharing the same attributes.

From GeoJSON feature to internal feature
----------------------------------------

[Turning a GeoJSON feature into a Tippecanoe feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson.cpp#L43) starts with some checking: does the geometry have a `coordinates` array? Is the `type` one of the kinds of things that GeoJSON defines? It also [checks the feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson.cpp#L85) for a special `tippecanoe` object that can specify a per-feature minzoom, maxzoom, or layer name, and for [the top-level feature `id`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson.cpp#L104) that is independent of the feature properties.

[For each of the feature's properties](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geojson.cpp#L192) it identifies the property's type and, if it is a compound JSON object that can't be natively represented as one of the attribute types that vector tiles supports, [stringifies it](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/read_json.cpp#L124). These are all just in memory for the moment, in the `full_keys` and `full_values` fields of the `serial_feature`. The keys go through a [`key_pool`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.hpp#L89) so that the many features that share an attribute name also share one copy of the string in memory.

It then calls [`parse_coordinates`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/read_json.cpp#L56) to recursively unpack the `coordinates` array from the `geometry`, and hands the whole thing to `serialize_feature`. See below for more about parsing and reprojecting geometry.

Internal representation of a feature
------------------------------------

The [`serial_feature` structure](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.hpp#L105) is the thing that every frontend produces and that every stage of tiling passes around. It is a big structure, but its fields divide into three groups, and the comments in the header say which is which:

 * The ones that are actually serialized to the `geom` file: the layer, segment, and sequence numbers; the geometry type `t`; the geometry itself; the feature `id`, if any; the per-feature `tippecanoe_minzoom` and `tippecanoe_maxzoom`, if any; the quadkey `index`; the `extent` (area, or a stand-in for it); the `label_point`; and the pooled `keys` and `values`.
 * The ones that only exist during initial serialization: `full_keys` and `full_values`, the string forms of the attributes, which get replaced by the pooled `keys` and `values` offsets.
 * The ones that only exist during tiling: the bounding box, the `dropped` state, whether the feature is polygon dust or was coalesced, the current detail and simplification level, and so on.

`dropped` deserves a note, because despite the name it is not a boolean. It is [`FEATURE_DROPPED` (-1), `FEATURE_KEPT` (0), a positive sequence number](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.hpp#L146) meaning "this is the *n*th extra feature retained by `--retain-points-multiplier` alongside its cluster's lead feature," or `INT_MAX` meaning "retained by `--preserve-multiplier-density-threshold`." Much of the dropping logic in `write_tile` is really about keeping these multiplier clusters intact.

Attribute values are represented by [`serial_val`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.hpp#L39): a type tag (one of the `mvt_value` types) plus the value as a string. Every number, integer or floating point, is `mvt_double` here and is stored in its stringified form, which is how integers too large for a double survive with their original precision.

Writing a feature to the temporary files
----------------------------------------

[`serialize_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L416) is where a feature from any frontend actually becomes bytes on disk, and it is where a surprising number of the command line options take effect. In order, it:

 * Accumulates the feature's coordinates into the two per-thread file bounding boxes: [one on the normal −180 to 180 plane and one on a 0 to 360 plane](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L425), so that at the end the narrower of the two can be reported as the tileset's antimeridian-aware bounds.
 * [Scales the geometry](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L304) down by `geometry_scale`, which also does the `--detect-longitude-wraparound` bookkeeping, with a [special case for jumps of exactly 360°](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L322), which in some data sets are an intentional line across the world rather than an accident.
 * Calls [`fix_polygon`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/clip.cpp#L1755) on polygons, to make sure the rings are wound in the correct direction, since GeoJSON identifies parent and child rings by their position in the `coordinates` array but vector tiles represent them by positive or negative area.
 * [Clips to any `--clip-bounding-box` or `--clip-polygon` regions](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L467), and gives up on the feature if nothing is left.
 * If `--no-simplification-of-shared-nodes` is in effect, [writes out the `vertex` and `node` records](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L507) described above. This is one of the more subtle parts of the program: for each vertex it records the two neighboring vertices, so that after a global sort it can tell the difference between a point where two features merely run alongside each other (same neighbors) and a point where they actually converge, diverge, or cross (different neighbors). Only the latter must be protected from simplification. It also unconditionally protects each ring's start point, its farthest point from the start, and the point farthest from the line between those two, so that polygons can't be simplified out of existence.
 * If `-zg` was given, [samples the distances between the vertices within the feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L608), which will feed into the maxzoom guess.
 * [Calculates the feature's extent](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L636): the area for polygons, and for lines the area of a circle whose diameter is the line's length. Points get theirs later, in `write_tile`, from the distance to the adjacent feature. This is what `--drop-smallest-as-needed` and `--order-largest-first` sort on.
 * [Chooses the point that the feature will be indexed by](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L686). For points this is the bounding box center. For lines and polygons it is an arbitrary but predictable vertex, chosen by hashing the geometry, so that a pile of nearly-identical LineStrings that follow the same route don't all land on the same index and make `-zg` think the data is much denser than it is. (Hashing the geometry rather than picking at random means that features with identical geometry still get identical indexes.) Polygons being dropped or coalesced by density instead use their center of mass.
 * [Generates a label point](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L734) for polygons, if `--generate-polygon-label-points` was given, using [`polygon_to_anchor`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geometry.cpp#L766), which tries to find a point that is well inside the polygon and not in one of its holes.
 * [Decides whether the index is needed at all](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L743) — it is only serialized if some option that uses it is in effect — and [adds a layermap entry](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L763) for the feature's layer. Because layers within each serialized feature are specified by number, not by name, different threads may assign different layer numbers to the same layer name, which will need to be reconciled later.
 * Applies the attribute options: [`--set-attribute`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L785), [`--attribute-type` coercion, `--single-precision`, `--use-attribute-for-id`, `--exclude`, `--include`, and `--exclude-all`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L801), and [`--maximum-string-attribute-length`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L870). It also [feeds each surviving attribute into the tilestats](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L863) — unless a shell filter is in use, in which case there is no point, because the filter may change everything.
 * [Interns each key and value in the string pool](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L878) with `addpool`, which returns the offset within the per-CPU `pool` file.
 * [Writes the feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L886) to the `geom` file, prefixed by its length, and [adds it to the `index`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L890).

All that remains is to increase the per-CPU bounding box to encompass the feature bounding box if necessary, and increment the user-visible progress indicator.

The string pool
---------------

[`addpool`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pool.cpp#L24) is worth its own section, because it is on the hot path for every attribute of every feature and it has been tuned accordingly. It has three layers of defense against being slow:

 * A [direct-mapped hash cache](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pool.cpp#L26) in front of the tree, per reader thread, which catches the overwhelmingly common case of the same handful of keys and values recurring over and over. (It verifies the type and the string on a hit, because a hash cache that trusted the hash would collide.)
 * A [depth limit](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pool.cpp#L62) on the tree search. If a string is so deep in the tree that the search is getting expensive, it is probably unique anyway, so it is appended to the pool without being added to the tree, and future copies of it just won't be deduplicated.
 * A [size limit](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pool.cpp#L90). Once the pool and tree together exceed a fraction of physical memory, the pool switches over to appending to the file, and stops maintaining the tree at all. Deduplication is a nice-to-have; thrashing is not.

The comparison function, [`swizzlecmp`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pool.cpp#L12), orders by hash first and only compares strings when the hashes match, which keeps the tree from degenerating into a list when the input is sorted.

Serialized representation of a feature
--------------------------------------

Serializing each feature [probably looks familiar](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L173) if you have done any serialization with [protozero](https://github.com/mapbox/protozero). There is not much sanity-checking so you have to be careful if you make changes, especially in the field that [does some bit packing](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L187) to combine the layer number and the flags for the presence of a label point, index, extent, `id`, `minzoom`, and `maxzoom` into the same number.

Serialization goes into a `std::string` and the caller writes that out, rather than going straight to a file. That is what makes it possible for the same function to serve both the frontends, writing to the per-thread `geom` files, and [`rewrite`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L443) during tiling, writing compressed into the next zoom level's files.

All of the primitive serialization types (integers of various sizes and signednesses) are spelled out earlier in the same `serial.cpp` file. They use `protozero`'s zigzag conversion functions to represent signed numbers as unsigned, and use its same variable-length encoding for numbers, with the high bit of each byte indicating that there are more bytes to follow.

Two pieces of magic are worth knowing about, because both are commented as `MAGIC` in the source and both will bite you if you forget them. The first is that [the feature minzoom is the last byte of the serialized feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L230), so that the reordering pass can rewrite it in place without decoding anything. The second is that [`deserialize_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L235) checks that it consumed exactly as many bytes as the length prefix promised, which is the check that catches it when the two halves get out of step.

Internal representation of geometry
-----------------------------------

Parsing the GeoJSON coordinates array into Tippecanoe's internal representation of geometry, as mentioned above, deserves a little bit more detail.

Tippecanoe's [internal geometry structure](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geometry.hpp#L26) is called `draw`, and is composed of an operation (moveto, lineto, closepath) and an `x` and `y` coordinate, plus an auxiliary field used during line simplification to track whether a point has been determined to be `necessary`. A series of `draw` operations [is called](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/geometry.hpp#L67) a `drawvec`. The `draw` structure uses some bitfields to keep its size down to 128 bits even with the `op` and `necessary` fields, to avoid eating a lot of unnecessary memory.

The [`parse_coordinates` function](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/read_json.cpp#L56) mostly just builds up a `drawvec` from the GeoJSON coordinate arrays for the feature, after doing some sanity checking to make sure the arrays contain numbers and an appropriate number of elements. It calls itself recursively to parse the multiple points that appear within each LineString, the multiple rings that appear within each Polygon, and so on.

It also [contains the call](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/read_json.cpp#L82) that reprojects the coordinates from their original projection (usually WGS84) to tile coordinates, where the world is a square, 2^32 units on each side.

There is also [a weird special case](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/read_json.cpp#L106) that adds a `closepath` operation after the last ring of each Polygon, so that the following ring can be identified as an outer ring. (Tippecanoe does not otherwise use `closepath`, except in the last stages of writing out the vector tile format, which requires it as part of each polygon ring.) The [`fix_polygon` function](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/clip.cpp#L1755) uses this special tag when it reorders the points in each ring to make sure that inner rings are oriented the opposite direction from outer rings. It also makes sure the ring is closed, with the last point duplicating the first, if it wasn't already.

Separately, in `serialize_feature`, there is a special case (to save space in the temporary files) that shifts each of these world coordinates down by [`geometry_scale`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/serial.cpp#L30), which was set back in the `main` function to correspond to the global `minzoom`, so that coordinates on disk are not stored in more detail than will eventually be used in the final tiles. The reduced coordinates will be shifted back up to world scale during each deserialization during tiling. The shift rounds rather than truncating, which matters for overzooming, and adds a `COORD_OFFSET` first so that features slightly off the edge of the world still shift correctly.

Merging temporary files after parsing
-------------------------------------

At this point Tippecanoe has all of the input from all of its input files parsed and copied into temporary files, but each layer's features are scattered across multiple files, depending on which CPU happened to read the feature. The files have to be consolidated before tiling can begin.

But it is important to note that [back when the temporary files were being opened](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1255), each of them was opened *twice*, and then [deleted](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1321). Each of them was opened as an integer file descriptor with `mkstemp`, and as a C `FILE` with `fopen`, and deleted with `unlink`. The two open file descriptors to each file cause it to continue to exist on disk even though it has been deleted from the temporary directory. Deleting the file before it is written should also serve as a hint to the operating system that there is no need ever to write the file's contents to physical disk if it is small enough to continue to fit in memory, in the operating system's buffer cache. (Using a deleted file for temporary storage is a traditional Unix idiom, and there is a `tmpfile` call in the standard library to create and delete a temporary file if you only need a single `FILE` reference to it.)

So now [the `FILE` for each is closed](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1899), so each temporary file is kept alive by only *one* file descriptor, not two, and any buffering that was being done inside the Tippecanoe process itself, as opposed to in the operating system, will have been flushed out.

Merging the string pool is the easiest. The `tree` files are of no more use and are simply closed and discarded. The `pool` files for each CPU [are concatenated together](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1950) into a new file, with an array to keep track of the offset into the new file where the data for each CPU begins. (Which thread some data originally came from is frequently referred to in the code as its `segment`.) Because the pool may be partly in memory and partly on disk by this point, the merge has to handle both cases. The combined pool is then [mapped back into memory](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2003) so it can be accessed like an array.

Then, if `--no-simplification-of-shared-nodes` was requested, there are two more merges:

 * [The vertices are sorted](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2015) with [`fqsort`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/sort.cpp#L9) — a quicksort that partitions into temporary files when the data doesn't fit in memory — and then scanned. Any middle vertex that appears with *different* neighbors in two different records is a place where features diverge, so it becomes a node.
 * [The nodes are sorted and deduplicated](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2080) the same way. The result is mapped into memory so that each tile can binary-search it, and, because that search would otherwise be a lot of cache misses, a [34-megabyte Bloom filter](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2075) is built alongside it so that the common "this point is not a shared node" answer can be given without touching the array at all. The nodes are keyed by quadkey so that the nodes for any one tile are adjacent in memory.

Doing this globally, once, is what keeps the memory in bounds: the alternative of assembling the node list separately within each tile is correct but ruinously expensive on large inputs.

Why sort the features?
----------------------

The geometry is going to be much more of a pain to merge, not just because it is typically so much bigger than the other files, but because we also want it in a specific order. In particular we want it to be ordered by quadkey so that

1. when the low zooms are thinned out by dropping some fraction of features, those features are evenly distributed by location rather than clumpy,
2. it is possible to make global estimates of the number of features in each tile, before actually generating those tiles, and
3. within each tile, the density of features near any particular feature can be estimated by the difference between its quadkey index and the numerically adjacent feature's quadkey index.

These characteristics will be used by the `-r` dot dropping rate, the `-Bg` guessing of an appropriate base zoom level, and the `-g` thinning of dense features, respectively. The maxzoom guess of `-zg` uses the same ordering for the same reason.

We can accomplish this by sorting the `index` by quadkey and then copying features from the original files into a new temporary file in index order. This is the user-visible "Reordering geometry" phase.

Sorting and merging
-------------------

The index itself may actually be uncomfortably large to sort. Each [index entry](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.hpp#L13) takes 32 bytes of storage, so on a 4GB laptop, 100 million features will exhaust available memory and grind progress to a halt, sometimes crashing the system in the process. And recopying the geometry in index order after sorting may be even worse, because the original order of the geometry probably has no relationship to the index order, so if it does not fit in memory, retrieving each feature may require a disk access.

Tippecanoe's strategy, then, is to do as much of the reordering as possible in a streaming way, without taking advantage of random access. This is an old, old technique, like people would have used in the 1970s with a computer that had almost no memory but had a lot of tape decks that could read and write at full speed as long as they were going in order. It still works the same way if you read a stream from one file and split it into streams into several output files, because they can all go as fast as the physical disk I/O can go.

The [top-level sort](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1089) is a [radix sort](https://en.wikipedia.org/wiki/Radix_sort). It [checks how many files it can have open at once](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1107), and if the answer is, say, 512, then it tries to split the index and geometry into 128 parts (512/4), based on the leading bits of each feature's quadkey.

It then [goes through each of those parts](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L890) and checks whether it would fit in memory. If it would, it splits that portion of the index up again by however many CPUs it has to work with, [sorts each of those sub-indices](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L413) in its own thread with the system `qsort`, and then [merges those sub-sub-indices](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L352) to copy the sub-geometry to the final geometry file in index order, which should be fast because it's all in memory. If one of the parts *didn't* fit in memory, then it does [another radix sort](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1054) to split it up further, based on the next bits of the quadkey. In at most a few passes of this it will have generated final sorted index and geometry files in quadkey order.

Two invariants here are easy to break and hard to notice. The first is that a bucket small enough to be written out directly, rather than through the merge, must still be [written one byte shorter than the index says](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1014) and then given a fresh minzoom byte, exactly as `merge()` does — the `MAGIC` byte again. Write the original byte as well as the new one and every feature read after it comes out shifted by one. The second is that each level of subdivision must [consume at least one bit of the index](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L746), which means at least two buckets: with only one, the recursion never reaches the prefix width that stops it, and the shift that selects a bucket is by the full width of the index, which is undefined.

Both invariants only matter on inputs large enough to recurse, which is more data than a test wants to handle, so `--prefer-radix-sort` exists to reach them: it [pretends there are only 8 KB of memory](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1103) so that a tiny test input goes down the deep path, and the test checks the radix-sorted output against the in-memory sort of the same data.

Guessing maxzoom
----------------

Once the index is in order, Tippecanoe can [guess a maxzoom](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2279) from the data, if `-zg` was given instead of an explicit `-z`.

The idea is that a good maxzoom is one where most features are distinguishable from each other, so it walks the sorted index and accumulates the mean and standard deviation of the log of the gaps between adjacent quadkeys, using [Welford's algorithm](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2312). Distances between features are typically lognormally distributed, so the geometric mean is the right average, and [1.5 standard deviations below the mean](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2353) is taken as the distance at which features should still be distinguishable. The conversion from quadkey gaps to feet is an empirical fit; the `#if 0` block just above the calculation is the code that produced the data for it.

Several things adjust the result afterward:

 * The [distances *within* each feature](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2416), sampled back in `serialize_feature`, can push the maxzoom higher, because a tileset of detailed coastlines needs resolution even if there are only a few features.
 * [Duplicate feature locations](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2407) push it higher too, since a pile of features at exactly the same point will never be distinguishable but will be dropped at the drop rate.
 * [Clustering](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2377) pushes it higher, so that features eventually become unclustered.
 * A [2-million-tile budget](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2439) pulls it back down, estimated from the total polygon area, so that a request that would spend a week filling in the interiors of polygons doesn't.
 * `--smallest-maximum-zoom-guess` sets a floor, and the [detail limits](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2432) set a ceiling.

The drop rate can be guessed at the same time, from [a curve fitted by eye](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2390) to the drop rates that looked right for a handful of real point tilesets: evenly spaced features want a large drop rate, clumpy features can get away with a small one.

Guessing base zoom and drop rate
--------------------------------

Independently of `-zg`, Tippecanoe can [calculate a base zoom and drop rate](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2490) if requested to do so with `-Bg` or `-rg`.

It can do this because all the features that are (centered in) the same tile are now guaranteed to be contiguous in the index because it is ordered by quadkey. So Tippecanoe can calculate the densest tile in each zoom level just by running through the index in order, accumulating one count of features for each zoom level, and resetting the counter [every time the tile number at that zoom level changes](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2536).

Now that it knows how many features are in the densest tile at each zoom level, it can calculate [what the lowest zoom level is](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2569) where the densest tile contains an acceptable number of features and [what fraction of features must be dropped](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2617) at lower zoom levels to make the densest tile acceptable at every zoom level. If no base zoom at or below the maxzoom will do, it [works from the other direction](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2589) instead, choosing a drop rate first and then the base zoom that rate implies.

Precalculating the dot dropping
-------------------------------

The per-feature minzoom field is what makes dot-dropping consistent from one zoom to the next, which is what keeps points from popping in and out as you zoom. Deciding it up front, for all the features at once, is what makes that consistency possible: no tile has to work out for itself which features it is entitled to show.

[`calc_feature_minzoom`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L299) assigns each feature the lowest zoom at which it will appear, by keeping a running counter per zoom level that is decremented by that zoom's interval (`droprate^(basezoom - z)`) for each feature that goes by. It is called from inside the merge, as the features are being copied into index order, so it sees them in quadkey order and therefore spreads its choices evenly over space rather than picking a random subset. `--preserve-point-density-threshold` adds an override: if a feature was assigned to a high zoom but is nevertheless very far from the last feature chosen for some low zoom, it gets pushed out at that low zoom anyway, so that sparse areas of the map don't go blank.

If the base zoom or drop rate had to be guessed, they weren't known yet when the merge ran, so there is a [fixup pass](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2672) that goes back and rewrites all the minzoom bytes in place, which is possible precisely because that byte is the last byte of each feature and its position is recorded in the index. `--drop-denser` is implemented [in the same pass](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2692), by sorting a sample of features by the gap to their predecessor and assigning zooms so that the sparsest ones appear first.

Running through the zoom levels
-------------------------------

Now it is finally time for tiling to begin. All that will remain after tiling is complete is to calculate the final bounding box and layer metadata and write it to the tileset.

The [outer loop of tiling](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3301) starts by running through the possible zoom levels. It normally starts at zoom 0 even if some higher `minzoom` was specified, because the entire geometry is now in a single file, not at all split up by tile. If we tried to use the same simple index-based technique as above to split it up into `minzoom` tiles, we would miss some features that are big enough to span (or be buffered into) multiple tiles. Instead, we must "divide and conquer" the low zooms to get to the high zooms correctly, even if some of them will not ultimately be written to the tileset.

The one exception is that [`choose_first_zoom`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L1147) checks whether the bounding box of all the features fits within a single tile at some higher zoom, and if so starts there, since dividing and conquering an empty world is a waste of time.

The basic strategy of tiling is that each zoom level will read through the current set of temporary files and will write out both a set of vector tiles into the tileset and a new set of internal temporary files for the next zoom level. For example, the initial single geometry file will produce the single zoom level 0 tile 0/0/0 as well as the four temporary files that will be used at the next step for tiles 1/0/0, 1/0/1, 1/1/0, and 1/1/1. Alternately, if the minzoom was 2, and we don't care about those zoom level 1 tiles at all, we can declare that the "next" zoom past 0 is 2, and the zoom level 0 processing can produce the right temporary files for tiles 2/0/0, 2/0/1, 2/0/2, 2/0/3, 2/1/0, and so on, and never do any work at all for zoom level 1.

Where it gets complicated is that we want Tippecanoe to be able to do as many of these things at the same time as possible, to take advantage of multiple CPUs. The zoom level 1 processing should be able to vectorize and subdivide all four of the zoom level 1 tiles at the same time rather than sequentially. But the ability to do this is limited by the number of CPUs, the number of files that can be open at one time, and the number of below-minzoom levels we are trying to skip over. Each CPU is going to be reading from one file and producing 4 (or 16, or 64, or maybe even 256, if we are skipping three zoom levels) new files as it tiles. Two CPUs can't write to the same output file without risking stomping on each other's work.

So `traverse_zooms` starts out by [making a new set of temporary files for the next zoom level](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3304) and calculating [how many threads there are](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3336) to split those files among. Then it takes all the current set of temporary files that need to be processed and [allocates them approximately evenly across the threads](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3363). (The assumption is that processing time will be proportional to file size, which may not be exactly right, but is reasonably close.)

Note also the `compressor` wrapped around each of those output files. The feature stream for each tile in the temporary files is zlib-compressed, which reduces both the disk space used and the I/O latency, at the cost of not being able to seek within a tile's data except by starting over. (Zoom 0's input is the exception: it is the file the sort produced, and it has to stay uncompressed so that the minzoom fixup can rewrite bytes in it.)

Once the input files and output files have been allocated to threads, it is time to [make a new set of parameter blocks](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3411) to tell each thread what files it is consuming and what other files it is producing, and to start the thread to do the work.

Each thread, then, [starts going through its list of input files](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3141). From each one, it reads a z/x/y tile number and [then calls the badly-named `write_tile`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3206) to do the work of processing that tile and writing out its children to that thread's set of output files. This loop continues until the thread has exhausted all the tiles in all its input files. The loop also does a little bit of work [to track the densest tile at maxzoom](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3214) which will be used later for the map center in the tileset metadata.

Each tile's data in the temporary file is preceded by an [uncompressed `estimated_complexity`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3148), the number of bytes of feature data that tile contains. It can only be known after the data has been written, so a placeholder is written first and then [rewritten with `pwrite`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2355) once the stream is finished (and likewise [for the initial zoom-0 file](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2227)). It is used by variable-depth pyramids, described below.

Retrying a whole zoom level
---------------------------

There is a loop around the whole zoom level, not just around each tile: [`for (size_t pass = 0;; pass++)`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3392). If any tile in the zoom found that it had to raise one of the as-needed thresholds (the minimum gap, the minimum extent, the minimum drop sequence, the minimum attribute value, or gamma) to make itself fit, that new threshold is [collected from all the threads](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3506), the zoom level is [erased from the output](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3563), and the whole zoom is tiled again with the higher threshold. This is why dropping is consistent across a zoom level rather than varying tile by tile.

Note that the tiles are written before it is known whether they will have to be thrown away, rather than the zoom being preflighted to find its thresholds first. That is the cheaper arrangement, because in the common case nothing has to be thrown away at all.

`--extend-zooms-if-still-dropping` is handled [here too](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3549): if the maxzoom is still dropping features, the maxzoom goes up by one and tiling continues.

The work of each tile
---------------------

The [first task of `write_tile`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1676) is to figure out whether it is actually subdividing the tile that it has been tasked with into children, grandchildren, great-grandchildren, or whatever, based on the minzoom and the number of output files it has to work with.

It then moves on to [a loop that usually only runs once](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1719), from the usual tile detail down to the minimum acceptable detail. In most cases this loop will be short-circuited at the end. It will only run multiple times if the tile is too big and Tippecanoe must try again — either at a higher dropping threshold or, as a last resort, at a lower resolution. If this happens it will also have to [rewind its input file](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1774) back to its original position, and restart the decompressor, so that it can reprocess the data.

Then it reads features one at a time from [`next_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1122), which does the opposite of the feature serialization described above, and rather more besides. For each feature it:

 * [Fills in the gap](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1180) to the previous feature, the squared planar distance that `--drop-densest-as-needed` and its relatives sort on. This is computed once, at zoom 0, and then carried along in the serialized feature, so that every zoom has the same idea of which features are in dense company.
 * [Clips the feature to the tile](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L974) plus its buffer. `decode_geometry` has already both undone the `geometry_scale` bit shifting *and* adjusted the offset of the geometry so that (0,0) is at the top-left corner of the tile instead of the top-left corner of the earth. Both of these will be undone again when the geometry is reserialized into the child data for the next zoom level. At zoom 0 there is a special case that [duplicates features near the antimeridian](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L977) onto both sides of the world.
 * [Writes the feature out to the child tiles](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1211) via [`rewrite`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L443) — but only on the first pass through the detail loop, since retries must not write the children again. `rewrite` works out from the feature's bounding box which children it can touch, and picks the shard for each child by [interleaving the low bits of x and y](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L507), being careful that all the data for any one child tile stays contiguous within one shard.
 * Applies the per-feature `tippecanoe.minzoom` and `maxzoom` and the [`-j` feature filter](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1231). Note the ordering: the child tiles are written *before* these tests, so that a feature excluded from this zoom still reaches the zooms where it belongs.
 * [Decides whether the feature is dropped by rate at this zoom](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1290), from the precalculated `feature_minzoom`, and if it is dropped, whether it should nevertheless be retained as one of the extra features of a `--retain-points-multiplier` cluster. The comparison uses a [fractional zoom level](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1163) derived from the bit-reversed index, so that the multiplier can target a specific number of features rather than only the powers of the drop rate.
 * [Removes null attributes](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1321) — after the filter has run, since a filter may want to test for them.

Back in `write_tile`, each feature that comes back is assigned to its layer and then run through the gauntlet of size-reduction strategies, in a long `else if` chain: [gamma](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1983), [`-K` clustering](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1993), [`--drop-densest-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2020), [`--cluster-densest-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2028), [`--coalesce-densest-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2051), [`--drop-smallest-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2065), [`--coalesce-smallest-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2075), [`--drop-fraction-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2089), [`--coalesce-fraction-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2097), and [`--drop-by-attribute-as-needed`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2110). They all have the same shape: sample the relevant quantity into a vector for later, compare it against the threshold for this pass, and either keep the feature or find an already-kept feature to [accumulate its attributes onto](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1544). Only the lead feature of a multiplier cluster can be dropped this way, but if it is, [it drags the rest of its cluster with it](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1970).

Two more things happen per feature before it is accepted:

 * Polygons that are too small to be worth drawing at this zoom are [reduced to dust](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2160) by `reduce_tiny_poly`, which accumulates their area and occasionally emits a placeholder square so that the area is still somehow represented.
 * If the tile is already hopeless — more features than could possibly fit even at one byte each — Tippecanoe [stops adding features](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2217) and just counts how many it skipped, so that it can extrapolate what the real size would have been without spending the memory to find out exactly. The size and feature limits are [scaled up](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2197) in proportion to the number of multiplier features being carried alongside the lead features, since those are expected to be filtered out by the consumer.

The rest of tiling
------------------

Once all the features have been read, the work becomes per-layer rather than per-feature. In order:

 * [Multiplier sequence numbers](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2393) are attached to each feature, so that a consumer can reconstruct the clusters.
 * [Cluster attributes](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2420) (`clustered`, `point_count`, `sqrt_point_count`, `point_count_abbreviated`) are added to anything that other features were clustered into, matching what [supercluster](https://github.com/mapbox/supercluster) produces.
 * The [tilestats](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2453) are updated, since attribute accumulation may have introduced values that weren't in the input.
 * [`--detect-shared-borders`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/shared_borders.cpp#L86) breaks the polygons of the layer into arcs at the points where they meet, so that a border shared between two polygons is simplified identically in both and no gaps open up between them.
 * [Simplification](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2467) runs across several threads. Each feature goes through [`simplify_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L595), which is either [Douglas-Peucker](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/clip.cpp#L909) or, with `--visvalingam`, [Visvalingam-Whyatt](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/visvalingam.cpp#L147); then [scales the geometry down to tile coordinates](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L682) and cleans polygons with [Clipper2](https://github.com/AngusJohnson/Clipper2), reviving any that were flattened out of existence as a [rectangle of the right area](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L551). Simplification can also happen [early, mid-tile](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2289), if the unsimplified geometry is piling up in memory.
 * [`--reorder` and `--coalesce`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2520) sort features so that identical-attribute features are adjacent, and then merge those runs into single multi-features.
 * [`--preserve-input-order`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2583) and [`--order-by`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2589) put the features into their final order. Both work on whole multiplier clusters, assembled and disassembled around the sort, so that a cluster's extra features stay with their lead feature.
 * [`--limit-tile-feature-count`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2595) truncates the layer, if asked.

Then the layers are [converted into an `mvt_tile`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2617), with the pooled keys and values turned back into tile attributes by [`decode_meta`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L235), the postfilter is run if there is one, and the tile is [encoded and compressed](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2876).

And now the size checks, which are the reason for the whole loop. If the tile has too many features or too many bytes — extrapolated upward if features were skipped — then Tippecanoe picks the strategy it was told to use and [raises that strategy's threshold](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L2913), aiming at a fraction of the current size, and goes around again. The functions that choose the new threshold ([`choose_mingap`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L754) and its siblings) work on the samples that were collected on the way through. If the threshold can't be raised any further, that is an error: there is nothing left to try, and looping forever is worse than failing. If no dropping strategy was requested at all, the detail is reduced by one and the tile is tried again from the top.

If the tile does fit, it is [written to the database or directory](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3048) under the `db_lock`, the strategies used are folded into the zoom's statistics, and `write_tile` returns.

Variable-depth tile pyramids
----------------------------

`--generate-variable-depth-tile-pyramid` cuts across everything above, so it gets its own section.

The idea is that a tile in an empty or simple part of the map doesn't need children: if all of its features can be included at full precision, a client can overzoom that tile instead of downloading deeper ones. So when it is enabled, `write_tile` [makes an extra first attempt](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1701) at a much higher detail (`30 - z`) with simplification turned off, and only bothers to do so if the `estimated_complexity` recorded with the tile's data suggests it might work. If everything fits at that detail and nothing was dropped, the tile is a leaf: it is [added to `skip_children`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3065) and its descendants are [skipped rather than written](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3179) at the next zoom.

The bookkeeping around that is fiddlier than it sounds:

 * The children's geometry is still in the stream even for a skipped tile, so [`skip_tile`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1659) has to read past it rather than seek.
 * A zoom that later has to start dropping features [invalidates the truncation](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3180), because a leaf is only legitimate if it is complete. Those revived tiles have never written their children, so they have to do it on [the pass where the dropping started](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3191) rather than on pass 0 as usual.
 * A feature with an explicit `tippecanoe.minzoom` deeper than the current zoom [blocks leafing](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1221), because it isn't in this tile and its children would never be generated, so it would appear at no zoom at all.
 * The tileset's reported maxzoom is the [deepest zoom actually written](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L3605), not the requested one.

Plugins
-------

Tippecanoe can pipe each tile's contents through a shell command, either before tiling (`-C`, the prefilter) or after (`-c`, the postfilter). In both cases the interchange format is newline-delimited GeoJSON, one feature per line, so the filter can be anything from `grep` to a program of your own.

[`setup_filter`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/plugin.cpp#L376) does the plumbing: two pipes, a `fork`, and an [`execlp` of `sh -c`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/plugin.cpp#L436) with the tile's z, x, and y as `$1`, `$2`, and `$3`.

The prefilter is the more interesting of the two, because it has to interpose itself in the middle of the feature-reading loop. [`run_prefilter`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1381) runs in its own thread — a real thread, not just a subprocess, because Tippecanoe needs to write to and read from the filter at the same time and would otherwise deadlock — calling `next_feature` and writing each feature out as GeoJSON in world coordinates. Meanwhile the main loop in `write_tile` reads the filter's output with [`parse_feature`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1882) instead of calling `next_feature` itself. Note that the features handed to the prefilter have already been clipped to the tile and already had their dot-dropping decision made — whether the feature was dropped is passed through as a top-level [`dropped` member](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/write_json.cpp#L301) of each GeoJSON object, alongside its `index`, `sequence`, and `extent` — so the filter sees what the tile would have contained.

The postfilter is simpler, because by the time it runs the tile is a finished set of `mvt_layer`s: [`filter_layers`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/plugin.cpp#L470) writes them out as GeoJSON from [a writer thread](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/plugin.cpp#L47) and [parses the result back](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/plugin.cpp#L78) into layers, updating the layermap and tilestats from whatever came out, since the filter may have invented layers and attributes that were never in the input.

Both filters are [skipped below the minzoom](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile.cpp#L1801), since those tiles are only being generated to subdivide their way toward real ones.

Feature filters
---------------

Filtering through a subprocess is flexible but slow. For the common cases there is `-j`, which takes a filter expression and applies it in-process. The syntax is [the Mapbox GL Style Specification filter syntax](https://docs.mapbox.com/style-spec/reference/other/#other-filter), extended with `$type`, `$id`, and `$zoom` pseudo-attributes and with an `attribute-filter` form that removes individual attributes rather than whole features.

[`eval`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/evaluator.cpp#L94) is a straightforward recursive interpreter over the parsed JSON of the expression. It also understands a second, Felt-style expression dialect, and `--unidecode-data` lets string comparisons match transliterated forms so that a filter for "Zurich" can find "Zürich".

The same evaluator is used by `tile-join` and `tippecanoe-overzoom`, which is why it lives in its own file.

Writing tileset metadata
------------------------

When tiling is done, `read_input` computes the tileset's bounds and center and writes the metadata.

The center is the center of [the densest tile at maxzoom](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2798), which `traverse_zooms` tracked, [clamped](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2807) to lie within the bounds. The bounds themselves come in two forms: the ordinary one, and an [antimeridian-aware one](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2820) that picks whichever of the two world planes tracked during input gives the narrower box, so that a tileset covering Fiji doesn't claim to span the entire globe.

The per-thread layermaps are [merged](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/main.cpp#L2831) — this is where the differing per-thread layer numbering is finally reconciled, by name — and [`make_metadata`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L668) assembles everything into a [`metadata` struct](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.hpp), which is then written either [into the sqlite `metadata` table](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L468) or [into a `metadata.json`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/dirtiles.cpp#L288) in the output directory. Having the struct in the middle is what keeps the two output forms from drifting apart, and lets `tile-join` and the PMTiles writer reuse the same code.

The fields are:

 * `name`, `description`, `version`, `type`, `format`, `minzoom`, `maxzoom`, `bounds`, `center`, and `attribution`, which are the standard mbtiles metadata.
 * `antimeridian_adjusted_bounds`, the alternative bounds described above.
 * `generator` and `generator_options`, which record the Tippecanoe version and the entire command line, so that a tileset can say how it was made.
 * `strategies`, [an array with one entry per zoom level](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L372) recording how many features were dropped by rate, by gamma, or as-needed; how many were coalesced; how many tiles had their detail reduced; how many tiny polygons there were; how many zooms were truncated by variable-depth pyramids; and what the largest tile would have been if nothing had been dropped. If you want to know whether your tileset is losing data, this is where to look.
 * `tippecanoe_decisions`, which records the basezoom, drop rate, and multiplier that were actually used, which matters when they were guessed rather than specified.
 * `json`, a string containing two things: `vector_layers`, the [per-layer list of attributes and their types](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L719) that style editors read, and `tilestats`, the [more detailed statistics](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L206) — counts, minimums, maximums, and sample values for every attribute of every layer. The tilestats are bounded by `--tile-stats-attributes-limit` and friends, because for a source with hundreds of millions of features the statistics can otherwise get quite large.

Finally, [`mbtiles_close`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/mbtiles.cpp#L823) runs `ANALYZE` and closes the database.

Converting to PMTiles
---------------------

If the output name ended in `.pmtiles`, everything above has been writing an ordinary mbtiles file, and [`mbtiles_map_image_to_pmtiles`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pmtiles_file.cpp#L139) now converts it. It reads the tiles back out in tile-id order, writes them into the single-file PMTiles layout, and translates the metadata into [the PMTiles flavor of JSON metadata](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/pmtiles_file.cpp#L86). Because the mbtiles schema already deduplicates tiles by content hash, the PMTiles directory can carry that deduplication straight through.

The other programs
------------------

Tippecanoe proper is only part of what gets built. The rest of the tools in the [Makefile](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/Makefile#L33) share most of their code with it:

**`tile-join`** merges tilesets, joins attributes onto their features from a CSV or a sqlite database, filters layers and attributes, and can overzoom to a deeper maxzoom on the way. Its [main loop](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile-join.cpp#L1059) reads the inputs in parallel through a set of readers, groups the tiles by z/x/y, [hands each group to a worker thread](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile-join.cpp#L895) that [appends each source tile's layers into one output tile](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/tile-join.cpp#L101), and reassembles the metadata from the inputs'. Because it works entirely on already-tiled data, it can do in a few minutes what re-tiling from source would take hours to do.

**`tippecanoe-overzoom`** takes one or more source tiles and produces a single deeper tile from them, doing the clipping, the multiplier de-duplication, the attribute accumulation, the filtering, the simplification, and optionally the binning of points into a supplied set of polygons. [The overzoom function itself](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/clip.cpp#L1453) lives in `clip.cpp` because `tile-join` uses it too. It converts each source feature's geometry back to world coordinates, offsets it into the destination tile, and then runs it through the same clipping code that tiling uses.

**`tippecanoe-decode`** turns tiles back into GeoJSON, which is indispensable for figuring out what actually ended up in a tileset. `--stats` reports the size and feature count of each layer instead of the contents.

**`tippecanoe-json-tool`** does GeoJSON manipulation outside of tiling: sorting features by an attribute (so that `tile-join`-style joins can be done with `sort` and `join`) and joining CSV attributes onto features.

**`tippecanoe-enumerate`** lists the z/x/y of every tile in a tileset, one per line, which is the input to shell pipelines that want to do something to each tile.

Tests
-----

The [test suite](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/Makefile#L136) is mostly of one kind: run Tippecanoe over a small input in [`tests/`](https://github.com/felt/tippecanoe/tree/63fcac725abeb841a101abfc64189edd66d1cf14/tests) with some set of options, decode the result, and compare it against a checked-in expected output. This makes it easy to see the effect of a change — the diff in the expected outputs *is* the change — but it also means that a change to feature ordering shows up as an enormous diff, which is why a few comments in the code apologize for not fixing something because it would churn all the fixtures.

There is also [`unit.cpp`](https://github.com/felt/tippecanoe/blob/63fcac725abeb841a101abfc64189edd66d1cf14/unit.cpp), a small set of [Catch](https://github.com/catchorg/Catch2) unit tests for the pieces that are awkward to test end-to-end.
