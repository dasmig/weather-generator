// Generates 1991-2020 at each station of a list, for scripts/validate.py.
//
// Usage: sample_weather TIER STATIONS OUT
//   TIER      lite or full
//   STATIONS  tab-separated id, latitude, longitude, elevation (m)
//   OUT       tab-separated id, date, tmax, tmin, precipitation, type, thunder,
//             cloud_cover, humidity, wind, snow_depth, gust, fog, condition,
//             sunshine_hours
//
// Every station uses the same seed, so the sample is one world and nearby
// stations share its weather as the library's spatial correlation gives.
//
// Build from the repository root:
//   clang++ -std=c++23 -O2 -I. -o build/sample_weather scripts/sample_weather.cpp

#include "dasmig/weathergen.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cerr << "usage: sample_weather TIER STATIONS OUT\n";
        return 2;
    }
    dasmig::wtg gen;
    if (!gen.load(std::string_view{argv[1]} == "full" ? dasmig::dataset::full : dasmig::dataset::lite))
    {
        std::cerr << "no data\n";
        return 1;
    }
    std::ifstream in{argv[2]};
    std::ofstream out{argv[3]};
    std::string line;
    constexpr std::uint64_t seed = 1;
    while (std::getline(in, line))
    {
        std::istringstream fields{line};
        std::string id;
        double lat = 0.0;
        double lon = 0.0;
        double elev = 0.0;
        if (!(fields >> id >> lat >> lon >> elev)) continue;
        dasmig::weather_request r;
        r.latitude = lat;
        r.longitude = lon;
        if (elev > -500.0) r.elevation = elev;
        r.date = std::chrono::year{1991} / 1 / 1;
        const auto days = static_cast<int>((std::chrono::sys_days{std::chrono::year{2021} / 1 / 1} -
                                            std::chrono::sys_days{r.date})
                                               .count());
        std::vector<dasmig::weather> series;
        try
        {
            series = gen.get_weather_series(r, days, seed);
        }
        catch (const std::invalid_argument& e)
        {
            std::cerr << id << ": " << e.what() << '\n';
            continue;
        }
        for (const auto& w : series)
        {
            out << id << '\t' << w.date_string() << '\t' << w.tmax << '\t' << w.tmin << '\t' << w.precipitation
                << '\t' << dasmig::to_string(w.type) << '\t' << (w.thunder ? 1 : 0) << '\t' << w.cloud_cover << '\t'
                << w.humidity << '\t' << w.wind << '\t' << w.snow_depth << '\t' << w.gust << '\t' << (w.fog ? 1 : 0)
                << '\t' << dasmig::to_string(w.condition) << '\t' << w.sunshine_hours << '\n';
        }
    }
    return 0;
}
