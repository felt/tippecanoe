"""Export chains with their scores and whether each survives, as GeoJSON.

One feature per chain, carrying both rankings and the admission outcome under
each strategy, so the alternatives can be compared by styling or filtering in
any viewer rather than by reading tables.

Properties per chain:
    chain_id     stable id, ordered by length
    length_m     the chain's own length
    cut_m        network length cut off from the main body if it were removed
    edges        number of graph edges in the chain
    keep_len     admitted when ranking by length, filtering best-first
    keep_grow    admitted when ranking by length, growing from what is in
    keep_cut     admitted when ranking by network cut, growing

Usage:
    python3 export.py <shapefile-base> <name-field> <zoom> <out-prefix> [budget]
"""

import json
import sys
from collections import defaultdict

import chainrank
import chains as C
import gaps
import grow as G
import render
import run as R
import trunkiness as T


def main():
    base, namef, z, prefix = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    budget = int(sys.argv[5]) if len(sys.argv) > 5 else 1500

    features = R.load(base, namef)
    lats = [f["coords"][0][1] for f in features]
    mx, my = T.local_scale(sum(lats) / len(lats))
    edges, node_ids = T.build_graph(features, mx, my)
    stroke_of, links = T.build_strokes(edges, len(node_ids), mx, my, return_pairs=True)

    cut, chain_len, bridges = chainrank.cut_scores(edges, len(node_ids), stroke_of)

    # Each feature belongs to the longest chain it touches.
    groups = defaultdict(list)
    best = [0.0] * len(features)
    where = {}
    for ei, e in enumerate(edges):
        for fi in e["features"]:
            if chain_len[stroke_of[ei]] > best[fi]:
                best[fi] = chain_len[stroke_of[ei]]
                where[fi] = stroke_of[ei]
    for fi, s in where.items():
        groups[s].append(fi)

    runs = {
        "keep_len": gaps.select_greedy_strokes(features, groups, chain_len, z, budget),
        "keep_grow": G.grow(features, edges, stroke_of, groups, chain_len, z, budget),
        "keep_cut": G.grow(features, edges, stroke_of, groups, cut, z, budget),
    }
    kept_features = {k: set().union(*v.values()) if v else set()
                     for k, v in runs.items()}

    by_chain = defaultdict(list)
    for ei in range(len(edges)):
        by_chain[stroke_of[ei]].append(ei)

    ordered = sorted(chain_len, key=lambda s: chain_len[s])
    out = []
    for cid, s in enumerate(ordered):
        coords, complete = C.stitch(by_chain[s], edges, links)
        if len(coords) < 2:
            continue
        props = {
            "chain_id": cid,
            "length_m": round(chain_len[s], 1),
            "cut_m": round(cut.get(s, 0.0), 1),
            "edges": len(by_chain[s]),
        }
        for k in runs:
            mine = groups.get(s, ())
            props[k] = bool(mine) and any(fi in kept_features[k] for fi in mine)
        out.append((props, coords))

    path = prefix + "_chains.json"
    with open(path, "w") as f:
        f.write('{"type":"FeatureCollection","features":[\n')
        for i, (props, coords) in enumerate(out):
            geom = {"type": "LineString",
                    "coordinates": [[round(x, 6), round(y, 6)] for x, y in coords]}
            f.write(json.dumps({"type": "Feature", "properties": props,
                                "geometry": geom}))
            f.write(",\n" if i + 1 < len(out) else "\n")
        f.write("]}\n")

    for k in runs:
        kl = sum(p["length_m"] for p, _c in out if p[k])
        print("  %-10s %5d of %5d chains, %.0f km"
              % (k, sum(1 for p, _c in out if p[k]), len(out), kl / 1000))
    print("  wrote %s" % path)

    # A map of the best strategy for each dataset, alongside what it dropped.
    for k in ("keep_grow", "keep_cut"):
        layers = [
            ("#e3e3e3", 0.5, "dropped",
             [c for p, c in out if not p[k]]),
            ("#12507e", 1.2, "kept (%s)" % k,
             [c for p, c in out if p[k]]),
        ]
        render.render_layers([{"coords": c} for _p, c in out], layers,
                             "%s_%s.svg" % (prefix, k),
                             title="%s — %s, z%d, %d vertices/tile"
                                   % (prefix, k, z, budget))
    print("  wrote %s_keep_grow.svg and %s_keep_cut.svg" % (prefix, prefix))


if __name__ == "__main__":
    main()
