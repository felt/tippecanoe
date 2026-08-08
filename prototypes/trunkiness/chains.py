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


def stitch(edge_list):
    """Order a chain's edges into coordinate sequences.

    Usually a chain is a simple path, but not always: at a 4-way node both
    through-pairs can be joined and later merge into one chain, giving that node
    degree 4. Rings and self-loops occur too. Together that is about 1.6% of
    chains on TIGER roads. So walk repeatedly until every edge is consumed and
    return one part per walk, rather than assuming a single path.
    """
    if len(edge_list) == 1:
        return [list(edge_list[0]["coords"])]

    at = defaultdict(list)
    for e in edge_list:
        at[e["u"]].append(e)
        at[e["v"]].append(e)

    used = set()
    parts = []
    starts = [n for n, es in at.items() if len(es) % 2 == 1] or list(at)

    for start in starts + list(at):
        while True:
            if not any(id(e) not in used for e in at[start]):
                break
            coords = []
            node = start
            while True:
                nxt = None
                for e in at[node]:
                    if id(e) not in used:
                        nxt = e
                        break
                if nxt is None:
                    break
                used.add(id(nxt))
                seq = nxt["coords"]
                if seq[-1] == _node_coord(nxt, node):
                    seq = seq[::-1]
                if coords and coords[-1] == seq[0]:
                    coords.extend(seq[1:])
                else:
                    coords.extend(seq)
                node = nxt["v"] if nxt["u"] == node else nxt["u"]
            if len(coords) >= 2:
                parts.append(coords)
        if len(used) == len(edge_list):
            break
    return parts


def _node_coord(edge, node):
    return edge["coords"][0] if edge["u"] == node else edge["coords"][-1]


def build_chains(features):
    lats = [f["coords"][0][1] for f in features]
    mx, my = T.local_scale(sum(lats) / len(lats))
    edges, node_ids = T.build_graph(features, mx, my)
    stroke_of = T.build_strokes(edges, len(node_ids), mx, my)  # untagged: angle only

    grouped = defaultdict(list)
    for ei, e in enumerate(edges):
        grouped[stroke_of[ei]].append(e)

    chains = []
    for s, es in grouped.items():
        parts = stitch(es)
        if not parts:
            continue
        chains.append({
            "parts": parts,
            "coords": max(parts, key=len),
            "length": sum(e["length"] for e in es),
            "edges": len(es),
        })
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
