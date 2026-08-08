"""Douglas-Peucker simplification in tile units, for realistic vertex costing.

Counting raw vertices against a per-tile budget is wrong at low zoom. Tippecanoe
simplifies geometry to the tile's resolution before a tile is measured, so I-5
does not cost a z7 tile its hundred thousand source vertices — it costs a few
dozen. Ignoring that makes any long chain look unaffordable and rejects exactly
the features the whole exercise is trying to keep.
"""

import math

DETAIL = 4096  # tile extent in units, matching tippecanoe's default detail of 12


def world_units(lon, lat, z):
    """Project to tile units at zoom z: 2^z * DETAIL units across the world."""
    n = (1 << z) * DETAIL
    x = (lon + 180.0) / 360.0 * n
    lat_r = math.radians(max(min(lat, 85.05112877), -85.05112877))
    y = (1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n
    return x, y


def douglas_peucker(pts, epsilon):
    """Iterative Douglas-Peucker; returns the indices kept."""
    if len(pts) < 3:
        return list(range(len(pts)))
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        if b <= a + 1:
            continue
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        best = -1.0
        besti = -1
        for i in range(a + 1, b):
            px, py = pts[i]
            if norm == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                d = abs(dy * px - dx * py + bx * ay - by * ax) / norm
            if d > best:
                best = d
                besti = i
        if best > epsilon:
            keep[besti] = True
            stack.append((a, besti))
            stack.append((besti, b))
    return [i for i, k in enumerate(keep) if k]


def simplified_coords(coords, z, epsilon=1.0):
    """The coordinates that survive simplification at zoom z."""
    pts = [world_units(lon, lat, z) for lon, lat in coords]
    idx = douglas_peucker(pts, epsilon)
    return [coords[i] for i in idx]
