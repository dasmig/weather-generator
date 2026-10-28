"""The fits and checks of weather 2.0 beyond scripts/calibrate.py: snow on
the ground, sunshine, gusts, fog and drizzle, and the measures of dry spells
and of the correlation between stations by distance.

Usage (GHCN is the directory of scripts/calibrate.py fetch, with by_station/):
  python fit_extras.py snow GHCN                 snowpack constants (grid search)
  python fit_extras.py sunshine SUNP             sunshine curve (CRU CL 2.0 grid_10min_sunp.dat.gz)
  python fit_extras.py events GHCN               gust, fog and drizzle regressions
  python fit_extras.py spells GHCN SAMPLE        dry-run hazards, stations against SAMPLE
  python fit_extras.py pairs GHCN [SAMPLE]       correlation by distance (with SAMPLE: generated)
  python fit_extras.py check GHCN SAMPLE         snow depth, fog, gusts and drizzle against SAMPLE

SAMPLE is scripts/sample_weather's output for the stations of all three lists
(ghcn_stations.txt, ghcn_holdout.txt, ghcn_thunder.txt), one seed for all, so
the sample is one world. Station arrays are cached under build/ghcn_cache.
"""
import argparse
import csv
import gzip
import itertools
import math
from collections import defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
CLIMATE = HERE.parent / "resources" / "full" / "climate.tsv"
CACHE = HERE.parent / "build" / "ghcn_cache"
LISTS = ("ghcn_stations.txt", "ghcn_holdout.txt", "ghcn_thunder.txt")
FIRST, LAST = 1991, 2020
START = np.datetime64("1991-01-01")
N = int((np.datetime64("2021-01-01") - START).astype(int))
ELEMENTS = ["TMAX", "TMIN", "PRCP", "SNWD", "AWND", "WSF5", "WT01", "WT02", "WT03", "WT14"]
DAYS = START + np.arange(N)
MONTH = DAYS.astype("datetime64[M]").astype(int) % 12
YEAR = DAYS.astype("datetime64[Y]").astype(int) + 1970
DOY = (DAYS - DAYS.astype("datetime64[Y]")).astype(int)
HARMONICS = np.stack([np.ones(N)] + [f(k * 2 * np.pi * DOY / 365.25) for k in (1, 2, 3) for f in (np.cos, np.sin)], 1)


# -- Data ---------------------------------------------------------------------

def stations():
    """{id: (latitude, longitude)} of the three station lists."""
    out = {}
    for name in LISTS:
        for line in (HERE / name).read_text().splitlines():
            if line and not line.startswith("#"):
                sid, lat, lon = line.split("\t")
                out[sid] = (float(lat), float(lon))
    return out


def observed(ghcn, sid):
    """A station's days of 1991-2020 as {element: array} (NaN where missing;
    weather types 1 where reported), cached."""
    CACHE.mkdir(parents=True, exist_ok=True)
    cache = CACHE / f"{sid}.npz"
    if cache.exists():
        z = np.load(cache)
        return {k: z[k] for k in z.files}
    path = Path(ghcn) / "by_station" / f"{sid}.csv.gz"
    if not path.exists():
        return None
    arr = {el: np.full(N, np.nan, dtype=np.float32) for el in ELEMENTS}
    with gzip.open(path, "rt", encoding="ascii", errors="replace") as f:
        for row in csv.reader(f):
            if len(row) < 6 or row[2] not in arr or row[5]:
                continue
            d = row[1]
            if not FIRST <= int(d[:4]) <= LAST:
                continue
            i = int((np.datetime64(f"{d[:4]}-{d[4:6]}-{d[6:]}") - START).astype(int))
            arr[row[2]][i] = 1.0 if row[2].startswith("WT") else int(row[3]) / 10.0
    np.savez_compressed(cache, **arr)
    return arr


def generated(sample):
    """{id: {column: array}} from a sample_weather file (full years only)."""
    by = defaultdict(list)
    with open(sample, encoding="ascii") as f:
        for line in f:
            r = line.rstrip("\n").split("\t")
            by[r[0]].append(r)
    out = {}
    for sid, rs in by.items():
        if len(rs) != N:
            continue
        col = lambda j: np.array([r[j] for r in rs])
        out[sid] = {"TMAX": col(2).astype(float), "TMIN": col(3).astype(float), "PRCP": col(4).astype(float),
                    "type": col(5), "thunder": col(6) == "1", "wind": col(9).astype(float),
                    "snow_depth": col(10).astype(float), "gust": col(11).astype(float), "fog": col(12) == "1",
                    "condition": col(13)}
    return out


