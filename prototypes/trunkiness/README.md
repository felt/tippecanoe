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

### Stroke building on untagged data

Names must not be used: the facility has to work on inputs whose attributes we
cannot predict. `build_strokes` therefore defaults to `use_names=False`. That
costs a lot, and it is worth being explicit about how much.

With GNIS names, each of the four trunk streams in HU8 02070004 assembled into a
single stroke (100%). Without them, the best is 35-49% of each trunk in its
largest stroke. End to end, under greedy whole-stroke admission:

    NHD 02070004 z11        kept km  within#   cross#     cov  largest
    strokes built with names   1782        0        0   49.6%    86.8%
    strokes built untagged     1733        0        0   51.5%    35.3%

Retained length and coverage barely move, but largest-connected-component
collapses from 86.8% to 35.3%. **The strong hydrography result depended on the
name attribute.** On roads the difference is smaller: 66.3% to 70.2% largest,
with more within-tile gaps (19 to 81).

This also exposes a blind spot in the gap metric. Gaps are counted per stroke,
so a trunk that fragments into three strokes, two admitted and one rejected,
scores zero gaps while drawing a visibly broken river. When strokes stop
corresponding to real linear features the metric stops meaning much, and
largest-connected-component is the number that still tells the truth — the same
lesson as the coverage-gaming episode above.

### Joining by shallowest angle alone is already optimal

Chains built untagged, joining purely by shallowest deflection with a 60 deg
gate, no length term:

    dataset          chains   over      longest   top10   p50     p90     p99
    Alameda roads    19355    9996 km   37.0 km   312 km  0.20    1.04    5.59 km
    NHD 02070004      4684    7914 km  117.6 km   471 km  1.09    3.41    9.26 km

The median is not the interesting number, since most chains are genuinely short
local streets and headwater twigs. Length-weighted, a random kilometre of road
sits in a chain averaging **3.9 km** and a random kilometre of river in one
averaging **7.4 km**.

Three things about this matching are worth recording, all verified rather than
assumed:

**Edge reversal is already handled** and is not the bottleneck. Each edge
registers both of its ends, so a join can pair either end of one edge with
either end of another. Roads use start-start or end-end for 3% of joins. NHD
splits 50/48 between the two start-end orderings because flowlines are digitized
downstream, so two tributaries both *end* at a confluence.

**Ranking the joins globally rather than per node changes nothing.** Each
edge-end belongs to exactly one node, so two nodes can never contend for the
same end; the matching decomposes per node and a global sort can only change
which cycles get broken. Sorting all 78528 candidate pairs by angle and by
accumulated chain length produced *byte-identical* join sets — 70535 joins, zero
differing.

**And the greedy is already choosing the longest chain.** At the trunk breaks,
the pair that consumed the contested end leads into a chain that is longer in 25
cases, equal in 11, and shorter in 0 — on hydrography 14 / 7 / 0. Angle and
chain length never disagree, which is why iterative refinement on chain length
is a fixed point from the first iteration.

Recursive lookahead does not rescue it either. `lookahead.py` ranks each
candidate join by how much chain is reachable if you extend outward from both
sides for a few more steps, with angle demoted to a tiebreak. It is worse than
plain angle at every depth tried:

    NHD 02070004     chains   longest   len-wtd mean   trunk coherence
    angle only         4684  117.6 km        7.40 km   35 / 46 / 48 / 49%
    lookahead d=1      4689   47.1 km        5.32 km   20 / 33 / 30 / 39%
    lookahead d=4      4687   64.6 km        6.81 km   20 / 17 / 44 / 53%

    Alameda roads      chains   longest   len-wtd mean   I-880
    angle only          19357   37.0 km        3.90 km     32%
    lookahead d=0       19557   25.1 km        2.73 km      2%
    lookahead d=4       19561   33.1 km        3.28 km      2%

Depth does help relative to d=0 — 2.73 to 3.28 km on roads — so the lookahead is
working; it just never recovers the ground that promoting length cost in the
first place. The reason is that at a junction *on* a trunk every candidate
continuation is attached to the same trunk, so all of them report nearly the
same reachable length: the score is dominated by the shared network beyond and
carries almost no signal, while the angle is the only thing that distinguishes
the through-route from the ramp. Making length primary discards the
discriminating signal and keeps the uninformative one.

That is also the cleaner explanation of the 25/11/0 result above. Reach is
roughly constant across the candidates at a trunk junction, which is precisely
why it can never usefully overrule the angle.

So a "% of I-580 in its largest chain" figure of 16% is not the algorithm
failing. It is measuring name agreement, and at an interchange the geometry
genuinely continues straighter into an adjoining chain that is itself 22-34 km
long. Given that attributes are off the table, name agreement is the wrong
yardstick; chain length is the right one, and by that measure the chains are
long.

