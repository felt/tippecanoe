"""Run the trunkiness prototype on a shapefile and compare it against the
heuristics tippecanoe uses today (drop-smallest and drop-densest).
"""

import json
import math
import sys
import time
from collections import defaultdict

import shpread
import trunkiness as T


def encode_quadkey(wx, wy):
    """Morton interleave, matching tippecanoe's default encode_index."""
    out = 0
    for i in range(32):
        out |= ((wx >> i) & 1) << (2 * i + 1)
        out |= ((wy >> i) & 1) << (2 * i)
    return out


def world_xy(lon, lat):
    n = 1 << 32
    x = int((lon + 180.0) / 360.0 * n)
    lat_r = math.radians(max(min(lat, 85.05112877), -85.05112877))
    y = int((1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def load(base, name_field):
    """Read a shapefile base name, or a .geojson/.json path."""
    if base.endswith(".geojson") or base.endswith(".json"):
        return load_geojson(base, name_field)
    feats = []
    for parts, attrs in shpread.read_shapefile(base):
        for coords in parts:
            if len(coords) < 2:
                continue
            feats.append({
                "coords": coords,
                "name": (attrs.get(name_field) or "").strip(),
                "attrs": attrs,
            })
    return feats


def load_geojson(path, name_field):
    with open(path) as f:
        doc = json.load(f)
    raw = doc["features"] if isinstance(doc, dict) else doc
    feats = []
    for feat in raw:
        g = feat.get("geometry")
        if not g:
            continue
        parts = ([g["coordinates"]] if g["type"] == "LineString"
                 else g["coordinates"] if g["type"] == "MultiLineString" else [])
        attrs = feat.get("properties") or {}
        for coords in parts:
            if len(coords) < 2:
                continue
            feats.append({
                "coords": [(c[0], c[1]) for c in coords],
                "name": str(attrs.get(name_field) or "").strip(),
                "attrs": attrs,
            })
    return feats


# ------------------------------------------------------------------ scoring

def score(features, local_zoom=12, verbose=True):
    t0 = time.time()
    lats = [c[1] for f in features for c in f["coords"][:1]]
    mx, my = T.local_scale(sum(lats) / len(lats))

    edges, node_ids = T.build_graph(features, mx, my)
    num_nodes = len(node_ids)
    if verbose:
        print("  noded: %d features -> %d edges, %d nodes  (%.1fs)"
              % (len(features), len(edges), num_nodes, time.time() - t0))

    stroke_of = T.build_strokes(edges, num_nodes, mx, my)
    stroke_len = defaultdict(float)
    for ei, e in enumerate(edges):
        stroke_len[stroke_of[ei]] += e["length"]
    if verbose:
        print("  strokes: %d strokes from %d edges (mean %.1f edges/stroke)  (%.1fs)"
              % (len(stroke_len), len(edges), len(edges) / max(1, len(stroke_len)),
                 time.time() - t0))

    bridges = T.find_bridges(edges, num_nodes)
    crit = T.criticality(edges, num_nodes, bridges)
    if verbose:
        print("  bridges: %d of %d edges (%.1f%%)  (%.1fs)"
              % (len(bridges), len(edges), 100.0 * len(bridges) / max(1, len(edges)),
                 time.time() - t0))

    # Roll edge-level measures up to whole features. Take the max: a feature
    # that carries any part of a trunk has to survive or the trunk breaks.
    feat_crit = defaultdict(float)
    feat_stroke = defaultdict(float)
    feat_len = defaultdict(float)
    for ei, e in enumerate(edges):
        for fi in e["features"]:
            feat_crit[fi] = max(feat_crit[fi], crit.get(ei, 0.0))
            feat_stroke[fi] = max(feat_stroke[fi], stroke_len[stroke_of[ei]])
            feat_len[fi] += e["length"]

    # Local neighborhood for percentile ranking.
    buckets = []
    for f in features:
        lon = sum(c[0] for c in f["coords"]) / len(f["coords"])
        lat = sum(c[1] for c in f["coords"]) / len(f["coords"])
        buckets.append(T.tile_of(lon, lat, local_zoom))

    n = len(features)
    crit_v = [feat_crit.get(i, 0.0) for i in range(n)]
    stroke_v = [feat_stroke.get(i, 0.0) for i in range(n)]

    pct_crit = T.local_percentile(crit_v, buckets)
    pct_stroke = T.local_percentile(stroke_v, buckets)
    combined = [max(pct_crit[i], pct_stroke[i]) for i in range(n)]
    final = T.local_percentile(combined, buckets)

    for i, f in enumerate(features):
        f["length"] = feat_len.get(i, 0.0)
        f["crit"] = crit_v[i]
        f["stroke_len"] = stroke_v[i]
        f["score"] = final[i]

    return edges, stroke_of


# --------------------------------------------------------------- evaluation

def densest_rank(features):
    """Reproduce tippecanoe's drop-densest ordering: gap in quadkey index order."""
    idx = []
    for i, f in enumerate(features):
        xs = [c[0] for c in f["coords"]]
        ys = [c[1] for c in f["coords"]]
        wx, wy = world_xy((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2)
        idx.append((encode_quadkey(wx, wy), i))
    idx.sort()
    gap = [0] * len(features)
    prev = 0
    for ix, i in idx:
        gap[i] = ix - prev
        prev = ix
    return gap


def evaluate(features, edges, budget_fraction, local_zoom, cover_zoom=14):
    """For each strategy, report continuity and spatial coverage of what survives.

    The budget is a fraction of total *vertices*, not of feature count, because
    that is what actually drives tile size. Strategies that keep a few long
    features and strategies that keep many short ones are then comparable.
    """
    n = len(features)
    verts = [len(f["coords"]) for f in features]
    total_verts = sum(verts)
    budget = total_verts * budget_fraction

    strategies = {
        "drop-smallest": [f["length"] for f in features],
        "drop-densest": densest_rank(features),
        "trunkiness": [f["score"] for f in features],
    }

    total_len = sum(f["length"] for f in features)

    # Every tile touched by any part of any feature, for the coverage measure.
    all_tiles = set()
    tiles_of_feat = []
    for f in features:
        ts = set(T.tile_of(c[0], c[1], cover_zoom) for c in f["coords"])
        tiles_of_feat.append(ts)
        all_tiles |= ts

    rows = []
    for label, vals in strategies.items():
        order = sorted(range(n), key=lambda i: vals[i], reverse=True)
        kept = set()
        spent = 0
        for i in order:
            if spent + verts[i] > budget:
                continue
            kept.add(i)
            spent += verts[i]

        kept_len = sum(features[i]["length"] for i in kept)

        # Connectivity of the surviving subnetwork.
        uf = T.UnionFind(max(max(e["u"], e["v"]) for e in edges) + 1 if edges else 1)
        comp_len = defaultdict(float)
        live = [e for e in edges if any(f in kept for f in e["features"])]
        for e in live:
            uf.union(e["u"], e["v"])
        for e in live:
            comp_len[uf.find(e["u"])] += e["length"]
        ncomp = len(comp_len)
        largest = max(comp_len.values()) / kept_len if comp_len and kept_len else 0.0

        covered = set()
        for i in kept:
            covered |= tiles_of_feat[i]
        coverage = len(covered) / len(all_tiles) if all_tiles else 0.0

        rows.append((label, kept_len / total_len, ncomp, largest, coverage))
    return rows


def write_geojson(features, path, limit_score=None):
    with open(path, "w") as f:
        f.write('{"type":"FeatureCollection","features":[\n')
        first = True
        for feat in features:
            if limit_score is not None and feat["score"] < limit_score:
                continue
            props = dict(feat["attrs"])
            props["trunk_score"] = round(feat["score"], 4)
            props["trunk_crit"] = round(feat["crit"], 6)
            props["stroke_len_m"] = round(feat["stroke_len"], 1)
            props["length_m"] = round(feat["length"], 1)
            geom = {"type": "LineString",
                    "coordinates": [[round(c[0], 6), round(c[1], 6)] for c in feat["coords"]]}
            if not first:
                f.write(",\n")
            first = False
            f.write(json.dumps({"type": "Feature", "properties": props, "geometry": geom}))
        f.write("\n]}\n")


def main():
    base = sys.argv[1]
    name_field = sys.argv[2] if len(sys.argv) > 2 else "FULLNAME"
    local_zoom = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    out = sys.argv[4] if len(sys.argv) > 4 else None

    print("== %s" % base)
    features = load(base, name_field)
    print("  loaded %d features" % len(features))

    edges, _ = score(features, local_zoom=local_zoom)

    print()
    print("  %-14s %8s %8s %9s %9s %9s" %
          ("strategy", "keep%", "len kept", "#comp", "largest", "tile cov"))
    for kf in (0.10, 0.25, 0.50):
        for label, lenfrac, ncomp, largest, cov in evaluate(
                features, edges, kf, local_zoom):
            print("  %-14s %8.0f%% %7.1f%% %9d %8.1f%% %8.1f%%"
                  % (label, kf * 100, lenfrac * 100, ncomp, largest * 100, cov * 100))
        print()

    if out:
        write_geojson(features, out)
        print("  wrote %s" % out)


if __name__ == "__main__":
    main()
