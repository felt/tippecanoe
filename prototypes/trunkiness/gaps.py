"""Measure and eliminate continuity gaps — strokes that get drawn in pieces.

A gap has two independent causes and they need different fixes:

  within-tile  the score varies along a stroke, so one tile's threshold falls in
               the middle of it. Fixed by collapsing the score to a single value
               per stroke.

  cross-tile   the score is constant along the stroke but neighbouring tiles
               choose different thresholds, so it is present in one tile and
               absent in the next. Only a threshold shared across the zoom fixes
               this, and a shared threshold is set by the densest tile — which
               thins everywhere else to what downtown can afford, unless the
               score is first normalized locally.

So the recipe that removes both is: score per stroke, normalize within a fine
neighbourhood, then apply one threshold per zoom.

Usage:
    python3 gaps.py <shapefile-base> <name-field> <zoom> [vertices-per-tile]
"""

import math
import sys
from collections import defaultdict

import run as R
import trunkiness as T


def edge_scores(features, edges, stroke_of, num_nodes, local_zoom, per_stroke=True):
    """Per-edge score, before rolling up to features."""
    stroke_len = defaultdict(float)
    for ei, e in enumerate(edges):
        stroke_len[stroke_of[ei]] += e["length"]

    bridges = T.find_bridges(edges, num_nodes)
    crit = T.criticality(edges, num_nodes, bridges)

    stroke_crit = defaultdict(float)
    for ei in range(len(edges)):
        s = stroke_of[ei]
        stroke_crit[s] = max(stroke_crit[s], crit.get(ei, 0.0))

    longest = max(stroke_len.values()) or 1.0
    if per_stroke:
        raw = [max(stroke_crit[stroke_of[ei]], stroke_len[stroke_of[ei]] / longest)
               for ei in range(len(edges))]
    else:
        raw = [max(crit.get(ei, 0.0), stroke_len[stroke_of[ei]] / longest)
               for ei in range(len(edges))]

    if local_zoom is None:
        edge_val = raw
    else:
        pts = [(e["coords"][len(e["coords"]) // 2][0],
                e["coords"][len(e["coords"]) // 2][1]) for e in edges]
        buckets = T.adaptive_buckets(pts, local_zoom, min_pop=16)
        pct = T.local_percentile(raw, buckets)
        # Keep the global magnitude in play. A pure local rank says only "big
        # for around here", which promotes a headwater twig in an empty bucket
        # over a trunk river competing in a crowded one. A geometric mean keeps
        # the hierarchy while still letting the best thing in a sparse area
        # place ahead of its neighbours; a max would just restore 1.0 to the
        # top of every bucket.
        edge_val = [math.sqrt(raw[i] * pct[i]) for i in range(len(raw))]

    if per_stroke:
        # One value per stroke: the length-weighted mean of its edges. Taking a
        # mean rather than a max keeps a long stroke from being promoted
        # everywhere just because it is locally dominant in one place.
        num = defaultdict(float)
        den = defaultdict(float)
        for ei, e in enumerate(edges):
            num[stroke_of[ei]] += edge_val[ei] * e["length"]
            den[stroke_of[ei]] += e["length"]
        edge_val = [num[stroke_of[ei]] / den[stroke_of[ei]] for ei in range(len(edges))]

    return edge_val


def score_strokes(features, edges, stroke_of, num_nodes, local_zoom, per_stroke=True):
    """Per-feature score. With per_stroke, every feature on a stroke gets one value."""
    edge_val = edge_scores(features, edges, stroke_of, num_nodes, local_zoom, per_stroke)
    vals = [0.0] * len(features)
    for ei, e in enumerate(edges):
        for fi in e["features"]:
            vals[fi] = max(vals[fi], edge_val[ei])
    return vals


def tile_members(features, z):
    members = defaultdict(list)
    for i, f in enumerate(features):
        per = defaultdict(int)
        for c in f["coords"]:
            per[T.tile_of(c[0], c[1], z)] += 1
        for t, cost in per.items():
            members[t].append((i, cost))
    return members


def lowest_admitted(members, vals, budget):
    """Threshold this tile can afford, admitting tied features as a group.

    Mirrors choose_minextent: a threshold is chosen and everything at or above
    it is kept, so the budget never splits a set of equally ranked features.
    """
    ordered = sorted(members, key=lambda m: vals[m[0]], reverse=True)
    spent = 0
    j = 0
    last = None
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
        spent += cost
        last = v
        j = k
    return last


def select(features, vals, z, budget, shared_threshold):
    members = tile_members(features, z)
    if shared_threshold:
        need = 0.0
        for _t, mem in members.items():
            if sum(c for _i, c in mem) <= budget:
                continue
            low = lowest_admitted(mem, vals, budget)
            if low is not None:
                need = max(need, low)
        return {t: set(i for i, _c in mem if vals[i] >= need)
                for t, mem in members.items()}

    kept = {}
    for t, mem in members.items():
        if sum(c for _i, c in mem) <= budget:
            kept[t] = set(i for i, _c in mem)
            continue
        low = lowest_admitted(mem, vals, budget)
        kept[t] = set(i for i, _c in mem if low is not None and vals[i] >= low)
    return kept


def stroke_groups(features, edges, stroke_of, edge_val):
    """Assign each feature to its best-scoring stroke, and score each stroke."""
    best = {}
    val = [0.0] * len(features)
    for ei, e in enumerate(edges):
        for fi in e["features"]:
            if edge_val[ei] > val[fi]:
                val[fi] = edge_val[ei]
                best[fi] = stroke_of[ei]
    groups = defaultdict(list)
    for fi, s in best.items():
        groups[s].append(fi)
    score = {}
    for ei, e in enumerate(edges):
        s = stroke_of[ei]
        score[s] = max(score.get(s, 0.0), edge_val[ei])
    return groups, score


def select_greedy_strokes(features, groups, stroke_score, z, budget):
    """Admit whole strokes, best first, if they fit in every tile they cross.

    A threshold shared across the zoom also gives all-or-nothing strokes, but it
    is set by the densest tile and so strips the whole zoom down to what
    downtown can afford. Greedy admission does not have that problem: rejecting
    a stroke frees nothing in tiles it never enters, so sparse tiles keep
    filling with local strokes long after the trunks have been placed.
    """
    cost = defaultdict(lambda: defaultdict(int))
    for i, f in enumerate(features):
        for c in f["coords"]:
            cost[i][T.tile_of(c[0], c[1], z)] += 1

    remaining = defaultdict(lambda: budget)
    kept = set()
    for s in sorted(groups, key=lambda s: -stroke_score[s]):
        need = defaultdict(int)
        for fi in groups[s]:
            for t, c in cost[fi].items():
                need[t] += c
        if all(remaining[t] >= c for t, c in need.items()):
            for t, c in need.items():
                remaining[t] -= c
            kept.update(groups[s])

    per_tile = defaultdict(set)
    for i in kept:
        for t in cost[i]:
            per_tile[t].add(i)
    return per_tile


def measure(features, edges, stroke_of, kept_in, z, tol=1e-6):
    cell_total = defaultdict(float)
    cell_kept = defaultdict(float)
    for ei, e in enumerate(edges):
        m = e["coords"][len(e["coords"]) // 2]
        t = T.tile_of(m[0], m[1], z)
        key = (stroke_of[ei], t)
        cell_total[key] += e["length"]
        if any(fi in kept_in.get(t, ()) for fi in e["features"]):
            cell_kept[key] += e["length"]

    within = sum(1 for k, L in cell_total.items()
                 if tol < cell_kept.get(k, 0.0) < L - tol)

    by_stroke = defaultdict(list)
    for (s, t), L in cell_total.items():
        by_stroke[s].append((L, cell_kept.get((s, t), 0.0)))
    cross = 0
    cross_gap = 0.0
    for _s, cells in by_stroke.items():
        if len(cells) < 2:
            continue
        if any(c[1] > tol for c in cells) and any(c[1] <= tol for c in cells):
            cross += 1
            cross_gap += sum(c[0] for c in cells if c[1] <= tol)

    all_fine = set()
    covered = set()
    for i, f in enumerate(features):
        for c in f["coords"]:
            fine = T.tile_of(c[0], c[1], z + 3)
            all_fine.add(fine)
            if i in kept_in.get(T.tile_of(c[0], c[1], z), ()):
                covered.add(fine)

    # Largest connected component of what survives. Coverage alone is gameable:
    # scattering isolated features maximizes distinct tiles touched while
    # producing a map of disconnected fragments. This is the check that catches it.
    max_node = max(max(e["u"], e["v"]) for e in edges) + 1
    uf = T.UnionFind(max_node)
    live = []
    for ei, e in enumerate(edges):
        m = e["coords"][len(e["coords"]) // 2]
        if any(fi in kept_in.get(T.tile_of(m[0], m[1], z), ()) for fi in e["features"]):
            live.append(e)
    for e in live:
        uf.union(e["u"], e["v"])
    comp = defaultdict(float)
    for e in live:
        comp[uf.find(e["u"])] += e["length"]
    kept_len = sum(cell_kept.values())
    largest = 100.0 * max(comp.values()) / kept_len if comp and kept_len else 0.0

    return (kept_len / 1000.0, within, cross, cross_gap / 1000.0,
            100.0 * len(covered) / len(all_fine) if all_fine else 0.0, largest)


def main():
    base = sys.argv[1]
    namef = sys.argv[2]
    z = int(sys.argv[3])
    budget = int(sys.argv[4]) if len(sys.argv) > 4 else 1500

    features = R.load(base, namef)
    lats = [f["coords"][0][1] for f in features]
    mx, my = T.local_scale(sum(lats) / len(lats))
    edges, node_ids = T.build_graph(features, mx, my)
    stroke_of = T.build_strokes(edges, len(node_ids), mx, my)

    variants = [
        ("per-edge score, per-tile", False, None, False),
        ("per-stroke, per-tile", True, None, False),
        ("per-stroke, shared thresh", True, None, True),
        ("per-stroke local z%d, per-tile" % (z + 4), True, z + 4, False),
        ("per-stroke local z%d, shared" % (z + 4), True, z + 4, True),
    ]

    print("== %s z%d, %d vertices/tile" % (base, z, budget))
    print("  %-32s %8s %8s %8s %10s %7s %8s"
          % ("variant", "kept km", "within#", "cross#", "cross gap", "cov", "largest"))
    for label, per_stroke, lz, shared in variants:
        vals = score_strokes(features, edges, stroke_of, len(node_ids), lz, per_stroke)
        kept_in = select(features, vals, z, budget, shared)
        k, w, c, cg, cov, lg = measure(features, edges, stroke_of, kept_in, z)
        print("  %-32s %8.0f %8d %8d %9.0fkm %6.1f%% %7.1f%%"
              % (label, k, w, c, cg, cov, lg))

    ev = edge_scores(features, edges, stroke_of, len(node_ids), z + 4, True)
    groups, sscore = stroke_groups(features, edges, stroke_of, ev)
    kept_in = select_greedy_strokes(features, groups, sscore, z, budget)
    k, w, c, cg, cov, lg = measure(features, edges, stroke_of, kept_in, z)
    print("  %-32s %8.0f %8d %8d %9.0fkm %6.1f%% %7.1f%%"
          % ("greedy whole strokes", k, w, c, cg, cov, lg))


if __name__ == "__main__":
    main()
