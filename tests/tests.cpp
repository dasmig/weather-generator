#include "catch_amalgamated.hpp"
#include "../dasmig/weathergen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

using namespace std::chrono_literals;

static std::filesystem::path lite_dir()
{
    for (const auto* candidate : {"resources/lite", "../resources/lite"})
    {
        if (std::filesystem::is_directory(candidate)) return candidate;
    }
    return {};
}

static dasmig::wtg make_loaded_generator()
{
    dasmig::wtg gen;
    if (!gen.load(dasmig::dataset::lite)) gen.load(lite_dir());
    REQUIRE(gen.has_data());
    return gen;
}

static dasmig::weather_request at(double lat, double lon, std::chrono::year_month_day date = 2023y / 7 / 1)
{
    dasmig::weather_request r;
    r.latitude = lat;
    r.longitude = lon;
    r.date = date;
    return r;
}

struct cell_row
{
    std::string key;
    double lat;
    double lon;
};

// The lite cells read straight from the file, for brute-force checks.
static std::vector<cell_row> lite_cells()
{
    std::vector<cell_row> out;
    std::ifstream in{lite_dir() / "climate.tsv"};
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line))
    {
        std::istringstream f{line};
        cell_row c;
        std::string lat;
        std::string lon;
        std::getline(f, c.key, '\t');
        std::getline(f, lat, '\t');
        std::getline(f, lon, '\t');
        c.lat = std::stod(lat);
        c.lon = std::stod(lon);
        out.push_back(c);
    }
    return out;
}

static double great_circle(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double rad = std::numbers::pi / 180.0;
    const double a = std::sin((lat2 - lat1) * rad / 2);
    const double b = std::sin((lon2 - lon1) * rad / 2);
    return 2 * std::asin(std::sqrt((a * a) + (std::cos(lat1 * rad) * std::cos(lat2 * rad) * b * b)));
}

static int month_of(const dasmig::weather& w) { return static_cast<int>(static_cast<unsigned>(w.date.month())) - 1; }

// One row of the climate table with every column, for layers.
static std::string station_row(const std::string& key, double lat, double lon, double tmean, double wet_days,
                               double area = 0.0)
{
    const auto months = [](double v)
    {
        std::string s;
        for (int i = 0; i < 12; ++i) s += (i ? ";" : "") + std::format("{}", v);
        return s;
    };
    return std::format("{}\t{}\t{}\t100\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", key, lat, lon, area, months(tmean),
                       months(8.0), months(wet_days * 4.0), months(wet_days), months(70), months(50), months(3));
}

static const std::string header = "key\tlatitude\tlongitude\televation\tarea\ttmean\tdtr\tprecipitation\twet_days\t"
                                  "humidity\tcloud\twind\n";

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_CASE("load(dataset::lite) loads the climate grid", "[loading]")
{
    const auto gen = make_loaded_generator();
    REQUIRE(gen.count() == 5163);
    REQUIRE(gen.layers().size() == 1);
}

TEST_CASE("load - non-existent path is a no-op", "[loading]")
{
    dasmig::wtg gen;
    gen.load(std::filesystem::path{"no/such/directory"});
    REQUIRE_FALSE(gen.has_data());
    REQUIRE(gen.data_hash() == 0);
}

#ifndef DASMIG_WEATHER_EMBED // compiled-in data is never missing
TEST_CASE("load(dataset) returns false for a missing tier", "[loading]")
{
    const auto old_cwd = std::filesystem::current_path();
    const auto tmp = std::filesystem::temp_directory_path() / "wtg_empty_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::wtg gen;
    const bool loaded = gen.load(dasmig::dataset::full);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);
    REQUIRE_FALSE(loaded);
    REQUIRE_FALSE(gen.has_data());
}
#endif

