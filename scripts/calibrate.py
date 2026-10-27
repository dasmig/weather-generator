"""Fit the daily weather model's parameters on GHCN-Daily stations.

The climate table (scripts/prepare_climate.py) gives monthly means only.
What turns them into days is fitted here, on daily observations from the
Global Historical Climatology Network (GHCN-Daily, NOAA NCEI, public
domain), 1961-1990 to match the CRU normals:

- rain persistence: d = P(wet | wet) - P(wet | dry), against the month's
  wet-day fraction f (then P(wet | dry) = f (1 - d), P(wet | wet) = P(wet |
  dry) + d, which keeps f);
- the gamma shape of wet-day amounts, against the mean wet-day amount;
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
- the thunder probability on wet days (WT03, at stations that report it).

Usage:
  python scripts/calibrate.py select GHCN_DIR          # writes scripts/ghcn_stations.txt
  python scripts/calibrate.py fetch GHCN_DIR           # downloads the stations' files
  python scripts/calibrate.py fit GHCN_DIR CRU_DIR     # prints the parameters

GHCN_DIR holds ghcnd-inventory.txt and ghcnd-stations.txt from
https://www.ncei.noaa.gov/pub/data/ghcn/daily/; `fetch` puts by_station/*.csv.gz
beside them. CRU_DIR is prepare_climate.py's input.
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
BASE = "https://www.ncei.noaa.gov/pub/data/ghcn/daily/by_station/"
FIRST, LAST = 1961, 1990
WET = 1  # tenths of a mm: a wet day has at least 0.1 mm


# -- select ------------------------------------------------------------------


def select(ghcn):
    """One station per 10-degree box: the one with the longest TMAX, TMIN and
    PRCP record covering 1961-1990 (ties by id)."""
    spans = defaultdict(dict)
    where = {}
    with open(ghcn / "ghcnd-inventory.txt", encoding="ascii") as f:
        for line in f:
            sid, lat, lon, el, y0, y1 = line.split()
            if el in ("TMAX", "TMIN", "PRCP"):
                spans[sid][el] = (int(y0), int(y1))
                where[sid] = (float(lat), float(lon))
    best = {}
    for sid, s in spans.items():
        if len(s) < 3 or any(a > FIRST or b < LAST for a, b in s.values()):
            continue
        length = min(b - a for a, b in s.values())
        lat, lon = where[sid]
        box = (math.floor(lat / 10), math.floor(lon / 10))
        if box not in best or (length, sid) > best[box][0]:
            best[box] = ((length, sid), sid)
    chosen = sorted(v[1] for v in best.values())
    with open(STATIONS, "w", encoding="ascii", newline="\n") as f:
        f.write("# GHCN-Daily stations for scripts/calibrate.py fit: one per 10-degree box,\n")
        f.write("# the longest TMAX/TMIN/PRCP record covering 1961-1990.\n")
        for sid in chosen:
            lat, lon = where[sid]
            f.write(f"{sid}\t{lat:.4f}\t{lon:.4f}\n")
    print(f"wrote {STATIONS} ({len(chosen)} stations)")


def stations():
    out = []
    for line in STATIONS.read_text(encoding="ascii").splitlines():
        if line and not line.startswith("#"):
            sid, lat, lon = line.split("\t")
            out.append((sid, float(lat), float(lon)))
    return out


# -- fetch -------------------------------------------------------------------


def fetch(ghcn):
    d = ghcn / "by_station"
    d.mkdir(exist_ok=True)
    for sid, _, _ in stations():
        path = d / f"{sid}.csv.gz"
        if path.exists():
            continue
        try:
            urllib.request.urlretrieve(BASE + f"{sid}.csv.gz", path)
        except OSError as e:
            print(f"{sid}: {e}", file=sys.stderr)
    print(f"{len(list(d.glob('*.csv.gz')))} files in {d}")


# -- fit ---------------------------------------------------------------------

STEP = 1.0 / 6.0


def cru_cells(cru, wanted):
    """{(row, col): {stem: [12 floats]}} for the 10' cells near the stations."""
    out = defaultdict(dict)
    for stem in ("tmp", "dtr", "rd0", "pre"):
        with gzip.open(cru / f"grid_10min_{stem}.dat.gz", "rt", encoding="ascii") as f:
            for line in f:
                p = line.split()
                key = (round((float(p[0]) + 90) / STEP - 0.5), round((float(p[1]) + 180) / STEP - 0.5))
                if key in wanted:
                    out[key][stem] = [float(x) for x in p[2:14]]
    return out


