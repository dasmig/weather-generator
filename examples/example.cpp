#include "../dasmig/weathergen.hpp"
#include <cstdint>
#include <iostream>

int main()
{
    // The singleton loads the lite tier (compiled in, or from resources/).
    auto& gen = dasmig::wthg::instance();
    if (!gen.has_data())
    {
        std::cerr << "Could not load the weather data.\n";
        return 1;
    }
    std::cout << "Loaded " << gen.count() << " entries.\n\n";

    std::cout << "Random:\n";
    for (int i = 0; i < 5; ++i)
    {
        std::cout << "  " << gen.get_weather(dasmig::weather_request{}) << '\n';
    }

    std::cout << "\nSeed 42, twice:\n";
    const auto a = gen.get_weather(dasmig::weather_request{}, std::uint64_t{42});
    const auto b = gen.get_weather(dasmig::weather_request{}, std::uint64_t{42});
    std::cout << "  " << a.to_json() << '\n'
              << "  " << b.to_json() << '\n';

    std::cout << "\nWith a layer that adds an entry:\n";
    gen.extend_from_string("key\tlabel\tweight\nzulu\tZulu\t10\n", "example.tsv");
    for (int i = 0; i < 5; ++i)
    {
        std::cout << "  " << gen.get_weather(dasmig::weather_request{}) << '\n';
    }
    return 0;
}