def cells():
    """{(row, col) on the 0.5-degree grid: {name: 12 floats}} of the full tier."""
    out = {}
    with open(CLIMATE, encoding="utf-8") as f:
        h = f.readline().rstrip("\n").split("\t")
        c = {n: i for i, n in enumerate(h)}
        for line in f:
            p = line.rstrip("\n").split("\t")
            lat, lon = (float(x) for x in p[c["key"]].split(","))
            out[(int((lat + 90) * 2), int((lon + 180) * 2))] = {
                k: np.array([float(x) for x in p[c[k]].split(";")])
                for k in ("tmean", "dtr", "humidity", "cloud", "wind", "wet_days")}
    return out


def cell_of(grid, lat, lon):
    i0, j0 = int((lat + 90) * 2), int((lon + 180) * 2)
    near = [(di * di + dj * dj, (i0 + di, j0 + dj)) for di in (-1, 0, 1) for dj in (-1, 0, 1)
            if (i0 + di, j0 + dj) in grid]
    return grid[min(near)[1]] if near else None


def logistic(x, y):
    """Logistic regression by Newton's method."""
    w = np.zeros(x.shape[1])
    for _ in range(50):
        p = 1 / (1 + np.exp(-x @ w))
        h = (x * (p * (1 - p))[:, None]).T @ x + 1e-9 * np.eye(x.shape[1])
        w += np.linalg.solve(h, x.T @ (y - p))
    return w


def good_days(o):
    return ~np.isnan(o["TMAX"]) & ~np.isnan(o["TMIN"]) & ~np.isnan(o["PRCP"])


def monthly_wind_ratio(aw):
    wm = np.full(N, np.nan)
    for m in range(12):
        s = ~np.isnan(aw) & (MONTH == m)
        if s.sum() > 100:
            wm[MONTH == m] = aw[s].mean()
    return aw / wm


# -- Snow on the ground ---------------------------------------------------------

def snow_share(t):
    """The generator's snow water share of a wet day's amount by daily mean."""
    s = 1 / (1 + np.exp(-(0.4632 - 0.6058 * t)))
    return np.where(t > 1.0, 0.5 * s, s)


def snowpack(t, pr, params):
    """Snow depth (cm) for stations x days, per parameter set (melt rate,
    old-snow density, settling, settling while melting)."""
    melt_rate, rho_old, k, k_wet = (params[:, j][:, None] for j in range(4))
    sw = pr * snow_share(t)
    swe = np.zeros((len(params), t.shape[0]))
    rho = np.full_like(swe, 100.0)
    out = np.zeros((len(params), *t.shape), dtype=np.float32)
    for i in range(t.shape[1]):
        new = np.nan_to_num(sw[:, i])[None, :]
        tot = swe + new
        rho = np.where(tot > 0, (swe * rho + new * 100.0) / np.maximum(tot, 1e-12), 100.0)
        melt = melt_rate * np.maximum(t[:, i][None, :], 0.0)
        rho = rho_old - (rho_old - rho) * np.where(melt > 0, np.exp(-k_wet), np.exp(-k))
        swe = np.maximum(tot - melt, 0.0)
        rho = np.where(swe > 0, rho, 100.0)
        out[:, :, i] = swe / rho * 100.0
    return out


