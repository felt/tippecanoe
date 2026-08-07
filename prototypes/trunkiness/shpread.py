"""Minimal pure-stdlib ESRI shapefile reader (polyline shapes + dBASE attributes).

Only supports what this prototype needs: shape types 3 (PolyLine) and 13
(PolyLineZ), plus the dBASE III field types TIGER uses (C, N, F, D, L).
"""

import struct


def read_shp(path):
    """Yield (parts, ) for each record, where parts is a list of coordinate rings.

    Each ring is a list of (x, y) tuples. Null shapes yield an empty list.
    """
    with open(path, "rb") as f:
        data = f.read()

    (filecode,) = struct.unpack_from(">i", data, 0)
    if filecode != 9994:
        raise ValueError("%s: not a shapefile (file code %d)" % (path, filecode))
    (file_length_words,) = struct.unpack_from(">i", data, 24)
    end = file_length_words * 2

    pos = 100
    while pos < end:
        _recno, content_words = struct.unpack_from(">ii", data, pos)
        pos += 8
        content_end = pos + content_words * 2

        (shape_type,) = struct.unpack_from("<i", data, pos)
        if shape_type == 0:  # null shape
            yield []
            pos = content_end
            continue
        if shape_type not in (3, 13, 23):
            raise ValueError("%s: unsupported shape type %d" % (path, shape_type))

        num_parts, num_points = struct.unpack_from("<ii", data, pos + 36)
        parts_off = pos + 44
        pts_off = parts_off + 4 * num_parts

        part_starts = struct.unpack_from("<%di" % num_parts, data, parts_off)
        coords = struct.unpack_from("<%dd" % (2 * num_points), data, pts_off)

        parts = []
        for i in range(num_parts):
            a = part_starts[i]
            b = part_starts[i + 1] if i + 1 < num_parts else num_points
            parts.append([(coords[2 * j], coords[2 * j + 1]) for j in range(a, b)])
        yield parts

        pos = content_end


def read_dbf(path):
    """Yield a dict of field values for each (non-deleted) record."""
    with open(path, "rb") as f:
        data = f.read()

    num_records, header_len, record_len = struct.unpack_from("<IHH", data, 4)

    fields = []
    off = 32
    while data[off] != 0x0D:
        raw = data[off:off + 32]
        name = raw[0:11].split(b"\0")[0].decode("latin-1")
        ftype = chr(raw[11])
        flen = raw[16]
        fields.append((name, ftype, flen))
        off += 32

    pos = header_len
    for _ in range(num_records):
        rec = data[pos:pos + record_len]
        pos += record_len
        if not rec or rec[0:1] == b"*":  # deleted
            continue
        out = {}
        o = 1
        for name, ftype, flen in fields:
            raw = rec[o:o + flen]
            o += flen
            val = raw.decode("latin-1").strip()
            if ftype in ("N", "F"):
                try:
                    val = float(val) if ("." in val or ftype == "F") else int(val)
                except ValueError:
                    val = None
            elif ftype == "L":
                val = val.upper() in ("Y", "T")
            out[name] = val
        yield out


def read_shapefile(base):
    """Read base.shp + base.dbf together, yielding (parts, attributes) pairs."""
    for parts, attrs in zip(read_shp(base + ".shp"), read_dbf(base + ".dbf")):
        yield parts, attrs
