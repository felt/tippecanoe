"""Offline prototype: score LineString features by network "trunkiness".

The pipeline is:

  1. node the network on exact shared coordinates (interior vertices included,
     because merged road layers T-junction into the middle of a feature)
  2. split features into graph edges at those nodes
  3. chain edges into strokes by good continuation (smallest deflection angle,
     preferring a name match)
  4. score each edge by two terms:
       - criticality: how much network is orphaned by removing it, via
         n1*n2 length-weighted betweenness on the bridge tree
       - prominence: the length of the stroke it belongs to
  5. convert to a percentile rank within a local neighborhood, so that a single
     globally-chosen threshold keeps the top X% everywhere

Pure stdlib. Lengths are in meters via an equirectangular approximation, which
is fine at the scale of a county or a HUC8.
"""

import math
import sys
from collections import defaultdict

EARTH_R = 6378137.0

# A stroke may continue through a junction if the two edges deflect by less than
# this. Named continuations are allowed a looser limit, since a matching street
# name is strong evidence of continuation even around a bend.
MAX_DEFLECTION = 60.0
MAX_DEFLECTION_NAMED = 120.0

# Distance along an edge used to measure its departure bearing at a node.
# Using the immediately adjacent vertex is too noisy on densely digitized lines.
BEARING_DIST = 25.0


# ---------------------------------------------------------------- geometry

def local_scale(lat):
    """Meters per degree of longitude and latitude at this latitude."""
    return math.cos(math.radians(lat)) * EARTH_R * math.pi / 180.0, EARTH_R * math.pi / 180.0


def chain_length(coords, mx, my):
    total = 0.0
    for i in range(1, len(coords)):
        dx = (coords[i][0] - coords[i - 1][0]) * mx
        dy = (coords[i][1] - coords[i - 1][1]) * my
        total += math.hypot(dx, dy)
    return total


def departure_bearing(coords, mx, my):
    """Bearing (radians) leaving coords[0], measured BEARING_DIST along the chain."""
    x0, y0 = coords[0]
    acc = 0.0
    for i in range(1, len(coords)):
        dx = (coords[i][0] - x0) * mx
        dy = (coords[i][1] - y0) * my
        acc = math.hypot(dx, dy)
        if acc >= BEARING_DIST:
            return math.atan2(dy, dx)
    # Whole edge is shorter than BEARING_DIST: use its far end.
    dx = (coords[-1][0] - x0) * mx
    dy = (coords[-1][1] - y0) * my
    if dx == 0 and dy == 0:
        return 0.0
    return math.atan2(dy, dx)


def deflection_deg(b1, b2):
    """0 means the two departures are exactly collinear (a straight through-path)."""
    diff = abs(b1 - b2) % (2 * math.pi)
    if diff > math.pi:
        diff = 2 * math.pi - diff
    return abs(180.0 - math.degrees(diff))


# ---------------------------------------------------------------- union-find

class UnionFind:
    def __init__(self, n):
        self.parent = list(range(n))
        self.rank = [0] * n

    def find(self, a):
        p = self.parent
        while p[a] != a:
            p[a] = p[p[a]]
            a = p[a]
        return a

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra == rb:
            return False
        if self.rank[ra] < self.rank[rb]:
            ra, rb = rb, ra
        self.parent[rb] = ra
        if self.rank[ra] == self.rank[rb]:
            self.rank[ra] += 1
        return True


# ---------------------------------------------------------------- noding

