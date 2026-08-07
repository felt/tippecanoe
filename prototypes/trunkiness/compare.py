"""Compare drop strategies under a realistic per-tile budget, and sweep the
neighborhood size used to normalize the trunkiness score.
"""

import sys
from collections import defaultdict

import run as R
import tileeval
import trunkiness as T


def build(features, local_zooms):
    lats = [f["coords"][0][1] for f in features]
    mx, my = T.local_scale(sum(lats) / len(lats))

    edges, node_ids = T.build_graph(features, mx, my)
    stroke_of = T.build_strokes(edges, len(node_ids), mx, my)
    stroke_len = defaultdict(float)
    for ei, e in enumerate(edges):
        stroke_len[stroke_of[ei]] += e["length"]
    bridges = T.find_bridges(edges, len(node_ids))
    crit = T.criticality(edges, len(node_ids), bridges)

    n = len(features)
    feat_crit = [0.0] * n
    feat_stroke = [0.0] * n
    feat_len = [0.0] * n
    for ei, e in enumerate(edges):
        for fi in e["features"]:
            feat_crit[fi] = max(feat_crit[fi], crit.get(ei, 0.0))
            feat_stroke[fi] = max(feat_stroke[fi], stroke_len[stroke_of[ei]])
            feat_len[fi] += e["length"]
    for i, f in enumerate(features):
        f["length"] = feat_len[i]

    print("  %d features -> %d edges, %d nodes, %d strokes, %d bridges (%.0f%%)"
          % (n, len(edges), len(node_ids), len(stroke_len), len(bridges),
             100.0 * len(bridges) / max(1, len(edges))))

    ranks = {
        "drop-smallest": feat_len,
        "drop-densest": R.densest_rank(features),
    }

    # Raw, globally comparable score: no local normalization at all.
    m = max(feat_stroke) or 1.0
    ranks["trunk/raw"] = [max(feat_crit[i], feat_stroke[i] / m) for i in range(n)]

    # Locally normalized at a few neighborhood sizes.
    for lz in local_zooms:
        buckets = []
        for f in features:
            lon = sum(c[0] for c in f["coords"]) / len(f["coords"])
            lat = sum(c[1] for c in f["coords"]) / len(f["coords"])
            buckets.append(T.tile_of(lon, lat, lz))
        pc = T.local_percentile(feat_crit, buckets)
        ps = T.local_percentile(feat_stroke, buckets)
        ranks["trunk/local z%d" % lz] = [max(pc[i], ps[i]) for i in range(n)]

    return edges, ranks


def main():
    base = sys.argv[1]
    name_field = sys.argv[2] if len(sys.argv) > 2 else "FULLNAME"
    zooms = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "10,12").split(",")]
    budget = int(sys.argv[4]) if len(sys.argv) > 4 else 1500

    print("== %s" % base)
    features = R.load(base, name_field)
    edges, ranks = build(features, local_zooms=[10, 12, 14])

    for z in zooms:
        print()
        print("  zoom %d, budget %d vertices/tile, coverage measured at z%d"
              % (z, budget, z + 3))
        res = tileeval.run(features, edges, ranks, z, budget, z + 3)
        any_key = next(iter(res))
        print("  (%d tiles, %d over budget)"
              % (res[any_key]["total_tiles"], res[any_key]["congested_tiles"]))
        print("    %-16s %9s %8s %9s %9s" % ("strategy", "len kept", "#comp", "largest", "tile cov"))
        for label in ranks:
            r = res[label]
            print("    %-16s %8.1f%% %8d %8.1f%% %8.1f%%"
                  % (label, r["len_frac"] * 100, r["ncomp"],
                     r["largest"] * 100, r["coverage"] * 100))


if __name__ == "__main__":
    main()
