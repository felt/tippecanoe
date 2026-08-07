"""Produce the side-by-side comparison figures for both datasets."""

import sys
from collections import defaultdict

import compare as C
import render
import run as R
import tileeval


def kept_sets(features, edges, ranks, z, budget, labels):
    tiles = tileeval.assign_to_tiles(features, z)
    out = []
    for label in labels:
        vals = ranks[label]
        kept = set()
        for t, members in tiles.items():
            if sum(c for _i, c in members) <= budget:
                kept.update(i for i, _c in members)
                continue
            spent = 0
            for i, cost in sorted(members, key=lambda m: vals[m[0]], reverse=True):
                if spent + cost > budget:
                    continue
                kept.add(i)
                spent += cost
        out.append((label, kept))
    return out


def main():
    jobs = [
        ("data/nhd_02070004/Shape/NHDFlowline", "gnis_name", 11, 1500,
         "nhd.svg", "NHD HU8 02070004 (Conococheague-Opequon), z11, 1500 vertices/tile"),
        ("data/b5571d37-tl_2025_06001_roads/tl_2025_06001_roads", "FULLNAME", 12, 1500,
         "roads.svg", "TIGER 2025 Alameda County roads, z12, 1500 vertices/tile"),
    ]
    labels = ["drop-smallest", "drop-densest", "trunk/raw"]

    for base, namef, z, budget, out, title in jobs:
        features = R.load(base, namef)
        edges, ranks = C.build(features, local_zooms=[12])
        panels = kept_sets(features, edges, ranks, z, budget, labels)
        render.render(features, panels, out, title=title)
        print("wrote %s" % out)


if __name__ == "__main__":
    main()
