"""Build resources/<tier>/climate.tsv from the CRU CL 2.0 10-minute grids.

Source: New, Lister, Hulme and Makin (2002), "A high-resolution data set of
surface climate over global land areas", Climate Research 21:1-25; the
Climatic Research Unit's 10-minute climatology of 1961-1990 means,
https://crudata.uea.ac.uk/cru/data/hrg/tmc/. Download the files
grid_10min_{tmp,dtr,pre,rd0,reh,sunp,wnd,elv}.dat.gz into one directory.

Each tier aggregates the 10' land cells into square blocks (lite 2 degrees,
full 0.5 degrees). A block's row holds the mean of its land cells:
the land centroid (so a coastal block's point lies on its land), the mean
elevation, an area weight (the sum of cos(latitude) over its land cells,
for drawing a random place by land area) and twelve monthly values of each
element. Elements a cell lacks (wind, humidity and sunshine have their own
land masks) are averaged over the cells that have them; a block with none
takes the nearest block's values that has them.

Usage: python -I scripts/prepare_climate.py CRU_DIR [--tier lite|full|all]
         [--out resources]
"""

import argparse
import gzip
import math
from pathlib import Path

TIERS = {"lite": 2.0, "full": 0.5}

# (file stem, output column, decimals). Monthly columns, in output order.
ELEMENTS = [
    ("tmp", "tmean", 1),
    ("dtr", "dtr", 1),
    ("pre", "precipitation", 1),
    ("rd0", "wet_days", 1),
    ("reh", "humidity", 0),
    ("sunp", "sunshine", 0),
    ("wnd", "wind", 1),
]
REQUIRED = {"tmp", "dtr", "pre", "rd0"}

STEP = 1.0 / 6.0  # the 10' grid


def cell_index(lat, lon):
    """Row and column of a 10' cell centre on the global grid."""
    return (round((lat + 90.0) / STEP - 0.5), round((lon + 180.0) / STEP - 0.5))


def read_grid(path, values):
    """{(row, col): [floats]} with the first `values` numbers after lat, lon."""
    grid = {}
    with gzip.open(path, "rt", encoding="ascii") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2 + values:
                continue
            lat, lon = float(parts[0]), float(parts[1])
            grid[cell_index(lat, lon)] = [float(x) for x in parts[2 : 2 + values]]
    return grid


def fmt(x, decimals):
    text = f"{x:.{decimals}f}"
    if text in ("-0", "-0.0"):
        text = text[1:]
    return text


def build(cru, degrees):
    grids = {}
    for stem, _, _ in ELEMENTS:
        grids[stem] = read_grid(cru / f"grid_10min_{stem}.dat.gz", 12)
        print(f"  {stem}: {len(grids[stem])} cells")
    elv = read_grid(cru / "grid_10min_elv.dat.gz", 1)

    land = set(grids["tmp"])
    for stem in REQUIRED:
        land &= set(grids[stem])

    per = round(degrees / STEP)
    blocks = {}
    for (i, j) in land:
        blocks.setdefault((i // per, j // per), []).append((i, j))

    rows = []
    for (bi, bj), cells in blocks.items():
        lats = [(i + 0.5) * STEP - 90.0 for i, _ in cells]
        lons = [(j + 0.5) * STEP - 180.0 for _, j in cells]
        w = [math.cos(math.radians(la)) for la in lats]
        wsum = sum(w)
        row = {
            "block": (bi, bj),
            "key": f"{(bi + 0.5) * degrees - 90.0:.2f},{(bj + 0.5) * degrees - 180.0:.2f}",
            "latitude": sum(a * b for a, b in zip(lats, w)) / wsum,
            "longitude": sum(a * b for a, b in zip(lons, w)) / wsum,
            "area": wsum,
        }
        el = [elv[c][0] for c in cells if c in elv]
        row["elevation"] = 1000.0 * sum(el) / len(el) if el else 0.0
        for stem, col, _ in ELEMENTS:
            have = [grids[stem][c] for c in cells if c in grids[stem]]
            row[col] = [sum(v[m] for v in have) / len(have) for m in range(12)] if have else None
        rows.append(row)

    # Fill an element a block lacks from the nearest block that has it.
    for _, col, _ in ELEMENTS:
        donors = [r for r in rows if r[col] is not None]
        for r in rows:
            if r[col] is None:
                bi, bj = r["block"]
                best = min(donors, key=lambda d: (d["block"][0] - bi) ** 2 + (d["block"][1] - bj) ** 2)
                r[col] = best[col]

    rows.sort(key=lambda r: r["block"])
    return rows


def write(rows, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    header = ["key", "latitude", "longitude", "elevation", "area"] + [c for _, c, _ in ELEMENTS]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\t".join(header) + "\n")
        for r in rows:
            cells = [
                r["key"],
                fmt(r["latitude"], 3),
                fmt(r["longitude"], 3),
                fmt(r["elevation"], 0),
                fmt(r["area"], 3),
            ]
            for _, col, dec in ELEMENTS:
                cells.append(";".join(fmt(x, dec) for x in r[col]))
            f.write("\t".join(cells) + "\n")
    print(f"wrote {path} ({len(rows)} cells, {path.stat().st_size / 1e6:.1f} MB)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cru", type=Path)
    ap.add_argument("--tier", default="all", choices=["lite", "full", "all"])
    ap.add_argument("--degrees", type=float, help="override the tier's block size (to measure)")
    ap.add_argument("--out", type=Path, default=Path("resources"))
    args = ap.parse_args()
    tiers = list(TIERS) if args.tier == "all" else [args.tier]
    for tier in tiers:
        degrees = args.degrees or TIERS[tier]
        print(f"{tier}: {degrees} degree blocks")
        write(build(args.cru, degrees), args.out / tier / "climate.tsv")


if __name__ == "__main__":
    main()
