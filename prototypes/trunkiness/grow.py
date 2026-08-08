"""Connectivity-aware admission: grow the kept network instead of filtering it.

Ranking chains and admitting them best-first, whatever the ranking, treats each
chain independently — so the result is a set of good chains that need not touch
each other. Two fixes to that are possible and they are different:

  * rank better (chainrank.py scores a chain by how much of the network is cut
    off when it is removed), or
  * admit better, which is what this does.

Chains are admitted in descending score as before, but a chain that touches the
already-admitted set is preferred over a higher-scoring one that floats free.
That is a Prim-style growth over the chain adjacency graph rather than a filter,
and it optimizes directly for the thing being measured: one connected network
rather than a scatter of individually-worthy pieces.

`bridgehead` controls how much score a chain must have to be admitted while
disconnected — it seeds new components so that separate river systems and
separate road networks each get started, instead of everything having to hang
off whatever was admitted first.
"""

import heapq
from collections import defaultdict

import trunkiness as T


def chain_adjacency(edges, stroke_of):
    """Which chains touch which, via a shared node."""
    at = defaultdict(set)
    for ei, e in enumerate(edges):
        at[e["u"]].add(stroke_of[ei])
        at[e["v"]].add(stroke_of[ei])
    adj = defaultdict(set)
    for _n, ss in at.items():
        for a in ss:
            for b in ss:
                if a != b:
                    adj[a].add(b)
    return adj


def grow(features, edges, stroke_of, groups, score, z, budget, bridgehead=0.25):
    """Admit whole chains, preferring ones that attach to what is already in."""
    adj = chain_adjacency(edges, stroke_of)

    cost = defaultdict(lambda: defaultdict(int))
    for i, f in enumerate(features):
        for c in f["coords"]:
            cost[i][T.tile_of(c[0], c[1], z)] += 1

    need = {}
    for s, fis in groups.items():
        per = defaultdict(int)
        for fi in fis:
            for t, c in cost[fi].items():
                per[t] += c
        need[s] = per

    remaining = defaultdict(lambda: budget)
    kept = set()
    admitted = set()

    ranked = sorted(groups, key=lambda s: -score.get(s, 0.0))
    top = score.get(ranked[0], 0.0) if ranked else 0.0
    seed_floor = top * bridgehead

    # Chains adjacent to the admitted set, best-first.
    frontier = []
    seen = set()

    def fits(s):
        return all(remaining[t] >= c for t, c in need[s].items())

    def admit(s):
        for t, c in need[s].items():
            remaining[t] -= c
        admitted.add(s)
        kept.update(groups[s])
        for nb in adj.get(s, ()):
            if nb not in admitted and nb in groups and nb not in seen:
                seen.add(nb)
                heapq.heappush(frontier, (-score.get(nb, 0.0), nb))

    for s in ranked:
        # Drain everything reachable from what is already admitted first.
        while frontier:
            _neg, cand = heapq.heappop(frontier)
            seen.discard(cand)
            if cand in admitted:
                continue
            if fits(cand):
                admit(cand)
        if s in admitted:
            continue
        # Seed a new component only if the chain is worth starting one for.
        if score.get(s, 0.0) < seed_floor and admitted:
            continue
        if fits(s):
            admit(s)

    # Second pass: spend whatever budget is left on anything that still fits.
    for s in ranked:
        if s not in admitted and fits(s):
            admit(s)

    per_tile = defaultdict(set)
    for i in kept:
        for t in cost[i]:
            per_tile[t].add(i)
    return per_tile