def snow_cmd(ghcn):
    data = []
    for sid in sorted(stations()):
        o = observed(ghcn, sid)
        if o is None:
            continue
        good = good_days(o)
        sd = o["SNWD"].astype(float)
        if np.sum(~np.isnan(sd) & good) < 3650 or np.nansum(sd > 0) < 100:
            continue
        data.append((np.where(good, (o["TMAX"] + o["TMIN"]) / 2, np.nan), np.where(good, o["PRCP"], np.nan), sd, good))
    print("snow depth stations", len(data))
    t = np.nan_to_num(np.stack([d[0] for d in data]).astype(float), nan=5.0)
    pr = np.stack([d[1] for d in data]).astype(float)

    def score(depth, verbose=False):
        co, cg, do, dg = [], [], [], []
        for j, (_, _, sd, good) in enumerate(data):
            ok = ~np.isnan(sd) & good
            for m in range(12):
                s = ok & (MONTH == m)
                if s.sum() < 200:
                    continue
                a, b = (sd[s] >= 1).mean(), (depth[j][s] >= 1).mean()
                if a < 0.02 and b < 0.02:
                    continue
                co.append(a), cg.append(b), do.append(sd[s].mean()), dg.append(depth[j][s].mean())
        co, cg, do, dg = map(np.array, (co, cg, do, dg))
        if verbose:
            print(f"  station-months {len(co)}: cover obs {np.median(co):.3f} bias {np.median(cg - co):+.3f};"
                  f" depth obs {np.median(do):.2f} cm bias {np.median(dg - do):+.2f}")
        return np.median(np.abs(cg - co)) + np.median(np.abs(dg - do)) / 20.0

    grid = np.array(list(itertools.product((1.5, 2.5, 3.5, 4.5), (250.0, 350.0, 450.0), (0.01, 0.03), (0.1, 0.3))))
    results = []
    for i in range(0, len(grid), 12):
        part = grid[i:i + 12]
        depth = snowpack(t, pr, part)
        results += [(score(depth[q]), tuple(part[q])) for q in range(len(part))]
    results.sort()
    print("best (score; melt_rate, snow_old, settle, settle_melting):")
    for r in results[:5]:
        print(" ", r)
    score(snowpack(t, pr, np.array([results[0][1]]))[0], True)


# -- Sunshine -------------------------------------------------------------------

def sunshine_cmd(sunp):
    """(1 - C)^p over the generator's daily cloud matches CRU CL 2.0's monthly
    sunshine against the full tier's cloud."""
    from scipy import optimize
    total, count = defaultdict(float), defaultdict(int)
    with gzip.open(sunp, "rt") as f:
        for line in f:
            p = line.split()
            if len(p) >= 14:
                key = (min(359, int((float(p[0]) + 90) * 2)), min(719, int((float(p[1]) + 180) * 2)))
                total[key] = total[key] + np.array([float(x) for x in p[2:14]])
                count[key] += 1
    month_length = np.array([31, 28.25, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31])
    rows = []
    for key, c in cells().items():
        lat = key[0] / 2 - 90
        if key in total and abs(lat) < 66:
            s = total[key] / count[key] / 100
            f = np.minimum(c["wet_days"], month_length) / month_length
            rows += [(c["cloud"][m] / 100, s[m], f[m]) for m in range(12)]
    r = np.array(rows)
    rng = np.random.default_rng(1)
    sub = r[rng.choice(len(r), 20000, replace=False)]
    # The generator's daily cloud: Beta(4) around the wet or dry day's mean.
    cbar, f = sub[:, 0], sub[:, 2]
    cw = cbar + 0.6 * (1 - cbar)
    cd = np.where(f < 1, (cbar - f * cw) / np.maximum(1 - f, 1e-9), cbar)
    wet = rng.random((len(sub), 400)) < f[:, None]
    mean = np.clip(np.where(wet, cw[:, None], cd[:, None]), 0.02, 0.98)
    cloud = rng.beta(mean * 4, (1 - mean) * 4)
    res = optimize.least_squares(lambda x: ((1 - cloud) ** x[0]).mean(1) - sub[:, 1], [1.0])
    e = ((1 - cloud) ** res.x[0]).mean(1) - sub[:, 1]
    e1 = (1 - cloud).mean(1) - sub[:, 1]
    print(f"cell-months {len(r)}; sunshine_power {res.x[0]:.3f}: RMSE {np.sqrt(np.mean(e ** 2)):.3f}"
          f" (1 - C: {np.sqrt(np.mean(e1 ** 2)):.3f})")


# -- Gusts, fog and drizzle --------------------------------------------------------

