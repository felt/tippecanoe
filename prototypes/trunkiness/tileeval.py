"""Evaluate drop strategies the way tippecanoe actually applies them: per tile,
against a per-tile size budget, at a given zoom.

A feature is present in every tile its geometry touches, and costs that tile the
number of its vertices that land there. Each tile keeps the highest-ranked
features until it runs out of budget. Tiles under budget drop nothing, which is
what makes this different from a single global threshold: rural tiles are
untouched and only congested tiles have to choose.
"""

from collections import defaultdict

import trunkiness as T


def assign_to_tiles(features, z):
    """tile -> [(feature index, vertex cost in this tile)]"""
    tiles = defaultdict(list)
    for i, f in enumerate(features):
        per = defaultdict(int)
        for c in f["coords"]:
            per[T.tile_of(c[0], c[1], z)] += 1
        for t, cost in per.items():
            tiles[t].append((i, cost))
    return tiles


def run(features, edges, ranks, z, budget, cover_zoom):
    """Apply a per-tile budget at zoom z; return whole-dataset quality measures.

    `ranks` maps a strategy label to a per-feature score (higher survives).
    """
    tiles = assign_to_tiles(features, z)

    all_fine = set()
    fine_of_feat = []
    for f in features:
        ts = set(T.tile_of(c[0], c[1], cover_zoom) for c in f["coords"])
        fine_of_feat.append(ts)
        all_fine |= ts

    total_len = sum(f["length"] for f in features)
    max_node = max(max(e["u"], e["v"]) for e in edges) + 1

    out = {}
    for label, vals in ranks.items():
        kept = set()
        congested = 0
        for t, members in tiles.items():
            total_cost = sum(c for _i, c in members)
            if total_cost <= budget:
                kept.update(i for i, _c in members)
                continue
            congested += 1
            spent = 0
            for i, cost in sorted(members, key=lambda m: vals[m[0]], reverse=True):
                if spent + cost > budget:
                    continue
                kept.add(i)
                spent += cost

        kept_len = sum(features[i]["length"] for i in kept)

        uf = T.UnionFind(max_node)
        live = [e for e in edges if e["feature"] in kept]
        for e in live:
            uf.union(e["u"], e["v"])
        comp_len = defaultdict(float)
        for e in live:
            comp_len[uf.find(e["u"])] += e["length"]
        largest = max(comp_len.values()) / kept_len if comp_len and kept_len else 0.0

        covered = set()
        for i in kept:
            covered |= fine_of_feat[i]

        out[label] = {
            "kept_features": len(kept),
            "len_frac": kept_len / total_len,
            "ncomp": len(comp_len),
            "largest": largest,
            "coverage": len(covered) / len(all_fine) if all_fine else 0.0,
            "congested_tiles": congested,
            "total_tiles": len(tiles),
        }
    return out
