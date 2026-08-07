"""Measure continuity gaps: strokes that get drawn in pieces.

A gap has two independent causes, and they need different fixes:

  within-tile  the score varies along a single stroke, so a threshold inside
               one tile keeps part of it and drops the rest

  cross-tile   the score is constant along the stroke, but neighbouring tiles
               choose different thresholds, so the stroke is present in one
               tile and absent in the next

Reported separately, because making the score constant per stroke fixes the
first and does nothing at all for the second.
"""

from collections import defaultdict

import trunkiness as T


def measure(features, edges, stroke_of, vals, z, budget, tol=1e-6):
    # Per tile: which features are present, and what they cost.
    present = defaultdict(list)
    for i, f in enumerate(features):
        per = defaultdict(int)
        for c in f["coords"]:
            per[T.tile_of(c[0], c[1], z)] += 1
        for t, cost in per.items():
            present[t].append((i, cost))

    kept_in = {}
    for t, members in present.items():
        if sum(c for _i, c in members) <= budget:
            kept_in[t] = set(i for i, _c in members)
            continue
        # Faithful to choose_minextent: pick a threshold and keep everything at
        # or above it. Features that tie are admitted as a group, never split by
        # the budget running out partway through them.
        ordered = sorted(members, key=lambda m: vals[m[0]], reverse=True)
        keep = set()
        spent = 0
        j = 0
        while j < len(ordered):
            k = j
            v = vals[ordered[j][0]]
            group = []
            while k < len(ordered) and vals[ordered[k][0]] == v:
                group.append(ordered[k])
                k += 1
            cost = sum(c for _i, c in group)
            if spent + cost > budget:
                break
            for i, _c in group:
                keep.add(i)
            spent += cost
            j = k
        kept_in[t] = keep

    # Length of each stroke inside each tile, and how much of it survives.
    # Attribute an edge to the tile of its midpoint: good enough at these sizes.
    cell_total = defaultdict(float)
    cell_kept = defaultdict(float)
    for ei, e in enumerate(edges):
        mid = e["coords"][len(e["coords"]) // 2]
        t = T.tile_of(mid[0], mid[1], z)
        key = (stroke_of[ei], t)
        cell_total[key] += e["length"]
        if any(fi in kept_in.get(t, ()) for fi in e["features"]):
            cell_kept[key] += e["length"]

    within_broken = 0
    within_gap_len = 0.0
    kept_len = 0.0
    for key, L in cell_total.items():
        K = cell_kept.get(key, 0.0)
        kept_len += K
        if K > tol and K < L - tol:
            within_broken += 1
            within_gap_len += L - K

    # Cross-tile: a stroke fully present in some tiles, fully absent in others.
    by_stroke = defaultdict(list)
    for (s, t), L in cell_total.items():
        by_stroke[s].append((t, L, cell_kept.get((s, t), 0.0)))
    cross_broken = 0
    cross_gap_len = 0.0
    for s, cells in by_stroke.items():
        if len(cells) < 2:
            continue
        shown = [c for c in cells if c[2] > tol]
        blank = [c for c in cells if c[2] <= tol]
        if shown and blank:
            cross_broken += 1
            cross_gap_len += sum(c[1] for c in blank)

    return {
        "kept_km": kept_len / 1000.0,
        "within_broken": within_broken,
        "within_gap_km": within_gap_len / 1000.0,
        "cross_broken": cross_broken,
        "cross_gap_km": cross_gap_len / 1000.0,
        "strokes_shown": sum(1 for s, cells in by_stroke.items()
                             if any(c[2] > tol for c in cells)),
    }