def events_cmd(ghcn):
    grid = cells()
    gust, fog, fog_all, drizzle = [], [], [], []
    for sid, (lat, lon) in sorted(stations().items()):
        o = observed(ghcn, sid)
        c = cell_of(grid, lat, lon) if o is not None else None
        if c is None:
            continue
        tx, tn, pr = (o[k].astype(float) for k in ("TMAX", "TMIN", "PRCP"))
        good = good_days(o)
        thunder = o["WT03"] == 1
        aw, g5 = o["AWND"].astype(float), o["WSF5"].astype(float)
        ratio = monthly_wind_ratio(aw)
        sel = good & ~np.isnan(aw) & ~np.isnan(g5) & (aw >= 0.3) & (g5 > aw)
        if sel.sum() > 1000:
            gust.append(np.stack([aw[sel], g5[sel], thunder[sel]], 1))
        f1 = o["WT01"] == 1
        if f1.sum() >= 300 and len(set(YEAR[f1])) >= 10:
            base = good & np.isin(YEAR, sorted(set(YEAR[f1])))
            lw = np.log(np.clip(ratio, 0.1, 5))
            x = np.stack([np.ones(N), c["humidity"][MONTH] / 100, np.clip((tx - tn) / np.maximum(c["dtr"][MONTH], 1), 0, 3),
                          (pr >= 0.1).astype(float), np.log1p(np.maximum(pr, 0)),
                          np.cos(2 * np.pi * DOY / 365.25) * np.sign(lat), lw], 1)
            y = np.stack([f1, o["WT02"] == 1], 1)
            fog.append((x[base & ~np.isnan(lw)], y[base & ~np.isnan(lw)]))
            fog_all.append((x[base], y[base]))
        dz = o["WT14"] == 1
        if dz.sum() >= 300 and len(set(YEAR[dz])) >= 10:
            sel = good & np.isin(YEAR, sorted(set(YEAR[dz]))) & (pr >= 0.1) & ~thunder & ((tx + tn) / 2 > 0)
            drizzle.append(np.stack([dz[sel], tx[sel], pr[sel], c["humidity"][MONTH][sel] / 100], 1))

    g = np.concatenate(gust)
    x = np.stack([np.ones(len(g)), np.log(g[:, 0]), g[:, 2]], 1)
    y = np.log(g[:, 1] / g[:, 0])
    coef, *_ = np.linalg.lstsq(x, y, rcond=None)
    print(f"gust days {len(g)}: ln(gust / wind) = c . (1, ln wind, thunder) {np.round(coef, 4)},"
          f" residual sd {np.std(y - x @ coef):.4f}")

    # Fog: the slopes where wind is reported too, then the intercept over all
    # fog-reporting station-years (the wind term at its mean where missing).
    xw = np.concatenate([a for a, _ in fog])
    yw = np.concatenate([b for _, b in fog])
    xa = np.concatenate([a for a, _ in fog_all])
    ya = np.concatenate([b for _, b in fog_all])
    xa[:, 6] = np.where(np.isnan(xa[:, 6]), np.nanmean(xw[:, 6]), xa[:, 6])
    names = "(1, humidity, range ratio, wet, ln(1 + mm), winter, ln wind ratio)"
    for col, name in ((0, "fog"), (1, "heavy fog")):
        w = logistic(xw, yw[:, col].astype(float))
        off = xa[:, 1:] @ w[1:]
        b = w[0]
        for _ in range(50):
            p = 1 / (1 + np.exp(-(b + off)))
            b += (ya[:, col] - p).sum() / (p * (1 - p)).sum()
        w[0] = b
        print(f"{name} {names}: {np.round(w, 4)} (share {ya[:, col].mean():.4f} of {len(ya)} days)")

    d = np.concatenate(drizzle)
    x = np.stack([np.ones(len(d)), d[:, 1] / 10, (d[:, 1] / 10) ** 2, np.log(d[:, 2]), d[:, 3]], 1)
    print(f"drizzle on {len(d)} rain days, (1, tmax/10, (tmax/10)^2, ln mm, humidity): {np.round(logistic(x, d[:, 0]), 4)}")


# -- Dry spells ---------------------------------------------------------------------

def dry_runs(wet, good):
    """The dry days before each day (0 after a wet day, -1 unknown)."""
    out = np.full(len(wet), -1)
    d = -1
    for i in range(len(wet)):
        out[i] = d
        if not good[i]:
            d = -1
        elif wet[i]:
            d = 0
        else:
            d = d + 1 if d >= 0 else -1
    return out


