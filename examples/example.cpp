#include "../dasmig/weathergen.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>

int main()
{
    using namespace std::chrono_literals;

    // The singleton loads the lite tier (compiled in, or from resources/).
    auto& gen = dasmig::wthg::instance();
    if (!gen.has_data())
    {
        std::cerr << "Could not load the weather data.\n";
        return 1;
    }
    std::cout << "Loaded " << gen.count() << " climate cells.\n\n";

    // A week in Paris, from a fixed seed.
    dasmig::weather_request paris;
    paris.latitude = 48.85;
    paris.longitude = 2.35;
    paris.date = 2024y / 1 / 15;
    std::cout << "Paris, a week from 2024-01-15 (seed 7):\n";
    for (const auto& day : gen.get_weather_series(paris, 7, 7)) std::cout << "  " << day << '\n';

    // Any day of that week alone is the same as inside the series.
    paris.date = 2024y / 1 / 18;
    std::cout << "\n2024-01-18 alone, same seed:\n  " << gen.get_weather(paris, 7) << '\n';

    // The same place higher up, and in a warmer climate.
    paris.elevation = 1500.0;
    std::cout << "  at 1,500 m: " << gen.get_weather(paris, 7) << '\n';
    paris.elevation.reset();
    paris.warming = 1.2;
    std::cout << "  1.2 °C warmer: " << gen.get_weather(paris, 7) << '\n';

    std::cout << "\nRandom places on land, 2023-07-01:\n";
    for (int i = 0; i < 5; ++i)
    {
        const auto w = gen.get_weather(dasmig::weather_request{});
        std::cout << "  " << w.cell << ": " << w << '\n';
    }

    const auto normals = gen.climate_at(-3.1, -60.0);
    std::cout << "\nManaus normals (cell " << normals.cell << "): January " << normals.tmean[0] << " °C, "
              << normals.precipitation[0] << " mm on " << normals.wet_days[0] << " days\n";

    std::cout << "\nOne day as JSON:\n  " << gen.get_weather(dasmig::weather_request{}, std::uint64_t{42}).to_json()
              << '\n';
    return 0;
}
