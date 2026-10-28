# Data and Method

The data in `resources/`, the model that turns it into days, and how well the result matches real weather (rule 6 of the dasmig standard). The climate tables are derived by `scripts/prepare_climate.py`; the model's constants are fitted by `scripts/calibrate.py` and checked by `scripts/validate.py`; the few curated or tuned values are marked as such here and in `dasmig/weathergen.hpp`.

## Tiers

| Tier | Grid | Cells | File | Loads in |
| --- | --- | --- | --- | --- |
| `lite` | 2 degrees | 5,163 | 2.0 MB (`climate.tsv`) | about 60 ms |
| `full` | 0.5 degrees | 67,213 | 26.0 MB | about 0.8 s |

`lite` is compiled in with `DASMIG_WEATHER_EMBED` (2.1 MB of source). Historical requests also read `history/history.tsv` (23.1 MB, 5,163 2-degree blocks, below), for either tier; it is never compiled in. Both cover every land cell of the source except Antarctica, which the source does not have, and both carry the same 18 KB `land.tsv`. `full` follows coasts, mountains and rain shadows; `lite`'s 2-degree blocks average them away, so a point's normals are further from its own (see [Validation](#validation)).

## `climate.tsv`: monthly normals, derived

Keyed by `key`, the cell's or block's centre (`"49.00,3.00"`). Built by `scripts/prepare_climate.py` from three sources:

- **CRU TS 4.10** (Harris, Osborn, Jones and Lister 2020, Climatic Research Unit, University of East Anglia; ODbL): monthly grids at 0.5 degrees over land, interpolated from station records month by month. A cell's normal is the 1991-2020 mean of each calendar month (with at least 25 of the 30 years).
- **CRU CL 2.0** (New, Lister, Hulme and Makin 2002; ODbL): 1961-1990 means at 10 minutes of arc, for what CRU TS lacks: elevation and wind, and the land within each half-degree cell.
- **Natural Earth** 1:10m land and minor islands (public domain), for which cells are land. CRU TS also has 207 cells with no Natural Earth land, most of them in open sea around ocean weather ships; they are dropped.

`full` is the half-degree grid. `lite` aggregates it into 2-degree blocks, each value the area-weighted mean of the block's cells.

| Column | Source and method | Reliability |
| --- | --- | --- |
| `latitude`, `longitude` | the land centroid of the cell's 10' CRU CL land cells, so a coastal cell's point lies on its land | exact for the grid |
| `elevation` | CRU CL `elv`, mean over the 10' land cells, metres | a cell mean: the GHCN stations sit 35 to 46 m below their cell in the median (valleys), and a mountain cell's peaks and valleys differ by far more |
| `area` | the sum of cos(latitude) over the 10' land cells, the weight for drawing a random place | exact for the grid |
| `tmean`, `dtr` | CRU TS `tmp` and `dtr` (mean temperature, diurnal range; Tmax = tmp + dtr / 2, Tmin = tmp - dtr / 2), °C | good: every land cell-month has a temperature station within reach; weaker in mountains, deserts and the high Arctic, where stations are far apart |
| `precipitation` | CRU TS `pre`, mm per month | good in the mid-latitudes; 89% of cell-months have a station within reach, the rest relax to CRU's 1961-1990 climatology; gauge undercatch is not corrected, so snowy and windy places read low |
| `wet_days` | CRU TS `wet`, days with at least 0.1 mm, capped at the month's length | fair: 89% of cell-months have a station within reach, and CRU fills the rest from the precipitation |
| `humidity` | from CRU TS `vap`: vapour pressure over the saturation vapour pressure (Magnus, over water) averaged over a sinusoidal day between Tmin and Tmax, % | fair: 2.2 points above CRU CL 2.0's measured `reh` in the median |
| `cloud` | CRU TS `cld`, % cover | fair: CRU derives most of it from the diurnal range and sunshine records where cloud is not observed |
| `wind` | CRU CL `wnd`, m/s at 10 m, 1961-1990 (CRU TS has no wind) | rough: few stations, and sites differ in exposure |

A cell with no 10' CRU CL land cell (a small island) takes the nearest cell's elevation and wind and one 10' cell's area.

Against the 1961-1990 CRU CL 2.0 table this replaces, land is 0.74 °C warmer on area average (1.20 °C north of 60°), precipitation is unchanged in the median (ratio 1.005), and there are 1.4 fewer wet days a year.

## `land.tsv`: the land mask

The land cells of the half-degree grid (the `full` cells), as runs of columns per row: `row` (0 at -89.75°, the south) and `columns` (`"111-238;327-328"`, 0 at -179.75°). A requested point's distance to land is the distance on the sphere to the nearest land cell's rectangle, or to the nearest cell's point if that is nearer (for layer cells). Up to 100 km the point takes the nearest cell's weather and reports the distance; beyond it the call throws.

The mask's resolution is half a degree: a point on a small island or peninsula can read a few km out, and a wide estuary or a lake counts as land (Natural Earth's land includes lakes).

## `history/history.tsv`: the months of 1991-2020, derived

Keyed by `key`, the 2-degree block's centre (the `lite` keys), with `first_year` (1991) and three columns of 360 values separated by `;`, one per month from January 1991 to December 2020. Built by `scripts/prepare_history.py` from the same CRU TS 4.10 `tmp`, `pre` and `wet` grids as the normals, each month area-weighted over the block's half-degree land cells as `lite` is:

| Column | Value | Reliability |
| --- | --- | --- |
| `tmean` | the month's mean temperature minus the block's 1991-2020 mean for that calendar month, °C, to 0.1 | good where `tmean` is: every land cell-month has a station within reach |
| `precipitation` | the month's precipitation as a percentage of the block's normal, whole, 0 to 2000 (100 where the normal is below 0.05 mm) | fair: 89% of cell-months have a station within reach; the rest relax to CRU's climatology, so their months read as normal; desert months with a tiny normal swing wildly |
| `wet_days` | the month's wet days minus the normal, days, to 0.1 | fair: as `wet_days` in the normals |

The largest anomalies are 12.4 °C (a Siberian winter month), 2000% (the clip, in deserts) and 26.8 wet days. A cell of either tier takes the block its point lies in, so a `full` cell gets its block's anomaly on its own normals.

## Generation Model

A Richardson (1981) type weather generator, the family of WGEN, LARS-WG and CLIGEN. Each day:

1. **Parameters.** Every monthly normal is turned into twelve control points whose linear interpolation between mid-months gives back the monthly mean (`(c[m-1] + 6 c[m] + c[m+1]) / 8 = v[m]`, solved by iteration), and the day's value is read off that line. This keeps monthly means while avoiding steps at the turn of the month.
2. **Month anomalies.** Each place and calendar month (every January 2007 its own) draws two standard normal anomalies, interpolated between mid-months like the normals. One shifts the temperature by `0.48` of the day's spread. The other scales the odds of a wet day (log sd `0.32`) and the wet-day amount (log sd `0.28`), with corrections that keep their means; scaling the odds rather than the fraction keeps a very wet month's fraction below 1 without losing wet days. They give the year-to-year spread of monthly means that a daily chain alone underestimates (the "overdispersion" problem of weather generators).
3. **Wet or dry.** A two-state Markov chain keeps the day's wet-day fraction f: P(wet | dry) = f (1 - d), P(wet | wet) = P(wet | dry) + d, with persistence d = 0.270 + 0.006 f.
4. **Amount.** A wet day's amount is gamma distributed with the month's mean wet-day amount (precipitation / wet days) and shape 1.146 - 0.224 ln(mean mm), clamped to 0.4 to 1.5: light-rain climates have more even amounts, heavy-rain climates more skewed ones. At least 0.1 mm.
5. **Temperature.** The daily mean temperature and the daily range have standardized residuals from a lag-1 autoregression, x(t) = A x(t-1) + B e(t) (Richardson's A and B, fitted). The mean is the season's, shifted on wet days by `-0.016` of the diurnal range (dry days by the opposite share, so the monthly mean stays), plus the residual and the month anomaly times the day's spread. The range is log-normal around the season's range, shifted on wet days by `-0.220` of it, so it is never negative. Tmax and Tmin are the mean plus and minus half the range; `tmean` is halfway, and `frost` is a Tmin below 0.
6. **Spreads.** The day-to-day spreads of the mean and of the range, in °C, are linear in what the climate table knows: annual range, diurnal range, wet-day fraction, season (the day's mean against the year's, -1 to 1), season times annual range, and |latitude|. Clamped to 0.2 to 12 °C.
7. **Form.** Snow with probability logistic(0.463 - 0.606 T) on the daily mean temperature (one half at 0.8 °C); snow with a daily mean above 1 °C is reported as sleet. Snowfall is 1 cm per mm of water for snow, half that for sleet.
8. **Thunder** on a rain or sleet day with probability logistic(-5.10 + 0.121 Tmax + 0.343 ln mm).
9. **Cloud, humidity, wind.** Wet days cover `0.6` of the clear sky left on top of the month's mean cloud and dry days less, so the monthly mean stays, and the day's cover is Beta with concentration 4. Humidity is normal around the monthly mean, `10` points higher on wet days (lower on dry days to keep the mean), spread `7` points, clamped to 5 to 100. Wind is Weibull with shape 2.90 around the monthly mean.
10. **The sun.** Sunrise and sunset by NOAA's solar calculator equations (after Meeus): the sun's declination and the equation of time at the point's local mean noon, and the hour angle at which the sun's centre is 0.833° below the horizon (refraction and the sun's radius). Where that hour angle does not exist, the day is polar day or night (24 or 0 hours, no sunrise). Sunshine hours are the daylight times the clear share of the sky, 1 - cloud cover, a simplification (curated): real sunshine falls faster than cloud rises near the horizon and under thin cloud.
11. **Elevation and warming.** Temperatures move by 6.5 °C per km between the requested and the cell's elevation, and by `warming`.
12. **Historical months** (`historical`). Step 2's drawn anomalies are replaced by the month as it was: its temperature anomaly is added to the mean, the log ratio of its wet-day odds to the normal's scales the odds of a wet day, and the log ratio of its amount per wet day (precipitation ratio over wet-day ratio, clamped to ±3) scales the wet-day amount. Like the normals they are interpolated between mid-months through control points; the controls are the months filtered by the inverse of the `(1, 6, 1) / 8` interpolation, √2 r^|k| with r = 2√2 - 3 over 12 months each way, so a control does not depend on the days a call asks for. Months outside 1991-2020 have zero anomaly. The residual keeps its within-month share, sqrt(1 - 0.48² v).
13. **Hours** (`hourly`; curated). From the day, its neighbours and the day's own `hours` stream, in local mean solar time:
    - Temperature after Cesaraccio et al. (2001): a sine from Tmin at sunrise to Tmax at the warmest hour (max(sunset - 4 h, the middle of the day)), a sine down to the sunset temperature Tmax - 0.39 (Tmax - next Tmin), and a square-root fall to the next Tmin at the next sunrise; the hours before sunrise finish the night from the day before's sunset. Without a sunrise, a cosine between Tmin and Tmax, coolest at 03:00.
    - Precipitation: a gamma (shape 2) number of wet hours with mean 2 + 2.5 ln(1 + mm), halved with thunder, rounded and at least 1; in one spell, or with probability 0.3 (not with thunder) two. Thunder spells are centred on a normal draw around 15:00. Each wet hour's weight is an exponential draw; the day's tenths of a mm are shared by weight with every wet hour given at least one, and the rest by largest remainder, so the hours sum to the day. Snowfall is shared the same way.
    - Cloud: logistic(logit of the day's cover + an AR(1) normal, lag 0.8), at least 0.85 in a wet hour. Wind: the day's wind times (1 + 0.25 cos(15° (h - 14))) times log-normal AR(1) noise (sd 0.2, lag 0.8), scaled so the hours average to the day's wind.
    - Humidity: the day's vapour pressure (humidity times the saturation pressure at Tmean, Magnus) over each hour's saturation pressure, clamped to 5 to 100, at least 90 in a wet hour. Sunshine: the day's hours shared by each hour's daylight times its clear sky, no hour above its daylight.
    - None of these is fitted on hourly records: they are shapes from the literature and general climatology, consistent with the day by construction.
14. **Imperial units** (`units`). The unrounded temperatures, amount and wind are converted (°F = 1.8 °C + 32; 25.4 mm per inch; 0.44704 m/s per mph) and rounded to 0.1 °F, 0.01 in, 0.1 in of snow and 0.1 mph. Nothing else changes.

### Determinism and series

A day must not depend on where a series started. Each day of each place has its own random stream (`child_seed_of(place seed, day number)`), and a call reads 48 days before its first:

- The rain chain is read back to the latest day whose uniform draw decides the state whatever the day before was (below P(wet | dry), wet; above P(wet | wet), dry), and run forward from there. Within 48 days this fails with probability below 1e-20; then the chain starts from f.
- The temperature residual is the sum of A^k B e(t - k) over the 48 days (A^48 is below 1e-7).
- Month anomalies come from the place's month stream by month number; historical months from the table, through controls of a fixed reach.
- A day's hours come from its own stream (`hours`) and its neighbours' values, which a call simulates one day either side of the days it returns.

So a day alone, a day inside any series and a day in an overlapping series are equal, which the tests check. The place seed is the call seed split by the cell key, so points in one cell share weather and different cells do not.

All arithmetic is IEEE `+ - * /` and `sqrt` with contraction fenced, and dasmig-core's `exp`, `log` and degree trigonometry (for the cell search, the land distance, the sun and the hours), so results are identical on every compiler and CPU (known-answer tests at -O0 to -O3, with `-ffp-contract=fast` and `-march=native`, on GCC and Clang).

## Constants and Their Sources

Fitted by `scripts/calibrate.py fit` on **GHCN-Daily** (Menne et al. 2012, NOAA NCEI), 1991-2020, on the stations of `scripts/ghcn_stations.txt`: one per 10-degree box, the longest TMAX, TMIN and PRCP record covering the period. 301 stations are listed; 162 have at least 15 years of complete days and a climate cell nearby and are used. Each station's statistics are taken against the `full` cell's normals, so the constants map what the table has to what the days do.

| Constant | Value | Fit |
| --- | --- | --- |
| Persistence d | 0.277 + 0.006 f (code: 0.270 + 0.006 f, tuned) | 1,855 station-months, R² 0.00, RMSE 0.10: d does not depend on f (median 0.28; Geng et al. 1986 found 0.25) |
| Gamma shape | 1.146 - 0.224 ln(mean mm) | 1,837 station-months, R² 0.49, RMSE 0.18; median shape 0.77 |
| Wet-day shifts | mean 0.032, range 0.220 of dtr (code: mean 0.016, tuned) | 1,871 station-months |
| Spread of the daily mean | 7 coefficients | R² 0.78, RMSE 0.86 °C |
| Spread of the daily range | 7 coefficients | R² 0.64, RMSE 0.62 °C; median CV 0.35 |
| A, B | A = [[0.695, 0.068], [-0.052, 0.307]], B = [[0.711, 0], [0.100, 0.944]] | 1.4 million day pairs; lag-1 correlation of the daily mean 0.70 |
| Wind Weibull shape | 2.90 | 209,649 days (AWND, any years: few stations report wind before the 1980s); CV of daily over monthly mean 0.37 |
| Snow | logit p = 0.463 - 0.606 T | 87,850 wet days with SNOW reported |
| Thunder | logit p = -5.10 + 0.121 Tmax + 0.343 ln mm | 257,694 wet days at US stations, whose WT03 reports are consistent (elsewhere thunder is rarely reported in GHCN-Daily): the fit list's and the 148 of `scripts/ghcn_thunder.txt`, one per 3-degree box with 20 or more years of WT03, away from the holdout stations; thunder on 12.6% of them |

**Tuned** with `scripts/validate.py`, on the fit stations only, to correct what the daily fits cannot see once all parts run together:

- Persistence lowered from 0.277 to 0.270: the month anomalies of the wet-day odds also cluster wet days, which raises the generated persistence.
- The wet-day shift of the mean (0.016 of the diurnal range): set so the generated dry-minus-wet Tmax matches the stations once the residuals and anomalies are added.
- The month anomalies (0.48, 0.32, 0.28): set so the year-to-year spread of monthly Tmax, wet days and totals matches.

**Curated**: everything in the hours (step 13); the cloud and humidity contrasts of wet days (0.6, 10 points) and their spreads (Beta concentration 4, 7 points), from general climatology, since GHCN-Daily has no cloud or humidity; sunshine as daylight times the clear sky; the sleet threshold (snow with a daily mean above 1 °C), the 10:1 snow ratio, and the standard-atmosphere lapse rate (6.5 °C per km).

## Validation

`scripts/validate.py` compares 30 generated years (`scripts/sample_weather.cpp`, at each station's point and elevation) with each station's 1991-2020 days, month by month. A station counts with at least 15 years of complete days (Tmax, Tmin and precipitation), a station-month with at least 300 of them. "Bias" is the median of generated minus observed over station-months, "|err|" the median absolute difference. For Tmax, Tmin, wet days and totals, "vs cell" compares the generated value with the nearest `full` cell's normal (temperatures moved to the station's elevation), which separates the model's error from the normals' error at the station.

The spreads (sd) are of the daily values around the seasonal cycle (three harmonics) and the wet or dry mean; year-to-year spreads use years with at least 25 days of the month; the longest dry spell is the mean over years of each year's longest run of dry days. Snow shares count station-months with snow reported, and thunder shares US stations with at least 100 thunder reports (WT03).

**Holdout stations**, the independent check: `scripts/ghcn_holdout.txt`, per 10-degree box the longest record among stations that are not on the fit list and at least 100 km from its station. 237 are listed, 119 have the data (1,428 station-months, 193 with snow, 72 with thunder reports, 115 dry-spell values). Nothing was fitted or tuned on them.

| Metric | Observed median | `full` bias | `full` \|err\| | `lite` bias | `lite` \|err\| |
| --- | --- | --- | --- | --- | --- |
| Tmax, °C (vs cell) | 14.69 | +0.04 (-0.02) | 0.59 (0.23) | +0.03 (-0.05) | 0.75 (0.54) |
| Tmin, °C (vs cell) | 5.06 | -0.07 (+0.00) | 0.70 (0.22) | -0.10 (-0.02) | 0.87 (0.49) |
| Wet-day fraction (vs cell) | 0.387 | -0.006 (+0.000) | 0.044 (0.015) | +0.001 (+0.004) | 0.047 (0.020) |
| Monthly total, mm (vs cell) | 42.4 | +0.33 (-0.16) | 5.88 (2.84) | +0.98 (+0.25) | 7.15 (4.45) |
| Wet-day amount, mm | 4.05 | +0.09 | 0.64 | +0.11 | 0.69 |
| 95th percentile of wet days, mm | 14.6 | -0.44 | 2.68 | -0.46 | 2.82 |
| Persistence d | 0.279 | +0.005 | 0.065 | +0.009 | 0.065 |
| sd of daily Tmax, °C | 4.16 | +0.04 | 0.68 | +0.05 | 0.62 |
| sd of daily Tmin, °C | 3.60 | +0.08 | 0.58 | +0.14 | 0.59 |
| sd of the daily mean, °C | 3.51 | +0.03 | 0.58 | +0.03 | 0.57 |
| sd of the daily range, °C | 3.35 | -0.01 | 0.43 | -0.02 | 0.46 |
| Correlation of daily mean and range | 0.081 | -0.014 | 0.247 | -0.020 | 0.244 |
| Lag-1 correlation of Tmax | 0.716 | -0.061 | 0.086 | -0.063 | 0.087 |
| Dry-day minus wet-day Tmax, °C | 1.32 | -0.09 | 1.41 | -0.11 | 1.38 |
| Year-to-year sd of monthly Tmax, °C | 1.91 | +0.02 | 0.40 | +0.01 | 0.40 |
| Year-to-year CV of monthly totals | 0.576 | -0.027 | 0.116 | -0.031 | 0.116 |
| Year-to-year sd of the wet-day fraction | 0.117 | -0.003 | 0.019 | -0.001 | 0.019 |
| Longest dry spell of the year, days | 19.7 | -1.20 | 3.47 | -2.04 | 3.55 |
| Snow share of wet days | 0.463 | +0.018 | 0.053 | +0.018 | 0.055 |
| Thunder share of wet days | 0.087 | -0.006 | 0.042 | -0.014 | 0.035 |

**Fit stations** (the stations the constants were fitted and tuned on; `--list` defaults to them): 162 stations, 1,943 station-months.

| Metric | Observed median | `full` bias | `full` \|err\| | `lite` bias | `lite` \|err\| |
| --- | --- | --- | --- | --- | --- |
| Tmax, °C (vs cell) | 13.66 | +0.04 (+0.00) | 0.60 (0.23) | +0.06 (+0.01) | 0.76 (0.45) |
| Tmin, °C (vs cell) | 4.74 | -0.13 (+0.01) | 0.72 (0.22) | -0.13 (-0.01) | 0.84 (0.42) |
| Wet-day fraction (vs cell) | 0.404 | -0.013 (+0.000) | 0.045 (0.014) | -0.006 (+0.002) | 0.046 (0.020) |
| Monthly total, mm (vs cell) | 48.3 | +0.43 (-0.16) | 5.74 (3.17) | +0.60 (-0.02) | 6.83 (4.36) |
| Wet-day amount, mm | 4.44 | +0.23 | 0.69 | +0.15 | 0.72 |
| 95th percentile of wet days, mm | 15.6 | -0.30 | 2.54 | -0.44 | 2.98 |
| Persistence d | 0.278 | +0.007 | 0.068 | +0.003 | 0.066 |
| sd of daily Tmax, °C | 3.96 | -0.02 | 0.63 | -0.02 | 0.61 |
| sd of daily Tmin, °C | 3.47 | +0.11 | 0.53 | +0.16 | 0.55 |
| sd of the daily mean, °C | 3.35 | -0.00 | 0.53 | +0.04 | 0.52 |
| sd of the daily range, °C | 3.10 | +0.03 | 0.39 | +0.04 | 0.39 |
| Correlation of daily mean and range | 0.103 | -0.036 | 0.242 | -0.036 | 0.238 |
| Lag-1 correlation of Tmax | 0.713 | -0.060 | 0.086 | -0.059 | 0.086 |
| Dry-day minus wet-day Tmax, °C | 0.92 | +0.01 | 1.33 | +0.05 | 1.35 |
| Year-to-year sd of monthly Tmax, °C | 1.88 | -0.00 | 0.40 | +0.02 | 0.40 |
| Year-to-year CV of monthly totals | 0.580 | -0.007 | 0.113 | -0.017 | 0.111 |
| Year-to-year sd of the wet-day fraction | 0.121 | -0.003 | 0.019 | -0.004 | 0.020 |
| Longest dry spell of the year, days | 18.0 | -0.33 | 3.31 | -1.71 | 3.54 |
| Snow share of wet days | 0.502 | +0.006 | 0.029 | +0.006 | 0.028 |
| Thunder share of wet days | 0.090 | +0.001 | 0.043 | +0.000 | 0.036 |

What this says:

- **Means** of temperature, wet days and totals are those of the cell (the "vs cell" biases are within 0.05 °C, 0.004 and 0.3 mm). The error left at a station is mostly the cell's normals against the station's own: a cell mean over terrain the station is one point of. That is why `lite` is worse than `full`, and why its "vs cell" errors, against the `full` cell, are larger.
- **Day-to-day behaviour** (persistence, amounts, spreads, the wet-day contrast, snow and thunder shares) and **year-to-year spread** have biases of a few percent, on the holdout stations as on the fit stations.
- **Known shortfalls**: the longest dry spell is 6% short on the holdout stations (10% in `lite`), because a first-order chain forgets a dry spell too quickly (a higher-order or semi-Markov chain would fix it); the wettest days are 0.4 mm (3%) light; Tmin's spread is 2 to 5% high; the lag-1 correlation of Tmax is 0.06 low; and the holdout stations' wet days are 0.09 °C too warm against their dry days, relative to a contrast of 1.3 °C.

## Rebuilding the Data

```bash
# 1. CRU TS 4.10: cru_ts4.10.{1991.2000,2001.2010,2011.2020}.{tmp,dtr,pre,wet,cld,vap}.dat.gz
#    and the {tmp,pre,wet}.stn.gz station counts, from
#    https://crudata.uea.ac.uk/cru/data/hrg/cru_ts_4.10/cruts.2604091129.v4.10/<var>/
#    into CRUTS_DIR.
#    CRU CL 2.0: grid_10min_{elv,wnd,reh}.dat.gz from
#    https://crudata.uea.ac.uk/cru/data/hrg/tmc/ into CRUCL_DIR.
#    Natural Earth: ne_10m_land.zip and ne_10m_minor_islands.zip from
#    https://www.naturalearthdata.com/downloads/10m-physical-vectors/, unzipped into NE_DIR.
#    prepare_climate.py imports numpy; run it without -I.
python scripts/prepare_climate.py CRUTS_DIR CRUCL_DIR NE_DIR --tier all --out resources
python scripts/prepare_history.py CRUTS_DIR CRUCL_DIR NE_DIR --out resources   # history/history.tsv
make embed

# 2. GHCN-Daily: ghcnd-inventory.txt and ghcnd-stations.txt from
#    https://www.ncei.noaa.gov/pub/data/ghcn/daily/ into GHCN_DIR.
python scripts/calibrate.py select GHCN_DIR    # scripts/ghcn_stations.txt
python scripts/calibrate.py holdout GHCN_DIR   # scripts/ghcn_holdout.txt
python scripts/calibrate.py thunder GHCN_DIR   # scripts/ghcn_thunder.txt
python scripts/calibrate.py fetch GHCN_DIR
python scripts/calibrate.py fetch GHCN_DIR --list scripts/ghcn_holdout.txt
python scripts/calibrate.py fetch GHCN_DIR --list scripts/ghcn_thunder.txt
python scripts/calibrate.py fit GHCN_DIR       # prints the constants for dasmig/weathergen.hpp

# 3. Validation, for a tier and a station list.
python scripts/validate.py stations GHCN_DIR build/stations.tsv --list scripts/ghcn_holdout.txt
clang++ -std=c++23 -O2 -I. -o build/sample_weather scripts/sample_weather.cpp
build/sample_weather full build/stations.tsv build/sample.tsv
python scripts/validate.py compare GHCN_DIR resources/full/climate.tsv build/sample.tsv --list scripts/ghcn_holdout.txt
```

The station lists are fixed by the inventory file; NCEI updates GHCN-Daily, so a later inventory can pick other stations. A change of the climate tables changes `data_hash()` and every known-answer test.

## Licences

The climate tables and `history.tsv` are adapted from CRU TS 4.10 and CRU CL 2.0, which the Climatic Research Unit releases under the Open Database License with attribution and share-alike: weather generated from them is a Produced Work that needs a notice, and the tables themselves, or tables built from them, must stay under the ODbL. The land mask is from Natural Earth, in the public domain. GHCN-Daily records are not shipped; only constants fitted on them. See [`LICENSE_DATA.txt`](../LICENSE_DATA.txt).
