"""Deprioritize ramp-like branches when chaining, by their shape.

At an interchange the mainline curves slightly while a ramp leaves almost
straight, so the ramp wins on deflection angle and breaks the trunk. But a ramp
gives itself away by what it does next: it keeps turning. Measuring the total
absolute turning over the next few hundred metres of each candidate branch
separates them cleanly. On TIGER Alameda, median turning over 300 m is 9.3
degrees for a primary road and 58.6 for a ramp.

The measure is not scale-free across datasets: rivers meander, so the median for
NHD flowlines is 134 degrees against 30 for roads. A fixed penalty tuned on
roads therefore damages hydrography. Normalizing each branch's turning by the
median for the dataset makes one weight work for both.

Nothing here reads an attribute; turning is computed from geometry alone. Road
class is used only to check the result.
"""

import math
from collections import defaultdict

import trunkiness as T

WINDOW = 300.0     # metres of lookahead over which turning is accumulated
MAX_STEPS = 6      # bound the walk when edges are short
EXIT_CAP = 90.0    # a branch that dead-ends is no worse than one turning 90 deg


def internal_turning(coords, mx, my):
    """Total absolute turning within a chain of coordinates, in degrees."""
    total = 0.0
    for i in range(1, len(coords) - 1):
        if mx is None:
            sx, sy = T.scale_at(coords[i][1])
        else:
            sx, sy = mx, my
        ax = (coords[i][0] - coords[i - 1][0]) * sx
        ay = (coords[i][1] - coords[i - 1][1]) * sy
        bx = (coords[i + 1][0] - coords[i][0]) * sx
        by = (coords[i + 1][1] - coords[i][1]) * sy
        if (ax or ay) and (bx or by):
            a = math.degrees(math.atan2(by, bx) - math.atan2(ay, ax))
            while a > 180:
                a -= 360
            while a < -180:
                a += 360
            total += abs(a)
    return total


def turn_ahead_table(edges, incident, bearing, mx, my, window=WINDOW):
    """Turning over the next `window` metres from each (edge, end), following
    the straightest continuation."""
    eturn = [internal_turning(e["coords"], mx, my) for e in edges]

    def walk(ei, end, remaining, depth):
        L = edges[ei]["length"]
        t = eturn[ei]
        if L >= remaining or depth >= MAX_STEPS:
            return t * (min(1.0, remaining / L) if L else 1.0)
        far = 1 - end
        node = edges[ei]["u"] if far == 0 else edges[ei]["v"]
        best = None
        for f, fe in incident[node]:
            if f == ei:
                continue
            d = T.deflection_deg(bearing[(ei, far)], bearing[(f, fe)])
            if d <= T.MAX_DEFLECTION and (best is None or d < best[0]):
                best = (d, f, fe)
        if best is None:
            return t
        return t + best[0] + walk(best[1], best[2], remaining - L, depth + 1)

    table = {}
    for ei in range(len(edges)):
        for end in (0, 1):
            table[(ei, end)] = walk(ei, end, window, 0)
    return table


def exit_angle_table(edges, incident, bearing, rounds=1, decay=0.5):
    """How cheaply a branch can keep going straight beyond its far end.

    With rounds=0 this is simply the shallowest continuation available at the
    far end. Each further round requires that continuation to itself have a good
    continuation, discounted by `decay` — so an edge scores well only if it
    meets a shallow mate at its far end *and* that mate does too. One round is
    worth a lot on hydrography (largest connected component 59.6% to 66.6% at
    z11) and is neutral on the road networks; two overshoots.

    A trunk continues straight at both ends; a ramp joins its far end at a
    steep angle because it is merging rather than passing through. This is a
    separate signal from curvature — the turning window stops after WINDOW
    metres and never reaches the far end of a long edge — and being an angle it
    is directly comparable across datasets, unlike accumulated turning.

    On TIGER Alameda the median is 1.1 degrees for a primary road and 7.3 for a
    ramp, with p90 of 4.3 against 45.6.
    """
    table = {}
    for ei, e in enumerate(edges):
        for end in (0, 1):
            far = 1 - end
            node = e["u"] if far == 0 else e["v"]
            best = EXIT_CAP
            for f, fe in incident[node]:
                if f == ei:
                    continue
                d = T.deflection_deg(bearing[(ei, far)], bearing[(f, fe)])
                if d < best:
                    best = d
            table[(ei, end)] = min(best, EXIT_CAP)

    for _ in range(rounds):
        nxt = {}
        for ei, e in enumerate(edges):
            for end in (0, 1):
                far = 1 - end
                node = e["u"] if far == 0 else e["v"]
                best = EXIT_CAP
                for f, fe in incident[node]:
                    if f == ei:
                        continue
                    d = (T.deflection_deg(bearing[(ei, far)], bearing[(f, fe)])
                         + decay * table[(f, fe)])
                    if d < best:
                        best = d
                nxt[(ei, end)] = min(best, EXIT_CAP)
        table = nxt
    return table


def build_strokes(edges, num_nodes, mx, my, alpha=6.0, beta=1.0,
                  gate=T.MAX_DEFLECTION, return_pairs=False):
    """Chain edges by deflection angle, penalized by how much each branch curves.

    Two penalties apply, and they catch different things. `alpha` weights the
    turning over the next WINDOW metres, in units of the dataset's own median so
    one value serves roads and rivers; its useful range is 4 to 8, flat across
    it, degrading above 12. `beta` weights the angle at which the branch meets
    its neighbours at the *far* end, which the turning window cannot see on a
    long edge; it is already in degrees so it needs no normalizing.
    """
    incident = defaultdict(list)
    for ei, e in enumerate(edges):
        incident[e["u"]].append((ei, 0))
        if e["v"] != e["u"]:
            incident[e["v"]].append((ei, 1))

    bearing = {}
    for ei, e in enumerate(edges):
        bearing[(ei, 0)] = T.departure_bearing(e["coords"], mx, my)
        bearing[(ei, 1)] = T.departure_bearing(e["coords"][::-1], mx, my)

    ta = turn_ahead_table(edges, incident, bearing, mx, my)
    vals = sorted(ta.values())
    median = vals[len(vals) // 2] or 1.0
    ex = exit_angle_table(edges, incident, bearing)

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
                penalty = (alpha * (ta[a] + ta[b]) / median
                           + beta * (ex[a] + ex[b]))
                cands.append((d + penalty, a, b))
    cands.sort(key=lambda c: c[0])

    uf = T.UnionFind(len(edges))
    paired = set()
    pair_of = {}
    for _s, a, b in cands:
        if a in paired or b in paired:
            continue
        if uf.find(a[0]) == uf.find(b[0]):
            continue
        uf.union(a[0], b[0])
        paired.add(a)
        paired.add(b)
        pair_of[a] = b
        pair_of[b] = a

    strokes = [uf.find(ei) for ei in range(len(edges))]
    if return_pairs:
        # Callers reconstructing chain geometry must follow these rather than
        # node coincidence: two edges can share a node without being paired.
        return strokes, pair_of
    return strokes