TEST_CASE("the singleton loads data on first use", "[loading]")
{
    REQUIRE(dasmig::wtg::instance().has_data());
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

TEST_CASE("get_weather throws when no data is loaded", "[generation]")
{
    dasmig::wtg gen;
    REQUIRE_THROWS_AS(gen.get_weather(dasmig::weather_request{}), std::runtime_error);
    REQUIRE_THROWS_AS(gen.get_weather(dasmig::weather_request{}, std::uint64_t{1}), std::runtime_error);
    REQUIRE_THROWS_AS(gen.get_weather_series(dasmig::weather_request{}, 3, 1), std::runtime_error);
    REQUIRE_THROWS_AS((void)gen.climate_at(0, 0), std::runtime_error);
}

TEST_CASE("get_weather with a seed is deterministic", "[generation][seed]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 100; ++s)
    {
        REQUIRE(gen.get_weather(dasmig::weather_request{}, s).to_json() ==
                gen.get_weather(dasmig::weather_request{}, s).to_json());
    }
}

TEST_CASE("a result replays from its seed", "[generation][seed]")
{
    auto gen = make_loaded_generator();
    const auto v = gen.get_weather(dasmig::weather_request{});
    REQUIRE(gen.get_weather(dasmig::weather_request{}, v.seed()).to_json() == v.to_json());
    const auto series = gen.get_weather_series(at(51.5, -0.1), 5);
    REQUIRE(gen.get_weather_series(at(51.5, -0.1), 5, series.front().seed()).back().to_json() ==
            series.back().to_json());
}

TEST_CASE("explicit per-call seeds replay across generator instances", "[seed]")
{
    const auto first = make_loaded_generator();
    const auto second = make_loaded_generator();
    REQUIRE(first.get_weather(dasmig::weather_request{}, 42).to_json() ==
            second.get_weather(dasmig::weather_request{}, 42).to_json());
}

TEST_CASE("invalid requests are rejected", "[generation][errors]")
{
    const auto gen = make_loaded_generator();
    dasmig::weather_request r;
    r.latitude = 10.0;
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument); // no longitude
    r = at(91.0, 0.0);
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument);
    r = at(0.0, -180.5);
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument);
    r = at(std::nan(""), 0.0);
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument);
    r = at(0.0, 0.0, 2023y / 2 / 30);
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument);
    r = at(0.0, 0.0);
    r.elevation = 9500.0;
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument);
    r = at(0.0, 0.0);
    r.warming = 11.0;
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(gen.get_weather_series(at(0, 0), 0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(gen.get_weather_series(at(0, 0), dasmig::wtg::max_series_days + 1, 1),
                      std::invalid_argument);
    REQUIRE_THROWS_AS((void)gen.climate_at(-90.5, 0), std::invalid_argument);
}

TEST_CASE("a day's weather does not depend on where its series starts", "[generation][series]")
{
    const auto gen = make_loaded_generator();
    for (const auto& [lat, lon] : std::vector<std::pair<double, double>>{{48.85, 2.35}, {-3.1, -60.0}, {62.0, 129.7}})
    {
        // Across a year end and a leap day.
        const auto series = gen.get_weather_series(at(lat, lon, 2023y / 12 / 1), 120, 9);
        REQUIRE(series.size() == 120);
        REQUIRE(series.front().date == 2023y / 12 / 1);
        REQUIRE(series.back().date == std::chrono::year_month_day{std::chrono::sys_days{2023y / 12 / 1} + std::chrono::days{119}});
        for (const int start : {0, 1, 31, 89})
        {
            const auto from = std::chrono::year_month_day{std::chrono::sys_days{2023y / 12 / 1} + std::chrono::days{start}};
            const auto part = gen.get_weather_series(at(lat, lon, from), 120 - start, 9);
            for (std::size_t i = 0; i < part.size(); ++i)
            {
                REQUIRE(part[i].to_json() == series[static_cast<std::size_t>(start) + i].to_json());
            }
        }
        for (std::size_t i = 0; i < series.size(); i += 7)
        {
            REQUIRE(gen.get_weather(at(lat, lon, series[i].date), 9).to_json() == series[i].to_json());
        }
    }
}

TEST_CASE("days are invariant: temperatures, precipitation and labels agree", "[generation]")
{
    const auto gen = make_loaded_generator();
    std::set<dasmig::weather_condition> seen;
    for (std::uint64_t s = 0; s < 3000; ++s)
    {
        auto r = dasmig::weather_request{};
        r.date = std::chrono::year_month_day{std::chrono::sys_days{2023y / 1 / 1} + std::chrono::days{s % 365}};
        const auto w = gen.get_weather(r, s);
        seen.insert(w.condition);
        REQUIRE(w.tmin <= w.tmean);
        REQUIRE(w.tmean <= w.tmax);
        REQUIRE(w.frost == (w.tmin < 0.0));
        REQUIRE(w.daylight_hours >= 0.0);
        REQUIRE(w.daylight_hours <= 24.0);
        REQUIRE(w.sunshine_hours >= 0.0);
        REQUIRE(w.sunshine_hours <= w.daylight_hours + 0.05);
        REQUIRE(w.cloud_cover >= 0.0);
        REQUIRE(w.cloud_cover <= 1.0);
        REQUIRE(w.humidity >= 5.0);
        REQUIRE(w.humidity <= 100.0);
        REQUIRE(w.wind >= 0.0);
        REQUIRE(w.gust >= w.wind);
        REQUIRE(w.snow_depth >= 0.0);
        REQUIRE(w.solar_radiation >= 0.0);
        if (w.condition == dasmig::weather_condition::fog)
        {
            REQUIRE(w.fog);
            REQUIRE(w.type == dasmig::precipitation_type::none);
        }
        if (w.condition == dasmig::weather_condition::drizzle)
        {
            REQUIRE(w.type == dasmig::precipitation_type::rain);
            REQUIRE(w.precipitation < 2.5);
        }
        if (w.type == dasmig::precipitation_type::none)
        {
            REQUIRE(w.precipitation == 0.0);
            REQUIRE(w.snowfall == 0.0);
            REQUIRE_FALSE(w.thunder);
        }
        else
        {
            REQUIRE(w.precipitation >= 0.1);
        }
        if (w.type == dasmig::precipitation_type::rain) REQUIRE(w.snowfall == 0.0);
        if (w.type == dasmig::precipitation_type::snow)
        {
            REQUIRE(w.snowfall == w.precipitation);
            REQUIRE_FALSE(w.thunder);
        }
        if (w.thunder) REQUIRE(w.condition == dasmig::weather_condition::thunderstorm);
    }
    REQUIRE(seen.size() == 14); // every condition occurs
}

// ---------------------------------------------------------------------------
// Places
// ---------------------------------------------------------------------------

TEST_CASE("a point uses the nearest cell", "[place]")
{
    const auto gen = make_loaded_generator();
    const auto cells = lite_cells();
    REQUIRE(cells.size() == gen.count());
    dasmig::core::rng r{3};
    for (int i = 0; i < 300; ++i)
    {
        const double lat = dasmig::core::uniform_real(r, -60.0, 80.0);
        const double lon = dasmig::core::uniform_real(r, -180.0, 180.0);
        double best = 10.0;
        for (const auto& cell : cells) best = std::min(best, great_circle(lat, lon, cell.lat, cell.lon));
        dasmig::climate_normals c;
        try
        {
            c = gen.climate_at(lat, lon);
        }
        catch (const std::invalid_argument&)
        {
            continue; // at sea: the sea rule has its own test
        }
        REQUIRE(great_circle(lat, lon, c.latitude, c.longitude) == Catch::Approx(best).margin(1e-9));
    }
    // Paris is in the 2-degree block centred at 49, 3; the dateline wraps.
    REQUIRE(gen.climate_at(48.85, 2.35).cell == "49.00,3.00");
    REQUIRE(gen.climate_at(-17.0, 179.99).cell == gen.climate_at(-17.0, -179.99).cell);
}

TEST_CASE("points at sea are refused beyond 100 km from land", "[place][errors]")
{
    const auto gen = make_loaded_generator();
    REQUIRE(gen.get_weather(at(48.85, 2.35), 1).distance_km == 0.0); // Paris
    REQUIRE_THROWS_AS(gen.get_weather(at(0.0, -140.0), 1), std::invalid_argument); // the Pacific
    REQUIRE_THROWS_AS(gen.get_weather(at(40.0, -40.0), 1), std::invalid_argument);  // the Atlantic
    REQUIRE_THROWS_AS(gen.get_weather(at(-80.0, 0.0), 1), std::invalid_argument);  // Antarctica
    REQUIRE_THROWS_AS((void)gen.climate_at(0.0, -140.0), std::invalid_argument);
    // The Bay of Biscay off the French coast (15 km from the mask's
    // 0.5-degree land cells): weather of the nearest cell, with the distance.
    const auto off = gen.get_weather(at(46.0, -2.2), 1);
    REQUIRE(off.distance_km > 0.0);
    REQUIRE(off.distance_km <= 100.0);
    // Distance grows westward into the bay, then the point is refused, and
    // stays refused.
    double previous = 0.0;
    bool refused = false;
    for (double lon = -1.3; lon > -6.0; lon -= 0.1)
    {
        try
        {
            const double d = gen.get_weather(at(46.0, lon), 1).distance_km;
            REQUIRE_FALSE(refused);
            REQUIRE(d >= previous);
            REQUIRE(d <= 100.0);
            previous = d;
        }
        catch (const std::invalid_argument&)
        {
            refused = true;
        }
    }
    REQUIRE(refused);
    REQUIRE(previous > 80.0);
    // A drawn place is always on land.
    for (std::uint64_t s = 0; s < 300; ++s) REQUIRE(gen.get_weather({}, s).distance_km == 0.0);
}

TEST_CASE("a requested point is reported, a drawn place is a cell's", "[place]")
{
    const auto gen = make_loaded_generator();
    const auto w = gen.get_weather(at(48.85, 2.35), 1);
    REQUIRE(w.latitude == 48.85);
    REQUIRE(w.longitude == 2.35);
    REQUIRE(w.cell == "49.00,3.00");
    REQUIRE(w.elevation == std::round(gen.climate_at(48.85, 2.35).elevation));

    // Two points in one cell share the weather.
    auto same = gen.get_weather(at(48.5, 2.9), 1);
    REQUIRE(same.cell == w.cell);
    REQUIRE(same.tmax == w.tmax);
    REQUIRE(same.precipitation == w.precipitation);

    std::set<std::string> cells;
    for (std::uint64_t s = 0; s < 500; ++s)
    {
        const auto d = gen.get_weather({}, s);
        const auto c = gen.climate_at(d.latitude, d.longitude);
        REQUIRE(c.cell == d.cell);
        REQUIRE(c.latitude == d.latitude);
        cells.insert(d.cell);
    }
    REQUIRE(cells.size() > 400);
}

TEST_CASE("places are drawn by land area", "[place][stats]")
{
    const auto gen = make_loaded_generator();
    int tropics = 0;
    int north = 0;
    constexpr int draws = 6000;
    for (std::uint64_t s = 0; s < draws; ++s)
    {
        const double lat = gen.get_weather({}, s).latitude;
        if (std::abs(lat) < 23.44) ++tropics;
        if (lat > 60.0) ++north;
    }
    // About a third of the land outside Antarctica is tropical, a sixth north of 60.
    REQUIRE(tropics == Catch::Approx(draws * 0.35).margin(draws * 0.05));
    REQUIRE(north == Catch::Approx(draws * 0.15).margin(draws * 0.05));
}

TEST_CASE("elevation and warming shift the temperatures", "[place]")
{
    const auto gen = make_loaded_generator();
    const auto base = gen.get_weather_series(at(46.5, 8.0), 60, 4);
    auto high = at(46.5, 8.0);
    high.elevation = gen.climate_at(46.5, 8.0).elevation + 1000.0;
    auto warm = at(46.5, 8.0);
    warm.warming = 2.0;
    const auto up = gen.get_weather_series(high, 60, 4);
    const auto hot = gen.get_weather_series(warm, 60, 4);
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        REQUIRE(up[i].tmax == Catch::Approx(base[i].tmax - 6.5).margin(0.11));
        REQUIRE(up[i].tmin == Catch::Approx(base[i].tmin - 6.5).margin(0.11));
        REQUIRE(hot[i].tmax == Catch::Approx(base[i].tmax + 2.0).margin(0.11));
        REQUIRE(up[i].precipitation == base[i].precipitation); // the same wet days and amounts
    }
}

