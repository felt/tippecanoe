"""Rank chains by their contribution to holding the network together.

Ranking a chain by its own length says nothing about what depends on it. The
question that matters is: if this whole chain were removed, how much of the
network would be cut off from the main body?

That is a chain-level question, and it is not the same as the edge-level
criticality used earlier. An edge in the middle of a river's main stem may not
be a bridge whose removal orphans much, while removing the entire stem detaches
every tributary hanging off it.

Removing a chain can only disconnect the network along bridges, so the work is
done on the bridge tree: contract every 2-edge-connected component, leaving a
forest whose nodes carry the length inside them and whose edges are bridges.
Deleting a chain deletes its bridges from that forest; what is left splits into
components, and the score is everything that is neither the chain itself nor the
largest surviving piece.

Usage:
    python3 chainrank.py <shapefile-base> <name-field> [zoom] [vertices-per-tile]
"""

import sys
from collections import defaultdict

import gaps
import run as R
import trunkiness as T


def bridge_forest(edges, num_nodes, bridges):
    """Contract non-bridge edges. Returns (component of each node, node weights)."""
    uf = T.UnionFind(num_nodes)
    for ei, e in enumerate(edges):
        if ei not in bridges:
            uf.union(e["u"], e["v"])

    weight = defaultdict(float)
    for ei, e in enumerate(edges):
        if ei not in bridges:
            weight[uf.find(e["u"])] += e["length"]
    return uf, weight


def cut_scores(edges, num_nodes, stroke_of):
    """For each chain, the length cut off from the main body by removing it."""
    bridges = T.find_bridges(edges, num_nodes)
    uf, node_weight = bridge_forest(edges, num_nodes, bridges)

    total = sum(e["length"] for e in edges)
    chain_len = defaultdict(float)
    for ei, e in enumerate(edges):
        chain_len[stroke_of[ei]] += e["length"]

    # Bridges grouped by the chain they belong to.
    by_chain = defaultdict(list)
    for ei in bridges:
        by_chain[stroke_of[ei]].append(ei)

    # Nodes of the contracted forest that carry weight, so isolated components
    # still count even when no bridge touches them.
    comps = set(uf.find(n) for e in edges for n in (e["u"], e["v"]))

    scores = {}
    for s in chain_len:
        mine = set(by_chain.get(s, ()))
        if not mine:
            scores[s] = 0.0  # removing it disconnects nothing
            continue

        f = T.UnionFind(num_nodes)
        for ei in bridges:
            if ei in mine:
                continue
            e = edges[ei]
            f.union(uf.find(e["u"]), uf.find(e["v"]))

        acc = defaultdict(float)
        for c in comps:
            acc[f.find(c)] += node_weight.get(c, 0.0)
        for ei in bridges:
            if ei in mine:
                continue
            acc[f.find(uf.find(edges[ei]["u"]))] += edges[ei]["length"]

        largest = max(acc.values()) if acc else 0.0
        scores[s] = max(0.0, total - chain_len[s] - largest)
    return scores, chain_len, bridges


def main():
    base, namef = sys.argv[1], sys.argv[2]
    z = int(sys.argv[3]) if len(sys.argv) > 3 else 11
    budget = int(sys.argv[4]) if len(sys.argv) > 4 else 1500

    features = R.load(base, namef)
    lats = [f["coords"][0][1] for f in features]
    mx, my = T.local_scale(sum(lats) / len(lats))
    edges, node_ids = T.build_graph(features, mx, my)
    stroke_of = T.build_strokes(edges, len(node_ids), mx, my)

    scores, chain_len, bridges = cut_scores(edges, len(node_ids), stroke_of)
    total = sum(chain_len.values())
    print("== %s" % base.split("/")[-1])
    print("   %d chains, %d bridges of %d edges, network %.0f km"
          % (len(chain_len), len(bridges), len(edges), total / 1000))

    fname = [f["name"] for f in features]
    enames = [set(fname[fi] for fi in e["features"]) for e in edges]
    label = {}
    for ei, e in enumerate(edges):
        s = stroke_of[ei]
        for n in enames[ei]:
            if n:
                label.setdefault(s, defaultdict(float))[n] += e["length"]

    def name_of(s):
        d = label.get(s)
        return max(d.items(), key=lambda kv: kv[1])[0] if d else "<unnamed>"

    for title, key in (("by chain length", lambda s: chain_len[s]),
                       ("by network cut", lambda s: scores[s])):
        print("\n   top 10 %s:" % title)
        for s in sorted(chain_len, key=key, reverse=True)[:10]:
            print("      len %6.1f km  cut %6.1f km  %s"
                  % (chain_len[s] / 1000, scores[s] / 1000, name_of(s)))

    # Downstream: does ranking by cut beat ranking by length under the budget?
    groups = defaultdict(list)
    best = [0.0] * len(features)
    where = {}
    for ei, e in enumerate(edges):
        for fi in e["features"]:
            if chain_len[stroke_of[ei]] > best[fi]:
                best[fi] = chain_len[stroke_of[ei]]
                where[fi] = stroke_of[ei]
    for fi, s in where.items():
        groups[s].append(fi)

    print("\n   under a %d-vertex budget at z%d:" % (budget, z))
    print("      %-16s %8s %8s %8s %7s %8s"
          % ("rank by", "kept km", "within#", "cross#", "cov", "largest"))
    for title, sc in (("chain length", chain_len), ("network cut", scores)):
        kept_in = gaps.select_greedy_strokes(features, groups, sc, z, budget)
        k, w, c, cg, cov, lg = gaps.measure(features, edges, stroke_of, kept_in, z)
        print("      %-16s %8.0f %8d %8d %6.1f%% %7.1f%%" % (title, k, w, c, cov, lg))


if __name__ == "__main__":
    main()
