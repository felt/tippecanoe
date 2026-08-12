A tour of Tippecanoe
====================

Tippecanoe has what sounds like a simple job: It takes geographic objects in one file format ([GeoJSON](http://geojson.org/)) and copies them into a different file format ([Mapbox Vector Tiles](https://www.mapbox.com/vector-tiles/specification/)). But it takes a lot of code to do that. What's really going on?

Starting up
-----------

[The main function](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1728) has the job of processing the list of options and files that the user provides. I'll go into more detail later about what all those options actually do, but the list of them is [spelled out here](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1765) with long names, and [then again](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1836) with the cryptic short names and the constraints that are placed on some of them.

After the options are processed, we reach the [three core operations](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L2097): create a new mbtiles file, read input into it, and then close the mbtiles file.

Creating an mbtiles file
------------------------

[Creating an mbtiles file](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/mbtiles.cpp#L19) is a series of sqlite operations, most of which I copied from [mbutil](https://github.com/mapbox/mbutil). It opens the database and tries to create `metadata` and `tiles` tables with indices. There is an option to keep going even if these operations fail, to support a geocoding use case that needed to add new, non-conflicting, tiles to an existing mbtiles file.

Temporary files to read input into
----------------------------------

[Reading input](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L859) is deceptively named because it also ultimately invokes the whole tiling process.

Its first job is to [create as many sets of temporary files](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L864) as your computer has CPUs, so that multiple threads can read input into these files at the same time without interfering with each other. At the end of the actual input stage, the per-thread temporary files will be merged or concatenated together so that tiling will operate on the combined results.

The temporary files are:

 * `geom`, which, as the name suggests, contains the feature geometry, plus [other characteristics of each feature](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/serial.hpp#L22) or references into other temporary files.
 * `index`, which is an index of the features in `geom` [by their byte offet within the file and their quadkey-encoded bounding box center](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.hpp#L1). It will be used later to merge the per-thread `geom` files back together.
 * `pool`, which contains one deduplicated instance of each property key or value that appears in any feature.
 * `tree`, which is a binary tree of `pool` entries, so that it can look up the existing pooled copy of any keys or values that are duplicated across multiple features, or add a new entry to the pool when a key or value is used for the first time.
 * `meta`, which stores keys and value indices into the `pool` for any features that have enough properties or span a large enough number of tiles that it will take a lot of space to have many copies of the same key/value list.

All of this data is in temporary files instead of in memory because it can be very large, and putting it on disk makes it possible to tile data that is too big to fit in memory. Processing will be much faster if the data does fit in memory, though, in which case the copy on disk is just incidental to what is in memory in the operating system's file buffers.

What is straightforwardly in memory, not on disk, because it is small, is [the "layermap"](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1026) for each CPU, which is a list (by name) of the tile layers and all the property keys that have been used for any features in that layer. This will eventually end up in the mbtiles metadata.

Three ways to read input
------------------------

Now that the temporary files are in place, it is time to [start reading input](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1042), either from `stdin` or from the GeoJSON files named on the command line. There are three ways that input may files may be read, depending on whether the input is from a file or a stream and whether the user has asked for parallel input processing.

1. If the `--read-parallel` option was specified, Tippecanoe [first tries to map the file into memory](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1062). If this was successful, it [creates several threads to parse parts of it in parallel](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1075) as will be described below.

2. If the file can't be mapped into memory (which means that it is an input stream, not a file on disk), Tippecanoe [opens it as a regular file](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1085). If the user has nevertheless asked for parallel processing of input, Tippecanoe [starts reading the stream into another temporary file](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1095) that it *can* map into memory. Once it has accumulated enough to be worth [starting up several parser threads](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1144), it does that, and the main thread goes back to reading more streaming input into the next temporary file. This back-and-forth continues until the whole input has been consumed.

3. If the user didn't ask for parallel input, it just [runs a single JSON parser](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1198) on the streaming input file.

Parsing GeoJSON in parallel
---------------------------

To avoid massive digression I will first talk about parsing GeoJSON in parallel. The [`do_read_parallel` function](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L303) starts with the file already mapped into memory and knows how many CPUs can work on it simultaneously. It starts by [making a guess](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L309) that each CPU should have an equal-sized fraction of the file to work on, and then adjusts this guess by moving the divisions forward until each of them begins with a newline character.

(Note that GeoJSON itself makes no guarantee that feature objects are separated by newlines, or that newlines will never appear in the middle of a feature. This lack of a guarantee is why parallel parsing only happens if the user asks for it, because it will misinterpret many correct GeoJSON inputs that are not designed for line-delimited use.)

It [sets up a parameter block for each CPU](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L332) pointing it to all the global settings and to the per-CPU temporary files and semi-global properties (bounding box, layermaps) and [a JSON parser](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L333) pointed to the portion of the file that was allocated to that CPU, and [creates a thread](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L362) to run the single-stream parsing described immediately below, from that JSON parser. Then it [waits for all the threads to finish](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L368) and is done.

Parsing a single GeoJSON stream
-------------------------------

[To parse GeoJSON](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L475), whether from a fraction of a memory-mapped file as described immediately above, or from a regular file stream, Tippecanoe uses uses [a generic JSON pull-parser](https://github.com/ericfischer/json-pull). By pull-parser, I mean that the parser returns JSON tokens and objects one at a time as they are encountered, rather than building up a full JSON object structure in memory for the entire input file and then presenting that to the caller.

Tippecanoe loops over the tokens and objects that the parser returns until it encounters either a [bare geometry](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L510) (an object whose `type` is `Point`, `LineString`, `Polygon`, `MultiPoint`, `MultiLineString`, or `MultiPolygon` and is not contained within something that could be a feature) or [a feature](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L549) (an object whose `type` is `Feature`). In either case, it calls `serialize_geometry` to turn the GeoJSON feature (or bare geometry) into an internal feature and then write it into the temporary files.

Internal representation of a feature
------------------------------------

[Turning a GeoJSON feature into a Tippecanoe feature](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L170) starts with some checking: does the geometry have a `coordinates` array? Is the `type` one of the kinds of things that GeoJSON defines? It also [checks the feature](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L212) for a special `tippecanoe` object that can specify a per-feature minzoom, maxzoom, or layer name, and for the top-level feature `id` tag that is independent of the feature properties.

[For each of the feature's properties](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L271) it identifies the property's type and, if it is a compound JSON object that can't be natively represented as one of the attribute types that vector tiles supports, stringifies it. These are all just in memory for the moment.

It then calls `parse_geometry` to recursively unpack the `coordinates` array from the `geometry`. This is also in memory for the moment. If the geometry was a polygon, there is a `fix_polygon` pass to make sure the rings are wound in the correct direction, since GeoJSON identifies parent and child rings by their position in the `coordinates` array but vector tiles represent them by positive or negative area. See below for more about parsing and reprojecting geometry.

Now that the geometry (and its bounding box) is available, Tippecanoe [can make the decision](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L325) whether the feature's properties should be serialized as part of the main feature, in the `geom` file, or indirected out into the `meta` file to avoid duplication across many high-zoom tiles.

For each feature, Tippecanoe also uses the bounding box to calculate [the lowest zoom level at which it will appear](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L357). This doesn't really make sense any more and should be removed, but it used to be useful for saving a little bit of time unpacking geometry for very short LineStrings, and when dot-dropping for points by zoom level was random instead of spatially distributed. (More about that in the section on tiling below.)

> EDIT: the lowest-zoom-level field is now used to precalculate dot dropping so it can be consistent from each zoom to the next

More usefully, if a feature has a named layer, Tippecanoe [adds a layermap entry for it](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L375) to the per-CPU layermap table. Because layers within each serialized feature are specified by number, not by name, different threads may assign different layer numbers here to the same layer name, which will need to be reconciled later.

Tippecanoe then [assembles everything about the feature](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L391) into an object that can be serialized to the temporary files. If the properties are going to go in the separate `meta` file instead of as part of the rest of the feature in the `geom` file, [they are written there now](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L406). As each key and value is either added to the object or written to the `meta` file, the `addpool` call either looks it up within or adds it to the `pool` and `tree` files and returns its unique offset within the per-CPU `pool` file.

The last stage of serialization, after [writing the feature to the `geom` file](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L420) in `serialize_feature`, is [to add the feature to the `index`](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L422).

All that remains is to [increase the per-CPU bounding box](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L437) to encompass the feature bounding box if necessary, and increment the user-visible progress indicator.

Serialized representation of a feature
--------------------------------------

Serializing each feature to the `geom` file [probably looks familiar](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/serial.cpp#L177) if you have done any serialization with [protozero](https://github.com/mapbox/protozero). The main difference is that the serialization here goes straight to the file rather than into an in-memory buffer that is then written to the file. There is not much sanity-checking so you have to be careful if you make changes, especially in the field that [does some bit packing](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/serial.cpp#L181) to combine the layer number and the flags for the presence of `minzoom`, `maxzoom`, and `id` into the same number.

All of the primitive serialization types (integers of various sizes and signednesses) are spelled out earlier in the same `serial.cpp` file. They use `protozero`'s zigzag conversion functions to represent signed numbers as unsigned, and use its same variable-length encoding for numbers, with the high bit of each byte indicating that there are more bytes to follow.

Internal representation of geometry
-----------------------------------

[Parsing the GeoJSON coordinates array](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L78) into Tippecanoe's internal representation of geometry, as mentioned above, deserves a little bit more detail.

Tippecanoe's [internal geometry structure](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geometry.hpp#L17) is called `draw`, and is composed of an operation (moveto, lineto, closepath) and an `x` and `y` coordinate, plus an auxiliary field used during line simplification to track whether a point has determined to be `necessary`. A series of `draw` operations [is called](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geometry.hpp#L33) a `drawvec`. The `draw` structure uses some bitfields to keep its size down to 128 bits even with the `op` and `necessary` fields to avoid eating a lot of unnecessary memory.

The `parse_geometry` function mostly just builds up a `drawvec` from the GeoJSON coordinate arrays for the feature, after doing some sanity checking to make sure the arrays contain numbers and an appropriate number of elements. It calls itself recursively to parse the multiple points that appear within each LineString, the multiple rings that appear within each Polygon, and so on.

It also [contains the call](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L106) that reprojects the coordinates from their original projection (usually WGS84) to tile coordinates, where the world is a square, 2^32 units on each side. There is a special case (to save space in the temporary files) that shifts each of these world coordinates down by `geometry_scale`, which was set back in the `main` function to correspond to the global `minzoom`, so that coordinates on disk are not stored in more detail than will eventually be used in the final tiles. The reduced coordinates will be shifted back up to world scale during each deserialization during tiling.

There is also [a weird special case](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geojson.cpp#L164) that adds a `closepath` operation after the last ring of each Polygon, so that the following ring can be identified as an outer ring. (Tippecanoe does not otherwise use `closepath`, except in the last stages of writing out the vector tile format, which requires it as part of each polygon ring.) The [`fix_polygon` function](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/geometry.cpp#L1133) uses this special tag when it reorders the points in each ring to make sure that inner rings are oriented the opposite direction from outer rings. It also makes sure the ring is closed, with the last point duplicating the first, if it wasn't already.

Merging temporary files after parsing
-------------------------------------

At this point Tippecanoe has all of the GeoJSON input from all of its input files parsed and copied into temporary files, but each layer's features are scattered across multiple files, depending on which CPU happened to read the feature. The files have to be consolidated before tiling can begin.

The `tree` file is of no more use and is [simply closed and discarded](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1229). The other temporary files that have been being written to are closed.

But it is important to note that [back when the temporary files were being opened](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L879), each of them was opened *twice*, and then deleted. Each of them was opened as an integer file descriptor with `mkstemp`, and as a C `FILE` with `fopen`, and deleted with `unlink`. The two open file descriptors to each file cause it to continue to exist on disk even though it has been deleted from the `/tmp` directory. Deleting the file before it is written should also serve as a hint to the operating system that there is no need to write ever the file's contents to physical disk if it is small enough to continue to fit in memory, in the operating system's buffer cache. (Using a deleted file for temporary storage is a traditional Unix idiom, and there is a `tmpfile` call in the standard library to create and delete a temporary file if you only need a single `FILE` reference to it.)

So now the `FILE` for each is closed, so each temporary file is kept alive by only *one* file descriptor, not two, and any buffering that was being done inside the Tippecanoe process itself, as opposed to in the operating system, will have been flushed out.

Merging the `meta` and `pool` files is the easiest. The files for each CPU [are just concatenated together](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1241) into a new file, with an array to keep track of the offset into the new file where the data for each CPU begins. (Which thread some data originally came from is frequently referred to in the code as its `segment`.) The new combined `meta` and `pool` files are then mapped back into memory so they can be accessed like an array.

Why sort the features?
----------------------

The geometry is going to be much more of a pain to merge, not just because it is typically so much bigger than the other files, but because we also want it in a specific order. In particular we want it to be ordered by quadkey so that

1. when the low zooms are thinned out by dropping some fraction of features, those features are evenly distributed by location rather than clumpy,
2. it is possible to make global estimates of the number of features in each tile, before actually generating those tiles, and
3. within each tile, the density of features near any particular feature can be estimated by the difference between its quadkey index and the numerically adjacent feature's quadkey index.

These characteristics will be used by the `-r` dot dropping rate, the `-Bg` guessing of an appropriate base zoom level, and the `-g` thinning of dense features, respectively.

We can accomplish this by sorting the `index` by quadkey and then copying features from the original files into a new temporary file into index order. This is the user-visible "Reordering geometry" phase.

Sorting and merging
-------------------

The index itself may actually be uncomfortably large to sort. Each [index entry](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.hpp#L1) takes 32 bytes of storage, so on a 4GB laptop, 100 million features will exhaust available memory and grind progress to a halt, sometimes crashing the system in the process. And recopying the geometry in index order after sorting may be even worse, because the original order of the geometry probably has no relationship to the index order, so if it does not fit in memory, retrieving each feature may require a disk access.

Tippecanoe's strategy, then, is to do as much of the reordering as possible in a streaming way, without taking advantage of random access. This is an old, old technique, like people would have used in the 1970s with a computer that had almost no memory but had a lot of tape decks that could read and write at full speed as long as they were going in order. It still works the same way if you read a stream from one file and split it into streams into several output files, because they can all go as fast as the physical disk I/O can go.

The [top-level sort](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L485) is a [radix sort](https://en.wikipedia.org/wiki/Radix_sort). It [checks how many files it can have open at once](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L821), and if the answer is, say, 512, then it tries to split the index and geometry into 128 parts (512/4), based on the first 7 bits (2^7 = 128) of each feature's quadkey.

It then [goes through each of those 128 parts](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L619) and checks whether it would fit in memory. If it would, it splits that portion of the index up again by however many CPUs it has to work with, [sorts each of those sub-indices](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L258) in its own thread with the system `qsort`, and then [merges those sub-sub-indices](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L212) to copy the sub-geometry to the final geometry file in index order, which should be fast because it's all in memory. If one of the 128 parts *didn't* fit in memory, then it does [another radix sort](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L764) to split it up further, based on the next 7 bits of the quadkey. In at most a few passes of this it will have generated final sorted index and geometry files in quadkey order.

Guessing base zoom and drop rate
--------------------------------

Now that the index is in order, Tippecanoe can [calculate a base zoom and drop rate](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1417) if requested to do so with `-Bg` or `-rg`, as [described in a previous devlog](https://github.com/mapbox/hey/issues/5107).

It can do this because all the features that are (centered in) the same tile are now guaranteed to be contiguous in the index because it is ordered by quadkey. So Tippecanoe can calculate the densest tile in each zoom level just by running through the index in order, accumulating one count of features for each zoom level, and resetting the counter [every time the tile number at that zoom level changes](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1469).

Now that it knows how many features are in the densest tile at each zoom level, it can calculate [what the lowest zoom level is](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1506) where the densest tile contains an acceptable number of features and [what fraction of features must be dropped](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1543) at lower zoom levels to make the densest tile acceptable at every zoom level.

Running through the zoom levels
-------------------------------

Now it is finally time for [tiling to begin](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/main.cpp#L1615). All that will remain after tiling is complete is to calculate the final bounding box and layer metadata and write it to the mbtiles file.

The [outer loop of tiling](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L1276) starts by running through the possible zoom levels. Notice that it starts at 0 even if some higher `minzoom` was specified, because the entire geometry is now in a single file, not at all split up by tile. If we tried to use the same simple index-based technique as above to split it up into `minzoom` tiles, we would miss some features that are big enough to span (or be buffered into) multiple tiles. Instead, we must "divide and conquer" the low zooms to get to the high zooms correctly, even if some of them will not ultimately be written to the tileset. (Early versions of Tippecanoe did use the index instead, and it worked badly with large and buffered features.)

> EDIT: It now starts at a higher zoom than 0 if the bbox of all the features is contained within one tile at a higher zoom

The basic strategy of tiling is that each zoom level will read through the current set of temporary files and will write out both a set of vector tiles into the mbtiles and a new set of internal temporary files for the next zoom level. For example, the initial single geometry file will produce the single zoom level 0 tile 0/0/0 as well as the four temporary files that will be used at the next step for tiles 1/0/0, 1/0/1, 1/1/0, and 1/1/1. Alternately, if the minzoom was 2, and we don't care about those zoom level 1 tiles at all, we can declare that the "next" zoom past 0 is 2, and the zoom level 0 processing can produce the right temporary files for tiles 2/0/0, 2/0/1, 2/0/2, 2/0/3, 2/1/0, and so on, and never do any work at all for zoom level 1.

Where it gets complicated is that we want Tippecanoe to be able to do as many of these things at the same time as possible, to take advantage of multiple CPUs. The zoom level 1 processing should be able to vectorize and subdivide all four of the zoom level 1 tiles at the same time rather than sequentially. But the ability to do this is limited by the number of CPUs, the number of files that can be open at one time, and the number of below-minzoom levels we are trying to skip over. Each CPU is going to be reading from one file and producing 4 (or 16, or 64, or maybe even 256, if we are skipping three zoom levels) new files as it tiles. Two CPUs can't write to the same output file without risking stomping on each other's work.

So `traverse_zooms` starts out by [making a new set of temporary files for the next zoom level](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L1282) and calculating [how many tasks there are](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L1299) to split those files among. Then it takes all the current set of temporary files that need to be processed and [allocates them approximately evenly across the threads](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L1333). (The assumption is that processing time will be proportional to file size, which may not be exactly right, but is reasonably close.)

Once the input files and output files have been allocated to threads, it is time to [make a new set of parameter blocks](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L13790) to tell each thread what files it is consuming and what other files it is producing, and to start the thread to do the work.

Each thread, then, [starts going through its list of input files](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L1172). From each one, it a reads a z/x/y tile number and [then calls the badly-named `write_tile`](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L1172) to do the work of processing that tile and writing out its children to that thread's set of output files. This loop continues until the thread has exhausted all the tiles in all its input files. The loop also does a little bit of work [to track the densest tile at maxzoom](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L1223) which will be used later for the map center in the mbtiles metadata.

The work of each tile
---------------------

The [first task of `write_tile`](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L571) is to figure out whether it is actually subdividing the tile that it has been tasked with into children, grandchildren, great-grandchildren, or whatever, based on the minzoom and the number of output files it has to work with.

It then moves on [a loop that usually only runs once](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L594), from the usual tile detail down to the minimum acceptable detail. In most cases this loop will be short-circuited at the end. It will only run multiple times if a tile is too big to be legally included in an mbtiles upload, and Tippecanoe must try successively lower resolutions for the tile in an attempt to make the data fit. If this happens it will also have to [rewind its input file](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L622) back to its original position so that it can reprocess the data.

But in general it is just going to read features from the input file and handle them. It does [the opposite of the feature serialization described above](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L630) to read features out of the temporary file and back into memory.

It should be noted that the [call to `decode_geometry`](https://github.com/mapbox/tippecanoe/blob/dc86eb6b5a425c91c95666594a7c8289b60eb09d/tile.cpp#L662) to turn serialized geometry back into internal geometry both undoes the `geometry_scale` bit shifting *and* adjusts the offset of the geometry so that (0,0) is at the top-left corner of the tile instead of the top-left corner of the earth. Both of these will be undone again when the geometry is reserialized into the child data for the next zoom level.

Plugins WIP
-----------

The rest of tiling
------------------

Writing mbtiles metadata
------------------------
