"""Fit the daily weather model's parameters on GHCN-Daily stations.

The climate table (scripts/prepare_climate.py) gives monthly means only.
What turns them into days is fitted here, on daily observations from the
Global Historical Climatology Network (GHCN-Daily, NOAA NCEI, public
domain), 1991-2020 to match the climate table's normals:

- rain persistence: d = P(wet | wet) - P(wet | dry), against the month's
  wet-day fraction f (then P(wet | dry) = f (1 - d), P(wet | wet) = P(wet |
  dry) + d, which keeps f);
- the Weibull shape of wet-day amounts, against the mean wet-day amount;
- how much the daily mean and range of wet days differ from dry days, as a
  share of the diurnal range;
- the day-to-day spread of the daily mean temperature and of the daily range
  around the season, against predictors the climate table has (annual range,
  diurnal range, wet-day fraction, season, latitude);
- the lag-0 and lag-1 correlations of the standardized mean and range
  residuals, which give Richardson's (1981) A and B matrices;
- the Weibull shape of daily mean wind against the monthly mean (AWND, any
  years, since few stations report it before the 1980s);
- the rain/snow split by daily mean temperature (SNOW > 0 on wet days);
- the thunder probability on wet days (WT03, at the US stations of the fit
  list and of scripts/ghcn_thunder.txt, whose reports are consistent).

Usage:
  python scripts/calibrate.py select GHCN_DIR          # writes scripts/ghcn_stations.txt
  python scripts/calibrate.py holdout GHCN_DIR         # writes scripts/ghcn_holdout.txt
  python scripts/calibrate.py thunder GHCN_DIR         # writes scripts/ghcn_thunder.txt
  python scripts/calibrate.py fetch GHCN_DIR [--list FILE]  # downloads the stations' files
  python scripts/calibrate.py fit GHCN_DIR [CLIMATE]   # prints the parameters

GHCN_DIR holds ghcnd-inventory.txt and ghcnd-stations.txt from
https://www.ncei.noaa.gov/pub/data/ghcn/daily/; `fetch` puts by_station/*.csv.gz
beside them. CLIMATE is the full tier's climate.tsv (the default), so each
station is fitted against the normals the library uses.
"""

import argparse
import calendar
import csv
import gzip
import math
import sys
import urllib.request
from collections import defaultdict
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
STATIONS = HERE / "ghcn_stations.txt"
HOLDOUT = HERE / "ghcn_holdout.txt"
THUNDER = HERE / "ghcn_thunder.txt"
BASE = "https://www.ncei.noaa.gov/pub/data/ghcn/daily/by_station/"
FIRST, LAST = 1991, 2020
WET = 1  # tenths of a mm: a wet day has at least 0.1 mm


# -- select ------------------------------------------------------------------


def candidates(ghcn):
    """Stations with TMAX, TMIN and PRCP records covering 1991-2020: id ->
    (record length in years, lat, lon)."""
    spans = defaultdict(dict)
    where = {}
    with open(ghcn / "ghcnd-inventory.txt", encoding="ascii") as f:
        for line in f:
            sid, lat, lon, el, y0, y1 = line.split()
            if el in ("TMAX", "TMIN", "PRCP"):
                spans[sid][el] = (int(y0), int(y1))
                where[sid] = (float(lat), float(lon))
    out = {}
    for sid, s in spans.items():
        if len(s) < 3 or any(a > FIRST or b < LAST for a, b in s.values()):
            continue
        out[sid] = (min(b - a for a, b in s.values()), *where[sid])
    return out


def box_of(lat, lon):
    return (math.floor(lat / 10), math.floor(lon / 10))


def best_per_box(cands, keep=lambda sid: True):
    """The longest record per 10-degree box among the stations kept (ties by id)."""
    best = {}
    for sid, (length, lat, lon) in cands.items():
        if not keep(sid):
            continue
        box = box_of(lat, lon)
        if box not in best or (length, sid) > best[box]:
            best[box] = (length, sid)
    return sorted(v[1] for v in best.values())


