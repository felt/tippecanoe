"""Export chains and admission outcomes as GeoJSON and maps, current settings.

Chains are built with the curvature and far-end-angle penalties, admission is
growth-based against per-zoom simplified vertex costs, and geometry is stitched
along the pairings so each chain emits as one LineString.

Usage:
    python3 export_chains.py <shapefile-base> <name-field> <zoom> <prefix> [perlat]
"""

import json
import sys
from collections import defaultdict

import chains as C
import curvature as CV
import gaps
import grow
import render
import run as R
import simplify as S
import trunkiness as T

BUDGET = 1500
# 7 decimals is about 1 cm. Six is about 9 cm per vertex, which is a large
# relative error on the sub-10 m service stubs that dense OSM extracts are full
# of, and shows up as spurious length mismatches when validating the output.
PRECISION = 7


def main():
    base, namef, z, prefix = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    perlat = len(sys.argv) > 5 and sys.argv[5] == "perlat"

    feats = R.load(base, namef)
    mxmy = (None, None) if perlat else T.local_scale(
        sum(f["coords"][0][1] for f in feats) / len(feats))
    edges, nid = T.build_graph(feats, *mxmy)
    so, links = CV.build_strokes(edges, len(nid), *mxmy, return_pairs=True)

    clen = defaultdict(float)
    for ei, e in enumerate(edges):
        clen[so[ei]] += e["length"]
    groups = defaultdict(list)
    best = [0.0] * len(feats)
    where = {}
    for ei, e in enumerate(edges):
        for fi in e["features"]:
            if clen[so[ei]] > best[fi]:
                best[fi] = clen[so[ei]]
                where[fi] = so[ei]
    for fi, s in where.items():
        groups[s].append(fi)
    flen = [0.0] * len(feats)
    for e in edges:
        for fi in e["features"]:
            flen[fi] += e["length"]
    for i, f in enumerate(feats):
        f["length"] = flen[i]

    fz = [{"coords": S.simplified_coords(f["coords"], z), "length": f["length"]}
          for f in feats]
    kept_in = grow.grow(fz, edges, so, groups, clen, z, BUDGET)
    r = gaps.measure(feats, edges, so, kept_in, z)
    print("  z%d: kept %.0f km, within %d, cross %d, cov %.1f%%, largest %.1f%%"
          % (z, r[0], r[1], r[2], r[4], r[5]))
    kf = set().union(*kept_in.values()) if kept_in else set()

    by = defaultdict(list)
    for ei in range(len(edges)):
        by[so[ei]].append(ei)

    rows = []
    for cid, s in enumerate(sorted(clen, key=lambda s: clen[s])):
        coords, _ok = C.stitch(by[s], edges, links)
        if len(coords) < 2:
            continue
        L = clen[s]
        turn = CV.internal_turning(coords, *mxmy)
        mine = groups.get(s, ())
        rows.append(({"chain_id": cid,
                      "length_m": round(L, 1),
                      "edges": len(by[s]),
                      "turn_deg_per_km": round(turn / (L / 1000.0), 1) if L else 0,
                      "keep": bool(mine) and any(fi in kf for fi in mine)},
                     coords))

    path = prefix + "_chains.json"
    with open(path, "w") as f:
        f.write('{"type":"FeatureCollection","features":[\n')
        for i, (p, c) in enumerate(rows):
            f.write(json.dumps({"type": "Feature", "properties": p,
                                "geometry": {"type": "LineString",
                                             "coordinates": [[round(x, 7), round(y, 7)]
                                                             for x, y in c]}}))
            f.write(",\n" if i + 1 < len(rows) else "\n")
        f.write("]}\n")
    print("  wrote %s (%d chains, %d kept)"
          % (path, len(rows), sum(1 for p, _c in rows if p["keep"])))

    render.render_layers(
        [{"coords": c} for _p, c in rows],
        [("#e3e3e3", 0.5, "dropped", [c for p, c in rows if not p["keep"]]),
         ("#12507e", 1.2, "kept", [c for p, c in rows if p["keep"]])],
        prefix + "_keep.svg",
        title="%s — chains kept at z%d, %d vertices/tile" % (prefix, z, BUDGET))
    print("  wrote %s_keep.svg" % prefix)


if __name__ == "__main__":
    main()