def build_graph(features, mx, my):
    """Split features into graph edges at every shared or terminal coordinate.

    `features` is a list of dicts with 'coords' and 'name'. Returns
    (edges, node_ids) where each edge is a dict with u, v, coords, length,
    name and feature index.
    """
    # How many distinct features touch each coordinate, and does any feature
    # terminate there? Either condition makes it a real network node.
    touching = defaultdict(set)
    terminal = set()
    repeated = set()
    for fi, feat in enumerate(features):
        coords = feat["coords"]
        seen_here = set()
        for c in coords:
            if c in seen_here:
                repeated.add(c)
            seen_here.add(c)
            touching[c].add(fi)
        terminal.add(coords[0])
        terminal.add(coords[-1])

    is_node = set()
    for c, fs in touching.items():
        if len(fs) >= 2 or c in terminal or c in repeated:
            is_node.add(c)

    node_ids = {}

    def node_id(c):
        nid = node_ids.get(c)
        if nid is None:
            nid = len(node_ids)
            node_ids[c] = nid
        return nid

    edges = []
    for fi, feat in enumerate(features):
        coords = feat["coords"]
        start = 0
        for i in range(1, len(coords)):
            if coords[i] in is_node or i == len(coords) - 1:
                seg = coords[start:i + 1]
                # Skip degenerate pieces: a 2-point segment that starts and ends
                # at the same coordinate carries no length and no connectivity.
                if len(seg) >= 3 or (len(seg) == 2 and seg[0] != seg[-1]):
                    edges.append({
                        "u": node_id(seg[0]),
                        "v": node_id(seg[-1]),
                        "coords": seg,
                        "length": chain_length(seg, mx, my),
                        "name": feat["name"],
                        "features": [fi],
                    })
                start = i

    return collapse_coincident(edges), node_ids


def collapse_coincident(edges):
    """Merge graph edges that trace exactly the same chain of coordinates.

    Source data routinely carries one centerline several times: TIGER files
    I-880 and Nimitz Fwy as separate features over identical geometry. Left in
    place these are parallel edges, which are never bridges, so they silently
    suppress the whole criticality term. The surviving edge keeps every feature
    that lies on it so scores still reach all of them.
    """
    groups = {}
    order = []
    for e in edges:
        c = tuple(e["coords"])
        key = c if c[0] <= c[-1] else c[::-1]
        if key in groups:
            groups[key]["features"].extend(e["features"])
            if not groups[key]["name"]:
                groups[key]["name"] = e["name"]
        else:
            groups[key] = e
            order.append(key)
    return [groups[k] for k in order]


# ---------------------------------------------------------------- strokes

def build_strokes(edges, num_nodes, mx, my, use_names=False):
    """Chain edges into strokes by good continuation. Returns a stroke id per edge.

    Name matching is off by default: this has to work on untagged data, and we
    cannot know which attributes any given input carries.
    """
    incident = defaultdict(list)  # node -> [(edge index, which end)]
    for ei, e in enumerate(edges):
        incident[e["u"]].append((ei, 0))
        if e["v"] != e["u"]:
            incident[e["v"]].append((ei, 1))

    # Departure bearing of each edge end.
    bearing = {}
    for ei, e in enumerate(edges):
        bearing[(ei, 0)] = departure_bearing(e["coords"], mx, my)
        bearing[(ei, 1)] = departure_bearing(e["coords"][::-1], mx, my)

    uf = UnionFind(len(edges))
    paired = set()  # edge ends already consumed by a stroke

    for node, ends in incident.items():
        if len(ends) < 2:
            continue
        cands = []
        for i in range(len(ends)):
            for j in range(i + 1, len(ends)):
                e1, e2 = ends[i], ends[j]
                if e1[0] == e2[0]:
                    continue  # both ends of one edge at this node: a self loop
                d = deflection_deg(bearing[e1], bearing[e2])
                n1, n2 = edges[e1[0]]["name"], edges[e2[0]]["name"]
                named = use_names and bool(n1) and n1 == n2
                limit = MAX_DEFLECTION_NAMED if named else MAX_DEFLECTION
                if d <= limit:
                    # Name matches sort ahead of unnamed continuations.
                    cands.append(((0 if named else 1, d), e1, e2))
        cands.sort(key=lambda c: c[0])
        for _key, e1, e2 in cands:
            if e1 in paired or e2 in paired:
                continue
            if uf.find(e1[0]) == uf.find(e2[0]):
                continue  # would close a loop onto itself
            uf.union(e1[0], e2[0])
            paired.add(e1)
            paired.add(e2)

    return [uf.find(ei) for ei in range(len(edges))]


# ---------------------------------------------------------------- bridges

