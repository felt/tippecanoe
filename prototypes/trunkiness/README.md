# LineString "trunkiness" scoring — offline prototype

An experiment in ranking LineString features by their role in the network, so
that when tiles have to shed features the network degrades by thinning rather
than by falling apart.

Nothing here is wired into tippecanoe. It is a standalone, stdlib-only Python
prototype used to decide whether the idea is worth implementing in the tiler.

## Why

`--drop-smallest-as-needed` keys on `sf.extent`, which for a line is
`M_PI * (dist/2) * (dist/2)` — a pseudo-area derived from the length of *one
feature* (`serial.cpp:661`). But where one feature ends and the next begins is a
digitizing convention, not a property of the network. TIGER splits roads at
county lines; NHD splits flowlines at every confluence, so the trunk of a river
arrives as hundreds of short reaches that each look like a twig.

`--drop-densest-as-needed` keys on `sf.gap`, the quadkey distance between
successive feature start points. That is a density measure with no notion of
what the feature is, so a threshold removes whichever contiguous region is
densest — the urban core — rather than thinning it.

Neither has any concept of "would dropping this open a gap in something
continuous".

## What it computes

1. **Node the network on exact shared coordinates**, including interior
   vertices. This matters: merged road layers T-junction into the *middle* of a
   feature. Only coordinates that are bitwise equal are joined — there is no
   proximity snapping.
2. **Split features into graph edges** at those nodes, then **collapse
   coincident edges** — see below, this one is not optional.
3. **Chain edges into strokes** by good continuation: at each junction, pair the
   two edges with the smallest deflection angle, preferring a name match and
   allowing a looser angle when the names agree.
4. **Score each edge on two terms**:
   - *criticality* — length-weighted `n1*n2` betweenness on the bridge tree.
     Contract every non-bridge edge (Tarjan) and the result is a forest, on
     which betweenness is two linear passes. Non-bridge edges score 0: removing
     one disconnects nothing. Normalized per connected component so a small
     isolated network still keeps its own trunk.
   - *prominence* — the length of the stroke the edge belongs to.
5. **Roll up to features** by taking the max over their edges: a feature
   carrying any part of a trunk has to survive or the trunk breaks.

Two terms are needed because each covers the other's blind spot. On a river
network the basin outlet is pendant in the bridge tree, so criticality
underrates it — the stroke term rescues it. In a dense urban grid nothing is a
bridge, so criticality is uniformly 0 and the stroke term does all the work.

## Results

Evaluated the way tippecanoe actually applies dropping: per tile, against a
per-tile vertex budget, at a given zoom. Tiles under budget drop nothing.
`largest` is the share of retained length in the largest connected component —
the continuity measure. `tile cov` is the fraction of fine tiles that retain
anything — the "did whole areas fall out" measure.

NHD HU8 02070004 (Conococheague-Opequon), z11, 1500 vertices/tile:

    strategy          len kept    #comp   largest  tile cov
    drop-smallest        22.5%      621      3.7%     64.5%
    drop-densest         20.8%     1071      4.0%     79.6%
    trunk/raw            23.3%      184     75.7%     56.0%

TIGER 2025 Alameda County roads, z12, 1500 vertices/tile:

    strategy          len kept    #comp   largest  tile cov
    drop-smallest        24.5%      145     50.5%     72.0%
    drop-densest         19.5%      806     38.2%     84.4%
    trunk/raw            23.8%      118     61.1%     70.2%
    trunk/local z14      22.4%      390     52.3%     80.1%
    class only           22.4%      546     44.9%     69.1%

Hydrography is the dramatic case: continuity goes from 3.7% to 75.7% of
retained length in one component, at the same budget and slightly *more* length
kept. On roads the gain is real but much smaller (50.5% to 61.1%), because a
road network is mostly non-bridge and stays connected under most thinning.

`class only` ranks by the TIGER MTFCC road class, as a reference point for how
far an attribute alone gets you. It is worse than the network score on every
measure here, but it selects a different and in places more cartographically
defensible set — see the limits section.

Sanity checks against ground truth. Longest strokes recovered in Alameda are
I-580, I-880/Nimitz, I-680/Sinclair and MacArthur Fwy; in HU8 02070004 they are
Conococheague Creek, the Potomac, Opequon Creek and Antietam Creek, which are
the streams the subbasin is named for.

