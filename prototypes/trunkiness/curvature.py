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


def build_strokes(edges, num_nodes, mx, my, alpha=6.0, gate=T.MAX_DEFLECTION):
    """Chain edges by deflection angle, penalized by how much each branch curves.

    `alpha` is in units of the dataset's own median turning, so the same value
    applies to a road network and a river network. The useful range is roughly
    4 to 8 on both datasets tested; the response is flat across it and degrades
    above about 12, where the penalty starts overriding the angle that actually
    distinguishes a through-route from a side branch.
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
                penalty = alpha * (ta[a] + ta[b]) / median
                cands.append((d + penalty, a, b))
    cands.sort(key=lambda c: c[0])

    uf = T.UnionFind(len(edges))
    paired = set()
    for _s, a, b in cands:
        if a in paired or b in paired:
            continue
        if uf.find(a[0]) == uf.find(b[0]):
            continue
        uf.union(a[0], b[0])
        paired.add(a)
        paired.add(b)
    return [uf.find(ei) for ei in range(len(edges))]
