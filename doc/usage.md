# Usage Guide

weather-generator draws daily weather for a place on land and a date: the low and the high, rain, sleet or snow and how much, thunder, cloud, humidity, wind, sunrise, sunset and sunshine, around the place's 1991-2020 climate normals. This guide covers everything beyond the [README](../README.md). For the full API reference, run `doxygen Doxyfile` from the repository root and open `doc/api/html/index.html`; for the data and the model, see [Data and Method](data.md).

[TOC]

<!-- GitHub-rendered TOC (Doxygen uses [TOC] above instead) -->
<ul>
<li><a href="#quick-start">Quick Start</a></li>
<li><a href="#installation">Installation</a></li>
<li><a href="#loading-resources">Loading Resources</a></li>
<li><a href="#the-request">The Request</a></li>
<li><a href="#the-result">The Result</a></li>
<li><a href="#series">Series</a></li>
<li><a href="#imperial-units">Imperial Units</a></li>
<li><a href="#hours">Hours</a></li>
<li><a href="#historical-months">Historical Months</a></li>
<li><a href="#climate-normals">Climate Normals</a></li>
<li><a href="#what-is-not-modelled">What Is Not Modelled</a></li>
<li><a href="#seeding-and-deterministic-generation">Seeding and Deterministic Generation</a></li>
<li><a href="#extending-data">Extending Data</a></li>
<li><a href="#replay-and-json">Replay and JSON</a></li>
<li><a href="#compiled-in-data">Compiled-in Data</a></li>
<li><a href="#thread-safety">Thread Safety</a></li>
<li><a href="#batch-generation">Batch Generation</a></li>
<li><a href="#error-reference">Error Reference</a></li>
</ul>

## Quick Start

```cpp
#include <dasmig/weathergen.hpp>
#include <iostream>

int main()
{
    using namespace std::chrono_literals;

    auto& gen = dasmig::wtg::instance();

    dasmig::weather_request paris;
    paris.latitude = 48.85;
    paris.longitude = 2.35;
    paris.date = 2024y / 1 / 15;
    for (const auto& day : gen.get_weather_series(paris, 4, 7)) std::cout << day << '\n';
}
```

```
2024-01-15: rain, 6.8 to 9.4 °C, 2.8 mm
2024-01-16: light rain, 5.4 to 10.2 °C, 0.3 mm
2024-01-17: light rain, 6.7 to 8.7 °C, 1.2 mm
2024-01-18: mostly cloudy, 4.1 to 9.4 °C
```

## Installation