def build_strokes_global(edges, num_nodes, mx, my, max_deflection=MAX_DEFLECTION,
                         use_names=False, weight="projected"):
    """Chain edges into strokes by a single global pass over all candidate pairs.

    build_strokes() matches greedily *within* each node, with nodes visited in
    arbitrary order, so a marginal pairing at one junction can consume an
    edge-end that a much stronger continuation elsewhere needed. Ranking every
    candidate pair in the whole network together and making one pass in that
    order lets the strongest continuations claim their ends first.

    Pairs are ordered by combined length and deflection together, so the long
    shallow joins go first and the short sharp ones last. `weight` picks how the
    two are combined:

        projected   (d1 + d2) * cos(deflection)  — combined length projected
                    onto the straight-through direction
        linear      (d1 + d2) * (1 - deflection / max_deflection)

    The pass is single: pair scores are computed once from edge lengths and not
    updated as chains grow.
    """
    incident = defaultdict(list)
    for ei, e in enumerate(edges):
        incident[e["u"]].append((ei, 0))
        if e["v"] != e["u"]:
            incident[e["v"]].append((ei, 1))

    bearing = {}
    for ei, e in enumerate(edges):
        bearing[(ei, 0)] = departure_bearing(e["coords"], mx, my)
        bearing[(ei, 1)] = departure_bearing(e["coords"][::-1], mx, my)

    candidates = []
    for _node, ends in incident.items():
        if len(ends) < 2:
            continue
        for i in range(len(ends)):
            for j in range(i + 1, len(ends)):
                e1, e2 = ends[i], ends[j]
                if e1[0] == e2[0]:
                    continue
                d = deflection_deg(bearing[e1], bearing[e2])
                n1, n2 = edges[e1[0]]["name"], edges[e2[0]]["name"]
                named = use_names and bool(n1) and n1 == n2
                limit = MAX_DEFLECTION_NAMED if named else max_deflection
                if d > limit:
                    continue
                total = edges[e1[0]]["length"] + edges[e2[0]]["length"]
                if weight == "angle":
                    # Shallowest first, length ignored. The name bonus has to be
                    # subtractive here: multiplying a negative score by 2 would
                    # rank name matches last instead of first.
                    w = -(d - (45.0 if named else 0.0))
                    candidates.append((w, e1, e2))
                    continue
                if weight == "linear":
                    w = total * max(0.0, 1.0 - d / max(1e-9, max_deflection))
                else:
                    w = total * math.cos(math.radians(min(d, 90.0)))
                if named:
                    # A shared name is strong evidence of continuation; let it
                    # outrank geometry rather than merely loosening the limit.
                    w *= 2.0
                candidates.append((w, e1, e2))

    candidates.sort(key=lambda c: -c[0])

    uf = UnionFind(len(edges))
    paired = set()
    for _w, e1, e2 in candidates:
        if e1 in paired or e2 in paired:
            continue
        if uf.find(e1[0]) == uf.find(e2[0]):
            continue  # would close a loop onto itself
        uf.union(e1[0], e2[0])
        paired.add(e1)
        paired.add(e2)

    return [uf.find(ei) for ei in range(len(edges))]


def find_bridges(edges, num_nodes):
    """Iterative Tarjan bridge finding. Returns a set of bridge edge indices."""
    adj = defaultdict(list)
    for ei, e in enumerate(edges):
        if e["u"] == e["v"]:
            continue  # self loops are never bridges
        adj[e["u"]].append((e["v"], ei))
        adj[e["v"]].append((e["u"], ei))

    disc = [0] * num_nodes
    low = [0] * num_nodes
    visited = [False] * num_nodes
    bridges = set()
    timer = [1]

    for root in range(num_nodes):
        if visited[root]:
            continue
        # stack frames: (node, incoming edge id, iterator position)
        stack = [(root, -1, 0)]
        visited[root] = True
        disc[root] = low[root] = timer[0]
        timer[0] += 1
        while stack:
            node, in_edge, idx = stack[-1]
            neigh = adj[node]
            if idx < len(neigh):
                stack[-1] = (node, in_edge, idx + 1)
                to, eid = neigh[idx]
                if eid == in_edge:
                    continue  # don't go back along the same edge (parallel edges are fine)
                if visited[to]:
                    if disc[to] < low[node]:
                        low[node] = disc[to]
                else:
                    visited[to] = True
                    disc[to] = low[to] = timer[0]
                    timer[0] += 1
                    stack.append((to, eid, 0))
            else:
                stack.pop()
                if stack:
                    parent = stack[-1][0]
                    if low[node] < low[parent]:
                        low[parent] = low[node]
                    if low[node] > disc[parent]:
                        bridges.add(in_edge)
    return bridges


