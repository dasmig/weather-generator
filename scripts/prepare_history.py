"""Build resources/history/history.tsv: each month of 1991-2020 as it was.

For every 2-degree block of the lite tier, the actual monthly mean
temperature, precipitation and wet-day count of each month from January 1991
to December 2020, as departures from the block's 1991-2020 normals: the
temperature anomaly in degrees C, the precipitation as a percentage of the
normal, and the wet-day anomaly in days. A request with `historical` set uses
them on top of the normals of its cell (a full-tier cell takes the anomalies
of the 2-degree block it lies in).

Source: CRU TS 4.10 (Harris, Osborn, Jones and Lister 2020, Scientific Data
7:109; Climatic Research Unit, University of East Anglia), the same monthly
0.5-degree grids of tmp, pre and wet that prepare_climate.py averages into the
normals, area-weighted into blocks the same way. Where CRU TS has no station
within reach it relaxes to its own climatology, so a month there is close to
the normal.

Usage: python scripts/prepare_history.py CRUTS_DIR CRUCL_DIR NE_DIR [--out resources]
(numpy is needed, so not -I; the inputs are only read as numbers.)
"""

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_climate as pc  # noqa: E402

FIRST_YEAR = 1991
YEARS = 30
DEGREES = 2.0
# (CRU TS variable, output column, scale to units)
VARIABLES = [("tmp", "tmean", 0.1), ("pre", "precipitation", 0.1), ("wet", "wet_days", 0.01)]


def monthly(cruts, var, scale):
    """(360 months, 360, 720) values for 1991-2020, NaN where missing."""
    out = []
    for decade in pc.DECADES:
        grid = pc.read_ts(cruts / f"cru_ts{pc.VERSION}.{decade}.{var}.dat.gz").astype(float)
        grid[grid == pc.MISSING] = np.nan
        out.append(grid * scale)
    series = np.concatenate(out)
    assert series.shape[0] == 12 * YEARS, series.shape
    return series


def fmt(x, decimals):
    text = f"{x:.{decimals}f}"
    if text.startswith("-") and float(text) == 0.0:
        text = text[1:]
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cruts", type=Path)
    ap.add_argument("crucl", type=Path)
    ap.add_argument("ne", type=Path)
    ap.add_argument("--out", type=Path, default=Path("resources"))
    args = ap.parse_args()

    cells, _ = pc.build_cells(args.cruts, args.crucl, args.ne)
    per = round(DEGREES / 0.5)
    blocks = {}
    for c in cells:
        blocks.setdefault((c["i"] // per, c["j"] // per), []).append(c)

    series = {col: monthly(args.cruts, var, scale) for var, col, scale in VARIABLES}
    path = args.out / "history" / "history.tsv"
    path.parent.mkdir(parents=True, exist_ok=True)
    biggest = {"tmean": 0.0, "precipitation": 0.0, "wet_days": 0.0}
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# CRU TS {pc.VERSION}: each month of {FIRST_YEAR}-{FIRST_YEAR + YEARS - 1} against the "
                "1991-2020 normals, per 2-degree block (scripts/prepare_history.py)\n")
        f.write("key\tfirst_year\ttmean\tprecipitation\twet_days\n")
        for (bi, bj), group in sorted(blocks.items()):
            w = np.array([c["area"] for c in group])
            ii = np.array([c["i"] for c in group])
            jj = np.array([c["j"] for c in group])
            out = {}
            for col in ("tmean", "precipitation", "wet_days"):
                v = series[col][:, ii, jj]  # (months, cells)
                ok = ~np.isnan(v)
                ww = np.where(ok, w[None, :], 0.0)
                actual = np.where(ww.sum(axis=1) > 0, (np.nan_to_num(v) * ww).sum(axis=1) / np.maximum(ww.sum(axis=1), 1e-300), np.nan)
                normal = np.array([np.nanmean(actual[m::12]) for m in range(12)])
                normal_of = np.tile(normal, YEARS)
                if col == "precipitation":
                    ratio = np.where(normal_of > 0.05, 100.0 * actual / np.maximum(normal_of, 1e-9), 100.0)
                    ratio = np.clip(np.nan_to_num(ratio, nan=100.0), 0.0, 2000.0)
                    out[col] = ";".join(fmt(x, 0) for x in ratio)
                    biggest[col] = max(biggest[col], float(ratio.max()))
                else:
                    anomaly = np.nan_to_num(actual - normal_of)
                    out[col] = ";".join(fmt(x, 1) for x in anomaly)
                    biggest[col] = max(biggest[col], float(np.abs(anomaly).max()))
            key = f"{(bi + 0.5) * DEGREES - 90.0:.2f},{(bj + 0.5) * DEGREES - 180.0:.2f}"
            f.write(f"{key}\t{FIRST_YEAR}\t{out['tmean']}\t{out['precipitation']}\t{out['wet_days']}\n")
    print(f"wrote {path} ({len(blocks)} blocks, {path.stat().st_size / 1e6:.1f} MB); largest |tmean| anomaly "
          f"{biggest['tmean']:.1f} C, precipitation {biggest['precipitation']:.0f}%, |wet_days| {biggest['wet_days']:.1f}")


if __name__ == "__main__":
    main()
