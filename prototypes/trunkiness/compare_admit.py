"""Compare ranking (length vs network cut) against admission (filter vs grow)."""
import sys
from collections import defaultdict
import chainrank, gaps, grow, run as R, trunkiness as T

base, namef, z = sys.argv[1], sys.argv[2], int(sys.argv[3])
budget = int(sys.argv[4]) if len(sys.argv) > 4 else 1500

features = R.load(base, namef)
lats = [f["coords"][0][1] for f in features]
mx, my = T.local_scale(sum(lats)/len(lats))
edges, node_ids = T.build_graph(features, mx, my)
stroke_of = T.build_strokes(edges, len(node_ids), mx, my)
scores, chain_len, bridges = chainrank.cut_scores(edges, len(node_ids), stroke_of)

groups = defaultdict(list); best=[0.0]*len(features); where={}
for ei, e in enumerate(edges):
    for fi in e["features"]:
        if chain_len[stroke_of[ei]] > best[fi]:
            best[fi] = chain_len[stroke_of[ei]]; where[fi] = stroke_of[ei]
for fi, s in where.items(): groups[s].append(fi)

# a combined rank: how much depends on it, plus how much of it there is
comb = {s: (scores[s] * chain_len[s]) ** 0.5 for s in chain_len}

print("== %s z%d budget %d" % (base.split('/')[-1], z, budget))
print("   %-22s %-8s %8s %8s %8s %7s %8s" % ("rank by","admit","kept km","within#","cross#","cov","largest"))
for rname, sc in (("chain length", chain_len), ("network cut", scores), ("sqrt(cut*length)", comb)):
    for aname, fn in (("filter", gaps.select_greedy_strokes),
                      ("grow", None)):
        if fn: kept_in = fn(features, groups, sc, z, budget)
        else:  kept_in = grow.grow(features, edges, stroke_of, groups, sc, z, budget)
        k,w,c,cg,cov,lg = gaps.measure(features, edges, stroke_of, kept_in, z)
        print("   %-22s %-8s %8.0f %8d %8d %6.1f%% %7.1f%%" % (rname, aname, k, w, c, cov, lg))
