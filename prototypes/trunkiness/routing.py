"""Radius-limited, density-weighted edge betweenness with a junction penalty.

Three ideas, each answering a specific failure of the earlier scores:

* **Density-weighted origins and destinations.** You cannot ask the data which
  places matter without external knowledge, but you do not have to: sample
  origins and destinations in proportion to local network density. Cities are
  where the density is, which is why they are cities in the data. This is a
  gravity model with the network as its own demand model.

* **A radius limit.** Restricting trips to within distance R and sweeping R
  produces a hierarchy rather than a single ranking: at small R local streets
  carry the traffic, at large R only long-haul corridors do. The coarsest R at
  which a stroke still ranks is a zoom ranking, and it is monotone.

* **A junction penalty.** Pure distance does not prefer a freeway, which is not
  shorter in metres than the surface street beside it. Charging a fixed cost per
  junction traversed approximates uninterrupted travel and needs no speed or
  road-class attribute, only the geometry.

Betweenness is accumulated over shortest-path trees from sampled origins, which
is a single-predecessor approximation to Brandes. With a junction penalty the
float costs rarely tie, so the approximation is close, and only the ranking
matters here.
"""

import heapq
import math
import random
from collections import defaultdict


def node_density(edges, coords_of_node, cell_m=1000.0, mx=1.0, my=1.0):
    """Local network density at each node: how many nodes share its ~1km cell.

    A crude but effective stand-in for settlement: dense street networks are
    towns, sparse ones are countryside.
    """
    cell = defaultdict(int)
    key_of = {}
    for n, (lon, lat) in coords_of_node.items():
        k = (int(lon * mx / cell_m), int(lat * my / cell_m))
        key_of[n] = k
        cell[k] += 1
    return {n: cell[key_of[n]] for n in coords_of_node}


def build_adj(edges):
    adj = defaultdict(list)
    for ei, e in enumerate(edges):
        adj[e["u"]].append((e["v"], ei, e["length"]))
        adj[e["v"]].append((e["u"], ei, e["length"]))
    return adj


def betweenness(edges, adj, density, radius, junction_penalty=200.0,
                samples=200, seed=1):
    """Edge load from density-weighted trips of at most `radius` cost."""
    nodes = list(adj.keys())
    if not nodes:
        return {}
    rng = random.Random(seed)

    weights = [max(1, density.get(n, 1)) for n in nodes]
    total = float(sum(weights))
    sources = rng.choices(nodes, weights=weights, k=min(samples, len(nodes)))

    load = defaultdict(float)
    for s in sources:
        dist = {s: 0.0}
        parent_edge = {}
        parent = {}
        order = []
        pq = [(0.0, s)]
        done = set()
        while pq:
            d, v = heapq.heappop(pq)
            if v in done:
                continue
            done.add(v)
            order.append(v)
            for w, ei, length in adj[v]:
                if w in done:
                    continue
                nd = d + length + junction_penalty
                if nd > radius:
                    continue
                if nd < dist.get(w, math.inf):
                    dist[w] = nd
                    parent[w] = v
                    parent_edge[w] = ei
                    heapq.heappush(pq, (nd, w))

        # Accumulate destination weight back up the shortest-path tree.
        acc = {n: float(max(1, density.get(n, 1))) for n in order}
        for v in reversed(order):
            if v == s:
                continue
            load[parent_edge[v]] += acc[v]
            acc[parent[v]] += acc[v]

    scale = total / float(len(sources))
    return {ei: v * scale for ei, v in load.items()}
