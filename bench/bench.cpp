// Benchmarks for weather-generator; see bench.hpp for the options.

#include "../dasmig/weathergen.hpp"
#include "bench.hpp"

int main(int argc, char** argv)
{
    dasmig::bench::runner run{"weather-generator", argc, argv};

    run.load("load", [&] {
        dasmig::wtg gen;
        return gen.load(run.tier());
    });

    dasmig::wtg gen;
    if (!gen.load(run.tier())) return run.missing_tier();

    dasmig::weather_request paris;
    paris.latitude = 48.85;
    paris.longitude = 2.35;

    run.calls("get_weather", [&](std::uint64_t seed) { return gen.get_weather({}, seed); });
    run.calls("get_weather at a point", [&](std::uint64_t seed) { return gen.get_weather(paris, seed); });
    run.calls("get_weather to_json", [&](std::uint64_t seed) {
        return gen.get_weather({}, seed).to_json();
    });
    run.calls("get_weather_series 365 days", [&](std::uint64_t seed) {
        return gen.get_weather_series(paris, 365, seed);
    });
    // Points on land (a point at sea throws): drawn places, moved within
    // their cells.
    std::vector<std::pair<double, double>> land;
    for (std::uint64_t s = 0; s < 1024; ++s)
    {
        const auto w = gen.get_weather({}, s);
        land.emplace_back(w.latitude + (static_cast<double>(s % 7) * 0.05), w.longitude + (static_cast<double>(s % 5) * 0.05));
    }
    run.calls("climate_at", [&](std::uint64_t seed) {
        const auto [lat, lon] = land[seed % land.size()];
        return gen.climate_at(lat, lon);
    });

    run.batches("get_weather_batch", [&](std::size_t count, std::size_t threads, std::uint64_t seed) {
        return gen.get_weather_batch({}, {.count = count, .threads = threads}, seed);
    });

    return run.finish();
}
