// Benchmarks for weather-generator; see bench.hpp for the options.

#include "../dasmig/weathergen.hpp"
#include "bench.hpp"

int main(int argc, char** argv)
{
    dasmig::bench::runner run{"weather-generator", argc, argv};

    run.load("load", [&] {
        dasmig::wthg gen;
        return gen.load(run.tier());
    });

    dasmig::wthg gen;
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
    run.calls("climate_at", [&](std::uint64_t seed) {
        return gen.climate_at(static_cast<double>(seed % 140) - 60.0, static_cast<double>(seed % 360) - 180.0);
    });

    run.batches("get_weather_batch", [&](std::size_t count, std::size_t threads, std::uint64_t seed) {
        return gen.get_weather_batch({}, {.count = count, .threads = threads}, seed);
    });

    return run.finish();
}
