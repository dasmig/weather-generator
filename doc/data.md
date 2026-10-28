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
2. **Month anomalies.** Each place and calendar month (every January 2007 its own) draws three standard normal anomalies, interpolated between mid-months like the normals. The first shifts the temperature by `0.40` of the day's spread. The second scales the odds of a wet day (log sd `0.32`) and the wet-day amount (log sd `0.28`), with corrections that keep their means; scaling the odds rather than the fraction keeps a very wet month's fraction below 1 without losing wet days. The third scales the diurnal range by `0.32` of the range's spread and is correlated `-0.3` with the second (cloudy wet months have small ranges). They give the year-to-year spread of monthly means that a daily chain alone underestimates (the "overdispersion" problem of weather generators).
3. **Wet or dry.** A semi-Markov chain keeps the day's wet-day fraction f. After a wet day, P(wet) = P(wet | dry) + d, with persistence d = 0.270 + 0.006 f. After L dry days, P(wet) = q h(L), with h(L) = 1 - 0.1 (1 - exp(-(L - 1) / 2)): rain comes back a little sooner after one dry day than after a week, which lengthens the longest dry spells while q, solved per day, keeps the mean dry spell 1 / P(wet | dry) of the first-order chain and so the wet-day fraction. Runs of 16 dry days or more share h(16).
4. **Amount.** A wet day's amount is Weibull with the month's mean wet-day amount (precipitation / wet days) and shape 1.000 - 0.119 ln(mean mm), clamped to 0.4 to 1.5: light-rain climates have more even amounts, heavy-rain climates more skewed ones. It is drawn by inversion from a standard normal that is `0.7` how deep in the rain the day is (the chain's uniform against the day's chance of rain, so the wettest days are those well inside a rain area) and the rest the amount's own draw. At least 0.1 mm. Weibull, with shapes fitted by maximum likelihood, replaces 1.x's gamma; with the rain depth, the wettest days are no longer light (see Validation).
5. **Rain classes.** A day falls in one of six classes by its own rain and its neighbours': wet after a dry day, wet after a wet day, dry after a wet day, dry before a wet day, dry between wet days, and dry between dry days (the reference). Each class shifts the daily mean by a share of the diurnal range and scales the range by 1 + a share, each share linear in what the climate table knows (season, wet-day fraction, |latitude|, annual range, diurnal range, cloud, humidity). Fronts bring warm days before the rain and cold days after it; wet days have small ranges and warm nights; a cloudy winter's wet days are warmer than its dry ones. The shifts are centred on the normal month (how often each class comes, from the chain at the normal wet-day fraction), so the monthly normals stay; historical days centre on the month's own fraction, since the history's temperature already has what its wet days did.
6. **Temperature.** The daily mean temperature and the daily range have standardized residuals from a lag-1 autoregression, x(t) = A x(t-1) + B e(t) (Richardson's A and B, fitted). The mean is the season's plus the class shift, plus the residual and the month anomaly times the day's spread (split so the total spread is the fitted one). The range is log-normal around the season's range times the class factor, its spread split the same way, so it is never negative. Tmax and Tmin are the mean plus and minus half the range; `tmean` is halfway, and `frost` is a Tmin below 0.
7. **Spreads.** The day-to-day spreads of the mean and of the range, in °C, are linear in what the climate table knows: annual range, diurnal range, wet-day fraction, season (the day's mean against the year's, -1 to 1), season times annual range, and |latitude|. They are spreads about the class shifts. Clamped to 0.2 to 12 °C.
8. **Form.** Snow with probability logistic(0.463 - 0.606 T) on the daily mean temperature (one half at 0.8 °C); snow with a daily mean above 1 °C is reported as sleet. Snowfall is 1 cm per mm of water for snow, half that for sleet.
9. **Thunder** on a rain or sleet day with probability logistic(-5.10 + 0.121 Tmax + 0.343 ln mm). **Drizzle** on a rain day without thunder above 0 °C with probability logistic(-4.86 + 0.419 x - 0.117 x² - 0.073 ln mm + 4.19 h), x = Tmax / 10, h the month's humidity / 100; a light rain day (below 2.5 mm) with drizzle is called drizzle.
10. **Snow on the ground** (`snow_depth`). The day's snow water (all of snow, half of sleet) adds to the pack; the pack melts by 3.5 mm of water per °C of daily mean above 0, new snow has a density of 100 kg/m³ and settles towards 350 kg/m³ by 1% a day, 26% a day while it melts. The depth is water over density. A day whose normal mean temperature (after elevation and warming) is 10 °C or more ends with no snow, so a day's snow depends only on the days since the last such day, which the call simulates (temperatures and snow only) from there; where no day of the year is that warm, the season starts in the middle of the warmest month.
11. **Cloud, humidity, wind.** Wet days cover `0.6` of the clear sky left on top of the month's mean cloud and dry days less, so the monthly mean stays, and the day's cover is Beta with concentration 4. Humidity is normal around the monthly mean, `10` points higher on wet days (lower on dry days to keep the mean), spread `7` points, clamped to 5 to 100. Wind is Weibull with shape 2.90 around the monthly mean.
12. **Gusts.** The day's strongest gust (a 5-second mean, as US stations report) is the wind times exp(1.679 - 0.490 ln wind + 0.227 thunder + 0.231 z), z standard normal, and at least 1.2 times the wind: in the median a 2 m/s day gusts to 3.8 times its mean wind, a 10 m/s day to 1.7 times.
13. **Fog.** Fog at some time of the day with probability logistic of the month's humidity, the day's range against the season's, rain and its amount, the season (1 at midwinter) and the day's wind against the month's (coefficients in `dasmig/weathergen.hpp`); heavy fog, a subset, by a second logistic. A dry day with heavy fog is called fog. In the hours, a foggy day's fog lasts from 3 hours before sunrise to 3 hours after it.
14. **The sun.** Sunrise and sunset by NOAA's solar calculator equations (after Meeus): the sun's declination and the equation of time at the point's local mean noon, and the hour angle at which the sun's centre is 0.833° below the horizon (refraction and the sun's radius). Where that hour angle does not exist, the day is polar day or night (24 or 0 hours, no sunrise). Sunshine hours are the daylight times (1 - cloud)^0.637: sunshine falls faster than cloud rises under thin and broken cloud, which 1.x's 1 - cloud missed. Solar radiation is FAO-56's extraterrestrial radiation on the day at the point's latitude times 0.25 + 0.50 times the sunny share (Angström-Prescott, FAO-56's default coefficients), MJ/m².
15. **Spatial correlation** (`spatial`, on by default). Each random part of the weather (the rain's uniform, the amount's normal, the two temperature innovations and the three month anomalies) is a regional field plus a local part. A field is the sum of two scales, each the smoothstep-bilinear interpolation of standard normals on nodes a scale's spacing apart (rows that far apart in latitude, each row with as many nodes as keep them that far apart along it), with weights whose squares add up to 1, times the scale's weight; the local part makes up the variance. So every place still has the same distribution, and places share more of their weather the nearer they are. The nodes' normals come from the call's own stream, so a seed is one world. Spacings and weights (degrees, weight): rain 3.5 and 0.47, 8.6 and 0.74; amount 2.9 and 0.36, 7.8 and 0.33; daily mean 5 and 0.40, 16 and 0.86; daily range 4.2 and 0.49, 10.3 and 0.62; month temperature 6 and 0.30, 24 and 0.95; month precipitation 3 and 0.60, 9.8 and 0.78. With `spatial` off, the fields are empty and each cell has weather of its own, as in 1.x.
16. **Elevation and warming.** Temperatures move by 6.5 °C per km between the requested and the cell's elevation, and by `warming`.
17. **Historical months** (`historical`). Step 2's drawn anomalies are replaced by the month as it was: its temperature anomaly is added to the mean, the log ratio of its wet-day odds to the normal's scales the odds of a wet day, and the log ratio of its amount per wet day (precipitation ratio over wet-day ratio, clamped to ±3) scales the wet-day amount; the range has no anomaly. Like the normals they are interpolated between mid-months through control points; the controls are the months filtered by the inverse of the `(1, 6, 1) / 8` interpolation, √2 r^|k| with r = 2√2 - 3 over 12 months each way, so a control does not depend on the days a call asks for. Months outside 1991-2020 have zero anomaly. The residuals keep their within-month shares, sqrt(1 - 0.40² v) and sqrt(1 - 0.32² v).
18. **Hours** (`hourly`; curated). From the day, its neighbours and the day's own `hours` stream, in local mean solar time:
    - Temperature after Cesaraccio et al. (2001): a sine from Tmin at sunrise to Tmax at the warmest hour (max(sunset - 4 h, the middle of the day)), a sine down to the sunset temperature Tmax - 0.39 (Tmax - next Tmin), and a square-root fall to the next Tmin at the next sunrise; the hours before sunrise finish the night from the day before's sunset. Without a sunrise, a cosine between Tmin and Tmax, coolest at 03:00.
    - Precipitation: a gamma (shape 2) number of wet hours with mean 2 + 2.5 ln(1 + mm), halved with thunder, rounded and at least 1; in one spell, or with probability 0.3 (not with thunder) two. Thunder spells are centred on a normal draw around 15:00. Each wet hour's weight is an exponential draw; the day's tenths of a mm are shared by weight with every wet hour given at least one, and the rest by largest remainder, so the hours sum to the day. Snowfall is shared the same way.
    - Cloud: logistic(logit of the day's cover + an AR(1) normal, lag 0.8), at least 0.85 in a wet hour. Wind: the day's wind times (1 + 0.25 cos(15° (h - 14))) times log-normal AR(1) noise (sd 0.2, lag 0.8), scaled so the hours average to the day's wind.
    - Humidity: the day's vapour pressure (humidity times the saturation pressure at Tmean, Magnus) over each hour's saturation pressure, clamped to 5 to 100, at least 90 in a wet hour. Sunshine: the day's hours shared by each hour's daylight times its sunny share, no hour above its daylight. Fog: around sunrise on a foggy day (step 13); a dry hour in heavy fog is called fog, a light rain hour (below 0.5 mm) on a drizzly day drizzle.
    - None of these is fitted on hourly records: they are shapes from the literature and general climatology, consistent with the day by construction.
19. **Imperial units** (`units`). The unrounded temperatures, amount, snow depth, wind and gust are converted (°F = 1.8 °C + 32; 25.4 mm per inch; 0.44704 m/s per mph) and rounded to 0.1 °F, 0.01 in, 0.1 in of snowfall and snow depth, and 0.1 mph. Solar radiation stays in MJ/m². Nothing else changes. JSON always names the unit system (`"units"`).

### Determinism and series

A day must not depend on where a series started. Each day of each place has its own random stream (`child_seed_of(place seed, day number)`), and a call reads days before its first:

- The rain chain's state on the day before the first day is found by coupling from the past: every state is run forward from 48 days before with the same uniforms, and once all runs have met, the state no longer depends on anything earlier. If they have not met, the search looks twice as far back, up to 4,096 days (in practice it never needs to). The days are the ones the temperature residual reads anyway.
- The temperature residual is the sum of A^k B e(t - k) over the 48 days (A^48 is below 1e-7).
- Snow on the ground is simulated from the day after the last day too warm for it (step 10), up to a year before; those days are worked out only as far as their snow needs.
- Month anomalies come from the place's month stream by month number and from the fields; historical months from the table, through controls of a fixed reach.
- The fields' node normals are drawn by day or month number from the call's stream, so a place's field value does not depend on what else the call asked for.
- A day's hours come from its own stream (`hours`) and its neighbours' values, which a call simulates one day either side of the days it returns.

So a day alone, a day inside any series and a day in an overlapping series are equal, which the tests check. The place seed is the call seed split by the cell key, so points in one cell share weather; different cells share the regional fields and nothing else.

All arithmetic is IEEE `+ - * /` and `sqrt` with contraction fenced, and dasmig-core's `exp`, `log` and degree trigonometry (for the cell search, the land distance, the sun, the radiation and the hours); the normal quantile is Acklam's rational approximation and its tail Hart's, both in fenced arithmetic, and the node normals come from a fixed 4,096-entry quantile table. So results are identical on every compiler and CPU (known-answer tests at -O0 to -O3, with `-ffp-contract=fast` and `-march=native`, on GCC and Clang).

### Cost

Snow on the ground and spatial correlation make a single day dearer than in 1.x. `make bench` on the development machine (Clang 22 -O2, `full`), 2.0 against 1.x:

| Call | 2.0 | 1.x |
| --- | --- | --- |
| a day at a random place, mean (median, 99th percentile) | 95 µs (58, 560) | 23 µs (24, 45) |
| a day in Paris on 1 July | 56 µs | 22 µs |
| a year in Paris as one series | 1.7 ms | 1.1 ms |
| a batch of single days, one thread | 10,200 a second | 44,100 a second |

The tail is the snow: in winter a call simulates the snow season so far, up to a year in places that are never snow-free, where 1.x read 48 days. Those days are worked out only as far as their snow needs (mean temperatures and snowfall), and a series reads them once, so a series costs only half as much again as in 1.x. The fields cost about a third of a snow-free day (Paris's day takes 31 to 37 µs with `spatial` off, against 48 to 55 µs on, in a separate run).

## Constants and Their Sources

Fitted by `scripts/calibrate.py fit` on **GHCN-Daily** (Menne et al. 2012, NOAA NCEI), 1991-2020, on the stations of `scripts/ghcn_stations.txt`: one per 10-degree box, the longest TMAX, TMIN and PRCP record covering the period. 301 stations are listed; 162 have at least 15 years of complete days and a climate cell nearby and are used. Each station's statistics are taken against the `full` cell's normals, so the constants map what the table has to what the days do. `scripts/fit_extras.py` fits what `calibrate.py` does not (snow on the ground, sunshine, gusts, fog, drizzle) on all three station lists' records, or on the CRU grids, and measures dry spells and correlation by distance.

| Constant | Value | Fit |
| --- | --- | --- |
| Persistence d | 0.277 + 0.006 f (code: 0.270 + 0.006 f, tuned) | 1,855 station-months, R² 0.00, RMSE 0.10: d does not depend on f (median 0.28; Geng et al. 1986 found 0.25) |
| Dry-spell hazard | h(L) = 1 - 0.1 (1 - exp(-(L - 1) / 2)) (tuned) | set so the fit stations' longest dry spell matches; the hazards it gives are below (`fit_extras.py spells`) |
| Weibull shape | 1.000 - 0.119 ln(mean mm) | maximum likelihood per station-month, 1,837 station-months, R² 0.44, RMSE 0.10; median shape 0.81 |
| Rain classes | 5 x 8 coefficients each for the mean shift and the range factor | least squares on each station-month's class means against its dry-between-dry days, 1,375 station-months; medians: wet days 0.02 (after dry) and -0.06 (after wet) of the diurnal range, dry days before rain +0.04, after rain -0.06; ranges 17 to 28% smaller on wet days |
| Spread of the daily mean | 7 coefficients (intercept -1.201, code -1.26, tuned) | about the class shifts; R² 0.81, RMSE 0.82 °C |
| Spread of the daily range | 7 coefficients (intercept -0.120, code -0.17, tuned) | R² 0.67, RMSE 0.55 °C; median CV 0.35 |
| A, B | A = [[0.704, 0.064], [-0.049, 0.298]], B = [[0.703, 0], [0.092, 0.949]] | 1.1 million day pairs, class shifts removed; lag-1 correlation of the daily mean 0.71 |
| Wind Weibull shape | 2.90 | 209,649 days (AWND, any years: few stations report wind before the 1980s); CV of daily over monthly mean 0.37 |
| Snow | logit p = 0.463 - 0.606 T | 87,850 wet days with SNOW reported |
| Thunder | logit p = -5.10 + 0.121 Tmax + 0.343 ln mm | 257,694 wet days at US stations, whose WT03 reports are consistent (elsewhere thunder is rarely reported in GHCN-Daily): the fit list's and the 148 of `scripts/ghcn_thunder.txt`, one per 3-degree box with 20 or more years of WT03, away from the holdout stations; thunder on 12.6% of them |
| Snowpack | melt 3.5 mm/°C/day, old snow 350 kg/m³, settling 0.01 and 0.3 a day | grid search driving the pack with each station's own weather against its snow depth (SNWD), 191 stations, 1,274 station-months with snow: cover 0.625 observed, -0.016 modelled; depth -0.3 cm |
| Sunshine | (1 - C)^0.637 | CRU CL 2.0 monthly sunshine (`sunp`, 1961-1990) against the full tier's cloud, 670,776 cell-months below 66°, over the generator's daily cloud: RMSE 0.103 of the possible sunshine (1 - C: 0.147) |
| Gusts | ln(gust / wind) = 1.679 - 0.490 ln wind + 0.227 thunder, residual sd 0.231 | 445,644 days with AWND and WSF5 |
| Fog, heavy fog | 7 coefficients each | logistic regression on 819,474 days of US stations reporting fog (WT01, WT02) in the years they report it: the slopes where wind is reported too, the intercepts refitted on all of them, since the stations with wind (mostly automated airports) report fog more often; fog on 26% of days, heavy fog on 3% |
| Drizzle | 5 coefficients | logistic regression on 25,895 rain days without thunder above 0 °C at stations reporting drizzle (WT14) |

**Tuned** with `scripts/validate.py`, on the fit stations only, to correct what the daily fits cannot see once all parts run together:

- Persistence lowered from 0.277 to 0.270: the month anomalies of the wet-day odds also cluster wet days, which raises the generated persistence.
- The dry-spell hazard (0.1, 2 days): set so the longest dry spell of the year matches.
- The spreads' intercepts (-1.26, -0.17): the class shifts, predicted rather than each station's own, add a little spread.
- The month anomalies (0.40, 0.32, 0.28, the range's 0.32 and -0.3): set so the year-to-year spread of monthly Tmax, wet days, totals and ranges matches.
- The amount's share of the rain depth (0.7) and the fields' spacings and weights: set so the correlations by distance of daily occurrence, amounts, temperatures and monthly means match the stations' (below).

**Curated**: everything in the hours (step 18); the cloud and humidity contrasts of wet days (0.6, 10 points) and their spreads (Beta concentration 4, 7 points), from general climatology, since GHCN-Daily has no cloud or humidity; the Angström-Prescott coefficients (FAO-56's defaults); the snow-free threshold (a normal daily mean of 10 °C) and the density of new snow (100 kg/m³); the fog's hours; the sleet threshold (snow with a daily mean above 1 °C), the 10:1 snow ratio, and the standard-atmosphere lapse rate (6.5 °C per km).

## Validation

`scripts/validate.py` compares 30 generated years (`scripts/sample_weather.cpp`, at each station's point and elevation, one seed for all stations) with each station's 1991-2020 days, month by month. A station counts with at least 15 years of complete days (Tmax, Tmin and precipitation), a station-month with at least 300 of them. "Bias" is the median of generated minus observed over station-months, "|err|" the median absolute difference. For Tmax, Tmin, wet days and totals, "vs cell" compares the generated value with the nearest `full` cell's normal (temperatures moved to the station's elevation), which separates the model's error from the normals' error at the station.

The spreads (sd) are of the daily values around the seasonal cycle (three harmonics) and the wet or dry mean; year-to-year spreads use years with at least 25 days of the month; the longest dry spell is the mean over years of each year's longest run of dry days. Snow shares count station-months with snow reported, and thunder shares US stations with at least 100 thunder reports (WT03).

**Holdout stations**, the independent check: `scripts/ghcn_holdout.txt`, per 10-degree box the longest record among stations that are not on the fit list and at least 100 km from its station. 237 are listed, 119 have the data (1,428 station-months, 197 with snow, 72 with thunder reports, 115 dry-spell values). Nothing was fitted or tuned on them.

| Metric | Observed median | `full` bias | `full` \|err\| | `lite` bias | `lite` \|err\| |
| --- | --- | --- | --- | --- | --- |
| Tmax, °C (vs cell) | 14.69 | +0.08 (-0.01) | 0.60 (0.23) | +0.02 (-0.04) | 0.75 (0.54) |
| Tmin, °C (vs cell) | 5.06 | -0.05 (+0.01) | 0.68 (0.23) | -0.08 (-0.02) | 0.88 (0.49) |
| Wet-day fraction (vs cell) | 0.387 | -0.007 (-0.001) | 0.044 (0.015) | +0.000 (+0.006) | 0.051 (0.020) |
| Monthly total, mm (vs cell) | 42.4 | +0.36 (-0.08) | 6.41 (2.90) | +0.82 (+0.29) | 7.08 (4.52) |
| Wet-day amount, mm | 4.04 | +0.13 | 0.71 | +0.12 | 0.73 |
| 95th percentile of wet days, mm | 14.6 | +0.08 | 2.69 | +0.03 | 2.85 |
| Persistence d | 0.279 | +0.005 | 0.068 | +0.003 | 0.066 |
| sd of daily Tmax, °C | 4.16 | +0.09 | 0.65 | +0.10 | 0.67 |
| sd of daily Tmin, °C | 3.60 | +0.07 | 0.58 | +0.10 | 0.57 |
| sd of the daily mean, °C | 3.50 | +0.07 | 0.56 | +0.08 | 0.57 |
| sd of the daily range, °C | 3.35 | -0.04 | 0.43 | -0.06 | 0.46 |
| Correlation of daily mean and range | 0.081 | +0.012 | 0.226 | +0.019 | 0.224 |
| Lag-1 correlation of Tmax | 0.716 | -0.048 | 0.085 | -0.051 | 0.084 |
| Dry-day minus wet-day Tmax, °C | 1.33 | -0.08 | 1.60 | -0.09 | 1.56 |
| Year-to-year sd of monthly Tmax, °C | 1.91 | +0.04 | 0.39 | +0.04 | 0.41 |
| Year-to-year CV of monthly totals | 0.576 | +0.001 | 0.121 | -0.004 | 0.120 |
| Year-to-year sd of the wet-day fraction | 0.117 | +0.000 | 0.020 | +0.001 | 0.019 |
| Longest dry spell of the year, days | 19.7 | -0.55 | 3.14 | -1.36 | 3.10 |
| Snow share of wet days | 0.430 | +0.008 | 0.043 | +0.008 | 0.045 |
| Thunder share of wet days | 0.087 | -0.008 | 0.033 | -0.006 | 0.036 |

**Fit stations** (the stations the constants were fitted and tuned on; `--list` defaults to them): 162 stations, 1,943 station-months.

| Metric | Observed median | `full` bias | `full` \|err\| | `lite` bias | `lite` \|err\| |
| --- | --- | --- | --- | --- | --- |
| Tmax, °C (vs cell) | 13.66 | +0.03 (-0.03) | 0.61 (0.24) | +0.13 (+0.02) | 0.78 (0.45) |
| Tmin, °C (vs cell) | 4.74 | -0.13 (-0.01) | 0.72 (0.23) | -0.08 (+0.03) | 0.84 (0.41) |
| Wet-day fraction (vs cell) | 0.404 | -0.010 (-0.001) | 0.044 (0.014) | -0.006 (+0.002) | 0.046 (0.020) |
| Monthly total, mm (vs cell) | 48.3 | +0.50 (-0.25) | 6.35 (3.40) | +0.53 (-0.03) | 6.89 (4.28) |
| Wet-day amount, mm | 4.44 | +0.19 | 0.73 | +0.15 | 0.74 |
| 95th percentile of wet days, mm | 15.6 | +0.10 | 2.83 | -0.01 | 2.94 |
| Persistence d | 0.278 | +0.004 | 0.066 | +0.006 | 0.068 |
| sd of daily Tmax, °C | 3.96 | -0.00 | 0.63 | +0.02 | 0.63 |
| sd of daily Tmin, °C | 3.48 | +0.09 | 0.53 | +0.10 | 0.56 |
| sd of the daily mean, °C | 3.35 | +0.01 | 0.54 | +0.03 | 0.53 |
| sd of the daily range, °C | 3.10 | -0.01 | 0.37 | -0.00 | 0.39 |
| Correlation of daily mean and range | 0.101 | -0.008 | 0.217 | -0.005 | 0.218 |
| Lag-1 correlation of Tmax | 0.713 | -0.044 | 0.085 | -0.046 | 0.084 |
| Dry-day minus wet-day Tmax, °C | 0.92 | +0.02 | 1.52 | +0.06 | 1.52 |
| Year-to-year sd of monthly Tmax, °C | 1.88 | +0.00 | 0.40 | +0.00 | 0.40 |
| Year-to-year CV of monthly totals | 0.580 | +0.016 | 0.118 | +0.001 | 0.113 |
| Year-to-year sd of the wet-day fraction | 0.121 | -0.003 | 0.020 | -0.002 | 0.020 |
| Longest dry spell of the year, days | 18.0 | -0.20 | 3.32 | -0.57 | 2.95 |
| Snow share of wet days | 0.494 | +0.000 | 0.025 | +0.000 | 0.028 |
| Thunder share of wet days | 0.090 | -0.001 | 0.035 | -0.003 | 0.036 |

**Against 1.x** (holdout, `full`): the longest dry spell's shortfall falls from 1.20 to 0.55 days (`lite` 2.04 to 1.36), the wettest days' from 0.44 mm to +0.08, and the lag-1 correlation of Tmax's from 0.061 to 0.048. The sd of daily Tmax is 0.09 °C high where it was 0.04, and the dry-minus-wet Tmax contrast is right on average (bias -0.08 against -0.09) but scattered more from station to station (|err| 1.60 against 1.41): the rain classes' shifts are predicted from the cell, and a station's own contrast varies more than the cell's predictors can follow.

**Dry spells** (`fit_extras.py spells`, all three lists, pooled by the month's wet-day fraction): the chance of rain after L dry days, observed / generated.

| Wet-day fraction | 1 day | 2 | 4 | 8 | 15 | 20 |
| --- | --- | --- | --- | --- | --- | --- |
| below 0.1 | 0.090 / 0.094 | 0.069 / 0.092 | 0.058 / 0.082 | 0.051 / 0.072 | 0.045 / 0.060 | 0.040 / 0.053 |
| 0.1 to 0.2 | 0.151 / 0.162 | 0.131 / 0.150 | 0.117 / 0.140 | 0.115 / 0.128 | 0.106 / 0.111 | 0.090 / 0.103 |
| 0.2 to 0.35 | 0.238 / 0.241 | 0.222 / 0.227 | 0.209 / 0.204 | 0.185 / 0.184 | 0.170 / 0.167 | 0.153 / 0.152 |
| 0.35 to 0.5 | 0.342 / 0.316 | 0.313 / 0.294 | 0.280 / 0.275 | 0.251 / 0.243 | 0.221 / 0.229 | 0.220 / 0.209 |
| 0.5 and over | 0.496 / 0.413 | 0.420 / 0.384 | 0.354 / 0.340 | 0.293 / 0.295 | 0.225 / 0.220 | 0.174 / 0.171 |

The middle climates, where most people live, follow the stations closely. Deserts' dry spells still end too readily after the first few days (their rain is more clustered than one hazard curve gives), and wet climates' rain comes back too slowly after a single dry day: there the first-order persistence understates P(wet | wet), which the chain makes up after one dry day.

**Snow, fog, gusts and drizzle** (`fit_extras.py check`, all three lists): stations against generated, median and median difference over stations.

| Measure | Stations | Observed median | Bias | \|err\| |
| --- | --- | --- | --- | --- |
| Share of days with 1 cm of snow or more | 214 | 0.364 | -0.013 | 0.041 |
| Snow depth on those days, cm | 214 | 15.0 | +0.7 | 5.0 |
| Share of days with fog (fog-reporting years) | 94 | 0.215 | +0.018 | 0.141 |
| Share of days dry with heavy fog | 94 | 0.002 | +0.010 | 0.015 |
| Mean daily gust, m/s | 61 | 10.5 | +1.0 | 1.1 |
| Median gust over wind | 61 | 2.93 | -0.18 | 0.35 |
| Drizzle share of light rain days | 10 | 0.172 | +0.063 | 0.063 |

Fog and heavy fog are reported very unevenly from station to station, so the station-to-station error is large; the model gives a typical reporting station's rate. Gusts are 1 m/s strong because the cells' wind normals (CRU CL 2.0) are higher than the reporting stations' measured wind; their ratio to the wind is close. Drizzle rests on 10 stations.

**Correlation by distance** (`fit_extras.py pairs`): station pairs from all three lists by distance, the median over pairs of each pair's mean over calendar months, observed / generated (one seed, `spatial` on). Occurrence is the tetrachoric correlation of wet days, amounts the rank correlation on days wet at both, mean and range the correlations of the daily residuals, monthly the correlation of the year-to-year monthly mean temperature and log total.

| km | Pairs | Occurrence | Amounts | Daily mean | Daily range | Monthly mean | Monthly total |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0-50 | 3 | 0.70 / 0.73 | 0.28 / 0.33 | 0.79 / 0.84 | 0.58 / 0.56 | 0.94 / 0.89 | 0.77 / 0.62 |
| 50-100 | 11 | 0.69 / 0.71 | 0.32 / 0.29 | 0.80 / 0.80 | 0.59 / 0.55 | 0.85 / 0.87 | 0.68 / 0.58 |
| 100-150 | 59 | 0.69 / 0.68 | 0.33 / 0.29 | 0.81 / 0.83 | 0.53 / 0.52 | 0.88 / 0.88 | 0.61 / 0.57 |
| 150-200 | 73 | 0.63 / 0.65 | 0.24 / 0.26 | 0.79 / 0.80 | 0.49 / 0.50 | 0.87 / 0.86 | 0.54 / 0.49 |
| 200-300 | 221 | 0.57 / 0.58 | 0.23 / 0.21 | 0.79 / 0.77 | 0.46 / 0.45 | 0.87 / 0.83 | 0.50 / 0.44 |
| 300-400 | 312 | 0.47 / 0.48 | 0.15 / 0.16 | 0.72 / 0.72 | 0.36 / 0.37 | 0.83 / 0.79 | 0.40 / 0.35 |
| 400-500 | 345 | 0.39 / 0.39 | 0.11 / 0.12 | 0.67 / 0.67 | 0.33 / 0.30 | 0.79 / 0.74 | 0.34 / 0.28 |
| 500-650 | 674 | 0.30 / 0.31 | 0.08 / 0.09 | 0.61 / 0.60 | 0.25 / 0.25 | 0.73 / 0.67 | 0.28 / 0.22 |
| 650-800 | 741 | 0.21 / 0.20 | 0.06 / 0.06 | 0.53 / 0.53 | 0.19 / 0.18 | 0.68 / 0.60 | 0.21 / 0.16 |
| 800-1000 | 1,204 | 0.14 / 0.12 | 0.03 / 0.04 | 0.44 / 0.45 | 0.13 / 0.12 | 0.59 / 0.54 | 0.15 / 0.11 |
| 1000-1250 | 1,730 | 0.06 / 0.05 | 0.01 / 0.02 | 0.33 / 0.35 | 0.08 / 0.07 | 0.49 / 0.44 | 0.08 / 0.06 |
| 1250-1500 | 1,868 | 0.01 / 0.01 | 0.00 / 0.01 | 0.23 / 0.26 | 0.04 / 0.03 | 0.39 / 0.36 | 0.04 / 0.03 |
| 1500-2000 | 4,041 | -0.03 / 0.00 | -0.01 / 0.00 | 0.10 / 0.14 | 0.01 / 0.01 | 0.24 / 0.24 | 0.00 / 0.01 |
| 2000-2500 | 4,013 | -0.04 / 0.00 | -0.01 / 0.00 | -0.01 / 0.05 | -0.01 / 0.00 | 0.09 / 0.11 | -0.03 / 0.00 |

Day to day, the generated correlations follow the stations' within about 0.05 at every distance. Months are shared less than the stations' are: the year-to-year monthly mean temperature is 0.04 to 0.08 low from 200 to 1,250 km, and monthly totals 0.04 to 0.06 low from 100 to 1,000 km (0.10 to 0.15 within 100 km, on only 14 pairs). Beyond 1,500 km the stations' wet days are slightly anticorrelated (-0.03), which fields of positive weights cannot give.

What this says:

- **Means** of temperature, wet days and totals are those of the cell (the "vs cell" biases are within 0.05 °C, 0.006 and 0.3 mm). The error left at a station is mostly the cell's normals against the station's own: a cell mean over terrain the station is one point of. That is why `lite` is worse than `full`, and why its "vs cell" errors, against the `full` cell, are larger.
- **Day-to-day behaviour** (persistence, amounts, spreads, snow and thunder shares) and **year-to-year spread** have biases of a few percent, on the holdout stations as on the fit stations. Nearby places' days are correlated as nearby stations' are.
- **Known shortfalls**: the longest dry spell is still 3% short on the holdout stations (7% in `lite`), and dry spells in deserts end too readily; Tmax's and Tmin's spreads are 2% high on the holdout stations; the lag-1 correlation of Tmax is 0.05 low; the dry-minus-wet contrast is right on average but follows a station less closely than 1.x's single shift did; nearby places' months are a little less alike than nearby stations'; fog rates are a typical station's; gusts are 1 m/s strong.

## Rebuilding the Data

```bash
# 1. CRU TS 4.10: cru_ts4.10.{1991.2000,2001.2010,2011.2020}.{tmp,dtr,pre,wet,cld,vap}.dat.gz
#    and the {tmp,pre,wet}.stn.gz station counts, from
#    https://crudata.uea.ac.uk/cru/data/hrg/cru_ts_4.10/cruts.2604091129.v4.10/<var>/
#    into CRUTS_DIR.
#    CRU CL 2.0: grid_10min_{elv,wnd,reh,sunp}.dat.gz from
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

# 4. The fits and checks beyond calibrate.py. The sample covers the stations
#    of all three lists (concatenate their validate.py stations files).
#    CRU CL 2.0 grid_10min_sunp.dat.gz is from the same page as step 1's.
python scripts/fit_extras.py snow GHCN_DIR
python scripts/fit_extras.py sunshine CRUCL_DIR/grid_10min_sunp.dat.gz
python scripts/fit_extras.py events GHCN_DIR                    # gusts, fog, drizzle
python scripts/fit_extras.py spells GHCN_DIR build/sample_all.tsv
python scripts/fit_extras.py check GHCN_DIR build/sample_all.tsv  # snow depth, fog, gusts, drizzle
python scripts/fit_extras.py pairs GHCN_DIR                     # correlation by distance, stations
python scripts/fit_extras.py pairs GHCN_DIR build/sample_all.tsv  # and generated
```

The station lists are fixed by the inventory file; NCEI updates GHCN-Daily, so a later inventory can pick other stations. A change of the climate tables changes `data_hash()` and every known-answer test.

## Licences

The climate tables and `history.tsv` are adapted from CRU TS 4.10 and CRU CL 2.0, which the Climatic Research Unit releases under the Open Database License with attribution and share-alike: weather generated from them is a Produced Work that needs a notice, and the tables themselves, or tables built from them, must stay under the ODbL. The land mask is from Natural Earth, in the public domain. GHCN-Daily records are not shipped; only constants fitted on them. See [`LICENSE_DATA.txt`](../LICENSE_DATA.txt).