### Noding at interior vertices is what makes it work

On TIGER `roads`, which is merged rather than topologically noded:

    endpoint-only noding  -> 14772 connected components over 25528 features
    full noding           ->    37 connected components
    junctions found only at interior vertices: 52615

Against `tl_2025_06001_edges` (already noded) restricted to `ROADFLG=Y` as a
control, the reconstructed graph has 67,157 nodes vs 59,669, and 10,811 km vs
9,995 km. The residual is TIGER carrying the same centerline as several features
under different names (`I- 880` and `Nimitz Fwy` are separate features).

### Coincident duplicate centerlines have to be collapsed first

Source data routinely carries one centerline several times. TIGER files I-880
and Nimitz Fwy as separate features over the same geometry — they share 2000 of
2000 vertices. They are not byte-identical *features* (the two are split into
different numbers of pieces, so a plain duplicate-feature test finds only 0.8%
of the file), but after noding they become duplicate *graph edges*, which is
exact and cheap to detect.

On Alameda roads, **36.7% of graph edges were coincident duplicates**. Left in,
they do two kinds of damage:

- Parallel edges are never bridges, so the criticality term is suppressed
  almost everywhere. Collapsing them raised the bridge count from 10% to 15% of
  edges and materially changed the ranking.
- They make every shared vertex a junction, which destroys any measure based on
  junction spacing. Junctions per km for I-580 read 14.31 before the collapse
  and 3.03 after; by TIGER class the medians go from S1100 17.64 / S1400 8.68
  (backwards) to S1100 3.08 / S1400 9.23 (correct).

They also inflate apparent connectivity, because keeping I-880 implicitly keeps
Nimitz Fwy's edges. The roads figures above are post-collapse and are therefore
lower, and more honest, than a first pass without it.

### Local normalization is a continuity/coverage dial, not a free win

Converting the score to a percentile within a local neighborhood trades
continuity for spatial evenness, and the best neighborhood size depends on the
zoom being rendered — so a single precomputed attribute cannot be optimal at
every zoom. `trunk/raw` (globally comparable, no local normalization) is the
safer default: it beats `drop-smallest` on continuity at equal cost and never
collapses. `trunk/local z14` approaches `drop-densest` on coverage while keeping
far better continuity, but degrades at low zoom.

## Continuity gaps: the thing that actually matters

Ranking features is not really the goal. A ranking that is *right* but that
draws half of I-80 is worse than a mediocre ranking that draws it whole. So
`gaps.py` measures gaps directly, distinguishing two independent causes:

- **within-tile** — the score varies along a stroke, so one tile's threshold
  falls in the middle of it
- **cross-tile** — the score is constant along the stroke, but neighbouring
  tiles pick different thresholds, so it appears in one and not the next

Both are measured against a threshold-style admission that mirrors
`choose_minextent`: pick a value, keep everything at or above it. Modelling this
as a greedy fill instead is wrong and hides the result, because a greedy fill
splits sets of equally ranked features when the budget runs out partway through.

NHD HU8 02070004 at z11, 1500 vertices/tile:

    variant                           kept km  within#   cross#  cross gap     cov
    per-edge score, per-tile             1634        5       57       365km   44.4%
    per-stroke, per-tile                 1618        0       58       381km   43.8%
    per-stroke, shared thresh             372        0        0         0km    8.8%
    per-stroke local z15, per-tile       1128        0       55       623km   44.5%
    per-stroke local z15, shared         1084        0        0         0km   54.4%

TIGER Alameda County roads at z12:

    variant                           kept km  within#   cross#  cross gap     cov
    per-edge score, per-tile             1369       10      170       591km   52.2%
    per-stroke, per-tile                 1368        9      170       591km   52.2%
    per-stroke, shared thresh             287        2        1         1km   10.4%
    per-stroke local z16, per-tile       1210       14      157       760km   57.4%
    per-stroke local z16, shared          568       10        1        12km   36.6%

Three findings:

1. **Collapsing the score to one value per stroke removes within-tile gaps and
   costs nothing.** On hydrography it goes to exactly zero. This is free and
   should be done regardless of anything else. Roads keep a residue of 9-14
   because a feature can lie on more than one stroke, and taking the max over
   them drags in part of a lower-ranked stroke.

