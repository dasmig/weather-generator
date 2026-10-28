"""Compare generated weather with GHCN-Daily observations, 1991-2020.

For each station of a list (scripts/ghcn_holdout.txt, which the fit never
sees, or the calibration stations, scripts/ghcn_stations.txt) and month, the
statistics the model is meant to reproduce are computed on the station's
days and on 30 generated years at the same point and elevation, and the
differences are summarized over all station-months. Where the climate cell
holds the same statistic (mean Tmax and Tmin, wet days, precipitation),
the generated value is also compared with the cell, which separates the
model's error from the error of the normals at the station.

Usage:
  python scripts/validate.py stations GHCN_DIR OUT.tsv [--list FILE]   # the sampler's input
  build/sample_weather full OUT.tsv SAMPLE.tsv                         # scripts/sample_weather.cpp
  python scripts/validate.py compare GHCN_DIR CLIMATE SAMPLE.tsv [--list FILE]

--list defaults to the calibration stations; CLIMATE is the full tier's
climate.tsv.
"""

import argparse
import calendar
import math
from collections import defaultdict
from pathlib import Path

import numpy as np

import calibrate as cal


LAPSE = 0.0065  # degrees C per metre, as the library's


def station_elevations(ghcn):
    """{station id: elevation in m, -999.9 where unknown}."""
    elev = {}
    with open(ghcn / "ghcnd-stations.txt", encoding="ascii", errors="replace") as f:
        for line in f:
            elev[line[0:11]] = float(line[31:37])
    return elev