// ---------------------------------------------------------------------------
// The sun
// ---------------------------------------------------------------------------

// NOAA's equations again in plain double arithmetic with std::sin and
// std::acos, as the reference for the library's own trigonometry.
static std::pair<double, double> reference_sun(std::chrono::sys_days day, double lat, double lon)
{
    constexpr double rad = std::numbers::pi / 180.0;
    const auto sin_d = [&](double x) { return std::sin(x * rad); };
    const auto cos_d = [&](double x) { return std::cos(x * rad); };
    const double jd = static_cast<double>(day.time_since_epoch().count()) + 2440587.5 + 0.5 - (lon / 360.0);
    const double t = (jd - 2451545.0) / 36525.0;
    const double l0 = std::fmod(280.46646 + (t * (36000.76983 + (t * 0.0003032))), 360.0);
    const double m = 357.52911 + (t * (35999.05029 - (0.0001537 * t)));
    const double e = 0.016708634 - (t * (0.000042037 + (0.0000001267 * t)));
    const double c = (sin_d(m) * (1.914602 - (t * (0.004817 + (0.000014 * t))))) +
                     (sin_d(2 * m) * (0.019993 - (0.000101 * t))) + (sin_d(3 * m) * 0.000289);
    const double omega = 125.04 - (1934.136 * t);
    const double lambda = l0 + c - 0.00569 - (0.00478 * sin_d(omega));
    const double eps0 = 23.0 + ((26.0 + ((21.448 - (t * (46.815 + (t * (0.00059 - (t * 0.001813)))))) / 60.0)) / 60.0);
    const double eps = eps0 + (0.00256 * cos_d(omega));
    const double decl = std::asin(sin_d(eps) * sin_d(lambda)) / rad;
    const double y = std::pow(std::tan(eps / 2 * rad), 2);
    const double eq = 4.0 / rad *
                      ((y * sin_d(2 * l0)) - (2 * e * sin_d(m)) + (4 * e * y * sin_d(m) * cos_d(2 * l0)) -
                       (0.5 * y * y * sin_d(4 * l0)) - (1.25 * e * e * sin_d(2 * m)));
    const double ha = std::acos((cos_d(90.833) / (cos_d(lat) * cos_d(decl))) - (std::tan(lat * rad) * std::tan(decl * rad))) / rad;
    const double noon = 720.0 - (4.0 * lon) - eq;
    return {noon - (4 * ha), noon + (4 * ha)};
}

static double minutes_after(const std::chrono::sys_time<std::chrono::minutes>& t, std::chrono::sys_days day)
{
    return static_cast<double>((t - std::chrono::sys_time<std::chrono::minutes>{day}).count());
}

TEST_CASE("sunrise and sunset follow NOAA's equations", "[sun]")
{
    const auto gen = make_loaded_generator();
    // Paris on 1 July 2023: 05:51 and 21:58 summer time.
    const auto paris = gen.get_weather(at(48.85, 2.35, 2023y / 7 / 1), 1);
    REQUIRE(paris.sunrise.has_value());
    REQUIRE(dasmig::weather::time_string(*paris.sunrise) == "2023-07-01T03:51Z");
    REQUIRE(dasmig::weather::time_string(*paris.sunset) == "2023-07-01T19:58Z");
    REQUIRE(paris.daylight_hours == Catch::Approx(16.11).margin(0.02));
    // Sydney on 1 January 2023: 05:47 local, the day before in UTC.
    const auto sydney = gen.get_weather(at(-33.87, 151.21, 2023y / 1 / 1), 1);
    REQUIRE(dasmig::weather::time_string(*sydney.sunrise).starts_with("2022-12-31T18:4"));

    dasmig::core::rng r{8};
    int checked = 0;
    for (int i = 0; i < 2000; ++i)
    {
        const double lat = dasmig::core::uniform_real(r, -55.0, 70.0);
        const double lon = dasmig::core::uniform_real(r, -180.0, 180.0);
        const auto day = std::chrono::sys_days{2000y / 1 / 1} + std::chrono::days{dasmig::core::uniform_int(r, 0, 20000)};
        dasmig::weather w;
        try
        {
            w = gen.get_weather(at(lat, lon, std::chrono::year_month_day{day}), 1);
        }
        catch (const std::invalid_argument&)
        {
            continue;
        }
        const auto [rise, set] = reference_sun(day, lat, lon);
        INFO(lat << ", " << lon << " day " << day.time_since_epoch().count());
        if (std::isnan(rise))
        {
            REQUIRE_FALSE(w.sunrise.has_value());
            continue;
        }
        REQUIRE(w.sunrise.has_value());
        REQUIRE(minutes_after(*w.sunrise, day) == Catch::Approx(rise).margin(0.51));
        REQUIRE(minutes_after(*w.sunset, day) == Catch::Approx(set).margin(0.51));
        REQUIRE(w.daylight_hours == Catch::Approx((set - rise) / 60.0).margin(0.006));
        ++checked;
    }
    REQUIRE(checked > 500);
}

TEST_CASE("polar day and polar night", "[sun]")
{
    const auto gen = make_loaded_generator();
    const auto summer = gen.get_weather(at(69.65, 18.96, 2023y / 6 / 21), 1); // Tromsø
    REQUIRE(summer.daylight_hours == 24.0);
    REQUIRE_FALSE(summer.sunrise.has_value());
    REQUIRE_FALSE(summer.sunset.has_value());
    const auto winter = gen.get_weather(at(69.65, 18.96, 2023y / 12 / 21), 1);
    REQUIRE(winter.daylight_hours == 0.0);
    REQUIRE(winter.sunshine_hours == 0.0);
    REQUIRE_FALSE(winter.sunrise.has_value());
    REQUIRE(winter.to_json().find(R"("sunrise":null,"sunset":null)") != std::string::npos);
    // Near the equator a day is about 12 hours all year.
    for (const auto& d : gen.get_weather_series(at(-0.2, -78.5, 2023y / 1 / 1), 365, 1)) // Quito
    {
        REQUIRE(d.daylight_hours == Catch::Approx(12.1).margin(0.1));
    }
}