Copy `dasmig/weathergen.hpp`, `dasmig/core.hpp` and the `dasmig/core/` directory into your include path, keeping them together, and either the `resources/` folder or `dasmig/weathergen_data.hpp` (see [Compiled-in Data](#compiled-in-data)). Build with `-std=c++23` (GCC 14+, Clang 18+) or `/std:c++latest` (MSVC 19.38+).

## Loading Resources

`wtg::instance()` loads the `lite` tier on first use (or `full` if there is no `lite`), from the compiled-in copy or from the first of `resources/`, `../resources`, `weather-generator/resources` and `../weather-generator/resources` that has it. Independent instances start empty:

```cpp
dasmig::wtg gen;
bool ok = gen.load(dasmig::dataset::full);                 // false when the tier is not found
gen.load(std::filesystem::path{"path/to/resources/lite"}); // a directory with climate.tsv and land.tsv
gen.has_data();
gen.count();                                               // climate cells
```

| Tier | Grid | Cells | File | Loads in |
| --- | --- | --- | --- | --- |
| `lite` | 2 degrees | 5,163 | 2.0 MB | about 60 ms |
| `full` | 0.5 degrees | 67,213 | 26.0 MB | about 0.8 s |

Both cover every land cell of CRU TS except Antarctica, and share `land.tsv`, the half-degree land mask behind the sea rule below. `full` follows coasts, mountains and rain shadows more closely: against stations it was not fitted on, its monthly Tmax errors are about a fifth smaller than `lite`'s (see [Data and Method](data.md#validation)). Each `load` replaces the data and every layer loaded before.

## The Request

| Field | Default | Meaning |
| --- | --- | --- |
| `latitude`, `longitude` | unset: a place drawn by land area | degrees, -90 to 90 and -180 to 180; set both or neither |
| `elevation` | the cell's mean elevation | metres, -500 to 9000; temperatures move 6.5 °C per km from the cell's elevation |
| `date` | 2023-07-01 | any valid date; the first day of a series |
| `warming` | 0 | °C added to every temperature, -10 to 10 |
| `units` | `unit_system::metric` | `metric` or `imperial` (°F, inches, mph); see [Imperial Units](#imperial-units) |
| `hourly` | `false` | also give each day's 24 hours; see [Hours](#hours) |
| `historical` | `false` | draw each month around the month as it was, 1991-2020; see [Historical Months](#historical-months) |

A point uses the nearest climate cell (by distance on the sphere; ties go to the earlier row). The point's own latitude, longitude and elevation are returned; the climate is the cell's. Without a point, a cell is drawn with probability proportional to its land area, and the result carries the cell's point.

The data covers land. A point at sea gets the weather of the nearest cell if it is within 100 km of land (a bay, a strait, a ship near the coast), and `distance_km` says how far out it is; further out the call throws `std::invalid_argument`:

```
weather-generator: (40, -40) is 749 km from land; the data covers land only, without Antarctica
```

Land here is the half-degree mask, so a point on a small island or a thin peninsula can read a few km out to sea, and one in a wide estuary 0 km.

The normals are 1991-2020 means, centred on 2005. Land has warmed by about half a degree since; set `warming` to bring temperatures to a later climate. It moves temperatures only, so the share of snow moves with it, but rain amounts and wet days stay as they were.

The library never reads the clock. A request is used for one call only, so nothing a call asks for changes the next.

## The Result

| Field | Meaning |
| --- | --- |
| `date` | the day (`date_string()` gives `YYYY-MM-DD`) |
| `cell` | the climate cell's key, its block centre (`"49.00,3.00"`) |
| `latitude`, `longitude`, `elevation` | the requested point and elevation, or the cell's |
| `distance_km` | how far the point is from land, km, to 0.1: 0 on land, at most 100 |
| `tmin`, `tmax` | the day's lowest and highest temperature, °C, to 0.1 |
| `tmean` | halfway between them, °C, to 0.1 |
| `frost` | `tmin` below 0 °C |
| `precipitation` | mm of water, to 0.1; 0 on a dry day, at least 0.1 on a wet day |
| `snowfall` | fresh snow, cm, to 0.1: 1 cm per mm of water for snow, half that for sleet |
| `type` | `none`, `rain`, `sleet` or `snow` |
| `thunder` | thunder on a rain or sleet day |
| `cloud_cover` | mean cloud cover, 0 to 1, to 0.01 |
| `humidity` | mean relative humidity, %, whole |
| `wind` | mean wind speed at 10 m, m/s, to 0.1 |
| `daylight_hours` | from sunrise to sunset, hours, to 0.01: 24 in polar day, 0 in polar night |
| `sunshine_hours` | the daylight not covered by cloud, hours, to 0.1 |
| `sunrise`, `sunset` | `std::optional<std::chrono::sys_time<std::chrono::minutes>>`, UTC to the minute; empty in polar day and night. `weather::time_string(t)` gives `YYYY-MM-DDTHH:MMZ` |
| `condition` | the day in a word, below |
| `units` | `unit_system::metric` or `imperial`, as requested |
| `hours` | the 24 hours when the request set `hourly`, empty otherwise |

Sunrise and sunset come from NOAA's solar calculator equations at the point, for the sun's upper edge at the horizon with standard refraction (the tests check them against a plain double-precision implementation of the same equations, to half a minute). They are in UTC, so in the far east or west a sunrise can fall on the day before (Sydney's 1 January sunrise is at 18:48 UTC on 31 December). The library has no time zones: add the place's offset for local time.

| `condition` | When |
| --- | --- |
| `clear`, `partly_cloudy`, `mostly_cloudy`, `overcast` | dry, with cloud cover below 0.2, below 0.5, below 0.85, and above |
| `light_rain`, `rain`, `heavy_rain` | rain below 2.5 mm, below 10 mm, and 10 mm or more |
| `sleet` | sleet |
| `light_snow`, `snow`, `heavy_snow` | snow below 1 mm of water, below 10 mm, and 10 mm or more |
| `thunderstorm` | rain or sleet with thunder |

`to_string(condition)` gives the key (`partly_cloudy`) and `label(condition)` the English words (`partly cloudy`); `to_string(type)` gives `none`, `rain`, `sleet` or `snow`. The result converts to a one-line summary (`std::string`, `operator<<`):

```
2024-01-15: rain, 6.8 to 9.4 °C, 2.8 mm
2024-01-18: mostly cloudy, 4.1 to 9.4 °C
2023-12-21: snow, -4.5 to 2.2 °C, 9.8 cm of snow
```

## Series

`get_weather_series(request, days, seed)` returns `days` consecutive days from `request.date` (1 to `wtg::max_series_days`, 36,600, about a century). A series is what to use for a stretch of days: wet days cluster into spells, warm and cold spells last several days, and some months run wetter or colder than their normals, as in the records.

A day depends only on the seed, the cell and the date, not on where the series started:

```cpp
auto week = gen.get_weather_series(paris, 7, 7); // 15 to 21 January
paris.date = 2024y / 1 / 18;
auto day = gen.get_weather(paris, 7);            // the same as week[3]
```

So a game can ask for today alone each day and still get weather that runs on from yesterday's, and two series that overlap agree on the days they share. Points in the same cell with the same seed get the same weather (apart from the elevation's shift); different cells with the same seed get unrelated weather.

Each call reads 48 days before its first day, so one day alone costs about as much as ten days of a series: a single day takes about 15 µs, a year as one series about 0.5 ms (see `make bench`).

## Imperial Units

`units = dasmig::unit_system::imperial` gives the weather values in °F, inches and mph: `tmin`, `tmean` and `tmax` to 0.1 °F, `precipitation` to 0.01 in, `snowfall` to 0.1 in (10 in of snow per inch of water) and `wind` to 0.1 mph. The values are converted before rounding, so they are the metric day's, not its rounded numbers converted again. Everything else is the same day: `frost` is still a `tmin` below 32 °F, the condition thresholds are the metric ones, and the place keeps `elevation` in metres and `distance_km` in km. The request's `elevation` and `warming` are always metres and °C.

```cpp
paris.units = dasmig::unit_system::imperial;
gen.get_weather(paris, 3); // 2024-01-15: light rain, 34.1 to 43.6 °F, 0.05 in
```

The JSON of an imperial day has `"units":"imperial"` after the date. A metric day has no `units` key, so metric JSON is exactly 1.0's.

## Hours

`hourly = true` adds each day's 24 hours to `weather::hours`. The day itself does not change: a day with hours is the day without them plus the hours, and a day's hours, like the day, do not depend on where its series starts. They come from a stream of their own.

| Field | Meaning |
| --- | --- |
| `hour`, `time` | the hour of the place's mean solar day, 0 to 23, and its start in UTC |
| `temperature` | at the start of the hour, °C (°F), to 0.1 |
| `precipitation`, `snowfall`, `type`, `thunder` | in the hour; dry hours have type `none` |
| `cloud_cover`, `humidity`, `wind` | in the hour |
| `daylight_hours`, `sunshine_hours` | the part of the hour the sun is up, and the bright sunshine in it, 0 to 1, to 0.01 |
| `condition` | the hour in a word: the day's cloud thresholds, and per hour light rain below 0.5 mm, rain below 4 mm, heavy rain from 4 mm (snow below 0.5 mm of water, below 2 mm, and more) |

The hours are those of local mean solar time: hour 0 starts at the place's mean midnight, `4 * longitude` minutes before 00:00 UTC, so the coolest hour is near sunrise and the warmest in the afternoon at any longitude. Add the place's time zone yourself for clock time; it differs from mean solar time by up to an hour or two, and by daylight saving.

- **Temperature** rises as a sine from `tmin` at sunrise to `tmax` at the warmest hour (four hours before sunset, or midway through a short day), falls to the sunset temperature, then falls as a square root to the next day's `tmin` at the next sunrise (Cesaraccio et al. 2001). The hours before sunrise finish the previous night, so one day runs into the next with no jump at midnight. In polar day and night there is no sunrise: the hours follow a plain daily wave between `tmin` and `tmax`, coolest at 03:00.
- **Precipitation** falls in one spell of wet hours or (30% of days) two. The number of wet hours rises with the day's amount (2 + 2.5 ln(1 + mm) on average, half that on thunder days, which are one spell centred on mid-afternoon). In metric units the hours add up to the day's precipitation and snowfall exactly.
- **Cloud and wind** wander around the day's values from hour to hour. Wet hours are cloudy. The wind blows strongest in the early afternoon, and the hours average to the day's wind. Sunshine is the day's sunshine shared out by each hour's daylight and clear sky.
- **Humidity** keeps the day's vapour pressure, so it is highest in the cool of the night and lowest in the afternoon, and at least 90% while it rains.

```cpp
paris.hourly = true;
for (const auto& h : gen.get_weather(paris, 3).hours)
{
    std::cout << h.hour << ": " << h.temperature << " °C, " << h.precipitation << " mm\n";
}
```

The JSON of a day with hours has an `hours` array after `sunset`. A long hourly series is large: a year is 8,760 hours.

## Historical Months

`historical = true` draws each month around the month as it really was, from January 1991 to December 2020, instead of around the 1991-2020 normals. A cold winter is cold, a drought summer dry and a wet spring wet, while the days within each month are still drawn: the seed decides the weather of each day, and history decides the month's character.

```cpp
bool found = gen.load_history(); // once: resources/history/history.tsv (23 MB)
dasmig::weather_request r{.latitude = 48.85, .longitude = 2.35, .date = 2010y / 1 / 1};
r.historical = true;             // Paris in January 2010, 3.7 °C below the normal
auto month = gen.get_weather_series(r, 31, 7);
```

- **Data.** `history.tsv` holds, for each 2-degree block of land, each month's temperature anomaly (°C), its precipitation as a percentage of the normal and its wet-day anomaly (days), against the 1991-2020 normals of CRU TS 4.10. A cell takes the anomalies of the block its point lies in, so a `full`-tier cell gets the block's anomaly on its own normals. `load()` does not read it and it is not compiled in; call `load_history()` (or `load_history(path)`) before the first historical request.
- **Method.** The month's temperature anomaly moves the daily mean, and the precipitation and wet days set the odds of a wet day and the amount on a wet day, all interpolated between mid-months as the normals are. The random month anomalies of a normal request are left out, since the real month carries them. Within a month, days vary as on any other day.
- **Range.** Every day asked for must fall in 1991-2020, or the call throws `std::invalid_argument`; without the history loaded it throws `std::runtime_error`. The 48 days read before the first day may fall before 1991 and use the normals.
- **Replay.** The signature's `data` combines the climate's hash with the history's, so a historical result replays only against the same history.
- **Not modelled.** The diurnal range, cloud, humidity and wind of a historical month are the normals'; where CRU TS had no stations within reach, its month is close to the normal.

## Climate Normals

`climate_at(latitude, longitude)` returns the normals of the cell a point uses, the means its weather is drawn around:

```cpp
auto n = gen.climate_at(-3.1, -60.0); // Manaus
n.cell;              // "-3.00,-61.00"
n.tmean[0];          // 27.5 °C in January
n.precipitation[0];  // 271.1 mm
n.wet_days[0];       // 26.9 days with 0.1 mm or more
n.cloud[0];          // 89 % cover
n.to_json();
```

Each element has twelve values, January to December: `tmean` (°C), `dtr` (diurnal range, Tmax - Tmin, °C), `precipitation` (mm per month), `wet_days` (days with 0.1 mm or more), `humidity` (%), `cloud` (% cover) and `wind` (m/s at 10 m). A point more than 100 km from land throws, as for the weather.

## What Is Not Modelled

- **Hours are shaped, not simulated.** Hourly temperatures follow a fixed daily curve between the days' lows and highs, and rain timing is drawn per day; there are no fronts passing at a given hour.
- **No snowpack.** `snowfall` is the day's fresh snow; nothing tracks snow on the ground or melt.
- **No fog, hail, drizzle, gusts or wind direction.** `frost` is only a `tmin` below 0, not ground frost.
- **Cells are independent.** Neighbouring cells with one seed do not share fronts; weather is not spatially correlated.
- **No trend.** Every year is drawn from the same 1991-2020 climate, shifted by `warming` if set, unless `historical` draws 1991-2020 as it was. There are no El Niño years outside that range, no droughts longer than the model's month anomalies carry, and no extremes beyond the fitted distributions.
- **Dry spells run short.** Rain follows a first-order Markov chain, which makes the longest dry spell of a year about 6% shorter than in the records (10% in `lite`), and the wettest days (the 95th percentile) about 0.5 mm lighter. See [Data and Method](data.md#validation) for every measured bias.
- **No Antarctica.** The climatology has no Antarctic cells, and the continent is too far from any other land for the 100 km rule: a point there throws.

## Seeding and Deterministic Generation

- `get_weather(request, seed)` and `get_weather_series(request, days, seed)` give the same result for the same request, seed and data, on every platform, without advancing the instance engine.
- `get_weather(request)` and `get_weather_series(request, days)` draw a fresh seed from the instance engine, seeded from system entropy. There are no generator-level `seed()` or `unseed()` controls; for deterministic sequences, keep a `dasmig::core::rng` in the caller and pass its next value as the seed.
- `w.seed()` is the seed a result was generated from: `get_weather(request, w.seed())` replays it with the same request and data. Every day of a series carries the series' seed, and `get_weather` with that seed and the day's date gives the day back.
- The same seed for a different place gives unrelated weather; to give a whole world one weather "history", use one seed for every place.

## Extending Data

The shipped climate table is a base layer. Files with the `climate.tsv` columns can be loaded on top of it to add cells, change them or remove them, without editing the shipped file. Rows are keyed by `key`.

| Column | Required | Meaning |
| --- | --- | --- |
| `key` | yes | the cell's name; the shipped cells use their block centre, `"49.00,3.00"` |
| `latitude`, `longitude` | yes | the cell's point; requests use the nearest |
| `elevation` | no (0) | mean elevation, m |
| `area` | no (0) | land area weight for drawing a random place; 0 is never drawn |
| `tmean`, `dtr`, `precipitation`, `wet_days` | yes | twelve values each, separated by `;` |
| `humidity`, `cloud`, `wind` | no (70, 50, 3.5) | twelve values each: %, % cover, m/s |

```cpp
// A station's own normals: points nearer to it than to any cell centre use them.
gen.extend("mods/station.tsv");

// From text, for example supplied by a WebAssembly host: a desert oasis,
// dry all year, nearly cloudless, humid only in winter.
gen.extend_from_string(
    "key\tlatitude\tlongitude\televation\ttmean\tdtr\tprecipitation\twet_days\thumidity\tcloud\n"
    "oasis\t25.4\t29.0\t-20\t13;15;18;23;27;29;30;30;27;24;19;15\t15;15;15;15;15;15;14;14;14;14;14;14\t"
    "0;0;0;0;0;0;0;0;0;0;0;0\t0;0;0;0;0;0;0;0;0;0;0;0\t50;43;37;29;25;25;28;31;35;40;46;51\t"
    "8;7;6;5;3;1;1;1;2;3;6;8\n",
    "scenario");

// Remove cells: a layer needs only the key and a disabled column.
gen.extend_from_string("key\tdisabled\n49.00,3.00\t1\n", "cleanup");

// Add mode refuses to touch keys that are already loaded.
gen.extend("mods/new.tsv", dasmig::core::layer_mode::add);
```

- A layer's cell is a point among the others: requests nearest to it use it. Its `area` decides whether it can be drawn as a random place. The land mask stays the shipped one, so a layer cell at sea is reached only by points within 100 km of land, and a point within 100 km of its own position always counts as near enough.
- Leave out `humidity`, `cloud` and `wind` only for a temperate place: their defaults (70%, 50% cover, 3.5 m/s) would make a desert half cloudy.
- Every value in a layer is checked before anything changes (latitudes, longitudes, elevations, twelve values per month list, ranges such as 0 to 31 wet days). A bad layer throws `dasmig::core::tsv_error` with the file, line and column, and the data stays as it was.
- `layers()` lists what is loaded: source, mode, rows added, replaced and removed, and warnings such as unknown columns.
- `load(...)` replaces the base and drops every layer.

## Replay and JSON

Every result records the seed and the data it came from:

```cpp
auto w = gen.get_weather(dasmig::weather_request{});
w.signature().seed; // the call seed
w.signature().data; // gen.data_hash() when it was generated
```

`to_json()` gives the result as JSON, the form the WebAssembly layer and the game consume. Seeds are strings because JavaScript numbers cannot hold every 64-bit value. `to_json(writer)` writes into an existing `dasmig::core::json_writer`, for composing larger documents.

```json
{"date":"2023-07-01","place":{"cell":"37.00,87.00","latitude":36.996,"longitude":87,"elevation":4574,"distance_km":0},"condition":"mostly_cloudy","tmin":0,"tmean":5.5,"tmax":11.1,"frost":false,"precipitation":0,"snowfall":0,"type":"none","thunder":false,"cloud_cover":0.74,"humidity":56,"wind":6.3,"daylight_hours":14.66,"sunshine_hours":3.8,"sunrise":"2023-06-30T22:56Z","sunset":"2023-07-01T13:36Z","signature":{"seed":"000000000000002a","data":"590a3332eb0f63a0"}}
```

`sunrise` and `sunset` are `null` in polar day and night.

## Compiled-in Data

Define `DASMIG_WEATHER_EMBED` before including the header to compile the `lite` tier into the program. `load(dataset::lite)` and `instance()` then use it and need no files. The compiled-in data has the same `data_hash()` as the file, so seeds replay the same either way. `full` is never compiled in.

`dasmig/weathergen_data.hpp` is generated from `resources/lite` by `make embed`; CI checks it is up to date.

## Thread Safety

Each `wtg` instance is independent. Concurrent calls to the unseeded `get_weather(request)` and `get_weather_series(request, days)` on the **same** instance require external synchronisation (e.g. a `std::mutex`); the seeded calls and `climate_at` are `const` and may run concurrently while the data is not being loaded or extended.

## Batch Generation

`get_weather_batch` returns a `std::vector` of single days, and `for_each_weather` streams them to a sink in index order without holding the batch. Every item uses the same request with its own seed: with a place set, a batch is many possible versions of one day there; without one, each item draws its own place.

```cpp
auto& gen = dasmig::wtg::instance();
dasmig::weather_request paris;
paris.latitude = 48.85;
paris.longitude = 2.35;

// 1,000 possible 1 July 2023s in Paris, on four threads.
auto items = gen.get_weather_batch(paris, {.count = 1000, .threads = 4}, 42);

// Item 1 replays on its own: the batch seed and the index give its seed.
auto again = gen.get_weather(paris, dasmig::core::item_seed(42, 1));

// A million days at random places to a file as JSON Lines, never all in memory.
std::ofstream out{"days.jsonl"};
dasmig::core::jsonl_writer lines{out};
gen.for_each_weather(dasmig::weather_request{}, {.count = 1'000'000, .threads = 0}, 42, lines);
```

- **Item seeds.** Item `i` is exactly `get_weather(request, core::item_seed(batch_seed, first + i))`. A range can be generated directly (`first = 1000` starts at item 1000), two batches that follow each other equal one, and the output is the same for any `threads` (0 means the hardware count). Without a `batch_seed` one is drawn like the single getter draws its own.
- **Options.** `core::batch_options` has `count`, `first`, `threads` and `unique`. The request describes one item; the options describe the batch.
- **Uniqueness.** `.unique = core::unique_rule::by<weather>(key)` regenerates an item whose key already appeared in the batch (one day per cell, say), with a deterministic retry seed, and throws `std::runtime_error` naming the item when the attempts run out.
- **Sinks.** `core::jsonl_writer` writes one `to_json()` per line, `core::csv_writer` flattens each result's JSON into dotted columns (`place.cell`, `tmax`, ...). Any callable `(std::uint64_t index, weather&)` works.
- **Series are not batched.** For many places' series, call `get_weather_series` per place; it is `const` and safe to run on several threads.
- Batch calls use the same const getters from several threads, which the thread-safety rules above allow; the CI runs the batch tests under ThreadSanitizer.

## Error Reference

| Exception | When |
|-----------|------|
| `std::runtime_error` | Any generation call or `climate_at` before data is loaded; a historical request before `load_history()`; a unique batch that runs out of attempts |
| `std::invalid_argument` | A point more than 100 km from land (weather or `climate_at`); latitude or longitude out of range or set alone; elevation outside -500 to 9000 m; `warming` outside -10 to 10; an invalid date or `units`; `days` outside 1 to 36,600; a historical day outside 1991-2020; no place requested and no cell with an `area` |
| `dasmig::core::tsv_error` | A malformed `climate.tsv`, layer or `history.tsv` (message gives file, line and column); the data is unchanged |