def spells_cmd(ghcn, sample):
    gen = generated(sample)
    bands = [(0, 0.1), (0.1, 0.2), (0.2, 0.35), (0.35, 0.5), (0.5, 1.0)]
    lengths = (1, 2, 3, 4, 6, 8, 10, 15, 20)
    count = defaultdict(lambda: np.zeros((2, 2, 26)))  # band -> (obs, gen) x (days, wet) x L
    for sid in sorted(stations()):
        o = observed(ghcn, sid)
        if o is None or sid not in gen:
            continue
        good = good_days(o)
        if good.sum() < 15 * 365:
            continue
        wet = o["PRCP"] >= 0.1
        f = np.zeros(N)
        for m in range(12):
            s = good & (MONTH == m)
            f[MONTH == m] = wet[s].mean()
        for which, (w, g) in enumerate(((wet, good), (gen[sid]["PRCP"] >= 0.1, np.ones(N, bool)))):
            runs = dry_runs(w, g)
            for b in bands:
                sel = g & (f >= b[0]) & (f < b[1])
                for L in range(1, 26):
                    s = sel & ((runs == L) if L < 25 else (runs >= L))
                    count[b][which, 0, L] += s.sum()
                    count[b][which, 1, L] += w[s].sum()
    print("P(wet | L dry days before), stations / generated")
    for b in bands:
        c = count[b]
        p = c[:, 1] / np.maximum(c[:, 0], 1)
        print(f"  f {b[0]:.2f}-{b[1]:.2f}: " + " ".join(f"{L}:{p[0, L]:.3f}/{p[1, L]:.3f}" for L in lengths))


# -- Correlation by distance ------------------------------------------------------------

def tetrachoric(a, b):
    """Latent normal correlation of two binary arrays (Bonett and Price 2005)."""
    pa, pb = a.mean(), b.mean()
    if min(pa, pb) < 0.03 or max(pa, pb) > 0.97 or len(a) < 200:
        return np.nan
    n11, n10 = (a & b).sum() + 0.5, (a & ~b).sum() + 0.5
    n01, n00 = (~a & b).sum() + 0.5, (~a & ~b).sum() + 0.5
    pm = min(pa, 1 - pa, pb, 1 - pb)
    c = 0.5 * (1 - abs(pa - pb) / 5 - (0.5 - pm) ** 2)
    return math.cos(math.pi / (1 + (n11 * n00 / (n10 * n01)) ** c))


def km(a, b):
    p1, p2 = math.radians(a[0]), math.radians(b[0])
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(b[1] - a[1]) / 2) ** 2
    return 2 * 6371.0 * math.asin(math.sqrt(h))


def pairs_cmd(ghcn, sample):
    from scipy import stats
    gen = generated(sample) if sample else None
    st = stations()
    data = {}
    for sid in st:
        o = observed(ghcn, sid)
        if o is None:
            continue
        good = good_days(o) & (o["TMAX"] >= o["TMIN"])
        if good.sum() < 15 * 365 or (gen is not None and sid not in gen):
            continue
        d = gen[sid] if gen is not None else o
        e = {"good": good, "wet": d["PRCP"] >= 0.1, "pr": d["PRCP"].astype(float)}
        for name, t in (("m", (d["TMAX"] + d["TMIN"]) / 2), ("r", d["TMAX"] - d["TMIN"])):
            coef, *_ = np.linalg.lstsq(HARMONICS[good], t[good], rcond=None)
            e[name] = np.where(good, t - HARMONICS @ coef, np.nan)
        data[sid] = e
    bands = [0, 50, 100, 150, 200, 300, 400, 500, 650, 800, 1000, 1250, 1500, 2000, 2500]
    res = defaultdict(lambda: defaultdict(list))
    ids = sorted(data)
    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            dist = km(st[a], st[b])
            if dist > bands[-1]:
                continue
            band = max(k for k in range(len(bands) - 1) if bands[k] <= dist)
            x, y = data[a], data[b]
            both = x["good"] & y["good"]
            s = defaultdict(list)
            for m in range(12):
                sel = both & (MONTH == m)
                if sel.sum() < 300:
                    continue
                s["occ"].append(tetrachoric(x["wet"][sel], y["wet"][sel]))
                jw = sel & x["wet"] & y["wet"]
                if jw.sum() >= 30:
                    s["amt"].append(stats.spearmanr(x["pr"][jw], y["pr"][jw])[0])
                for k in ("m", "r"):
                    s["t" + k].append(np.corrcoef(x[k][sel], y[k][sel])[0, 1])
                months = [(np.mean(x["m"][sy]), np.mean(y["m"][sy]), math.log(np.mean(x["pr"][sy]) + 0.1),
                           math.log(np.mean(y["pr"][sy]) + 0.1))
                          for yr in range(FIRST, LAST + 1) if (sy := sel & (YEAR == yr)).sum() >= 25]
                if len(months) >= 15:
                    v = np.array(months)
                    s["mon_t"].append(np.corrcoef(v[:, 0], v[:, 1])[0, 1])
                    s["mon_p"].append(np.corrcoef(v[:, 2], v[:, 3])[0, 1])
            for k, v in s.items():
                res[k][band].append(np.nanmean(v))
    keys = ["occ", "amt", "tm", "tr", "mon_t", "mon_p"]
    print(f"stations {len(ids)}\n{'km':>10} {'pairs':>5} " + " ".join(f"{k:>6}" for k in keys))
    for band in range(len(bands) - 1):
        if res["tm"][band]:
            print(f"{bands[band]:>4}-{bands[band + 1]:<5} {len(res['tm'][band]):>5} " +
                  " ".join(f"{np.nanmedian(res[k][band]):>6.3f}" for k in keys))