// ---------------------------------------------------------------------------
// Climate statistics: 30 generated years against the cell's normals
// ---------------------------------------------------------------------------

TEST_CASE("generated years follow the normals", "[stats]")
{
    const auto gen = make_loaded_generator();
    // Paris, Manaus, Cairo, Yakutsk.
    for (const auto& [lat, lon] : std::vector<std::pair<double, double>>{
             {48.85, 2.35}, {-3.1, -60.0}, {30.0, 31.2}, {62.0, 129.7}})
    {
        const auto normals = gen.climate_at(lat, lon);
        std::array<double, 12> tmax{}, tmin{}, total{}, wet{}, n{};
        for (const std::uint64_t seed : {11U, 12U}) // 60 years
        {
            for (const auto& w : gen.get_weather_series(at(lat, lon, 1991y / 1 / 1), 30 * 365 + 7, seed))
            {
                const auto m = static_cast<std::size_t>(month_of(w));
                tmax[m] += w.tmax;
                tmin[m] += w.tmin;
                total[m] += w.precipitation;
                wet[m] += w.precipitation > 0 ? 1 : 0;
                n[m] += 1;
            }
        }
        // The months' anomalies leave 60-year means about 0.2 °C from the
        // normals at Paris and 0.6 °C at Yakutsk, whose winters vary most:
        // allow three times that, scaled by the annual range.
        const auto [coldest, warmest] = std::ranges::minmax(normals.tmean);
        const double margin = 0.5 + (0.02 * (warmest - coldest));
        double annual = 0;
        double annual_normal = 0;
        for (std::size_t m = 0; m < 12; ++m)
        {
            const double length = n[m] / 60.0;
            const double mean = normals.tmean[m];
            const double half = normals.dtr[m] / 2;
            INFO(normals.cell << " month " << m + 1);
            REQUIRE(tmax[m] / n[m] == Catch::Approx(mean + half).margin(margin));
            REQUIRE(tmin[m] / n[m] == Catch::Approx(mean - half).margin(margin));
            REQUIRE(wet[m] / n[m] == Catch::Approx(normals.wet_days[m] / length).margin(0.06));
            annual += total[m] / 60.0;
            annual_normal += normals.precipitation[m];
        }
        INFO(normals.cell);
        REQUIRE(annual == Catch::Approx(annual_normal).epsilon(0.12).margin(5.0));
    }
}

TEST_CASE("snow falls in a Siberian winter and never in the Amazon", "[stats]")
{
    const auto gen = make_loaded_generator();
    int snow = 0;
    int wet = 0;
    for (const auto& w : gen.get_weather_series(at(62.0, 129.7, 2000y / 1 / 1), 31, 2))
    {
        if (w.type != dasmig::precipitation_type::none) ++wet;
        if (w.type == dasmig::precipitation_type::snow) ++snow;
    }
    REQUIRE(wet > 0);
    REQUIRE(snow == wet);
    for (const auto& w : gen.get_weather_series(at(-3.1, -60.0, 2000y / 1 / 1), 365, 2))
    {
        REQUIRE(w.snowfall == 0.0);
    }
}

TEST_CASE("snow lies through a Siberian winter, melts by summer and never lies in the Amazon", "[stats][snow]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 5; ++s)
    {
        // Yakutsk: snow builds from October and lies all winter.
        REQUIRE(gen.get_weather(at(62.0, 129.7, 2001y / 3 / 1), s).snow_depth > 5.0);
        REQUIRE(gen.get_weather(at(62.0, 129.7, 2001y / 7 / 20), s).snow_depth == 0.0);
        // A day's snow depth does not depend on the call: the season is
        // simulated from its start whatever day is asked for.
        const auto series = gen.get_weather_series(at(62.0, 129.7, 2001y / 1 / 1), 90, s);
        REQUIRE(gen.get_weather(at(62.0, 129.7, 2001y / 3 / 1), s).snow_depth == series[59].snow_depth);
    }
    for (const auto& w : gen.get_weather_series(at(-3.1, -60.0, 2000y / 1 / 1), 365, 2))
    {
        REQUIRE(w.snow_depth == 0.0);
    }
    // Snow depth grows with the snowfall it keeps and shrinks as it melts
    // and settles: never more than the snow fallen since the season began.
    double fallen = 0.0;
    for (const auto& w : gen.get_weather_series(at(62.0, 129.7, 2000y / 7 / 15), 365, 4))
    {
        fallen += w.snowfall;
        REQUIRE(w.snow_depth <= fallen + 0.05);
    }
}

TEST_CASE("nearby places share their weather, unless spatial correlation is off", "[stats][spatial]")
{
    const auto gen = make_loaded_generator();
    // Paris and Rouen (110 km apart), the same seed, two years of days.
    const auto correlation = [&](bool spatial)
    {
        auto a = at(48.85, 2.35, 2001y / 1 / 1);
        auto b = at(49.44, 1.10, 2001y / 1 / 1);
        a.spatial = spatial;
        b.spatial = spatial;
        const auto x = gen.get_weather_series(a, 730, 7);
        const auto y = gen.get_weather_series(b, 730, 7);
        // Wet days (phi) and day-to-day changes of the mean temperature.
        double n11 = 0.0, n10 = 0.0, n01 = 0.0, n00 = 0.0;
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        for (std::size_t i = 1; i < x.size(); ++i)
        {
            const bool wx = x[i].precipitation > 0.0;
            const bool wy = y[i].precipitation > 0.0;
            (wx ? (wy ? n11 : n10) : (wy ? n01 : n00)) += 1.0;
            const double dx = x[i].tmean - x[i - 1].tmean;
            const double dy = y[i].tmean - y[i - 1].tmean;
            sxy += dx * dy;
            sxx += dx * dx;
            syy += dy * dy;
        }
        const double phi = ((n11 * n00) - (n10 * n01)) / std::sqrt((n11 + n10) * (n01 + n00) * (n11 + n01) * (n10 + n00));
        return std::pair{phi, sxy / std::sqrt(sxx * syy)};
    };
    const auto [wet_on, t_on] = correlation(true);
    const auto [wet_off, t_off] = correlation(false);
    REQUIRE(wet_on > 0.35);
    REQUIRE(t_on > 0.5);
    REQUIRE(std::abs(wet_off) < 0.1);
    REQUIRE(std::abs(t_off) < 0.1);
    // Far apart (Paris and Novosibirsk), places stay independent.
    const auto x = gen.get_weather_series(at(48.85, 2.35, 2001y / 1 / 1), 730, 7);
    const auto y = gen.get_weather_series(at(55.0, 82.9, 2001y / 1 / 1), 730, 7);
    double same = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) same += ((x[i].precipitation > 0.0) == (y[i].precipitation > 0.0)) ? 1.0 : 0.0;
    REQUIRE(same / static_cast<double>(x.size()) < 0.75);
}

