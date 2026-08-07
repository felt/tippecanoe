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
2. **Split features into graph edges** at those nodes.
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
    drop-smallest        24.5%      145     69.1%     72.0%
    drop-densest         19.5%      806     42.6%     84.4%
    trunk/raw            23.8%      121     84.0%     70.2%
    trunk/local z14      22.0%      391     73.9%     80.7%

Hydrography is the dramatic case: continuity goes from 3.7% to 75.7% of
retained length in one component, at the same budget and slightly *more* length
kept. On roads the gain is real but smaller (69% to 84%), because a road network
is already 90% non-bridge and stays connected under most thinning.

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

### Local normalization is a continuity/coverage dial, not a free win

Converting the score to a percentile within a local neighborhood trades
continuity for spatial evenness, and the best neighborhood size depends on the
zoom being rendered — so a single precomputed attribute cannot be optimal at
every zoom. `trunk/raw` (globally comparable, no local normalization) is the
safer default: it beats `drop-smallest` on continuity at equal cost and never
collapses. `trunk/local z14` approaches `drop-densest` on coverage while keeping
far better continuity, but degrades at low zoom.

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