2. **A threshold shared across the zoom removes cross-tile gaps, but on its own
   is ruinous** — 1618 km down to 372 km, coverage 43.8% down to 8.8% — because
   one global threshold is set by the densest tile and thins everywhere else to
   what downtown can afford.

3. **Normalizing locally first makes the shared threshold affordable.** With the
   score converted to a percentile within a fine neighbourhood and then averaged
   along the stroke, a single zoom-wide threshold gives zero gaps of either kind
   *and better coverage than the per-tile baseline* (54.4% vs 43.8%), for about
   a third less retained length. The neighbourhood has to be fine: at z+2 the
   buckets are too coarse and the shared threshold is still set by the worst
   tile.

The roads case is weaker on every count — gaps drop to near zero but retained
length halves and coverage falls from 57% to 37%. Road networks have far more
strokes competing per tile, so a shared threshold binds harder.

## What this does not fix

On roads the top of the ranking is right after the collapse — MacArthur Fwy,
I-80, John T Knox Fwy, I-580, Arthur H Breed Fwy, I-880, State Rte 84, Nimitz
Fwy, then E 14th St / International Blvd. But specific functionally important
links are still dropped:

    I- 580          1.000  kept
    I- 880          0.913  kept
    Ashby Ave       0.203  kept     (Berkeley surface street)
    I- 980          0.130  dropped
    San Pablo Ave   0.130  kept
    Broadway        0.123  dropped  (downtown Oakland)

I-980 is a short freeway spur and Broadway below I-580 is short, so the stroke
term cannot see them; both sit inside the biconnected urban core, so the
criticality term is identically zero there. Meanwhile a Berkeley or Alameda
grid street runs straight for miles and scores well on stroke length alone.

Three structural measures were tried against this and all failed:

- **Dual-graph (stroke adjacency) betweenness**, the space-syntax "choice"
  measure, where path cost is turns rather than distance. It ranks Otis Dr
  (0.100) above I-980 (0.082) and puts long surface arterials above I-580. The
  metric rewards strokes that touch *many* other strokes, and a freeway's
  defining property is that it touches very few.
- **Junction density** as a proxy for access control. Separates cleanly by class
  once coincident edges are collapsed (3.08 vs 9.23 junc/km), but still does not
  lift I-980 (7.33) above Ashby Ave (6.06): a short urban freeway has ramps.
- **Blending with the road class attribute.** Class alone does select I-980 and
  Broadway, but costs a lot of continuity (44.9% vs 61.1%), and the selection is
  partly an artifact of tie ordering among equal-class features. Weighted and
  lexicographic blends both lose the two features again, because once class
  ties are broken by trunkiness, I-980 sorts last among freeways and the tile
  budget runs out before it.

The deeper reason is that **I-980's importance is conditional on I-880 and I-580
being kept**. It matters because it joins two things already in the output. No
per-feature scalar can express that, and every `-as-needed` mechanism in
tippecanoe is a per-feature threshold test, so none of them can either — this is
a structural limit, not a tuning problem.

Getting these right needs a different shape of pass: choose a keep set by
whatever score, then run a **connectivity repair** step that adds back the
cheapest features needed to reconnect the pieces already chosen. That is
set-dependent rather than per-feature, but it is tractable inside the tile loop,
since by then the keep set is known and the stroke graph makes the candidate
connectors cheap to find.

## Caveats

- Exact coordinate matching only. Data whose connectivity is implied by
  proximity rather than identical coordinates will produce a shattered graph, and
  the failure is silent and looks plausible. Any real implementation wants to
  report the fraction of line endpoints that are shared and warn when it is near
  zero.
- Lengths use an equirectangular approximation, fine at county/HUC8 scale.
- The evaluation approximates tiling by assigning a feature to every tile its
  vertices touch, without clipping.

## Running it

    python3 compare.py <shapefile-base> <name-field> <zooms> <vertices-per-tile>

    python3 compare.py data/tl_2025_06001_roads FULLNAME 10,12 1500
    python3 compare.py data/NHDFlowline gnis_name 9,11 1500

`figures.py` writes the side-by-side SVGs. Data is not checked in: TIGER from
the Census FTP, NHD from
`https://prd-tnm.s3.amazonaws.com/StagedProducts/Hydrography/NHD/HU8/Shape/`.