def near(lat, lon, radius=3):
    i0, j0 = round((lat + 90) / STEP - 0.5), round((lon + 180) / STEP - 0.5)
    cells = [(i0 + di, j0 + dj) for di in range(-radius, radius + 1) for dj in range(-radius, radius + 1)]
    return sorted(cells, key=lambda c: (c[0] - i0) ** 2 + (c[1] - j0) ** 2)


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


def thom_shape(x):
    """Gamma shape by Thom's (1958) approximation to the maximum likelihood."""
    a = math.log(np.mean(x)) - np.mean(np.log(x))
    return (1 + math.sqrt(1 + 4 * a / 3)) / (4 * a) if a > 0 else float("nan")


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


def fit(ghcn, cru):
    st = stations()
    wanted = {c for _, lat, lon in st for c in near(lat, lon)}
    grid = cru_cells(cru, wanted)

    occ, amt, offs, sds, spells = [], [], [], [], []
    m0, m1, npairs = np.zeros((2, 2)), np.zeros((2, 2)), 0
    wind_ratio = []
    snow_t, snow_y = [], []
    thunder = []
    used = 0
    for sid, lat, lon in st:
        path = ghcn / "by_station" / f"{sid}.csv.gz"
        if not path.exists():
            continue
        cell = next((grid[c] for c in near(lat, lon) if len(grid.get(c, {})) == 4), None)
        if cell is None:
            continue
        data = read_station(path)

        # Daily arrays over 1961-1990 (NaN where missing).
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
                amt.append((np.mean(x), thom_shape(x), f))
            # Wet/dry offsets and spread.
            if w.sum() >= 30 and dsel.sum() >= 30:
                dm = np.mean(res["m"][dsel]) - np.mean(res["m"][w])
                dr = np.mean(res["r"][dsel]) - np.mean(res["r"][w])
                offs.append((dtr_m, dm, dr, f))
                for name in res:
                    r = res[name]
                    cls = np.where(wet, np.mean(r[w]), np.mean(r[dsel]))
                    rr = r - cls
                    sd = np.std(rr[sel])
                    if name == "m":
                        sdm = sd
                    else:
                        sdr = sd
                    z[name][sel] = rr[sel] / sd
                sds.append(
                    (sdm, sdr, annual_range, cell["dtr"][m], cell["rd0"][m] / calendar.monthrange(2001, m + 1)[1],
                     season[m], abs(lat))
                )
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
    ols(np.stack([np.ones(len(amt)), np.log(amt[:, 0])], axis=1), amt[:, 1], ["1", "ln(mean)"], "gamma shape")
    print(f"  median shape = {np.median(amt[:, 1]):.3f}, 10-90% {np.percentile(amt[:, 1], 10):.3f}-"
          f"{np.percentile(amt[:, 1], 90):.3f}")

    # Wet/dry offsets as shares of the diurnal range (the median over the
    # station-months; a regression through the origin understates them).
    offs = np.array(offs)
    km = np.median(offs[:, 1] / offs[:, 0])
    kr = np.median(offs[:, 2] / offs[:, 0])
    print(f"\nwet/dry offsets (dry minus wet) = k * dtr: k_mean = {km:.4f}, k_range = {kr:.4f} (n = {len(offs)})")

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
    ap.add_argument("command", choices=["select", "fetch", "fit"])
    ap.add_argument("ghcn", type=Path)
    ap.add_argument("cru", type=Path, nargs="?")
    args = ap.parse_args()
    if args.command == "select":
        select(args.ghcn)
    elif args.command == "fetch":
        fetch(args.ghcn)
    else:
        fit(args.ghcn, args.cru)


if __name__ == "__main__":
    main()