def cell_elevations(climate):
    """{(row, col) on the 0.5-degree grid: the cell's mean elevation, m}."""
    out = {}
    with open(climate, encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        key, elevation = header.index("key"), header.index("elevation")
        for line in f:
            p = line.rstrip("\n").split("\t")
            lat, lon = (float(x) for x in p[key].split(","))
            out[(int((lat + 90.0) * 2), int((lon + 180.0) * 2))] = float(p[elevation])
    return out


def stations_cmd(ghcn, out, path):
    elev = station_elevations(ghcn)
    with open(out, "w", encoding="ascii", newline="\n") as f:
        for sid, lat, lon in cal.stations(path):
            e = elev.get(sid, -999.9)
            f.write(f"{sid}\t{lat}\t{lon}\t{e if e > -999 else -999}\n")
    print(f"wrote {out}")


def daily_obs(ghcn, sid):
    path = ghcn / "by_station" / f"{sid}.csv.gz"
    if not path.exists():
        return None
    data = cal.read_station(path)
    start = cal.ordinal(cal.FIRST * 10000 + 101)
    n = cal.ordinal(cal.LAST * 10000 + 1231) - start + 1
    out = {}
    for el in ("TMAX", "TMIN", "PRCP", "SNOW", "WT03"):
        a = np.full(n, np.nan)
        for date, v in data.get(el, {}).items():
            if cal.FIRST <= date // 10000 <= cal.LAST:
                a[cal.ordinal(date) - start] = v / 10.0 if el != "WT03" else 1.0
        out[el] = a
    out["snow"] = out["SNOW"] > 0
    out["has_snow"] = ~np.isnan(out["SNOW"])
    out["thunder"] = out["WT03"] == 1.0
    out["has_thunder"] = sid.startswith("US") and np.nansum(out["WT03"]) >= 100
    return out


def daily_gen(rows, n):
    out = {k: np.full(n, np.nan) for k in ("TMAX", "TMIN", "PRCP")}
    snow = np.zeros(n, bool)
    thunder = np.zeros(n, bool)
    start = cal.ordinal(cal.FIRST * 10000 + 101)
    for date, tx, tn, pr, typ, th in rows:
        i = cal.ordinal(date) - start
        out["TMAX"][i], out["TMIN"][i], out["PRCP"][i] = tx, tn, pr
        snow[i] = typ in ("snow", "sleet")
        thunder[i] = th
    out["snow"], out["has_snow"], out["thunder"], out["has_thunder"] = snow, np.ones(n, bool), thunder, True
    return out


def stats(d, n):
    """{month: {metric: value}} for one station's daily arrays."""
    start = cal.ordinal(cal.FIRST * 10000 + 101)
    days = (np.arange(n) + start).astype("datetime64[D]")
    month = days.astype("datetime64[M]").astype(int) % 12
    year = days.astype("datetime64[Y]").astype(int) + 1970
    doy = (days - days.astype("datetime64[Y]")).astype(int).astype(float)
    tx, tn, pr = d["TMAX"], d["TMIN"], d["PRCP"]
    good = ~np.isnan(tx) & ~np.isnan(tn) & ~np.isnan(pr)
    if good.sum() < 15 * 365:
        return None
    wet = pr >= cal.WET / 10.0
    h = cal.harmonics(doy[good])
    res = {}
    for name, t in (("x", tx), ("n", tn), ("m", (tx + tn) / 2), ("r", tx - tn)):
        coef, *_ = np.linalg.lstsq(h, t[good], rcond=None)
        r = np.full(n, np.nan)
        r[good] = t[good] - h @ coef
        res[name] = r
    out = {}
    for m in range(12):
        sel = good & (month == m)
        if sel.sum() < 300:
            continue
        w, dry = sel & wet, sel & ~wet
        s = {
            "tmax": np.mean(tx[sel]),
            "tmin": np.mean(tn[sel]),
            "wet": w.sum() / sel.sum(),
            "total": np.mean(pr[sel]) * calendar.monthrange(2001, m + 1)[1],
        }
        if w.sum() >= 30:
            s["amount"] = np.mean(pr[w])
            s["p95"] = np.percentile(pr[w], 95)
        prev = np.roll(sel, 1) & sel
        prev[0] = False
        pw = np.roll(wet, 1)
        if (prev & ~pw).sum() > 30 and (prev & pw).sum() > 30:
            s["persistence"] = (prev & pw & wet).sum() / (prev & pw).sum() - (prev & ~pw & wet).sum() / (prev & ~pw).sum()
        if w.sum() >= 30 and dry.sum() >= 30:
            anomalies = {}
            for name in res:
                r = res[name]
                cls = np.where(wet, np.mean(r[w]), np.mean(r[dry]))
                anomalies[name] = (r - cls)[sel]
                s["sd_t" + name] = np.std(anomalies[name])
            s["corr_mr"] = np.corrcoef(anomalies["m"], anomalies["r"])[0, 1]
            s["wet_cooler"] = np.mean(tx[dry]) - np.mean(tx[w])
        ok = sel & np.roll(sel, 1)
        ok[0] = False
        rx = res["x"]
        a, b = rx[ok], np.roll(rx, 1)[ok]
        s["lag1_tmax"] = np.corrcoef(a, b)[0, 1]
        # Year-to-year spread of the month's means (each year with 25 days).
        ys = [y for y in range(cal.FIRST, cal.LAST + 1) if (sel & (year == y)).sum() >= 25]
        if len(ys) >= 15:
            s["iav_tmax"] = np.std([np.mean(tx[sel & (year == y)]) for y in ys])
            totals = [np.mean(pr[sel & (year == y)]) for y in ys]
            if np.mean(totals) > 0:
                s["iav_total_cv"] = np.std(totals) / np.mean(totals)
            s["iav_wet"] = np.std([np.mean(wet[sel & (year == y)]) for y in ys])
        ws = w & d["has_snow"]
        if ws.sum() >= 30 and (d["snow"] & ws).sum() >= 1:
            s["snow"] = (d["snow"] & ws).sum() / ws.sum()
        if d["has_thunder"] and w.sum() >= 30:
            s["thunder"] = (d["thunder"] & w).sum() / w.sum()
        out[m] = s
    # Longest dry spell per year, averaged.
    spells = []
    for y in range(cal.FIRST, cal.LAST + 1):
        sel = good & (year == y)
        if sel.sum() < 330:
            continue
        run = best = 0
        for i in np.nonzero(year == y)[0]:
            run = run + 1 if good[i] and not wet[i] else 0
            best = max(best, run)
        spells.append(best)
    if spells:
        out["year"] = {"dry_spell": np.mean(spells)}
    return out


def compare_cmd(ghcn, climate, sample, path):
    gen_rows = defaultdict(list)
    with open(sample, encoding="ascii") as f:
        for line in f:
            sid, date, tx, tn, pr, typ, th, *_ = line.rstrip("\n").split("\t")
            gen_rows[sid].append((int(date.replace("-", "")), float(tx), float(tn), float(pr), typ, th == "1"))
    st = {sid: (lat, lon) for sid, lat, lon in cal.stations(path)}
    wanted = {c for lat, lon in st.values() for c in cal.near(lat, lon)}
    grid = cal.cru_cells(climate, wanted)
    cell_elev = cell_elevations(climate)
    station_elev = station_elevations(ghcn)
    n = cal.ordinal(cal.LAST * 10000 + 1231) - cal.ordinal(cal.FIRST * 10000 + 101) + 1

    diffs = defaultdict(list)
    obs_vals = defaultdict(list)
    vs_cru = defaultdict(list)
    used = 0
    for sid, rows in sorted(gen_rows.items()):
        obs = daily_obs(ghcn, sid)
        if obs is None:
            continue
        so, sg = stats(obs, n), stats(daily_gen(rows, n), n)
        if so is None or sg is None:
            continue
        used += 1
        lat, lon = st[sid]
        key = next((c for c in cal.near(lat, lon) if len(grid.get(c, {})) == len(cal.STEMS)), None)
        cell = grid[key] if key is not None else None
        # The cell's temperatures moved to the station's elevation, as the
        # generated days are, so the comparison measures the model only.
        e = station_elev.get(sid, -999.9)
        lapse = LAPSE * (cell_elev[key] - e) if cell is not None and e > -999 else 0.0
        for m, s in so.items():
            for k, v in s.items():
                if m in sg and k in sg[m]:
                    diffs[k].append(sg[m][k] - v)
                    obs_vals[k].append(v)
            if cell is not None and m != "year" and m in sg:
                ml = calendar.monthrange(2001, m + 1)[1]
                c = {
                    "tmax": cell["tmp"][m] + cell["dtr"][m] / 2 + lapse,
                    "tmin": cell["tmp"][m] - cell["dtr"][m] / 2 + lapse,
                    "wet": cell["rd0"][m] / ml,
                    "total": cell["pre"][m],
                }
                for k, v in c.items():
                    vs_cru[k].append(sg[m][k] - v)
    print(f"stations: {used}")
    print(f"{'metric':>12} {'n':>6} {'obs median':>11} {'bias':>8} {'median |err|':>13}   vs cell: bias, median |err|")
    for k in diffs:
        d = np.array(diffs[k])
        o = np.array(obs_vals[k])
        line = f"{k:>12} {len(d):>6} {np.median(o):>11.3f} {np.median(d):>+8.3f} {np.median(np.abs(d)):>13.3f}"
        if k in vs_cru:
            c = np.array(vs_cru[k])
            line += f"   {np.median(c):+.3f}, {np.median(np.abs(c)):.3f}"
        print(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["stations", "compare"])
    ap.add_argument("args", nargs="+", type=Path)
    ap.add_argument("--list", type=Path, default=cal.STATIONS, help="station list")
    a = ap.parse_args()
    if a.command == "stations":
        stations_cmd(*a.args, a.list)
    else:
        compare_cmd(*a.args, a.list)


if __name__ == "__main__":
    main()