TEST_CASE("fog, drizzle, gusts and solar radiation", "[stats]")
{
    const auto gen = make_loaded_generator();
    int fog = 0;
    int foggy = 0;
    int drizzle = 0;
    int days = 0;
    for (const auto& w : gen.get_weather_series(at(51.5, -0.1, 2001y / 1 / 1), 3 * 365, 11))
    {
        ++days;
        fog += w.fog ? 1 : 0;
        foggy += w.condition == dasmig::weather_condition::fog ? 1 : 0;
        drizzle += w.condition == dasmig::weather_condition::drizzle ? 1 : 0;
        REQUIRE(w.gust >= w.wind);
    }
    // London: fog on some days, thick fog on fewer, drizzle on some.
    REQUIRE(fog > days / 50);
    REQUIRE(fog < days / 2);
    REQUIRE(foggy > 0);
    REQUIRE(foggy < fog);
    REQUIRE(drizzle > 0);

    // Solar radiation: a clear midsummer day in Spain gets far more than a
    // midwinter one, and the polar night none.
    double summer = 0.0;
    double winter = 0.0;
    for (std::uint64_t s = 0; s < 20; ++s)
    {
        summer += gen.get_weather(at(40.0, -4.0, 2023y / 6 / 21), s).solar_radiation / 20.0;
        winter += gen.get_weather(at(40.0, -4.0, 2023y / 12 / 21), s).solar_radiation / 20.0;
        REQUIRE(gen.get_weather(at(78.2, 15.6, 2023y / 12 / 21), s).solar_radiation == 0.0);
    }
    REQUIRE(summer > 20.0);
    REQUIRE(summer < 35.0);
    REQUIRE(winter > 3.0);
    REQUIRE(winter < summer / 2.0);

    // Hours: fog only in the hours of a foggy day.
    for (std::uint64_t s = 0; s < 200; ++s)
    {
        auto r = at(51.5, -0.1, 2023y / 11 / 10);
        r.hourly = true;
        const auto w = gen.get_weather(r, s);
        bool any = false;
        for (const auto& h : w.hours)
        {
            any = any || h.fog;
        }
        REQUIRE(any == w.fog);
    }
}

// ---------------------------------------------------------------------------
// Conversion and JSON
// ---------------------------------------------------------------------------

TEST_CASE("string conversion and streaming give the summary", "[conversion]")
{
    const auto gen = make_loaded_generator();
    const auto v = gen.get_weather(at(48.85, 2.35), std::uint64_t{5});
    const std::string s = v;
    std::ostringstream os;
    os << v;
    REQUIRE(s == v.to_string());
    REQUIRE(os.str() == s);
    REQUIRE(s.starts_with("2023-07-01: "));
    REQUIRE(s.find(dasmig::label(v.condition)) != std::string::npos);
    REQUIRE(dasmig::to_string(dasmig::weather_condition::heavy_snow) == "heavy_snow");
    REQUIRE(dasmig::label(dasmig::weather_condition::partly_cloudy) == "partly cloudy");
    REQUIRE(dasmig::to_string(dasmig::precipitation_type::sleet) == "sleet");
}

