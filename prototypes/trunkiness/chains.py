"""Emit the chains produced by shallowest-angle joining, as GeoJSON and as a map.

Chains are stitched back into single LineStrings so they can be inspected as
whole objects rather than as the edges they were built from. Nothing here uses
attributes: joining is by deflection angle alone, with coincident edges
collapsed first.

Usage:
    python3 chains.py <shapefile-base> <name-field> <out-prefix>
"""

import json
import sys
from collections import defaultdict

import render
import run as R
import trunkiness as T

# Chain length classes, in metres, for the map. Short chains are drawn first and
# faintly so the long ones read on top.
CLASSES = [
    (0, 1000, "#dcdcdc", 0.4, "under 1 km"),
    (1000, 5000, "#8fb8d8", 0.7, "1-5 km"),
    (5000, 15000, "#2f6fa8", 1.1, "5-15 km"),
    (15000, float("inf"), "#0b3d66", 1.7, "15 km and over"),
]


def stitch(edge_ids, edges, links):
    """Walk a chain along the pairings that built it.

    Each edge has two ends and the matching consumes each end at most once, so
    in the pairing graph every edge has degree at most two: a chain is always a
    path or a cycle and never branches. Reconstructing adjacency from node
    coincidence instead is wrong, because two edges can share a node without
    having been paired to each other.
    """
    ends_used = set()
    start = None
    for ei in edge_ids:
        for end in (0, 1):
            if (ei, end) not in links:
                start = (ei, end)  # the chain terminates here, so enter here
                break
        if start:
            break
    if start is None:
        start = (edge_ids[0], 0)  # a cycle: begin anywhere

    coords = []
    cur = start
    while cur is not None and cur[0] not in ends_used:
        ei, enter = cur
        ends_used.add(ei)
        seq = edges[ei]["coords"]
        if enter == 1:
            seq = seq[::-1]
        if coords and coords[-1] == seq[0]:
            coords.extend(seq[1:])
        else:
            coords.extend(seq)
        cur = links.get((ei, 1 - enter))
    return coords, len(ends_used) == len(edge_ids)


def _node_coord(edge, node):
    return edge["coords"][0] if edge["u"] == node else edge["coords"][-1]


def build_chains(features):
    lats = [f["coords"][0][1] for f in features]
    mx, my = T.local_scale(sum(lats) / len(lats))
    edges, node_ids = T.build_graph(features, mx, my)
    # untagged: angle only. Keep the pairings, they are what defines the chain.
    stroke_of, links = T.build_strokes(edges, len(node_ids), mx, my,
                                       return_pairs=True)

    grouped = defaultdict(list)
    for ei in range(len(edges)):
        grouped[stroke_of[ei]].append(ei)

    chains = []
    incomplete = 0
    for s, eids in grouped.items():
        coords, complete = stitch(eids, edges, links)
        if len(coords) < 2:
            continue
        if not complete:
            incomplete += 1
        chains.append({
            "parts": [coords],
            "coords": coords,
            "length": sum(edges[ei]["length"] for ei in eids),
            "edges": len(eids),
        })
    if incomplete:
        print("  warning: %d chains not fully covered by their pairing walk"
              % incomplete)
    chains.sort(key=lambda c: c["length"])
    return chains, edges


def write_geojson(chains, path):
    with open(path, "w") as f:
        f.write('{"type":"FeatureCollection","features":[\n')
        for i, c in enumerate(chains):
            props = {"chain_id": i,
                     "length_m": round(c["length"], 1),
                     "edges": c["edges"]}
            parts = [[[round(x, 6), round(y, 6)] for x, y in p] for p in c["parts"]]
            geom = ({"type": "LineString", "coordinates": parts[0]} if len(parts) == 1
                    else {"type": "MultiLineString", "coordinates": parts})
            f.write(json.dumps({"type": "Feature", "properties": props, "geometry": geom}))
            f.write(",\n" if i + 1 < len(chains) else "\n")
        f.write("]}\n")


def write_map(chains, path, title):
    features = [{"coords": p} for c in chains for p in c["parts"]]
    layers = []
    for lo, hi, color, width, label in CLASSES:
        inclass = [c for c in chains if lo <= c["length"] < hi]
        sel = [p for c in inclass for p in c["parts"]]
        total = sum(c["length"] for c in inclass)
        layers.append((color, width, "%s  (%d chains, %.0f km)"
                       % (label, len(inclass), total / 1000), sel))
    render.render_layers(features, layers, path, title=title)


def main():
    base, namef, prefix = sys.argv[1], sys.argv[2], sys.argv[3]
    features = R.load(base, namef)
    chains, edges = build_chains(features)
    total = sum(c["length"] for c in chains)
    print("%s: %d chains over %.0f km; longest %.1f km"
          % (prefix, len(chains), total / 1000, chains[-1]["length"] / 1000))
    write_geojson(chains, prefix + "_chains.json")
    write_map(chains, prefix + "_chains.svg",
              "%s — chains from shallowest-angle joining (no attributes used)" % prefix)
    print("  wrote %s_chains.json and %s_chains.svg" % (prefix, prefix))


if __name__ == "__main__":
    main()
