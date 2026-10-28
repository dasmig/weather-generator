"""Build resources/<tier>/climate.tsv and land.tsv: 1991-2020 normals.

Sources:

- CRU TS 4.10 (Harris, Osborn, Jones and Lister 2020, "Version 4 of the CRU
  TS monthly high-resolution gridded multivariate climate dataset",
  Scientific Data 7:109; Climatic Research Unit, University of East Anglia),
  https://crudata.uea.ac.uk/cru/data/hrg/cru_ts_4.10/: the monthly 0.5-degree
  grids of tmp, dtr, pre, wet, cld and vap for 1991-2000, 2001-2010 and
  2011-2020 (cru_ts4.10.<decade>.<var>.dat.gz, ASCII), and the station
  counts of tmp, pre and wet (.stn.gz) for the summary.
- CRU CL 2.0 (New, Lister, Hulme and Makin 2002, Climate Research 21:1-25),
  https://crudata.uea.ac.uk/cru/data/hrg/tmc/: the 10-minute grids of
  elevation (elv) and wind (wnd, 1961-1990; CRU TS has no wind), and of
  relative humidity (reh), used only to check the humidity derived here.
- Natural Earth 1:10m land and minor islands (public domain),
  https://www.naturalearthdata.com/: which cells hold land. CRU TS's grid
  also has cells at sea (around ocean weather stations); a cell is kept only
  if it holds Natural Earth land.

A 0.5-degree cell's normals are the 1991-2020 means of each calendar month.
Relative humidity is the month's vapour pressure over the saturation vapour
pressure (Magnus, over water) averaged over a sinusoidal day between Tmin and
Tmax. Elevation and wind are the means of the cell's 10' CRU CL 2.0 land
cells (the nearest cell's where it has none), and the cell's point and area
weight come from them too: the land centroid and the sum of cos(latitude) of
the 10' land cells (one 10' cell's worth where there are none).

`full` is the 0.5-degree grid; `lite` aggregates it into 2-degree blocks,
area-weighted. Both tiers get the same land.tsv: the land cells of the
0.5-degree grid as runs of columns per row, for the 100 km sea rule.

Usage: python scripts/prepare_climate.py CRUTS_DIR CRUCL_DIR NE_DIR [--tier lite|full|all]
         [--out resources]
(numpy is needed, so not -I; the inputs are only read as numbers.)
"""

import argparse
import gzip
import math
from pathlib import Path

import numpy as np

TIERS = {"lite": 2.0, "full": 0.5}
DECADES = ["1991.2000", "2001.2010", "2011.2020"]
VERSION = "4.10"
ROWS, COLS = 360, 720
CL_STEP = 1.0 / 6.0
MISSING = -999

# (CRU TS variable, output column, scale to units, decimals)
TS = [
    ("tmp", "tmean", 0.1, 1),
    ("dtr", "dtr", 0.1, 1),
    ("pre", "precipitation", 0.1, 1),
    ("wet", "wet_days", 0.01, 1),
    ("cld", "cloud", 0.1, 0),
    ("vap", None, 0.1, 1),
]
COLUMNS = [("tmean", 1), ("dtr", 1), ("precipitation", 1), ("wet_days", 1), ("humidity", 0), ("cloud", 0), ("wind", 1)]


def read_ts(path):
    """A decadal ASCII grid as an int32 array (months, 360, 720): 120 blocks
    of 360 rows (south to north) of 720 values (west to east) in 8-character
    fields."""
    raw = gzip.open(path, "rb").read()
    lines = np.frombuffer(raw, dtype=np.uint8)
    width = COLS * 8 + 1
    if lines.size % width:
        raise SystemExit(f"{path}: lines are not {COLS} fields of 8 characters")
    fields = lines.reshape(-1, width)[:, : COLS * 8].reshape(-1, COLS, 8)
    digits = (fields >= ord("0")) & (fields <= ord("9"))
    value = np.zeros(fields.shape[:2], dtype=np.int64)
    for k in range(8):
        d = digits[:, :, k]
        value = np.where(d, value * 10 + (fields[:, :, k].astype(np.int64) - ord("0")), value)
    value = np.where((fields == ord("-")).any(axis=2), -value, value)
    return value.reshape(-1, ROWS, COLS).astype(np.int32)


