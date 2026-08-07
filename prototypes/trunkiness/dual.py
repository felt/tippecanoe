"""Betweenness on the dual (stroke-adjacency) graph.

Distance-weighted betweenness on the primal graph does not separate a freeway
from a surface street: the freeway is not shorter in metres, and in a regular
grid the shortest paths tie in every direction, which diffuses the measure.

Taking strokes as nodes and adjacency as edges makes the cost of a path its
number of *turns*. A freeway crosses the county in one stroke; the equivalent
trip on the grid takes many. This is the space-syntax "choice" measure, and it
recovers road hierarchy without needing a road-class attribute.

Betweenness is estimated from a sample of sources with Brandes accumulation,
which is O(k(V+E)) on a graph one to two orders of magnitude smaller than the
raw edge graph.
"""

import random
from collections import defaultdict, deque


def stroke_adjacency(edges, stroke_of):
    """Strokes that meet at a shared junction become adjacent dual nodes."""
    at_node = defaultdict(set)
    for ei, e in enumerate(edges):
        at_node[e["u"]].add(stroke_of[ei])
        at_node[e["v"]].add(stroke_of[ei])

    adj = defaultdict(set)
    for _node, strokes in at_node.items():
        ss = list(strokes)
        for i in range(len(ss)):
            for j in range(i + 1, len(ss)):
                adj[ss[i]].add(ss[j])
                adj[ss[j]].add(ss[i])
    return {k: list(v) for k, v in adj.items()}


def betweenness(adj, samples=400, seed=1):
    """Approximate node betweenness by Brandes accumulation from sampled sources."""
    nodes = list(adj.keys())
    if not nodes:
        return {}
    rng = random.Random(seed)
    sources = nodes if len(nodes) <= samples else rng.sample(nodes, samples)

    bc = defaultdict(float)
    for s in sources:
        # BFS: shortest-path counts and predecessors
        sigma = defaultdict(float)
        sigma[s] = 1.0
        dist = {s: 0}
        preds = defaultdict(list)
        order = []
        q = deque([s])
        while q:
            v = q.popleft()
            order.append(v)
            dv = dist[v]
            for w in adj.get(v, ()):
                if w not in dist:
                    dist[w] = dv + 1
                    q.append(w)
                if dist[w] == dv + 1:
                    sigma[w] += sigma[v]
                    preds[w].append(v)

        # accumulate dependencies back along the BFS tree
        delta = defaultdict(float)
        for w in reversed(order):
            coeff = (1.0 + delta[w]) / sigma[w]
            for v in preds[w]:
                delta[v] += sigma[v] * coeff
            if w != s:
                bc[w] += delta[w]

    scale = len(nodes) / float(len(sources))
    return {n: bc[n] * scale for n in nodes}