Including sum-of-distances in the ranking *does* change the result, and its sign
flips by dataset:

    median edge length      network   trunk features
    Alameda roads              74 m   33 m  (I-580, I-880)
    NHD 02070004              382 m   453-651 m  (Potomac, Opequon)

Trunk roads are noded at every ramp and cross street, so their edges are
*shorter* than average and longest-pairs-first selects against them. Trunk
rivers have long reaches between confluences, so the same term helps slightly.
Same rule, opposite effect, so it is unsafe as a generic heuristic and is not
used. Widening the bearing window (25 m to 500 m) does not help either: it
degrades hydrography badly, since rivers meander and a long window mismeasures
the local through-direction.

## Ranking chains by what depends on them

A chain's own length says nothing about what hangs off it. `chainrank.py` scores
each chain by how much of the network is cut off from the main body when the
whole chain is removed. That is a chain-level question and not the same as the
edge-level criticality above: an edge in the middle of a main stem may orphan
little, while removing the entire stem detaches every tributary on it. Since a
removal can only disconnect along bridges, the work happens on the bridge tree.

Ranking alone is only half of it. Admitting chains best-first, by any ranking,
still treats each chain independently, so the result is a set of individually
good chains that need not touch each other. `grow.py` admits them Prim-style
instead: a chain that attaches to what is already in is preferred over a
higher-scoring one that floats free, with a `bridgehead` floor so separate river
systems still get seeded rather than everything having to hang off the first
thing admitted.

The two are complementary. NHD HU8 02070004, z11, 1500 vertices/tile, untagged:

    rank by                admit     kept km  within#   cross#     cov  largest
    chain length           filter       1762        0        0   51.8%    26.2%
    chain length           grow         1717        0        0   48.4%    40.8%
    network cut            filter       1649        0        0   47.3%    38.0%
    network cut            grow         1573        0        0   44.0%    52.3%
    sqrt(cut * length)     grow         1600        0        0   43.9%    52.5%

Better ranking buys 26 to 38%, better admission 26 to 41%, and together 26 to
52% — a doubling of continuity for about 9% less retained length and 8 points of
coverage, with no gaps of either kind at any point.

TIGER Alameda County roads at z12 invert the ranking result completely:

    rank by                admit     kept km  within#   cross#     cov  largest
    chain length           filter       1460       87       18   63.2%    68.0%
    chain length           grow         1423       80       19   59.0%    82.4%
    network cut            filter       1127       95        6   68.6%     5.8%
    network cut            grow         1235       61       14   46.2%    16.3%
    sqrt(cut * length)     grow         1334       71       16   47.8%    11.8%

**The cut score is catastrophic on a road network** — 5.8% largest component
against 68% for plain chain length. Only 15% of road edges are bridges, so
almost every chain has a cut of exactly zero, including every freeway, because
they sit in the 2-edge-connected core and removing one disconnects nothing. What
does score highly is rural dead-ends and cul-de-sac trees. This is the same
blind spot as the edge-level criticality term, confirmed at chain level: a
removal-based measure only carries signal where the network is tree-like.

**Growth-based admission, by contrast, helps both** — 26 to 41% on hydrography
and 68 to 82% on roads, in each case for a few percent less retained length. It
makes no assumption about network structure, only that the output should be
connected.

So the two halves have very different standing. Prefer connectivity-aware
admission everywhere; treat the cut score as a hydrography-shaped heuristic that
must not be applied blind to meshed networks.

Two cautions about the cut score itself. It is dominated by short chokepoints: a
2.8 km piece of Back Creek scores 1072 km because it is the mouth the whole
sub-basin attaches through, so ranking on it alone promotes stubs over the
trunks they connect. And parallel features cancel each other — the Chesapeake
and Ohio Canal runs beside the Potomac, so each is the other's alternate path
and both score exactly 0 despite being the two longest chains in the subbasin.
Combining cut with length (`sqrt(cut * length)`) blunts the first problem; the
second is inherent to any measure based on what removal disconnects.

### Coordinates must be metric before any angle or distance is taken

The inputs are WGS84 degrees, and at these latitudes a degree of longitude is
only about 0.79 of a degree of latitude on the ground. Every angle and every
distance here is therefore computed after scaling longitude by cos(latitude):
`local_scale()` returns `mx = cos(lat) * m/deg` and `my = m/deg`, and both
`chain_length` and `departure_bearing` apply them before `hypot` and `atan2`
respectively — including the 25 m window over which a departure bearing is
measured, which is itself a distance.

It matters more than the 0.79 factor suggests, because it compounds through
every pairing decision:

    NHD 02070004        chains   longest   length-wtd mean   trunk coherence
    scaled (cos-lat)      4684  117.6 km          7.40 km   35% / 46% / 48%
    raw degrees           4949   37.3 km          3.73 km   16% / 14% /  6%

Two consequences for a real implementation:

- **A single scale factor for the whole input is not good enough.** These runs
  use the mean latitude of the dataset, which is fine here — cos varies only
  0.7890 to 0.7938 across Alameda and 0.7634 to 0.7764 across the HUC8 — but
  over a continental or global input it would be badly wrong. The scaling has to
  come from each feature's own latitude.
- **In tippecanoe's internal web mercator coordinates, angles need no correction
  at all.** Mercator is conformal, so it preserves angles locally. Building the
  same chains in mercator rather than in cos-lat metres reproduces 97.0% of the
  chains on roads and 89.9% on hydrography, and the residual is not the angles:
  it is the *bearing window*, since 25 mercator units is 25/cos(lat) ground
  metres. So a C++ pass can take deflection angles straight from projected
  coordinates and only needs latitude correction where it compares lengths.

## Statewide primary/secondary roads

TIGER's statewide California primary/secondary layer is the case this method
suits best. It is sparse, so chains are long: 7335 features become 449557 edges
and 1735 chains over 37692 km, with a longest chain of 929 km and a
length-weighted mean of 162 km, against 3.9 km for the full Alameda road
network. The longest chains recovered, with no attributes used, are I-5
(929 km, 92% I-5), State Route 99 (665 km), US 101 (639 km), I-15 and I-40.

    z8, 1500 vertices/tile   kept km  within#   cross#     cov  largest
    drop-smallest (today)      23027      255       70   87.3%    26.4%
    chain length + filter      22850       85       16   81.5%    93.0%
    chain length + grow        22276       73       13   77.9%    97.8%

Across zooms, `largest` for drop-smallest against chain length + grow:

    z6   20.9% -> 91.6%       z9    87.2% -> 98.4%
    z7   23.8% -> 94.0%       z10   96.8% -> 97.8%
    z8   26.4% -> 97.8%

The gain is largest where the budget bites hardest and closes as the constraint
relaxes, which is what should happen. Gaps fall in the same direction: at z8,
255 within-tile and 70 cross-tile breaks become 73 and 13.

### Vertex cost has to be measured after simplification

These numbers depend on a correction that only matters at low zoom. Tippecanoe
simplifies geometry to the tile's resolution before a tile is measured, so a
long chain does not cost a tile its raw vertices: statewide, only 3.8% of
vertices survive at z6 and 7.9% at z8. Costing raw overstates a long chain by
more than an order of magnitude, and whole-chain admission then rejects I-5
outright — precisely the feature the method exists to keep. Before this was
fixed the same run reported a largest component of 12.4% at z7, worse than
drop-smallest; after, it is 94.0%. `simplify.py` does Douglas-Peucker in tile
units, and admission is costed against the simplified geometry while length and
continuity are still measured on the full geometry.

## Deprioritizing ramp-like branches by shape

At an interchange the mainline curves slightly while a ramp leaves almost
straight, so the ramp wins on deflection and breaks the trunk. But a ramp gives
itself away by what it does next: it keeps turning. `curvature.py` accumulates
the total absolute turning over the next 300 m of each candidate branch and adds
it to the deflection as a penalty. No attribute is read; turning is geometry.
Road class is used only to check the result:

    median turning over the next 300 m, TIGER Alameda
      S1100 primary    9.3 deg      S1400 local   29.6 deg
      S1200 secondary 10.0 deg      S1630 ramp    58.6 deg

Note what this separates. It is not ramp from not-ramp — ramps at 58.6 sit close
to local streets at 29.6 — it is *trunk from everything else*, since trunk roads
are the only thing that does not curve.

The measure is not comparable across datasets: rivers meander, so the NHD median
is 134 degrees against 30 for roads, and a weight tuned on roads damages
hydrography (Antietam Creek falls from 49% to 33% of its length in one chain).
Normalizing each branch by the dataset's own median turning fixes that, and one
weight then serves both:

    alpha (in units of median turning)   longest   len-wtd mean   I-238   I-880
    Alameda roads    0.0                 37.0 km        3.90 km     32%     32%
                     1.5                 55.3 km        4.31 km     48%     48%
                     2.5                 55.3 km        4.39 km     48%     48%
    NHD 02070004     0.0                117.6 km        7.40 km       —       —
                     2.5                117.6 km        7.41 km       —       —

So it is a real gain on roads and harmless on hydrography — the first change
tried that is not a wash or a trade. On the specific case that prompted it,
LINEARID 11012813207468 breaking I-238, the road goes from 6 chains to 4 and its
largest chain from 32% to 48% of its length.

Downstream the effect is muted, because growth admission is already close to the
ceiling:

    Alameda z12   alpha 0.0 -> 1.5   largest 98.3% -> 98.5%, within-tile 144 -> 135
    prisec  z8    alpha 0.0 -> 1.5   largest 97.8% -> 98.3%, within-tile  73 ->  69

Worth having, since chain length is what the ranking keys on and better chains
make a better ranking, but it is not transformative at the output.

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
