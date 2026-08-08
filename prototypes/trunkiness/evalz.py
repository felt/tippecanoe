"""Evaluate admission with per-zoom simplified vertex costs."""
import sys
from collections import defaultdict
import chainrank, gaps, grow, run as R, simplify as S, trunkiness as T

base, namef, zooms = sys.argv[1], sys.argv[2], [int(x) for x in sys.argv[3].split(",")]
budget = int(sys.argv[4]) if len(sys.argv) > 4 else 1500
percoord = len(sys.argv) > 5 and sys.argv[5] == "perlat"

feats = R.load(base, namef)
mxmy = (None, None) if percoord else T.local_scale(
    sum(f["coords"][0][1] for f in feats) / len(feats))
edges, nid = T.build_graph(feats, *mxmy)
so = T.build_strokes(edges, len(nid), *mxmy)
cut, clen, bridges = chainrank.cut_scores(edges, len(nid), so)

groups = defaultdict(list); best=[0.0]*len(feats); where={}
for ei, e in enumerate(edges):
    for fi in e["features"]:
        if clen[so[ei]] > best[fi]: best[fi]=clen[so[ei]]; where[fi]=so[ei]
for fi, s in where.items(): groups[s].append(fi)
flen=[0.0]*len(feats)
for e in edges:
    for fi in e["features"]: flen[fi]+=e["length"]
for i,f in enumerate(feats): f["length"]=flen[i]

print("== %s | %d edges, %d chains, %d bridges (%.0f%%), %.0f km"
      % (base.split('/')[-1], len(edges), len(clen), len(bridges),
         100.0*len(bridges)/len(edges), sum(clen.values())/1000))

for z in zooms:
    # Cost model: what the tile actually pays after simplification at this zoom.
    fz = [{"coords": S.simplified_coords(f["coords"], z), "length": f["length"]}
          for f in feats]
    raw = sum(len(f["coords"]) for f in feats)
    simp = sum(len(f["coords"]) for f in fz)
    print("\n   z%d, %d vertices/tile (geometry simplifies to %.1f%% of raw)"
          % (z, budget, 100.0*simp/raw))
    print("      %-22s %8s %8s %8s %7s %8s"
          % ("strategy","kept km","within#","cross#","cov","largest"))
    ki = gaps.select(fz, flen, z, budget, False)
    r = gaps.measure(feats, edges, so, ki, z)
    print("      %-22s %8.0f %8d %8d %6.1f%% %7.1f%%" % ("drop-smallest (today)", r[0],r[1],r[2],r[4],r[5]))
    for lbl, sc in (("chain length", clen), ("network cut", cut)):
        for an in ("filter","grow"):
            ki = (gaps.select_greedy_strokes(fz, groups, sc, z, budget) if an=="filter"
                  else grow.grow(fz, edges, so, groups, sc, z, budget))
            r = gaps.measure(feats, edges, so, ki, z)
            print("      %-22s %8.0f %8d %8d %6.1f%% %7.1f%%"
                  % ("%s + %s"%(lbl,an), r[0],r[1],r[2],r[4],r[5]))
