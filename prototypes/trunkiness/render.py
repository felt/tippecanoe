"""Render side-by-side SVG panels of what each drop strategy keeps."""

import math

import trunkiness as T


def merc(lon, lat):
    x = (lon + 180.0) / 360.0
    lat_r = math.radians(max(min(lat, 85.05112877), -85.05112877))
    y = (1.0 - math.asinh(math.tan(lat_r)) / math.pi) / 2.0
    return x, y


def render_chains(features, panels, path, width=560, height=560, pad=14, title=""):
    """panels: list of (label, list of coordinate chains that survive).

    Chains rather than feature indices, because a feature can be kept in one
    tile and dropped in the next; drawing whole features would hide exactly the
    cross-tile gaps these figures are meant to show.
    """
    pts = [merc(c[0], c[1]) for f in features for c in f["coords"]]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    spanx, spany = maxx - minx or 1e-9, maxy - miny or 1e-9
    scale = min((width - 2 * pad) / spanx, (height - 2 * pad) / spany)

    def path_of(coords):
        pp = []
        for c in coords:
            x, y = merc(c[0], c[1])
            p = ((x - minx) * scale + pad, (y - miny) * scale + pad)
            if not pp or abs(p[0] - pp[-1][0]) + abs(p[1] - pp[-1][1]) > 1.0:
                pp.append(p)
        if len(pp) < 2:
            if len(coords) < 2:
                return ""
            a, b = coords[0], coords[-1]
            pp = [((merc(a[0], a[1])[0] - minx) * scale + pad,
                   (merc(a[0], a[1])[1] - miny) * scale + pad),
                  ((merc(b[0], b[1])[0] - minx) * scale + pad,
                   (merc(b[0], b[1])[1] - miny) * scale + pad)]
        return "M" + "L".join("%.1f %.1f" % p for p in pp)

    context = "".join(path_of(f["coords"]) for f in features)

    total_w = width * len(panels)
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'viewBox="0 0 %d %d">' % (total_w, height + 34, total_w, height + 34),
           '<rect width="%d" height="%d" fill="#ffffff"/>' % (total_w, height + 34)]
    if title:
        out.append('<text x="8" y="16" font-family="sans-serif" font-size="13" '
                   'fill="#111">%s</text>' % title)

    for pi, (label, chains) in enumerate(panels):
        out.append('<g transform="translate(%d,26)">' % (pi * width))
        out.append('<path fill="none" stroke="#e2e2e2" stroke-width="0.5" d="%s"/>'
                   % context)
        out.append('<path fill="none" stroke="#12507e" stroke-width="1.15" '
                   'stroke-linecap="round" d="%s"/>'
                   % "".join(path_of(c) for c in chains))
        out.append('<text x="6" y="%d" font-family="sans-serif" font-size="12" '
                   'fill="#333">%s</text>' % (height - 4, label))
        out.append('</g>')

    out.append('</svg>')
    with open(path, "w") as f:
        f.write("\n".join(out))
    return path


def render(features, panels, path, width=560, height=560, pad=14, title=""):
    """panels: list of (label, kept set of feature indices)."""
    pts = [merc(c[0], c[1]) for f in features for c in f["coords"]]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
    spanx, spany = maxx - minx or 1e-9, maxy - miny or 1e-9
    scale = min((width - 2 * pad) / spanx, (height - 2 * pad) / spany)

    def project(c):
        x, y = merc(c[0], c[1])
        return ((x - minx) * scale + pad, (y - miny) * scale + pad)

    # Thin to display resolution: at 560px a county's worth of vertices is far
    # more than the raster can show, and the full set makes the SVG unopenable.
    paths = []
    for f in features:
        pp = []
        for c in f["coords"]:
            p = project(c)
            if not pp or abs(p[0] - pp[-1][0]) + abs(p[1] - pp[-1][1]) > 1.0:
                pp.append(p)
        if len(pp) < 2:
            pp = [project(f["coords"][0]), project(f["coords"][-1])]
        paths.append("M" + "L".join("%.1f %.1f" % p for p in pp))

    total_w = width * len(panels)
    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d">' % (total_w, height + 34, total_w, height + 34))
    out.append('<rect width="%d" height="%d" fill="#ffffff"/>' % (total_w, height + 34))
    if title:
        out.append('<text x="8" y="16" font-family="sans-serif" font-size="13" '
                   'fill="#111">%s</text>' % title)

    for pi, (label, kept) in enumerate(panels):
        ox = pi * width
        out.append('<g transform="translate(%d,26)">' % ox)
        # One path element per layer rather than per feature: at this feature
        # count the per-element markup dominates the file size.
        out.append('<path fill="none" stroke="#e2e2e2" stroke-width="0.5" d="%s"/>'
                   % "".join(paths))
        out.append('<path fill="none" stroke="#12507e" stroke-width="1.15" '
                   'stroke-linecap="round" d="%s"/>'
                   % "".join(paths[i] for i in sorted(kept)))
        out.append('<text x="6" y="%d" font-family="sans-serif" font-size="12" '
                   'fill="#333">%s</text>' % (height - 4, label))
        out.append('</g>')

    out.append('</svg>')
    with open(path, "w") as f:
        f.write("\n".join(out))
    return path
