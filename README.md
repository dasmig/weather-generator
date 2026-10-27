# Weather Generator for C++

> **Requires C++23** (e.g., `-std=c++23` for GCC/Clang, `/std:c++latest` for MSVC).

[![Weather Generator for C++](https://raw.githubusercontent.com/dasmig/weather-generator/main/doc/weather-generator.png)](https://github.com/dasmig/weather-generator/releases)

[![GitHub license](https://img.shields.io/badge/license-MIT-blue.svg)](https://raw.githubusercontent.com/dasmig/weather-generator/main/LICENSE.MIT)
[![CI](https://github.com/dasmig/weather-generator/actions/workflows/ci.yml/badge.svg)](https://github.com/dasmig/weather-generator/actions/workflows/ci.yml)
[![GitHub Releases](https://img.shields.io/github/release/dasmig/weather-generator.svg)](https://github.com/dasmig/weather-generator/releases)
[![C++23](https://img.shields.io/badge/standard-C%2B%2B23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Header-only](https://img.shields.io/badge/type-header--only-green.svg)](https://github.com/dasmig/weather-generator#integration)
[![Documentation](https://img.shields.io/badge/docs-GitHub%20Pages-blue.svg)](https://dasmig.github.io/weather-generator/)

**[API Reference](https://dasmig.github.io/weather-generator/)** · **[Usage Guide](doc/usage.md)** · **[Data and Method](doc/data.md)** · **[Releases](https://github.com/dasmig/weather-generator/releases)**

Daily weather for any place on land and any date: the low and the high, rain or snow and how much, thunder, cloud, humidity and wind, drawn around the place's climate normals so that thirty generated years look like thirty real ones.

```
2024-01-15: rain, 5.4 to 7.9 °C, 3.0 mm
2024-01-16: sleet, 3.9 to 8.7 °C, 0.1 cm of snow
2024-01-17: light rain, 5.3 to 7.2 °C, 1.2 mm
2024-01-18: mostly cloudy, 2.4 to 7.8 °C
```

## Features

- **Any Place, Any Date**. Give a latitude and longitude (or leave them out for a place drawn by land area) and any date. The climate comes from the CRU CL 2.0 normals of the nearest cell: a 2-degree grid in `lite`, half a degree in `full`. An elevation moves temperatures by the standard lapse rate, and `warming` shifts them for a warmer climate than the 1961-1990 normals.

- **Weather That Behaves Like Weather**. Wet days cluster, warm spells last, and some months run wet or cold: a Richardson-type weather generator (a rain Markov chain, gamma-distributed amounts, autocorrelated daily mean and range, month-to-month anomalies), fitted on 176 GHCN-Daily stations and checked against 191. Monthly means, wet days, wet-day amounts, day-to-day spread and year-to-year spread land within a few percent of the stations (see [Data and Method](doc/data.md)).

- **Series That Agree**. A day's weather depends only on the seed, the place and the date, so the weather of 10 March is the same whether it is asked for alone or inside a series that starts in January, and overlapping series agree.

- **Rain, Snow, Sleet and Thunder**. The form of precipitation follows the day's temperature, and thunder its heat and rain, both fitted on station records. Twelve conditions from `clear` to `heavy_snow` and `thunderstorm`.

- **Climate Normals Too**. `climate_at(lat, lon)` returns the monthly normals a place's weather is drawn around: temperature, diurnal range, precipitation, wet days, humidity, sunshine and wind.

- **Deterministic Seeding**. Per-call `get_weather(request, seed)` for reproducible results, and `weather::seed()` for replaying a previous generation with the same request and data.

- **Batch Generation**. Generate thousands of results from one seed, on several threads with the same output for any thread count, and stream them as JSON Lines or CSV (see the usage guide).

- **Identical Everywhere**. A seed gives the same result on every compiler, standard library and CPU (GCC, Clang, MSVC, Emscripten; x86-64 and ARM64), checked by known-answer tests.

- **Extensible Data**. The climate table is a TSV file you can extend with layers (`extend()`): add a station's own normals and the points nearest it use them.

- **JSON and Replay**. `to_json()` on every result, and a `signature()` recording the seed and the data it came from.

- **Compiled-in Data**. Define `DASMIG_WEATHER_EMBED` to build the `lite` tier into the program, for WebAssembly and other builds without files.

- **Multi-Instance Support**. Construct independent `wthg` instances with their own data and random engine.

## Integration

[`weathergen.hpp`](https://github.com/dasmig/weather-generator/blob/main/dasmig/weathergen.hpp) is the library [released here](https://github.com/dasmig/weather-generator/releases). Keep [`core.hpp`](https://github.com/dasmig/weather-generator/blob/main/dasmig/core.hpp) and the [`core/`](https://github.com/dasmig/weather-generator/tree/main/dasmig/core) directory ([dasmig-core](https://github.com/dasmig/dasmig-core)) in the same directory. Add

```cpp
#include <dasmig/weathergen.hpp>
```

to the files you want to use it in, and supply the [`resources`](https://github.com/dasmig/weather-generator/tree/main/resources) folder (or define `DASMIG_WEATHER_EMBED`).

## Usage

```cpp
#include <dasmig/weathergen.hpp>
#include <iostream>

using namespace std::chrono_literals;

auto& gen = dasmig::wthg::instance(); // loads the lite tier

dasmig::weather_request paris;
paris.latitude = 48.85;
paris.longitude = 2.35;
paris.date = 2024y / 1 / 15;

auto day = gen.get_weather(paris, 7); // seed 7
std::cout << day << '\n';             // 2024-01-15: rain, 5.4 to 7.9 °C, 3.0 mm
std::cout << day.to_json() << '\n';   // every field, with the seed

for (const auto& d : gen.get_weather_series(paris, 7, 42)) // a week, seed 42
    std::cout << d << '\n';

paris.elevation = 1500.0; // the same place higher up
paris.warming = 1.2;      // in a climate 1.2 °C warmer

auto anywhere = gen.get_weather(dasmig::weather_request{}); // a place drawn by land area

// The same seed gives the same result everywhere.
auto again = gen.get_weather(paris, day.seed());
```

See the [usage guide](doc/usage.md) for everything else.

## Building

```bash
make            # example
make test       # tests
make bench      # benchmarks, results in build/bench.tsv
make test-embed # tests with the lite tier compiled in
make coverage   # code coverage
make lint       # clang-tidy
make docs       # API docs
make embed      # regenerate dasmig/weathergen_data.hpp from resources/lite
```

## Compiler Support

Tested with:
- Clang 18+ (`-std=c++23`)
- GCC 14+ (`-std=c++23`)
- MSVC 19.38+ (`/std:c++latest`)
- Emscripten 3.1.74

## Dependencies

| Dependency | Version | Bundled | Purpose |
|-----------|---------|---------|---------|
| [dasmig-core](https://github.com/dasmig/dasmig-core) | 1.2 | Yes (`core.hpp`, `core/`) | Platform-independent random engine and distributions, data layers, JSON |
| [Catch2](https://github.com/catchorg/Catch2) | 3.x | Yes (amalgamated) | Unit testing |

## Related Libraries

| Library | Description |
|---------|-------------|
| [address-generator](https://github.com/dasmig/address-generator) | Postal addresses with real streets by place |
| [biodata-generator](https://github.com/dasmig/biodata-generator) | Procedural human physical characteristics |
| [birth-generator](https://github.com/dasmig/birth-generator) | Demographically plausible birthdays |
| [city-generator](https://github.com/dasmig/city-generator) | Weighted city selection by population |
| [country-generator](https://github.com/dasmig/country-generator) | Weighted country selection by population |
| [device-generator](https://github.com/dasmig/device-generator) | Devices, browsers, user agents and IP addresses that fit together |
| [email-generator](https://github.com/dasmig/email-generator) | Email addresses in each country's style |
| [entity-generator](https://github.com/dasmig/entity-generator) | ECS-based entity generation |
| [interest-generator](https://github.com/dasmig/interest-generator) | Religion, hobbies and favourite media by country, age and sex |
| [name-generator](https://github.com/dasmig/name-generator) | Culturally appropriate full names |
| [nickname-generator](https://github.com/dasmig/nickname-generator) | Gamer-style nicknames |
| [occupation-generator](https://github.com/dasmig/occupation-generator) | Employment status, education and occupation by country, age and sex |
| [persona-generator](https://github.com/dasmig/persona-generator) | Complete characters composed from the other generators |
| [phone-generator](https://github.com/dasmig/phone-generator) | Phone numbers valid in each country's numbering plan |
| [relationship-generator](https://github.com/dasmig/relationship-generator) | Families around a person: parents, siblings, partners and children |
| [trait-generator](https://github.com/dasmig/trait-generator) | Personality traits with conflicts, synergies and Big Five axes |
| [user-generator](https://github.com/dasmig/user-generator) | Complete user records: account, username, email, phone and address |

## License

This library is released under the [MIT License](https://opensource.org/licenses/MIT). The data's sources are listed in [LICENSE_DATA.txt](LICENSE_DATA.txt) and [doc/data.md](doc/data.md).

```
MIT License

Copyright (c) 2026 Diego Dasso Migotto
```