TEST_CASE("to_json and signature", "[json]")
{
    const auto gen = make_loaded_generator();
    const auto v = gen.get_weather(at(48.85, 2.35), std::uint64_t{99});
    REQUIRE(v.signature().seed == 99);
    REQUIRE(v.seed() == 99);
    REQUIRE(v.signature().data == gen.data_hash());
    const auto json = v.to_json();
    REQUIRE(json.starts_with(R"({"date":"2023-07-01","units":"metric","place":{"cell":"49.00,3.00","latitude":48.85,)"));
    REQUIRE(json.find(R"("signature":{"seed":"0000000000000063")") != std::string::npos);

    dasmig::core::json_writer w;
    w.begin_array();
    v.to_json(w);
    w.end_array();
    REQUIRE(w.str() == "[" + json + "]");

    const auto c = gen.climate_at(48.85, 2.35).to_json();
    REQUIRE(c.starts_with(R"({"cell":"49.00,3.00","latitude":48.993,"longitude":3,)"));
    REQUIRE(c.find(R"("wet_days":[)") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Known answers (rule 2): the same seed and data give these exact results on
// every compiler and platform. A change here is a change of output and needs
// a major version.
// ---------------------------------------------------------------------------

TEST_CASE("known answers for fixed seeds", "[kat]")
{
    dasmig::wtg gen;
    gen.load(lite_dir());
    REQUIRE(gen.has_data());
    REQUIRE(gen.data_hash() == 0x590a3332eb0f63a0ULL);

    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{1}).to_json() ==
            R"({"date":"2023-07-01","units":"metric","place":{"cell":"-17.00,-65.00","latitude":-16.998,"longitude":-65,"elevation":1060,"distance_km":0},"condition":"fog","tmin":13.8,"tmean":20.1,"tmax":26.5,"frost":false,"precipitation":0,"snowfall":0,"snow_depth":0,"type":"none","thunder":false,"fog":true,"cloud_cover":0.19,"humidity":78,"wind":1.3,"gust":7.7,"daylight_hours":11.13,"sunshine_hours":9.7,"solar_radiation":17.7,"sunrise":"2023-07-01T10:50Z","sunset":"2023-07-01T21:58Z","signature":{"seed":"0000000000000001","data":"590a3332eb0f63a0"}})");
    REQUIRE(gen.get_weather(at(48.85, 2.35, 2024y / 1 / 15), std::uint64_t{3}).to_json() ==
            R"({"date":"2024-01-15","units":"metric","place":{"cell":"49.00,3.00","latitude":48.85,"longitude":2.35,"elevation":117,"distance_km":0},"condition":"light_rain","tmin":-0.6,"tmean":2,"tmax":4.5,"frost":true,"precipitation":1,"snowfall":0,"snow_depth":0,"type":"rain","thunder":false,"fog":true,"cloud_cover":1,"humidity":89,"wind":4.2,"gust":9.2,"daylight_hours":8.69,"sunshine_hours":0,"solar_radiation":2.4,"sunrise":"2024-01-15T07:39Z","sunset":"2024-01-15T16:21Z","signature":{"seed":"0000000000000003","data":"590a3332eb0f63a0"}})");
    REQUIRE(gen.get_weather(at(62.0, 129.7, 2024y / 2 / 29), std::uint64_t{5}).to_json() ==
            R"({"date":"2024-02-29","units":"metric","place":{"cell":"63.00,129.00","latitude":62,"longitude":129.7,"elevation":171,"distance_km":0},"condition":"fog","tmin":-32.2,"tmean":-28.2,"tmax":-24.3,"frost":true,"precipitation":0,"snowfall":0,"snow_depth":16.9,"type":"none","thunder":false,"fog":true,"cloud_cover":0.69,"humidity":92,"wind":1.8,"gust":5.9,"daylight_hours":10.25,"sunshine_hours":4.9,"solar_radiation":5.5,"sunrise":"2024-02-28T22:26Z","sunset":"2024-02-29T08:41Z","signature":{"seed":"0000000000000005","data":"590a3332eb0f63a0"}})");

    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::uint64_t s = 0; s < 10000; ++s)
    {
        auto r = dasmig::weather_request{};
        r.date = std::chrono::year_month_day{std::chrono::sys_days{2023y / 1 / 1} + std::chrono::days{s % 365}};
        for (const char c : gen.get_weather(r, s).to_json())
        {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
    }
    for (const auto& w : gen.get_weather_series(at(-33.9, 18.4, 1990y / 1 / 1), 3650, 7))
    {
        for (const char c : w.to_json())
        {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
    }
    REQUIRE(h == 0xd6f9d63fe65c33dcULL);
}

// ---------------------------------------------------------------------------
// Units, hours and historical months (1.1.0)
// ---------------------------------------------------------------------------

static std::filesystem::path history_file()
{
    for (const auto* candidate : {"resources/history/history.tsv", "../resources/history/history.tsv"})
    {
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    return {};
}

static std::uint64_t json_hash(std::uint64_t h, const std::string& json)
{
    for (const char c : json)
    {
        h ^= static_cast<unsigned char>(c);
        h *= 0x100000001b3ULL;
    }
    return h;
}

TEST_CASE("imperial units convert the same day", "[units]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 400; ++s)
    {
        auto r = dasmig::weather_request{};
        r.date = std::chrono::year_month_day{std::chrono::sys_days{2023y / 1 / 1} + std::chrono::days{s % 365}};
        const auto m = gen.get_weather(r, s);
        r.units = dasmig::unit_system::imperial;
        const auto i = gen.get_weather(r, s);
        REQUIRE(m.units == dasmig::unit_system::metric);
        REQUIRE(i.units == dasmig::unit_system::imperial);
        // Converted before rounding, so within the two roundings.
        REQUIRE(i.tmin == Catch::Approx((m.tmin * 1.8) + 32.0).margin(0.15));
        REQUIRE(i.tmean == Catch::Approx((m.tmean * 1.8) + 32.0).margin(0.15));
        REQUIRE(i.tmax == Catch::Approx((m.tmax * 1.8) + 32.0).margin(0.15));
        REQUIRE(i.precipitation == Catch::Approx(m.precipitation / 25.4).margin(0.007));
        REQUIRE(i.snowfall == Catch::Approx(m.snowfall / 2.54).margin(0.07));
        REQUIRE(i.wind == Catch::Approx(m.wind / 0.44704).margin(0.17));
        // Everything else is the same day.
        REQUIRE(i.frost == m.frost);
        REQUIRE(i.condition == m.condition);
        REQUIRE(i.type == m.type);
        REQUIRE(i.cloud_cover == m.cloud_cover);
        REQUIRE(i.humidity == m.humidity);
        REQUIRE(i.sunshine_hours == m.sunshine_hours);
        REQUIRE(i.elevation == m.elevation);
        REQUIRE(i.sunrise == m.sunrise);
    }
    const auto paris = at(48.85, 2.35, 2024y / 1 / 15);
    auto imperial = paris;
    imperial.units = dasmig::unit_system::imperial;
    // The JSON always says its units (since 2.0).
    REQUIRE(gen.get_weather(paris, 3).to_json().starts_with(R"({"date":"2024-01-15","units":"metric","place":)"));
    REQUIRE(gen.get_weather(imperial, 3).to_json().starts_with(R"({"date":"2024-01-15","units":"imperial","place":)"));
    REQUIRE(gen.get_weather(imperial, 3).to_string().find("°F") != std::string::npos);
    REQUIRE(dasmig::to_string(dasmig::unit_system::imperial) == "imperial");
    imperial.units = static_cast<dasmig::unit_system>(7);
    REQUIRE_THROWS_AS(gen.get_weather(imperial, 3), std::invalid_argument);
}

TEST_CASE("hours belong to their day and add up to it", "[hours]")
{
    const auto gen = make_loaded_generator();
    for (const auto& [lat, lon] :
         std::vector<std::pair<double, double>>{{48.85, 2.35}, {-33.9, 151.2}, {55.75, 37.6}, {1.3, 103.8}, {40.0, -105.0}})
    {
        auto r = at(lat, lon, 2023y / 11 / 20);
        const auto days = gen.get_weather_series(r, 60, 21);
        r.hourly = true;
        const auto hourly = gen.get_weather_series(r, 60, 21);
        REQUIRE(hourly.size() == days.size());
        for (std::size_t k = 0; k < days.size(); ++k)
        {
            const auto& d = hourly[k];
            REQUIRE(days[k].hours.empty());
            // The day itself does not change.
            auto copy = d;
            copy.hours.clear();
            REQUIRE(copy.to_json() == days[k].to_json());
            REQUIRE(d.hours.size() == 24);
            long long water = 0;
            long long snow = 0;
            double daylight = 0.0;
            double sunshine = 0.0;
            double wind = 0.0;
            // The night before runs from the day before's high, the evening
            // towards the next day's low, which can lie outside the day's
            // range (the series' ends have neighbours outside it).
            const bool inner = k > 0 && k + 1 < days.size();
            const double low = inner ? std::min({d.tmin, days[k + 1].tmin, days[k - 1].tmax}) - 0.05 : -1000.0;
            const double high = inner ? std::max({d.tmax, days[k - 1].tmax, days[k + 1].tmin}) + 0.05 : 1000.0;
            for (int h = 0; h < 24; ++h)
            {
                const auto& o = d.hours[static_cast<std::size_t>(h)];
                REQUIRE(o.hour == h);
                REQUIRE(o.time == d.hours.front().time + std::chrono::hours{h});
                water += std::llround(o.precipitation * 10.0);
                snow += std::llround(o.snowfall * 10.0);
                daylight += o.daylight_hours;
                sunshine += o.sunshine_hours;
                wind += o.wind;
                REQUIRE(o.temperature >= low);
                REQUIRE(o.temperature <= high);
                REQUIRE(o.sunshine_hours <= o.daylight_hours + 0.005);
                REQUIRE((o.cloud_cover >= 0.0 && o.cloud_cover <= 1.0));
                REQUIRE((o.humidity >= 5.0 && o.humidity <= 100.0));
                if (o.precipitation > 0.0) REQUIRE(o.type == d.type);
                else REQUIRE(o.type == dasmig::precipitation_type::none);
                if (o.thunder) REQUIRE(o.condition == dasmig::weather_condition::thunderstorm);
            }
            REQUIRE(water == std::llround(d.precipitation * 10.0));
            REQUIRE(snow == std::llround(d.snowfall * 10.0));
            REQUIRE(daylight == Catch::Approx(d.daylight_hours).margin(0.13));
            REQUIRE(sunshine == Catch::Approx(d.sunshine_hours).margin(0.2));
            REQUIRE(wind / 24.0 == Catch::Approx(d.wind).margin(0.1));
            // Hour 0 starts at local mean midnight.
            const auto midnight = std::chrono::sys_time<std::chrono::minutes>{std::chrono::sys_days{d.date}} -
                                  std::chrono::minutes{std::llround(4.0 * lon)};
            REQUIRE(d.hours.front().time == midnight);
            // Midnight is continuous: the next day starts near where this one ends.
            if (k + 1 < days.size())
            {
                REQUIRE(std::abs(hourly[k + 1].hours.front().temperature - d.hours.back().temperature) < 3.0);
            }
        }
        // A day's hours do not depend on where its series starts.
        auto one = r;
        one.date = hourly[17].date;
        REQUIRE(gen.get_weather(one, 21).to_json() == hourly[17].to_json());
    }
}

TEST_CASE("hours: the warmest hours follow the sun, polar days have hours too", "[hours]")
{
    const auto gen = make_loaded_generator();
    // Over many days, the coolest hour is near sunrise and the warmest in
    // the afternoon of local mean time, east or west.
    for (const double lon : {-120.0, 0.0, 115.0})
    {
        std::array<double, 24> mean{};
        for (std::uint64_t s = 0; s < 200; ++s)
        {
            auto r = at(35.0, lon, 2023y / 4 / 15);
            r.hourly = true;
            const auto w = gen.get_weather(r, s);
            for (std::size_t h = 0; h < 24; ++h) mean.at(h) += w.hours[h].temperature / 200.0;
        }
        const auto warmest = std::distance(mean.begin(), std::ranges::max_element(mean));
        const auto coolest = std::distance(mean.begin(), std::ranges::min_element(mean));
        REQUIRE((warmest >= 13 && warmest <= 16));
        REQUIRE((coolest >= 4 && coolest <= 6));
    }
    for (const auto date : {2023y / 6 / 21, 2023y / 12 / 21})
    {
        auto r = at(78.2, 15.6, date); // Svalbard: polar day, then polar night
        r.hourly = true;
        const auto w = gen.get_weather(r, 3);
        REQUIRE(w.hours.size() == 24);
        double daylight = 0.0;
        for (const auto& h : w.hours) daylight += h.daylight_hours;
        REQUIRE(daylight == (date == 2023y / 6 / 21 ? 24.0 : 0.0));
    }
}

TEST_CASE("historical months need the history and its years", "[history][errors]")
{
    auto gen = make_loaded_generator();
    auto r = at(48.85, 2.35, 2010y / 1 / 15);
    r.historical = true;
    REQUIRE_FALSE(gen.has_history());
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::runtime_error);
    REQUIRE_FALSE(history_file().empty());
    gen.load_history(history_file());
    REQUIRE(gen.has_history());
    REQUIRE_NOTHROW(gen.get_weather(r, 1));
    r.date = 1990y / 12 / 31;
    REQUIRE_THROWS_AS(gen.get_weather(r, 1), std::invalid_argument);
    r.date = 2020y / 12 / 31;
    REQUIRE_NOTHROW(gen.get_weather(r, 1));
    REQUIRE_THROWS_AS(gen.get_weather_series(r, 2, 1), std::invalid_argument); // runs into 2021
    REQUIRE_THROWS_AS(gen.load_history(std::filesystem::path{"no/such/history.tsv"}), dasmig::core::tsv_error);
    REQUIRE_THROWS_AS(gen.load_history(std::filesystem::path{lite_dir() / "climate.tsv"}), dasmig::core::tsv_error);
    REQUIRE(gen.has_history()); // a failed load keeps the months
}

TEST_CASE("historical months follow the months as they were", "[history][stats]")
{
    auto gen = make_loaded_generator();
    gen.load_history(history_file());
    // The anomalies of the block from the file itself.
    const auto doc = dasmig::core::tsv_document::load(history_file());
    const auto months_of = [&](const std::string& key, std::string_view column)
    {
        for (const auto& row : doc.rows())
        {
            if (row.get<std::string>("key") != key) continue;
            std::vector<double> out;
            for (const auto item : row.list(column)) out.push_back(std::stod(std::string{item}));
            return out;
        }
        FAIL("no block " << key);
        return std::vector<double>{};
    };
    // Paris in January 2010 (a cold winter in Europe) and a wet and a dry
    // month there, over many seeds.
    const auto tmean = months_of("49.00,3.00", "tmean");
    const auto precipitation = months_of("49.00,3.00", "precipitation");
    const auto month_index = [](int year, int month) { return static_cast<std::size_t>(((year - 1991) * 12) + month - 1); };
    REQUIRE(tmean[month_index(2010, 1)] < -1.0);

    const auto mean_of = [&](int year, unsigned month, bool historical)
    {
        double t = 0.0;
        double p = 0.0;
        constexpr int seeds = 60;
        for (std::uint64_t s = 0; s < seeds; ++s)
        {
            auto r = at(48.85, 2.35, std::chrono::year{year} / std::chrono::month{month} / 1);
            r.historical = historical;
            const auto days = gen.get_weather_series(r, 28, s);
            for (const auto& d : days)
            {
                t += d.tmean / (28.0 * seeds);
                p += d.precipitation / seeds;
            }
        }
        return std::pair{t, p};
    };
    for (const auto [year, month] : std::vector<std::pair<int, unsigned>>{{2010, 1}, {2003, 8}, {2016, 6}, {1996, 3}})
    {
        INFO(year << "-" << month);
        const auto [t_normal, p_normal] = mean_of(year, month, false);
        const auto [t_history, p_history] = mean_of(year, month, true);
        const auto k = month_index(year, static_cast<int>(month));
        REQUIRE(t_history - t_normal == Catch::Approx(tmean[k]).margin(0.6));
        REQUIRE(p_history / p_normal == Catch::Approx(precipitation[k] / 100.0).epsilon(0.25));
    }
    // The signature carries the history's data too.
    auto r = at(48.85, 2.35, 2010y / 1 / 15);
    REQUIRE(gen.get_weather(r, 1).signature().data == gen.data_hash());
    r.historical = true;
    REQUIRE(gen.get_weather(r, 1).signature().data != gen.data_hash());
    // A historical day does not depend on where its series starts either.
    const auto series = gen.get_weather_series(r, 90, 4);
    auto later = r;
    later.date = series[61].date;
    REQUIRE(gen.get_weather(later, 4).to_json() == series[61].to_json());
}

TEST_CASE("known answers for units, hours and historical months", "[kat]")
{
    auto gen = make_loaded_generator();
    gen.load_history(history_file());
    std::uint64_t h = 0xcbf29ce484222325ULL;
    auto r = at(48.85, 2.35, 2010y / 1 / 1);
    r.units = dasmig::unit_system::imperial;
    for (const auto& w : gen.get_weather_series(r, 365, 11)) h = json_hash(h, w.to_json());
    r.units = dasmig::unit_system::metric;
    r.hourly = true;
    for (const auto& w : gen.get_weather_series(r, 60, 12)) h = json_hash(h, w.to_json());
    r.hourly = false;
    r.historical = true;
    for (const auto& w : gen.get_weather_series(r, 365, 13)) h = json_hash(h, w.to_json());
    r = at(-33.9, 151.2, 1998y / 6 / 1);
    r.historical = true;
    r.hourly = true;
    r.units = dasmig::unit_system::imperial;
    for (const auto& w : gen.get_weather_series(r, 30, 14)) h = json_hash(h, w.to_json());
    REQUIRE(h == 0x3b94efcfdd013e22ULL);
}

// ---------------------------------------------------------------------------
// Extending data (rule 8)
// ---------------------------------------------------------------------------

TEST_CASE("extend adds a station, replaces and removes cells", "[extend]")
{
    auto gen = make_loaded_generator();
    const auto base_count = gen.count();
    const auto base_hash = gen.data_hash();
    const auto before = gen.get_weather(at(10.0, 10.0), 7);

    // A station between grid points serves the points nearest it.
    const auto& info = gen.extend_from_string(header + station_row("my-station", 10.0, 10.0, 25.0, 0.0), "mod.tsv");
    REQUIRE(info.added == 1);
    REQUIRE(gen.count() == base_count + 1);
    REQUIRE(gen.data_hash() != base_hash);
    REQUIRE(gen.layers().size() == 2);
    const auto w = gen.get_weather(at(10.0, 10.0), 7);
    REQUIRE(w.cell == "my-station");
    REQUIRE(w.type == dasmig::precipitation_type::none); // no wet days
    REQUIRE(w.signature().data == gen.data_hash());
    REQUIRE((w.tmax + w.tmin) / 2 == Catch::Approx(25.0).margin(8.0));
    // An area of 0: never drawn as a random place.
    for (std::uint64_t s = 0; s < 2000; ++s) REQUIRE(gen.get_weather({}, s).cell != "my-station");

    // Only the required columns: the rest take their defaults.
    gen.extend_from_string("key\tlatitude\tlongitude\ttmean\tdtr\tprecipitation\twet_days\n"
                           "bare\t-40\t-120\t" "10;10;10;10;10;10;10;10;10;10;10;10\t"
                           "5;5;5;5;5;5;5;5;5;5;5;5\t" "30;30;30;30;30;30;30;30;30;30;30;30\t"
                           "10;10;10;10;10;10;10;10;10;10;10;10\n",
                           "bare.tsv");
    REQUIRE(gen.climate_at(-40, -120).humidity[0] == 70.0);
    REQUIRE(gen.climate_at(-40, -120).elevation == 0.0);

    gen.extend_from_string("key\tdisabled\nmy-station\t1\nbare\t1\n", "remove.tsv");
    REQUIRE(gen.count() == base_count);

    // Reloading the base drops every layer.
    REQUIRE(gen.load(dasmig::dataset::lite));
    REQUIRE(gen.data_hash() == base_hash);
    REQUIRE(gen.get_weather(at(10.0, 10.0), 7).to_json() == before.to_json());
}

TEST_CASE("a bad layer is rejected and leaves the data unchanged", "[extend][errors]")
{
    auto gen = make_loaded_generator();
    const auto hash = gen.data_hash();
    const auto count = gen.count();

    const auto row = station_row("x", 0, 0, 20, 5);
    REQUIRE_THROWS_WITH(gen.extend_from_string(header + "x\tnorth\t0" + row.substr(row.find("\t100")), "bad.tsv"),
                        "bad.tsv:2: column 'latitude': expected a number, got 'north'");
    REQUIRE_THROWS_AS(gen.extend_from_string(header + station_row("x", 95, 0, 20, 5), "lat.tsv"),
                      dasmig::core::tsv_error);
    REQUIRE_THROWS_AS(gen.extend_from_string(header + station_row("x", 0, 0, 20, 40), "wet.tsv"),
                      dasmig::core::tsv_error); // 40 wet days in a month
    REQUIRE_THROWS_AS(gen.extend_from_string("key\tlatitude\tlongitude\ttmean\tdtr\tprecipitation\twet_days\n"
                                             "x\t0\t0\t1;2;3\t1;2;3\t1;2;3\t1;2;3\n",
                                             "short.tsv"),
                      dasmig::core::tsv_error); // three months
    REQUIRE_THROWS_AS(gen.extend_from_string("key\tlatitude\tlongitude\ttmean\nx\t0\t0\t1\n", "partial.tsv"),
                      dasmig::core::tsv_error); // required columns missing
    REQUIRE_THROWS_AS(gen.extend_from_string(header + station_row("49.00,3.00", 49, 3, 10, 10), "clash.tsv",
                                             dasmig::core::layer_mode::add),
                      dasmig::core::tsv_error); // the cell is already loaded

    REQUIRE(gen.data_hash() == hash);
    REQUIRE(gen.count() == count);
    REQUIRE(gen.layers().size() == 1);
}

#ifdef DASMIG_WEATHER_EMBED
TEST_CASE("compiled-in data needs no files and matches them", "[embed]")
{
    const auto old_cwd = std::filesystem::current_path();
    const auto tmp = std::filesystem::temp_directory_path() / "wtg_embed_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::wtg gen;
    const bool loaded = gen.load(dasmig::dataset::lite);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);

    REQUIRE(loaded);
    REQUIRE(gen.layers().front().source == "embedded:lite/climate.tsv");
    REQUIRE(gen.data_hash() == 0x590a3332eb0f63a0ULL); // same as the file
    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{1}).to_json() ==
            R"({"date":"2023-07-01","units":"metric","place":{"cell":"-17.00,-65.00","latitude":-16.998,"longitude":-65,"elevation":1060,"distance_km":0},"condition":"fog","tmin":13.8,"tmean":20.1,"tmax":26.5,"frost":false,"precipitation":0,"snowfall":0,"snow_depth":0,"type":"none","thunder":false,"fog":true,"cloud_cover":0.19,"humidity":78,"wind":1.3,"gust":7.7,"daylight_hours":11.13,"sunshine_hours":9.7,"solar_radiation":17.7,"sunrise":"2023-07-01T10:50Z","sunset":"2023-07-01T21:58Z","signature":{"seed":"0000000000000001","data":"590a3332eb0f63a0"}})");
}
#endif