def criticality(edges, num_nodes, bridges):
    """Length-weighted n1*n2 betweenness on the bridge tree, normalized per component.

    Non-bridge edges score 0: removing one disconnects nothing. Bridges score
    by how much network sits on each side, normalized by the size of their own
    connected component so that a small isolated network still keeps its trunk.
    """
    # Contract every non-bridge edge: the result is the 2-edge-connected components.
    uf = UnionFind(num_nodes)
    for ei, e in enumerate(edges):
        if ei not in bridges:
            uf.union(e["u"], e["v"])

    # Weight of each BCC (length of the non-bridge edges inside it) and total
    # weight of each connected component of the whole graph.
    comp_uf = UnionFind(num_nodes)
    for e in edges:
        comp_uf.union(e["u"], e["v"])

    bcc_weight = defaultdict(float)
    comp_weight = defaultdict(float)
    for ei, e in enumerate(edges):
        comp_weight[comp_uf.find(e["u"])] += e["length"]
        if ei not in bridges:
            bcc_weight[uf.find(e["u"])] += e["length"]

    # Bridge tree: BCC nodes joined by bridges. It is a forest.
    tree = defaultdict(list)
    for ei in bridges:
        e = edges[ei]
        a, b = uf.find(e["u"]), uf.find(e["v"])
        tree[a].append((b, ei))
        tree[b].append((a, ei))

    # Subtree weight below each bridge, by iterative post-order DFS.
    sub = {}
    scores = {}
    seen = set()
    for root in list(tree.keys()):
        if root in seen:
            continue
        seen.add(root)
        stack = [(root, -1, 0)]
        order = []
        while stack:
            node, in_edge, idx = stack[-1]
            if idx < len(tree[node]):
                stack[-1] = (node, in_edge, idx + 1)
                to, eid = tree[node][idx]
                if eid == in_edge or to in seen:
                    continue
                seen.add(to)
                stack.append((to, eid, 0))
            else:
                stack.pop()
                order.append((node, in_edge))

        for node, in_edge in order:
            w = bcc_weight.get(node, 0.0)
            for to, eid in tree[node]:
                if eid != in_edge and eid in sub:
                    w += sub[eid] + edges[eid]["length"]
            if in_edge >= 0:
                sub[in_edge] = w

    for ei in bridges:
        e = edges[ei]
        total = comp_weight[comp_uf.find(e["u"])]
        w1 = sub.get(ei, 0.0)
        w2 = total - w1 - e["length"]
        if total <= 0:
            scores[ei] = 0.0
        else:
            # normalized so a perfectly balanced split scores 1.0
            scores[ei] = (w1 * w2) / ((total / 2.0) ** 2)
    return scores


# ---------------------------------------------------------------- local rank

def tile_of(lon, lat, z):
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(max(min(lat, 85.05), -85.05))
    y = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    return (max(0, min(n - 1, x)), max(0, min(n - 1, y)))


def adaptive_buckets(points, fine_zoom, min_pop=16, coarsest=4):
    """Assign each point to the finest tile whose bucket has at least min_pop members.

    A fixed fine neighbourhood is unusable for ranking: at z15 a third of the
    buckets in a HUC8 hold a single edge, and a percentile within a bucket of
    one is 1.0, so every isolated twig outranks the trunk it drains into.
    Widening the neighbourhood until it holds enough features to be a real
    comparison set removes that.
    """
    counts = []
    keys = []
    for z in range(fine_zoom, coarsest - 1, -1):
        k = [tile_of(lon, lat, z) for lon, lat in points]
        c = defaultdict(int)
        for kk in k:
            c[kk] += 1
        keys.append(k)
        counts.append(c)

    out = []
    for i in range(len(points)):
        chosen = keys[-1][i]
        for level in range(len(keys)):
            if counts[level][keys[level][i]] >= min_pop:
                chosen = keys[level][i]
                break
        out.append(chosen)
    return out


def local_percentile(values, buckets):
    """Percentile of each value within its bucket, in [0, 1]."""
    by_bucket = defaultdict(list)
    for i, b in enumerate(buckets):
        by_bucket[b].append(i)

    out = [0.0] * len(values)
    for b, idxs in by_bucket.items():
        idxs.sort(key=lambda i: values[i])
        n = len(idxs)
        if n == 1:
            out[idxs[0]] = 1.0
            continue
        for rank, i in enumerate(idxs):
            out[i] = rank / (n - 1.0)
    return out
