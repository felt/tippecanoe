"""Stroke building with depth-limited lookahead.

The plain rule pairs the two edges at a junction with the smallest deflection.
This instead asks, of every candidate pair at an acceptable angle, which one
leads to the longest chain — by extending outward from both sides for `depth`
further steps, taking the best continuation at each step, and ranking on the
total reachable length.

depth=0 reduces to ranking by the two edges' own lengths, which is the rule
already known to hurt road networks, since a trunk road is noded at every ramp
and so has shorter-than-average edges. Increasing depth is what would let a
short edge that opens into a long run outrank a long edge that dead-ends.
"""

from collections import defaultdict

import trunkiness as T


def build(edges, num_nodes, mx, my, depth=2, gate=T.MAX_DEFLECTION):
    incident = defaultdict(list)
    for ei, e in enumerate(edges):
        incident[e["u"]].append((ei, 0))
        if e["v"] != e["u"]:
            incident[e["v"]].append((ei, 1))

    bearing = {}
    for ei, e in enumerate(edges):
        bearing[(ei, 0)] = T.departure_bearing(e["coords"], mx, my)
        bearing[(ei, 1)] = T.departure_bearing(e["coords"][::-1], mx, my)

    def node_of(ei, end):
        return edges[ei]["u"] if end == 0 else edges[ei]["v"]

    memo = {}

    def reach_out(ei, end_at_node, d):
        """Length of ei plus the best continuation onward, d further steps."""
        key = (ei, end_at_node, d)
        got = memo.get(key)
        if got is not None:
            return got
        total = edges[ei]["length"]
        if d > 0:
            far = 1 - end_at_node
            best = 0.0
            for f, fend in incident[node_of(ei, far)]:
                if f == ei:
                    continue
                if T.deflection_deg(bearing[(ei, far)], bearing[(f, fend)]) <= gate:
                    v = reach_out(f, fend, d - 1)
                    if v > best:
                        best = v
            total += best
        memo[key] = total
        return total

    cands = []
    for _node, ends in incident.items():
        if len(ends) < 2:
            continue
        for i in range(len(ends)):
            for j in range(i + 1, len(ends)):
                a, b = ends[i], ends[j]
                if a[0] == b[0]:
                    continue
                d = T.deflection_deg(bearing[a], bearing[b])
                if d > gate:
                    continue
                score = reach_out(a[0], a[1], depth) + reach_out(b[0], b[1], depth)
                cands.append((-score, d, a, b))

    # Longest reachable chain first; deflection only breaks ties.
    cands.sort(key=lambda c: (c[0], c[1]))

    uf = T.UnionFind(len(edges))
    paired = set()
    for _neg, _d, a, b in cands:
        if a in paired or b in paired:
            continue
        if uf.find(a[0]) == uf.find(b[0]):
            continue
        uf.union(a[0], b[0])
        paired.add(a)
        paired.add(b)
    return [uf.find(ei) for ei in range(len(edges))]