// ---------------------------------------------------------------------------
// Batch generation
// ---------------------------------------------------------------------------

TEST_CASE("weather batch: items equal the single calls for any thread count", "[batch]")
{
    auto gen = make_loaded_generator();
    const auto request = dasmig::weather_request{};
    const auto batch = gen.get_weather_batch(request, {.count = 200, .threads = 1}, 77);
    REQUIRE(batch.size() == 200);
    for (std::uint64_t i = 0; i < batch.size(); ++i)
    {
        REQUIRE(batch[i].to_json() == gen.get_weather(request, dasmig::core::item_seed(77, i)).to_json());
    }

    // A range is computed directly, on any number of threads.
    const auto tail = gen.get_weather_batch(request, {.count = 100, .first = 100, .threads = 4}, 77);
    REQUIRE(tail.size() == 100);
    for (std::size_t i = 0; i < tail.size(); ++i)
    {
        REQUIRE(tail[i].to_json() == batch[100 + i].to_json());
    }
    for (const std::size_t threads : {2U, 7U, 0U})
    {
        const auto other = gen.get_weather_batch(request, {.count = 200, .threads = threads}, 77);
        REQUIRE(other.size() == batch.size());
        for (std::size_t i = 0; i < other.size(); ++i)
        {
            REQUIRE(other[i].to_json() == batch[i].to_json());
        }
    }

    // Without a seed the batch still has the requested size.
    REQUIRE(gen.get_weather_batch(request, {.count = 5}).size() == 5);
}

TEST_CASE("weather batch: streaming gives the same items in order", "[batch]")
{
    auto gen = make_loaded_generator();
    const auto request = dasmig::weather_request{};
    const auto batch = gen.get_weather_batch(request, {.count = 300, .first = 10, .threads = 3}, 5);
    std::uint64_t next = 10;
    std::size_t seen = 0;
    gen.for_each_weather(request, {.count = 300, .first = 10, .threads = 3}, 5,
                         [&](std::uint64_t index, const dasmig::weather& item)
                         {
                             REQUIRE(index == next);
                             REQUIRE(item.to_json() == batch[seen].to_json());
                             ++next;
                             ++seen;
                         });
    REQUIRE(seen == batch.size());
}

TEST_CASE("weather batch: known answer of 10,000 items", "[batch][kat]")
{
    auto gen = make_loaded_generator();
    const auto request = dasmig::weather_request{};
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    gen.for_each_weather(request, {.count = 10000, .threads = 4}, 1,
                         [&](std::uint64_t, const dasmig::weather& item)
                         { hash = dasmig::core::hash_combine(hash, dasmig::core::fnv1a64(item.to_json())); });
    REQUIRE(hash == 0x71894cb102e53010ULL);
}
