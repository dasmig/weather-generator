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

    run.calls("get_weather", [&](std::uint64_t seed) { return gen.get_weather({}, seed); });
    run.calls("get_weather to_json", [&](std::uint64_t seed) {
        return gen.get_weather({}, seed).to_json();
    });

    run.batches("get_weather_batch", [&](std::size_t count, std::size_t threads, std::uint64_t seed) {
        return gen.get_weather_batch({}, {.count = count, .threads = threads}, seed);
    });

    return run.finish();
}
