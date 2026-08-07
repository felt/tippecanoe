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

## Continuity gaps: the thing that actually matters

Ranking features is not really the goal. A ranking that is *right* but that
draws half of I-80 is worse than a mediocre one that draws it whole. So
`gaps.py` measures gaps directly, distinguishing two independent causes:

- **within-tile** — the score varies along a stroke, so one tile's threshold
  falls in the middle of it
- **cross-tile** — the score is constant along the stroke, but neighbouring
  tiles pick different thresholds, so it appears in one and not the next

Both are measured against a threshold-style admission that mirrors
`choose_minextent`: pick a value, keep everything at or above it. Modelling this
as a greedy fill instead is wrong and hides the result, because a greedy fill
splits sets of equally ranked features when the budget runs out partway through.

NHD HU8 02070004 at z11, 1500 vertices/tile, over a 7914 km network:

    variant                           kept km  within#   cross#     cov  largest
    per-edge score, per-tile             1634        5       57   44.4%    81.4%
    per-stroke, per-tile                 1618        0       58   43.8%    80.6%
    per-stroke, shared threshold          372        0        0    8.8%   100.0%
    per-stroke local z15, shared          479        0        0   12.1%   100.0%
    greedy whole strokes                 1782        0        0   49.6%    86.8%

TIGER Alameda County roads at z12, over a 9996 km network:

    variant                           kept km  within#   cross#     cov  largest
    per-stroke, per-tile                 1361       12      165   53.3%    67.6%
    per-stroke, shared threshold          287        2        1   10.5%    99.9%
    greedy whole strokes                 1498       19        9   62.3%    66.3%

Three findings:

1. **Collapsing the score to one value per stroke removes within-tile gaps and
   costs nothing.** On hydrography it goes to exactly zero. Roads keep a residue
   because a feature can lie on more than one stroke.

2. **A threshold shared across the zoom removes cross-tile gaps but is not
   usable.** It is set by the densest tile, so it strips the whole zoom to what
   downtown can afford: 1618 km down to 372 km, coverage 43.8% down to 8.8%.
   Local normalization does not rescue it.

3. **Greedy whole-stroke admission does what the shared threshold was trying
   to.** Take strokes best-first and admit one only if it fits in every tile it
   crosses. This dominates on every axis at once — more retained length than the
   per-tile threshold, no gaps of either kind, best coverage and best
   continuity — because rejecting a stroke frees nothing in tiles it never
   enters, so sparse tiles keep filling with local strokes long after the trunks
   are placed. A single global threshold cannot express that.

The catch is that it is not a per-feature threshold test, so it does not drop
into the existing `-as-needed` machinery unchanged: admission depends on the
whole stroke and on every tile it crosses. It needs a pass that knows the
stroke-to-tile mapping, which is a global precomputation.

### Local normalization needs a minimum neighbourhood population

An earlier version of this ranked a plain local percentile within a fixed fine
neighbourhood, and reported that it made the shared threshold affordable. That
was wrong. At z15 a third of the buckets in a HUC8 hold a single edge, and a
percentile within a bucket of one is 1.0, so every isolated headwater twig
scored a perfect 1.0 and outranked the Potomac. The apparent coverage win was
the coverage metric being gamed: coverage counts distinct fine tiles touched, so
scattering isolated features maximizes it while producing a map of fragments.

Two changes fix it. Widen each bucket until it holds at least ~16 features, so a
percentile is taken against a real comparison set; and combine the local
percentile with the global magnitude as a geometric mean rather than replacing
one with the other, so a trunk competing in a crowded neighbourhood still
outranks a twig alone in an empty one. Pure local rank remains unusable even
with the population floor: its top picks are Sawmill Run and S Kelso Rd, and
largest-component falls to 23-43%.

Report largest-connected-component alongside coverage. Coverage alone cannot
tell a well-spread network from scattered debris, and that is exactly the
failure it hid.

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