def km(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    h = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(math.radians(lon2 - lon1) / 2) ** 2
    return 2 * 6371.0 * math.asin(math.sqrt(h))


def write_list(path, chosen, cands, header):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for line in header:
            f.write(f"# {line}\n")
        for sid in chosen:
            _, lat, lon = cands[sid]
            f.write(f"{sid}\t{lat:.4f}\t{lon:.4f}\n")
    print(f"wrote {path} ({len(chosen)} stations)")


def select(ghcn):
    """One station per 10-degree box: the one with the longest TMAX, TMIN and
    PRCP record covering 1991-2020 (ties by id)."""
    cands = candidates(ghcn)
    write_list(STATIONS, best_per_box(cands), cands, [
        "GHCN-Daily stations for scripts/calibrate.py fit: one per 10-degree box,",
        "the longest TMAX/TMIN/PRCP record covering 1991-2020.",
    ])


def holdout(ghcn):
    """Stations the fit never sees, for scripts/validate.py: per 10-degree box,
    the longest record among stations other than the fit's and at least
    100 km from it."""
    cands = candidates(ghcn)
    fit_by_box = {box_of(lat, lon): (lat, lon) for _, lat, lon in stations()}

    def keep(sid):
        _, lat, lon = cands[sid]
        f = fit_by_box.get(box_of(lat, lon))
        return f is None or km(lat, lon, *f) >= 100.0

    write_list(HOLDOUT, best_per_box(cands, keep), cands, [
        "GHCN-Daily stations for scripts/validate.py, none used by the fit: per",
        "10-degree box, the longest TMAX/TMIN/PRCP record covering 1991-2020",
        "among stations at least 100 km from the box's fit station.",
    ])


def thunder_list(ghcn):
    """US stations for the thunder fit: per 3-degree box, the longest
    TMAX/TMIN/PRCP record covering 1991-2020 among stations reporting WT03
    for 20 or more of those years, none on the fit or holdout lists or within
    100 km of a holdout station."""
    cands = candidates(ghcn)
    wt03 = {}
    with open(ghcn / "ghcnd-inventory.txt", encoding="ascii") as f:
        for line in f:
            sid, _, _, el, y0, y1 = line.split()
            if el == "WT03":
                wt03[sid] = min(int(y1), LAST) - max(int(y0), FIRST)
    taken = {sid for sid, _, _ in stations()} | {sid for sid, _, _ in stations(HOLDOUT)}
    held = [(lat, lon) for _, lat, lon in stations(HOLDOUT)]

    def keep(sid):
        _, lat, lon = cands[sid]
        return (sid.startswith("US") and wt03.get(sid, 0) >= 20 and sid not in taken
                and all(km(lat, lon, a, b) >= 100.0 for a, b in held))

    best = {}
    for sid, (length, lat, lon) in cands.items():
        if keep(sid):
            box = (math.floor(lat / 3), math.floor(lon / 3))
            if box not in best or (length, sid) > best[box]:
                best[box] = (length, sid)
    write_list(THUNDER, sorted(v[1] for v in best.values()), cands, [
        "GHCN-Daily US stations for the thunder fit of scripts/calibrate.py: per",
        "3-degree box, the longest TMAX/TMIN/PRCP record covering 1991-2020 among",
        "stations reporting WT03 for 20 or more of those years, away from the",
        "holdout stations.",
    ])


def thunder_samples(ghcn, sid):
    """(tmax, mm, thunder) for the wet days of one station, 1991-2020; empty
    when it reports thunder on fewer than 100 days."""
    path = ghcn / "by_station" / f"{sid}.csv.gz"
    if not path.exists():
        return []
    data = read_station(path)
    tx, pr, wt = data.get("TMAX", {}), data.get("PRCP", {}), data.get("WT03", {})
    days = [d for d in pr if FIRST <= d // 10000 <= LAST and d in tx]
    if sum(1 for d in days if d in wt) < 100:
        return []
    return [(tx[d] / 10.0, pr[d] / 10.0, 1.0 if d in wt else 0.0) for d in days if pr[d] >= WET]


def stations(path=STATIONS):
    out = []
    for line in Path(path).read_text(encoding="ascii").splitlines():
        if line and not line.startswith("#"):
            sid, lat, lon = line.split("\t")
            out.append((sid, float(lat), float(lon)))
    return out


# -- fetch -------------------------------------------------------------------


def fetch(ghcn, path=STATIONS):
    d = ghcn / "by_station"
    d.mkdir(exist_ok=True)
    for sid, _, _ in stations(path):
        path = d / f"{sid}.csv.gz"
        if path.exists():
            continue
        try:
            urllib.request.urlretrieve(BASE + f"{sid}.csv.gz", path)
        except OSError as e:
            print(f"{sid}: {e}", file=sys.stderr)
    print(f"{len(list(d.glob('*.csv.gz')))} files in {d}")


# -- fit ---------------------------------------------------------------------

CLIMATE = HERE.parent / "resources" / "full" / "climate.tsv"
STEMS = (("tmp", "tmean"), ("dtr", "dtr"), ("rd0", "wet_days"), ("pre", "precipitation"), ("cld", "cloud"),
         ("reh", "humidity"), ("wnd", "wind"))


def cru_cells(climate=CLIMATE, wanted=None):
    """{(row, col) on the 0.5-degree grid: {stem: [12 floats]}} from the full
    tier's climate table (stems as CRU's: tmp, dtr, rd0, pre, cld, reh, wnd)."""
    out = {}
    with open(climate, encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        col = {name: k for k, name in enumerate(header)}
        for line in f:
            p = line.rstrip("\n").split("\t")
            lat, lon = (float(x) for x in p[col["key"]].split(","))
            key = (int((lat + 90.0) * 2), int((lon + 180.0) * 2))
            if wanted is not None and key not in wanted:
                continue
            out[key] = {stem: [float(x) for x in p[col[name]].split(";")]
                        for stem, name in STEMS}
    return out


def near(lat, lon, radius=1):
    """0.5-degree cells around a point, nearest first."""
    i0, j0 = int((lat + 90.0) * 2), int((lon + 180.0) * 2)
    cells = [(i0 + di, j0 + dj) for di in range(-radius, radius + 1) for dj in range(-radius, radius + 1)]
    return sorted(cells, key=lambda c: (c[0] + 0.5 - (lat + 90.0) * 2) ** 2 + (c[1] + 0.5 - (lon + 180.0) * 2) ** 2)


def read_station(path):
    """{element: {date_int: value}} with failed-QC values dropped."""
    data = defaultdict(dict)
    keep = {"TMAX", "TMIN", "PRCP", "SNOW", "AWND", "WT03"}
    with gzip.open(path, "rt", encoding="ascii", errors="replace") as f:
        for row in csv.reader(f):
            if len(row) < 6 or row[2] not in keep or row[5]:
                continue
            data[row[2]][int(row[1])] = int(row[3])
    return data


def ordinal(date_int):
    y, m, d = date_int // 10000, date_int // 100 % 100, date_int % 100
    return np.datetime64(f"{y:04d}-{m:02d}-{d:02d}").astype(int)


def weibull_mle(x):
    """Weibull shape by maximum likelihood: the root c of
    sum(x^c ln x) / sum(x^c) - 1/c - mean(ln x), by bisection."""
    lx = np.log(x)

    def g(c):
        xc = x ** c
        return np.sum(xc * lx) / np.sum(xc) - 1 / c - lx.mean()

    lo, hi = 0.1, 10.0
    if g(lo) > 0 or g(hi) < 0:
        return float("nan")
    for _ in range(60):
        mid = (lo + hi) / 2
        if g(mid) < 0:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def harmonics(doy, k=3):
    t = 2 * np.pi * doy / 365.25
    cols = [np.ones_like(t)]
    for h in range(1, k + 1):
        cols += [np.cos(h * t), np.sin(h * t)]
    return np.stack(cols, axis=1)


def weibull_shape(cv):
    lo, hi = 0.5, 20.0
    for _ in range(80):
        k = (lo + hi) / 2
        g1, g2 = math.gamma(1 + 1 / k), math.gamma(1 + 2 / k)
        if math.sqrt(g2 / g1**2 - 1) > cv:
            lo = k
        else:
            hi = k
    return (lo + hi) / 2


def logistic(x, y):
    """Logistic regression by Newton's method; returns the coefficients."""
    w = np.zeros(x.shape[1])
    for _ in range(40):
        p = 1 / (1 + np.exp(-x @ w))
        g = x.T @ (y - p)
        h = (x * (p * (1 - p))[:, None]).T @ x
        w += np.linalg.solve(h, g)
    return w


def median_regression(x, y, iterations=100):
    """Least absolute deviations by iteratively reweighted least squares."""
    coef, *_ = np.linalg.lstsq(x, y, rcond=None)
    for _ in range(iterations):
        w = 1.0 / np.maximum(np.abs(y - x @ coef), 1e-4)
        coef = np.linalg.solve((x * w[:, None]).T @ x, (x * w[:, None]).T @ y)
    return coef


def ols(x, y, names, label):
    x, y = np.asarray(x), np.asarray(y)
    coef, *_ = np.linalg.lstsq(x, y, rcond=None)
    pred = x @ coef
    r2 = 1 - np.sum((y - pred) ** 2) / np.sum((y - y.mean()) ** 2)
    rmse = math.sqrt(np.mean((y - pred) ** 2))
    print(f"\n{label}: n = {len(y)}, R^2 = {r2:.3f}, RMSE = {rmse:.3f}")
    for n, c in zip(names, coef):
        print(f"  {n:>12} {c:+.5f}")
    return coef


def fit(ghcn, climate):
    st = stations()
    wanted = {c for _, lat, lon in st for c in near(lat, lon)}
    grid = cru_cells(climate, wanted)

    occ, amt, classes, sds, spells = [], [], [], [], []
    m0, m1, npairs = np.zeros((2, 2)), np.zeros((2, 2)), 0
    wind_ratio = []
    snow_t, snow_y = [], []
    thunder = []
    used = 0
    for sid, lat, lon in st:
        path = ghcn / "by_station" / f"{sid}.csv.gz"
        if not path.exists():
            continue
        cell = next((grid[c] for c in near(lat, lon) if len(grid.get(c, {})) == len(STEMS)), None)
        if cell is None:
            continue
        data = read_station(path)

        # Daily arrays over 1991-2020 (NaN where missing).
        start = ordinal(FIRST * 10000 + 101)
        n = ordinal(LAST * 10000 + 1231) - start + 1
        arr = {}
        for el in ("TMAX", "TMIN", "PRCP", "SNOW", "WT03"):
            a = np.full(n, np.nan)
            for date, v in data.get(el, {}).items():
                if FIRST <= date // 10000 <= LAST:
                    a[ordinal(date) - start] = v / 10.0 if el != "WT03" else 1.0
            arr[el] = a
        days = np.arange(n) + start
        dt = days.astype("datetime64[D]")
        month = dt.astype("datetime64[M]").astype(int) % 12
        doy = (dt - dt.astype("datetime64[Y]")).astype(int)
        tx, tn, pr = arr["TMAX"], arr["TMIN"], arr["PRCP"]
        good = ~np.isnan(tx) & ~np.isnan(tn) & ~np.isnan(pr) & (tx >= tn)
        if good.sum() < 15 * 365:
            continue
        used += 1
        wet = pr >= WET / 10.0

        tmp = np.array(cell["tmp"])
        annual_range = tmp.max() - tmp.min()
        season = (tmp - tmp.mean()) / max(annual_range / 2, 0.5)

        # Seasonal cycle of the daily mean and range (three harmonics), then
        # residuals.
        h = harmonics(doy[good].astype(float))
        res = {}
        for name, t in (("m", (tx + tn) / 2), ("r", tx - tn)):
            coef, *_ = np.linalg.lstsq(h, t[good], rcond=None)
            r = np.full(n, np.nan)
            r[good] = t[good] - h @ coef
            res[name] = r

        # Rain classes by the day's and its neighbours' rain: 0 wet after dry,
        # 1 wet after wet, 2 dry after wet, 3 dry before wet, 4 dry between
        # wet days, -1 dry between dry days.
        before, after = np.roll(wet, 1), np.roll(wet, -1)
        cls = np.select([wet & ~before, wet & before, ~wet & before & ~after, ~wet & ~before & after,
                         ~wet & before & after], [0, 1, 2, 3, 4], -1)
        known = good & np.roll(good, 1) & np.roll(good, -1)

        z = {name: np.full(n, np.nan) for name in res}
        for m in range(12):
            sel = good & (month == m)
            if sel.sum() < 300:
                continue
            w, dsel = sel & wet, sel & ~wet
            f = w.sum() / sel.sum()
            dtr_m = np.mean(tx[sel] - tn[sel])
            # Occurrence persistence.
            prev = np.roll(sel, 1) & sel
            prev[0] = False
            pw = wet[np.roll(np.arange(n), 1)]
            n_d = (prev & ~pw).sum()
            n_w = (prev & pw).sum()
            if n_d > 30 and n_w > 30:
                p01 = (prev & ~pw & wet).sum() / n_d
                p11 = (prev & pw & wet).sum() / n_w
                occ.append((f, p11 - p01))
            # Amounts.
            x = pr[w]
            if len(x) >= 40:
                amt.append((np.mean(x), weibull_mle(x), f))
            # Rain classes and spread. The class shifts are against the dry
            # days between dry days: the mean's as a share of the diurnal
            # range, the range's as a share of that reference day's range;
            # regressed on what the climate table has. The spreads and the
            # residuals for A and B are what the classes leave.
            ks = sel & known
            base = ks & (cls == -1)
            if base.sum() >= 30 and all((ks & (cls == k)).sum() >= 20 for k in range(5)):
                f_cell = cell["rd0"][m] / calendar.monthrange(2001, m + 1)[1]
                x = [1.0, season[m], f_cell, abs(lat) / 90, annual_range / 30, cell["dtr"][m] / 10,
                     cell["cld"][m] / 100, cell["reh"][m] / 100]
                range_base = np.mean((tx - tn)[base])
                for k in range(5):
                    kk = ks & (cls == k)
                    classes.append((k, x, (np.mean(res["m"][kk]) - np.mean(res["m"][base])) / dtr_m,
                                    (np.mean((tx - tn)[kk]) / range_base) - 1.0))
                for name in res:
                    r = res[name]
                    rr = np.full(n, np.nan)
                    for k in range(-1, 5):
                        kk = ks & (cls == k)
                        rr[kk] = r[kk] - np.mean(r[kk])
                    sd = np.nanstd(rr[ks])
                    if name == "m":
                        sdm = sd
                    else:
                        sdr = sd
                    z[name][ks] = rr[ks] / sd
                sds.append((sdm, sdr, annual_range, cell["dtr"][m], f_cell, season[m], abs(lat)))
            # Thunder on wet days, at the US stations (whose reports are
            # consistent; elsewhere WT03 is sparse).
            if sid.startswith("US") and np.nansum(arr["WT03"]) >= 100:
                for i in np.nonzero(w)[0]:
                    thunder.append((tx[i], pr[i], 1.0 if arr["WT03"][i] == 1.0 else 0.0))

        # Lag-0 and lag-1 correlations of the standardized residuals.
        zz = np.stack([z["m"], z["r"]], axis=1)
        ok0 = ~np.isnan(zz).any(axis=1)
        ok1 = ok0[1:] & ok0[:-1]
        a, b = zz[1:][ok1], zz[:-1][ok1]
        m0 += a.T @ a
        m1 += a.T @ b
        npairs += len(a)

        # Rain or snow by daily mean temperature.
        sn = arr["SNOW"]
        has = good & wet & ~np.isnan(sn)
        if has.sum() > 200 and (sn[has] > 0).sum() > 20:
            snow_t += list(((tx + tn) / 2)[has])
            snow_y += list((sn[has] > 0).astype(float))

        # Wind: daily over monthly mean, any years.
        aw = data.get("AWND", {})
        by_month = defaultdict(list)
        for date, v in aw.items():
            if v > 0:
                by_month[date // 100].append(v / 10.0)
        for vals in by_month.values():
            if len(vals) >= 25:
                mean = np.mean(vals)
                wind_ratio += [v / mean for v in vals]

    print(f"stations used: {used}")

    # Persistence d against f.
    occ = np.array(occ)
    ols(np.stack([np.ones(len(occ)), occ[:, 0]], axis=1), occ[:, 1], ["1", "f"], "persistence d = p11 - p01")
    print(f"  median d = {np.median(occ[:, 1]):.3f}")

    # Gamma shape against the mean wet-day amount.
    amt = np.array([a for a in amt if np.isfinite(a[1])])
    ols(np.stack([np.ones(len(amt)), np.log(amt[:, 0])], axis=1), amt[:, 1], ["1", "ln(mean)"], "Weibull shape")
    print(f"  median shape = {np.median(amt[:, 1]):.3f}, 10-90% {np.percentile(amt[:, 1], 10):.3f}-"
          f"{np.percentile(amt[:, 1], 90):.3f}")

    # Rain-class shifts: least squares, so the shifts are right on average
    # (a median regression leaves the winter months' large positive shifts
    # out, and wet days come out too cold against dry ones).
    labels = ["wet after dry", "wet after wet", "dry after wet", "dry before wet", "dry between wet"]
    for col, what in ((2, "mean shift, share of dtr"), (3, "range factor - 1")):
        print(f"\nrain classes, {what}: c . (1, season, f, |lat|/90, annual range/30, dtr/10, cloud, humidity)")
        for k in range(5):
            rows = [c for c in classes if c[0] == k]
            x = np.array([c[1] for c in rows])
            y = np.array([c[col] for c in rows])
            coef, *_ = np.linalg.lstsq(x, y, rcond=None)
            print(f"  {labels[k]:>16} (n = {len(y)}, median {np.median(y):+.3f}): {{" +
                  ", ".join(f"{c:.4f}" for c in coef) + "}")

    # Spread.
    sds = np.array(sds)
    names = ["1", "range", "dtr", "f", "season", "season*rng", "|lat|"]
    x = np.stack([np.ones(len(sds)), sds[:, 2], sds[:, 3], sds[:, 4], sds[:, 5], sds[:, 5] * sds[:, 2], sds[:, 6]],
                 axis=1)
    ols(x, sds[:, 0], names, "sd of the daily mean temperature")
    ols(x, sds[:, 1], names, "sd of the daily range")
    print(f"  median CV of the range = {np.median(sds[:, 1] / sds[:, 3]):.3f}")

    # A and B.
    m0 /= npairs
    m1 /= npairs
    a_mat = m1 @ np.linalg.inv(m0)
    bbt = m0 - a_mat @ m1.T
    b_mat = np.linalg.cholesky(bbt)
    print(f"\nlag-0 correlation (pairs = {npairs}):\n{m0}\nlag-1:\n{m1}")
    print(f"A =\n{a_mat}\nB =\n{b_mat}\neigenvalues of A: {np.linalg.eigvals(a_mat)}")

    # Wind.
    if wind_ratio:
        wr = np.array(wind_ratio)
        cv = np.std(wr) / np.mean(wr)
        print(f"\nwind: n = {len(wr)}, CV of daily over monthly mean = {cv:.3f}, Weibull k = {weibull_shape(cv):.3f}")

    # Snow.
    if snow_t:
        t, y = np.array(snow_t), np.array(snow_y)
        print(f"\nsnow share of wet days by daily mean temperature (n = {len(t)}):")
        for lo in np.arange(-6, 6.01, 0.5):
            s = (t >= lo) & (t < lo + 0.5)
            if s.sum() >= 50:
                print(f"  {lo:+5.1f} {y[s].mean():.3f} ({s.sum()})")
        w = logistic(np.stack([np.ones(len(t)), t], axis=1), y)
        print(f"  logistic: logit p = {w[0]:+.4f} {w[1]:+.4f} * T; p = 0.5 at {-w[0] / w[1]:+.2f}")

    # Thunder.
    extra = [x for sid, _, _ in stations(THUNDER) for x in thunder_samples(ghcn, sid)] if THUNDER.exists() else []
    print(f"\nthunder: {len(thunder)} wet days at fit stations, {len(extra)} at thunder stations")
    thunder += extra
    if thunder:
        th = np.array(thunder)
        print(f"\nthunder on wet days (n = {len(th)}, share {th[:, 2].mean():.3f}):")
        for lo in range(-10, 40, 5):
            for plo, phi in ((0.1, 2), (2, 10), (10, 30), (30, 1000)):
                s = (th[:, 0] >= lo) & (th[:, 0] < lo + 5) & (th[:, 1] >= plo) & (th[:, 1] < phi)
                if s.sum() >= 100:
                    print(f"  tmax {lo:+3d}..{lo + 5:+3d}  rain {plo:>4}..{phi:<4} {th[s, 2].mean():.3f} ({s.sum()})")
        xs = np.stack([np.ones(len(th)), th[:, 0], np.log(th[:, 1])], axis=1)
        w = logistic(xs, th[:, 2])
        print(f"  logistic: logit p = {w[0]:+.4f} {w[1]:+.4f} * tmax {w[2]:+.4f} * ln(rain mm)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["select", "holdout", "thunder", "fetch", "fit"])
    ap.add_argument("ghcn", type=Path)
    ap.add_argument("climate", type=Path, nargs="?", default=CLIMATE)
    ap.add_argument("--list", type=Path, default=STATIONS, help="station list for fetch")
    args = ap.parse_args()
    if args.command == "select":
        select(args.ghcn)
    elif args.command == "holdout":
        holdout(args.ghcn)
    elif args.command == "thunder":
        thunder_list(args.ghcn)
    elif args.command == "fetch":
        fetch(args.ghcn, args.list)
    else:
        fit(args.ghcn, args.climate)


if __name__ == "__main__":
    main()
