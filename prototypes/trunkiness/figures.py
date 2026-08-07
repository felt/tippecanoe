"""Side-by-side figures of what survives a per-tile budget under each strategy.

Features are clipped to the tiles where they actually survive, so a stroke kept
in one tile and dropped in the next shows as a break rather than being drawn
whole. That break is the artifact the whole exercise is about.
"""

import gaps
import render
import run as R
import trunkiness as T


def surviving_chains(features, kept_in, z):
    """Split each feature into the runs of it that land in tiles that kept it."""
    chains = []
    for i, f in enumerate(features):
        coords = f["coords"]
        run = []
        for j in range(len(coords) - 1):
            a, b = coords[j], coords[j + 1]
            mid = ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0)
            if i in kept_in.get(T.tile_of(mid[0], mid[1], z), ()):
                if not run:
                    run.append(a)
                run.append(b)
            elif run:
                chains.append(run)
                run = []
        if run:
            chains.append(run)
    return chains


def main():
    jobs = [
        ("data/nhd_02070004/Shape/NHDFlowline", "gnis_name", 11, 1500, "nhd.svg",
         "NHD HU8 02070004 (Conococheague-Opequon), z11, 1500 vertices/tile"),
        ("data/b5571d37-tl_2025_06001_roads/tl_2025_06001_roads", "FULLNAME", 12,
         1500, "roads.svg",
         "TIGER 2025 Alameda County roads, z12, 1500 vertices/tile"),
    ]

    for base, namef, z, budget, out, title in jobs:
        features = R.load(base, namef)
        lats = [f["coords"][0][1] for f in features]
        mx, my = T.local_scale(sum(lats) / len(lats))
        edges, node_ids = T.build_graph(features, mx, my)
        stroke_of = T.build_strokes(edges, len(node_ids), mx, my)

        flen = [0.0] * len(features)
        for e in edges:
            for fi in e["features"]:
                flen[fi] += e["length"]

        per_edge = gaps.score_strokes(features, edges, stroke_of, len(node_ids),
                                      None, per_stroke=False)
        per_stroke = gaps.score_strokes(features, edges, stroke_of, len(node_ids),
                                        None, per_stroke=True)
        local = gaps.score_strokes(features, edges, stroke_of, len(node_ids),
                                   z + 4, per_stroke=True)

        variants = [
            ("drop-smallest (today)", flen, False),
            ("trunkiness, per-tile threshold", per_stroke, False),
            ("per-stroke + local + shared threshold", local, True),
        ]

        panels = []
        for label, vals, shared in variants:
            kept_in = gaps.select(features, vals, z, budget, shared)
            panels.append((label, surviving_chains(features, kept_in, z)))

        render.render_chains(features, panels, out, title=title)
        print("wrote %s (%d features)" % (out, len(features)))


if __name__ == "__main__":
    main()