def normals(cruts, var, scale):
    """(12, 360, 720) 1991-2020 monthly means, NaN where any month is missing."""
    total = np.zeros((12, ROWS, COLS))
    count = np.zeros((12, ROWS, COLS))
    for decade in DECADES:
        grid = read_ts(cruts / f"cru_ts{VERSION}.{decade}.{var}.dat.gz")
        for k in range(grid.shape[0]):
            v = grid[k]
            ok = v != MISSING
            total[k % 12][ok] += v[ok]
            count[k % 12][ok] += 1
        print(f"  {var} {decade}: {grid.shape[0]} months")
    with np.errstate(invalid="ignore"):
        out = total / count * scale
    out[count < 25] = np.nan  # fewer than 25 of the 30 years
    return out


def station_share(cruts, var):
    """Share of land cell-months with at least one station within reach."""
    have = total = 0
    for decade in DECADES:
        grid = read_ts(cruts / f"cru_ts{VERSION}.{decade}.{var}.stn.gz")
        land = grid >= 0
        have += int((grid > 0).sum())
        total += int(land.sum())
    return have / total


def read_cl(path, values):
    """{(row, col) on the 0.5-degree grid: [(lat, lon, [values])...]} of the 10' cells."""
    out = {}
    with gzip.open(path, "rt", encoding="ascii") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2 + values:
                continue
            lat, lon = float(parts[0]), float(parts[1])
            key = (min(ROWS - 1, int((lat + 90.0) * 2)), min(COLS - 1, int((lon + 180.0) * 2)))
            out.setdefault(key, []).append((lat, lon, [float(x) for x in parts[2 : 2 + values]]))
    return out


def read_shapes(path):
    """The polygons of an ESRI shapefile: a list of records, each a list of
    rings, each an (n, 2) array of lon, lat."""
    import struct

    data = Path(path).read_bytes()
    out, pos = [], 100
    while pos < len(data):
        _, length = struct.unpack(">ii", data[pos:pos + 8])
        rec = data[pos + 8:pos + 8 + 2 * length]
        pos += 8 + 2 * length
        if struct.unpack("<i", rec[:4])[0] != 5:  # polygons only
            continue
        nparts, npoints = struct.unpack("<ii", rec[36:44])
        parts = list(struct.unpack(f"<{nparts}i", rec[44:44 + 4 * nparts])) + [npoints]
        pts = np.frombuffer(rec, dtype="<f8", count=2 * npoints, offset=44 + 4 * nparts).reshape(-1, 2)
        out.append([pts[parts[k]:parts[k + 1]] for k in range(nparts)])
    return out


def natural_earth_land(ne_dir):
    """(360, 720) bool: 0.5-degree cells holding Natural Earth 1:10m land or
    a minor island, from an even-odd scanline fill of the polygons on a
    0.1-degree grid of sample points, plus every cell holding a vertex (for
    islands smaller than the sample spacing)."""
    step = 0.1
    ys = -90.0 + step * (np.arange(1800) + 0.5)
    xs = -180.0 + step * (np.arange(3600) + 0.5)
    fine = np.zeros((1800, 3600), dtype=bool)
    cells = np.zeros((ROWS, COLS), dtype=bool)
    for name in ("ne_10m_land.shp", "ne_10m_minor_islands.shp"):
        for rings in read_shapes(ne_dir / name):
            edges = np.concatenate([np.stack([r[:-1], r[1:]], axis=1) for r in rings if len(r) > 1])
            x0, y0, x1, y1 = edges[:, 0, 0], edges[:, 0, 1], edges[:, 1, 0], edges[:, 1, 1]
            for r in rings:
                i = np.clip(((r[:, 1] + 90.0) * 2).astype(int), 0, ROWS - 1)
                j = np.clip(((r[:, 0] + 180.0) * 2).astype(int), 0, COLS - 1)
                cells[i, j] = True
            lo, hi = np.searchsorted(ys, min(y0.min(), y1.min())), np.searchsorted(ys, max(y0.max(), y1.max()))
            for k in range(lo, hi):
                y = ys[k]
                cross = (y0 <= y) != (y1 <= y)
                if not cross.any():
                    continue
                xa, ya, xb, yb = x0[cross], y0[cross], x1[cross], y1[cross]
                xi = np.sort(xa + (y - ya) * (xb - xa) / (yb - ya))
                for a, b in zip(xi[0::2], xi[1::2]):
                    fine[k, np.searchsorted(xs, a):np.searchsorted(xs, b)] = True
    cells |= fine.reshape(ROWS, 5, COLS, 5).any(axis=(1, 3))
    return cells


