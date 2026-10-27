#include "catch_amalgamated.hpp"
#include "../dasmig/weathergen.hpp"

#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static dasmig::wthg make_loaded_generator()
{
    dasmig::wthg gen;
    if (!gen.load(dasmig::dataset::lite))
    {
        for (const auto* candidate : {"resources/lite", "../resources/lite"})
        {
            if (std::filesystem::is_directory(candidate))
            {
                gen.load(std::filesystem::path{candidate});
                break;
            }
        }
    }
    REQUIRE(gen.has_data());
    return gen;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

TEST_CASE("load(dataset::lite) loads the catalogue", "[loading]")
{
    const auto gen = make_loaded_generator();
    REQUIRE(gen.count() > 0);
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
}

TEST_CASE("get_weather with a seed is deterministic", "[generation][seed]")
{
    const auto gen = make_loaded_generator();
    for (std::uint64_t s = 0; s < 100; ++s)
    {
        REQUIRE(gen.get_weather(dasmig::weather_request{}, s).to_json() == gen.get_weather(dasmig::weather_request{}, s).to_json());
    }
}

TEST_CASE("a result replays from its seed", "[generation][seed]")
{
    auto gen = make_loaded_generator();
    const auto v = gen.get_weather(dasmig::weather_request{});
    REQUIRE(gen.get_weather(dasmig::weather_request{}, v.seed()).to_json() == v.to_json());
}

TEST_CASE("explicit per-call seeds replay across generator instances", "[seed]")
{
    const auto first = make_loaded_generator();
    const auto second = make_loaded_generator();
    REQUIRE(first.get_weather(dasmig::weather_request{}, 42).to_json() == second.get_weather(dasmig::weather_request{}, 42).to_json());
}

TEST_CASE("draws follow the weights", "[generation][stats]")
{
    // The sample weights are 5 : 3 : 1.5 : 0.5 : 0 (total 10).
    const auto gen = make_loaded_generator();
    std::map<std::string, int> counts;
    constexpr int draws = 20000;
    for (std::uint64_t s = 0; s < draws; ++s) ++counts[gen.get_weather(dasmig::weather_request{}, s).key];
    REQUIRE(counts["echo"] == 0); // weight 0 is never drawn
    REQUIRE(counts["alpha"] == Catch::Approx(draws * 0.5).margin(draws * 0.02));
    REQUIRE(counts["bravo"] == Catch::Approx(draws * 0.3).margin(draws * 0.02));
    REQUIRE(counts["charlie"] == Catch::Approx(draws * 0.15).margin(draws * 0.02));
    REQUIRE(counts["delta"] == Catch::Approx(draws * 0.05).margin(draws * 0.02));
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

TEST_CASE("string conversion and streaming give the label", "[conversion]")
{
    const auto gen = make_loaded_generator();
    const auto v = gen.get_weather(dasmig::weather_request{}, std::uint64_t{5});
    const std::string s = v;
    std::ostringstream os;
    os << v;
    REQUIRE(s == v.label);
    REQUIRE(os.str() == v.label);
}

// ---------------------------------------------------------------------------
// Known answers (rule 2): the same seed and data give these exact results on
// every compiler and platform. A change here is a change of output and needs
// a major version. These are the answers for the sample catalogue written by
// tools/new-library.py; record new ones when the data or the output changes.
// ---------------------------------------------------------------------------

TEST_CASE("known answers for fixed seeds", "[kat]")
{
    dasmig::wthg gen;
    gen.load(std::filesystem::path{"resources/lite"});
    if (!gen.has_data()) gen.load(std::filesystem::path{"../resources/lite"});
    REQUIRE(gen.has_data());
    REQUIRE(gen.data_hash() == 0x1f2808f7b5353d9dULL);

    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{1}).to_json() ==
            R"({"key":"alpha","label":"Alpha","signature":{"seed":"0000000000000001","data":"1f2808f7b5353d9d"}})");
    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{3}).to_json() ==
            R"({"key":"bravo","label":"Bravo","signature":{"seed":"0000000000000003","data":"1f2808f7b5353d9d"}})");
    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{5}).to_json() ==
            R"({"key":"charlie","label":"Charlie","signature":{"seed":"0000000000000005","data":"1f2808f7b5353d9d"}})");

    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (std::uint64_t s = 0; s < 10000; ++s)
    {
        for (const char c : gen.get_weather(dasmig::weather_request{}, s).to_json())
        {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
    }
    REQUIRE(h == 0x0c2676f3f5cd4d8dULL);
}