# -- Checks ----------------------------------------------------------------------------

def check_cmd(ghcn, sample):
    gen = generated(sample)
    out = defaultdict(list)
    for sid in sorted(stations()):
        o = observed(ghcn, sid)
        if o is None or sid not in gen:
            continue
        g = gen[sid]
        good = good_days(o)
        sd = o["SNWD"].astype(float)
        ok = ~np.isnan(sd)
        if ok.sum() >= 3000 and (sd[ok] >= 1).mean() >= 0.02:
            out["snow cover"].append(((sd[ok] >= 1).mean(), (g["snow_depth"][ok] >= 1).mean()))
            if (g["snow_depth"][ok] >= 1).sum() > 100:
                out["snow depth (cm)"].append((np.median(sd[ok & (sd >= 1)]), np.median(g["snow_depth"][ok & (g["snow_depth"] >= 1)])))
        fog = o["WT01"] == 1
        if fog.sum() >= 300 and len(set(YEAR[fog])) >= 10:
            years = good & np.isin(YEAR, sorted(set(YEAR[fog])))
            out["fog"].append((fog[years].mean(), g["fog"][years].mean()))
            heavy = (o["WT02"] == 1) & ~(o["PRCP"] >= 0.1)
            out["thick fog, dry days"].append((heavy[years].mean(), (g["condition"] == "fog")[years].mean()))
        aw, g5 = o["AWND"].astype(float), o["WSF5"].astype(float)
        ok = ~np.isnan(aw) & ~np.isnan(g5) & (aw > 0.3)
        if ok.sum() > 1000:
            out["gust (m/s)"].append((np.mean(g5[ok]), np.mean(g["gust"][ok])))
            out["gust / wind"].append((np.median(g5[ok] / aw[ok]), np.median(g["gust"][ok] / np.maximum(g["wind"][ok], 0.1))))
        dz = o["WT14"] == 1
        if dz.sum() >= 300 and len(set(YEAR[dz])) >= 10:
            years = good & np.isin(YEAR, sorted(set(YEAR[dz])))
            light = years & (o["PRCP"] >= 0.1) & (o["PRCP"] < 2.5) & ~(o["WT03"] == 1)
            gl = years & (g["PRCP"] >= 0.1) & (g["PRCP"] < 2.5) & (g["type"] == "rain")
            out["drizzle, light rain days"].append(((dz & light).sum() / max(light.sum(), 1), (g["condition"][gl] == "drizzle").mean()))
    print(f"{'measure':>26} {'n':>4} {'stations':>9} {'bias':>8} {'|err|':>7}")
    for k, v in out.items():
        v = np.array(v)
        print(f"{k:>26} {len(v):>4} {np.median(v[:, 0]):>9.3f} {np.median(v[:, 1] - v[:, 0]):>+8.3f}"
              f" {np.median(np.abs(v[:, 1] - v[:, 0])):>7.3f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("snow").add_argument("ghcn")
    sub.add_parser("sunshine").add_argument("sunp")
    sub.add_parser("events").add_argument("ghcn")
    for name in ("spells", "check"):
        p = sub.add_parser(name)
        p.add_argument("ghcn")
        p.add_argument("sample")
    p = sub.add_parser("pairs")
    p.add_argument("ghcn")
    p.add_argument("sample", nargs="?")
    a = ap.parse_args()
    if a.cmd == "snow":
        snow_cmd(a.ghcn)
    elif a.cmd == "sunshine":
        sunshine_cmd(a.sunp)
    elif a.cmd == "events":
        events_cmd(a.ghcn)
    elif a.cmd == "spells":
        spells_cmd(a.ghcn, a.sample)
    elif a.cmd == "pairs":
        pairs_cmd(a.ghcn, a.sample)
    else:
        check_cmd(a.ghcn, a.sample)


if __name__ == "__main__":
    main()