def saturation(t):
    """Saturation vapour pressure over water, hPa (Magnus, WMO 2008)."""
    return 6.112 * np.exp(17.62 * t / (243.12 + t))


def humidity(tmean, dtr, vap):
    phase = np.sin(np.linspace(0.0, 2.0 * np.pi, 24, endpoint=False))
    t = tmean[..., None] + 0.5 * dtr[..., None] * phase
    rh = 100.0 * np.mean(vap[..., None] / saturation(t), axis=-1)
    return np.clip(rh, 1.0, 100.0)


def nearest_fill(cells, key):
    """Give cells without `key` the value of the nearest cell that has it."""
    donors = [c for c in cells if c[key] is not None]
    pts = np.array([[c["lat"], c["lon"]] for c in donors])
    for c in cells:
        if c[key] is None:
            d = (pts[:, 0] - c["lat"]) ** 2 + ((pts[:, 1] - c["lon"]) * math.cos(math.radians(c["lat"]))) ** 2
            c[key] = donors[int(np.argmin(d))][key]


def build_cells(cruts, crucl, ne_dir):
    grids = {}
    for var, col, scale, _ in TS:
        grids[var] = normals(cruts, var, scale)
    land = ~np.isnan(grids["tmp"]).any(axis=0)
    for var in ("dtr", "pre", "wet"):
        land &= ~np.isnan(grids[var]).any(axis=0)
    ne = natural_earth_land(ne_dir)
    print(f"  {int(land.sum())} CRU TS cells, {int((land & ~ne).sum())} of them without Natural Earth land (dropped), "
          f"{int((ne & ~land).sum())} Natural Earth land cells without CRU TS data")
    land &= ne
    rh = humidity(grids["tmp"], grids["dtr"], grids["vap"])

    elv = read_cl(crucl / "grid_10min_elv.dat.gz", 1)
    wnd = read_cl(crucl / "grid_10min_wnd.dat.gz", 12)
    reh = read_cl(crucl / "grid_10min_reh.dat.gz", 12)

    cells = []
    for i, j in zip(*np.nonzero(land)):
        lat_c, lon_c = -89.75 + 0.5 * i, -179.75 + 0.5 * j
        subs = elv.get((i, j), [])
        if subs:
            w = np.array([math.cos(math.radians(a)) for a, _, _ in subs])
            lat = float(np.dot(w, [a for a, _, _ in subs]) / w.sum())
            lon = float(np.dot(w, [b for _, b, _ in subs]) / w.sum())
            area = float(w.sum())
            elevation = 1000.0 * float(np.mean([v[0] for _, _, v in subs]))
        else:
            lat, lon, area, elevation = lat_c, lon_c, math.cos(math.radians(lat_c)), None
        ws = wnd.get((i, j))
        cl_rh = reh.get((i, j))
        cloud = grids["cld"][:, i, j]
        cells.append({
            "i": int(i), "j": int(j), "lat": lat, "lon": lon, "area": area, "elevation": elevation,
            "tmean": grids["tmp"][:, i, j], "dtr": grids["dtr"][:, i, j],
            "precipitation": grids["pre"][:, i, j],
            "wet_days": np.minimum(grids["wet"][:, i, j], [31, 28.25, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]),
            "humidity": rh[:, i, j] if not np.isnan(rh[:, i, j]).any() else None,
            "cloud": cloud if not np.isnan(cloud).any() else None,
            "wind": np.mean([v for _, _, v in ws], axis=0) if ws else None,
            "cl_rh": np.mean([v for _, _, v in cl_rh], axis=0) if cl_rh else None,
        })
    for key in ("elevation", "humidity", "cloud", "wind"):
        nearest_fill(cells, key)

    both = [c for c in cells if c["cl_rh"] is not None]
    diff = np.array([c["humidity"] - c["cl_rh"] for c in both]).ravel()
    print(f"  humidity from vapour pressure minus CRU CL 2.0 reh: median {np.median(diff):+.1f}, "
          f"median |diff| {np.median(np.abs(diff)):.1f} points over {len(both)} cells")
    for var in ("tmp", "pre", "wet"):
        print(f"  {var}: {station_share(cruts, var):.0%} of land cell-months with stations")
    return cells, land