// ---------------------------------------------------------------------------
// JSON and signatures
// ---------------------------------------------------------------------------

TEST_CASE("to_json and signature", "[json]")
{
    const auto gen = make_loaded_generator();
    const auto v = gen.get_weather(dasmig::weather_request{}, std::uint64_t{99});
    REQUIRE(v.signature().seed == 99);
    REQUIRE(v.seed() == 99);
    REQUIRE(v.signature().data == gen.data_hash());
    const auto json = v.to_json();
    REQUIRE(json.starts_with(R"({"key":")"));
    REQUIRE(json.find(R"("signature":{"seed":"0000000000000063")") != std::string::npos);

    dasmig::core::json_writer w;
    w.begin_array();
    v.to_json(w);
    w.end_array();
    REQUIRE(w.str() == "[" + json + "]");
}

// ---------------------------------------------------------------------------
// Extending data (rule 8)
// ---------------------------------------------------------------------------

TEST_CASE("extend overrides, adds and removes entries", "[extend]")
{
    auto gen = make_loaded_generator();
    const auto base_count = gen.count();
    const auto base_hash = gen.data_hash();
    const auto before = gen.get_weather(dasmig::weather_request{}, std::uint64_t{7});

    // Only the new entry has weight: every draw picks it.
    const auto& info = gen.extend_from_string("key\tlabel\tweight\n"
                                              "alpha\tAlpha\t0\nbravo\tBravo\t0\n"
                                              "charlie\tCharlie\t0\ndelta\tDelta\t0\n"
                                              "zulu\tZulu\t1\n",
                                              "mod.tsv");
    REQUIRE(info.replaced == 4);
    REQUIRE(info.added == 1);
    REQUIRE(gen.count() == base_count + 1);
    REQUIRE(gen.data_hash() != base_hash);
    REQUIRE(gen.layers().size() == 2);
    const auto after = gen.get_weather(dasmig::weather_request{}, std::uint64_t{7});
    REQUIRE(after.key == "zulu");
    REQUIRE(after.signature().data == gen.data_hash());

    gen.extend_from_string("key\tdisabled\nzulu\t1\n", "remove.tsv");
    REQUIRE(gen.count() == base_count);
    REQUIRE_THROWS_AS(gen.get_weather(dasmig::weather_request{}, std::uint64_t{7}), std::invalid_argument); // all weights 0

    // Reloading the base drops every layer.
    REQUIRE(gen.load(dasmig::dataset::lite));
    REQUIRE(gen.data_hash() == base_hash);
    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{7}).to_json() == before.to_json());
}

TEST_CASE("a bad layer is rejected and leaves the data unchanged", "[extend][errors]")
{
    auto gen = make_loaded_generator();
    const auto hash = gen.data_hash();
    const auto count = gen.count();

    REQUIRE_THROWS_WITH(gen.extend_from_string("key\tlabel\tweight\nzulu\tZulu\theavy\n", "bad.tsv"),
                        "bad.tsv:2: column 'weight': expected a number, got 'heavy'");
    REQUIRE_THROWS_AS(gen.extend_from_string("key\tlabel\tweight\nzulu\tZulu\t-1\n", "negative.tsv"),
                      dasmig::core::tsv_error);
    REQUIRE_THROWS_AS(gen.extend_from_string("key\tlabel\nzulu\tZulu\n", "partial.tsv"),
                      dasmig::core::tsv_error); // required column missing
    REQUIRE_THROWS_AS(gen.extend_from_string("key\tlabel\tweight\nalpha\tAlpha\t1\n", "clash.tsv",
                                             dasmig::core::layer_mode::add),
                      dasmig::core::tsv_error); // alpha already loaded

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
    REQUIRE(gen.layers().front().source == "embedded:lite/weather.tsv");
    REQUIRE(gen.data_hash() == 0x1f2808f7b5353d9dULL); // same as the file
    REQUIRE(gen.get_weather(dasmig::weather_request{}, std::uint64_t{1}).to_json() ==
            R"({"key":"alpha","label":"Alpha","signature":{"seed":"0000000000000001","data":"1f2808f7b5353d9d"}})");
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
    REQUIRE(hash == 0x50470532f17ebda1ULL);
}
