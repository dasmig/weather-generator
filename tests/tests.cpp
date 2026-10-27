#include "catch_amalgamated.hpp"
#include "../dasmig/weathergen.hpp"

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

static dasmig::wthg make_loaded_generator()
{
    dasmig::wthg gen;
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
                                  "humidity\tsunshine\twind\n";

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_CASE("load(dataset::lite) loads the climate grid", "[loading]")
{
    const auto gen = make_loaded_generator();
    REQUIRE(gen.count() == 5043);
    REQUIRE(gen.layers().size() == 1);
}

TEST_CASE("load - non-existent path is a no-op", "[loading]")
{
    dasmig::wthg gen;
    gen.load(std::filesystem::path{"no/such/directory"});
    REQUIRE_FALSE(gen.has_data());
    REQUIRE(gen.data_hash() == 0);
}

#ifndef DASMIG_WEATHER_EMBED // compiled-in data is never missing
TEST_CASE("load(dataset) returns false for a missing tier", "[loading]")
{
    const auto old_cwd = std::filesystem::current_path();
    const auto tmp = std::filesystem::temp_directory_path() / "wthg_empty_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::wthg gen;
    const bool loaded = gen.load(dasmig::dataset::full);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);
    REQUIRE_FALSE(loaded);
    REQUIRE_FALSE(gen.has_data());
}
#endif

TEST_CASE("the singleton loads data on first use", "[loading]")
{
    REQUIRE(dasmig::wthg::instance().has_data());
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

TEST_CASE("get_weather throws when no data is loaded", "[generation]")
{
    dasmig::wthg gen;
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
    REQUIRE_THROWS_AS(gen.get_weather_series(at(0, 0), dasmig::wthg::max_series_days + 1, 1),
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
        REQUIRE(w.tmin <= w.tmax);
        REQUIRE(w.cloud_cover >= 0.0);
        REQUIRE(w.cloud_cover <= 1.0);
        REQUIRE(w.humidity >= 5.0);
        REQUIRE(w.humidity <= 100.0);
        REQUIRE(w.wind >= 0.0);
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
    REQUIRE(seen.size() == 12); // every condition occurs
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
        const auto c = gen.climate_at(lat, lon);
        double best = 10.0;
        for (const auto& cell : cells) best = std::min(best, great_circle(lat, lon, cell.lat, cell.lon));
        REQUIRE(great_circle(lat, lon, c.latitude, c.longitude) == Catch::Approx(best).margin(1e-9));
    }
    // Paris is in the 2-degree block centred at 49, 3; the dateline wraps.
    REQUIRE(gen.climate_at(48.85, 2.35).cell == "49.00,3.00");
    REQUIRE(gen.climate_at(-17.0, 179.99).cell == gen.climate_at(-17.0, -179.99).cell);
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
            for (const auto& w : gen.get_weather_series(at(lat, lon, 1961y / 1 / 1), 30 * 365 + 7, seed))
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
    REQUIRE(json.starts_with(R"({"date":"2023-07-01","place":{"cell":"49.00,3.00","latitude":48.85,)"));
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
    dasmig::wthg gen;
    gen.load(lite_dir());
    REQUIRE(gen.has_data());
    REQUIRE(gen.data_hash() == 0x74fdb50d249dd970ULL);

    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{1}).to_json() ==
            R"({"date":"2023-07-01","place":{"cell":"-17.00,-61.00","latitude":-16.998,"longitude":-61,"elevation":344},"condition":"clear","tmin":15.3,"tmax":26.1,"precipitation":0,"snowfall":0,"type":"none","thunder":false,"cloud_cover":0.17,"humidity":62,"wind":4.9,"signature":{"seed":"0000000000000001","data":"74fdb50d249dd970"}})");
    REQUIRE(gen.get_weather(at(48.85, 2.35, 2024y / 1 / 15), std::uint64_t{3}).to_json() ==
            R"({"date":"2024-01-15","place":{"cell":"49.00,3.00","latitude":48.85,"longitude":2.35,"elevation":117},"condition":"sleet","tmin":-0.4,"tmax":4.8,"precipitation":1.1,"snowfall":0.6,"type":"sleet","thunder":false,"cloud_cover":1,"humidity":92,"wind":4.2,"signature":{"seed":"0000000000000003","data":"74fdb50d249dd970"}})");
    REQUIRE(gen.get_weather(at(62.0, 129.7, 2024y / 2 / 29), std::uint64_t{5}).to_json() ==
            R"({"date":"2024-02-29","place":{"cell":"63.00,129.00","latitude":62,"longitude":129.7,"elevation":171},"condition":"partly_cloudy","tmin":-42.7,"tmax":-35,"precipitation":0,"snowfall":0,"type":"none","thunder":false,"cloud_cover":0.44,"humidity":86,"wind":1.8,"signature":{"seed":"0000000000000005","data":"74fdb50d249dd970"}})");

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
    REQUIRE(h == 0xbdf80ad7f66e9d04ULL);
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
    const auto tmp = std::filesystem::temp_directory_path() / "wthg_embed_dir";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    dasmig::wthg gen;
    const bool loaded = gen.load(dasmig::dataset::lite);
    std::filesystem::current_path(old_cwd);
    std::filesystem::remove_all(tmp);

    REQUIRE(loaded);
    REQUIRE(gen.layers().front().source == "embedded:lite/climate.tsv");
    REQUIRE(gen.data_hash() == 0x74fdb50d249dd970ULL); // same as the file
    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{1}).to_json() ==
            R"({"date":"2023-07-01","place":{"cell":"-17.00,-61.00","latitude":-16.998,"longitude":-61,"elevation":344},"condition":"clear","tmin":15.3,"tmax":26.1,"precipitation":0,"snowfall":0,"type":"none","thunder":false,"cloud_cover":0.17,"humidity":62,"wind":4.9,"signature":{"seed":"0000000000000001","data":"74fdb50d249dd970"}})");
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
    REQUIRE(hash == 0x0777d6b2fa239ba4ULL);
}