def aggregate(cells, degrees):
    if degrees == 0.5:
        blocks = {(c["i"], c["j"]): [c] for c in cells}
        per = 1
    else:
        per = round(degrees / 0.5)
        blocks = {}
        for c in cells:
            blocks.setdefault((c["i"] // per, c["j"] // per), []).append(c)
    rows = []
    for (bi, bj), group in sorted(blocks.items()):
        w = np.array([c["area"] for c in group])
        mean = lambda key: np.dot(w, np.array([c[key] for c in group], dtype=float)) / w.sum()
        rows.append({
            "key": f"{(bi + 0.5) * degrees - 90.0:.2f},{(bj + 0.5) * degrees - 180.0:.2f}",
            "latitude": mean("lat"),
            "longitude": mean("lon"),
            "elevation": mean("elevation"),
            "area": w.sum(),
            **{col: mean(col) for col, _ in COLUMNS},
        })
    return rows


def fmt(x, decimals):
    text = f"{x:.{decimals}f}"
    if text.startswith("-") and float(text) == 0.0:
        text = text[1:]
    return text


def write(rows, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    header = ["key", "latitude", "longitude", "elevation", "area"] + [c for c, _ in COLUMNS]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(header) + "\n")
        for r in rows:
            out = [r["key"], fmt(r["latitude"], 3), fmt(r["longitude"], 3), fmt(r["elevation"], 0), fmt(r["area"], 3)]
            out += [";".join(fmt(x, dec) for x in r[col]) for col, dec in COLUMNS]
            f.write("\t".join(out) + "\n")
    print(f"wrote {path} ({len(rows)} cells, {path.stat().st_size / 1e6:.1f} MB)")


def write_land(land, path):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("row\tcolumns\n")
        for i in range(ROWS):
            cols = np.nonzero(land[i])[0]
            if cols.size == 0:
                continue
            runs, start = [], cols[0]
            for a, b in zip(cols, np.append(cols[1:], -10)):
                if b != a + 1:
                    runs.append(f"{start}-{a}" if a != start else f"{a}")
                    start = b
            f.write(f"{i}\t{';'.join(runs)}\n")
    print(f"wrote {path} ({int(land.sum())} land cells, {path.stat().st_size / 1e3:.0f} KB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cruts", type=Path)
    ap.add_argument("crucl", type=Path)
    ap.add_argument("ne", type=Path, help="Natural Earth ne_10m_land and ne_10m_minor_islands shapefiles")
    ap.add_argument("--tier", default="all", choices=["lite", "full", "all"])
    ap.add_argument("--out", type=Path, default=Path("resources"))
    args = ap.parse_args()
    cells, land = build_cells(args.cruts, args.crucl, args.ne)
    for tier in (list(TIERS) if args.tier == "all" else [args.tier]):
        print(f"{tier}: {TIERS[tier]} degree blocks")
        write(aggregate(cells, TIERS[tier]), args.out / tier / "climate.tsv")
        write_land(land, args.out / tier / "land.tsv")


if __name__ == "__main__":
    main()
