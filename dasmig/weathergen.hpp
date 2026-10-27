// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Dasso Migotto

#ifndef DASMIG_WEATHERGEN_HPP
#define DASMIG_WEATHERGEN_HPP

#include "core.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef DASMIG_WEATHER_EMBED
#include "weathergen_data.hpp"
#endif

/// @file weathergen.hpp
/// @brief Weather Generator library — daily weather for any place and date from climate normals for C++23.
/// @author Diego Dasso Migotto (diegomigotto at hotmail dot com)
/// @see See doc/usage.md for the narrative tutorial.

DASMIG_CORE_REQUIRE(1, 1);

namespace dasmig
{

/// @brief What a weather call asks for.
///
/// Every generation call takes one; `weather_request{}` asks for the
/// defaults. Choices are never stored on the generator, so one call cannot
/// change the next. The skeleton has no fields yet: add the library's own
/// (a country, an age, keys to include or exclude) here.
struct weather_request
{
};

/// @brief Return type for weather generation.
///
/// Supports implicit conversion to std::string (the label), streaming via
/// operator<< and JSON output via to_json().
class weather
{
  public:
    std::string key;   ///< Catalogue key (the `key` column).
    std::string label; ///< Display label (the `label` column).

    /// @brief Retrieve the random seed used to generate this weather.
    /// @return The call seed; pass it to get_weather(request, seed) with
    ///         the same request and data to replay this weather.
    [[nodiscard]] std::uint64_t seed() const { return _signature.seed; }

    /// @brief The seed and the data this weather was generated from.
    /// @return The replay signature; `data` is wthg::data_hash() at the
    ///         time of generation.
    [[nodiscard]] const core::seed_signature& signature() const
    {
        return _signature;
    }

    /// @brief Human-readable summary string.
    /// @return The label.
    [[nodiscard]] std::string to_string() const { return label; }

    /// @brief Write this weather as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object()
            .field("key", key)
            .field("label", label)
            .key("signature");
        _signature.to_json(w);
        w.end_object();
    }

    /// @brief This weather as a JSON document.
    /// @return Compact JSON, for example
    ///   `{"key":"...","label":"...","signature":{...}}`.
    [[nodiscard]] std::string to_json() const
    {
        core::json_writer w;
        to_json(w);
        return w.str();
    }

    /// @brief Implicit conversion to std::string.
    /// @return Summary string.
    operator std::string() const // NOLINT(hicpp-explicit-conversions)
    {
        return to_string();
    }

    /// @brief Stream the summary to an output stream.
    /// @param os Output stream.
    /// @param v The weather to stream.
    /// @return The stream.
    friend std::ostream& operator<<(std::ostream& os, const weather& v)
    {
        os << v.to_string();
        return os;
    }

  private:
    core::seed_signature _signature;
    friend class wthg;
};

/// @brief Weather Generator: draws entries from a weighted catalogue.
///
/// Each draw comes from the `"pick"` stream split from the call seed, so
/// fields added later draw from streams of their own and leave this one
/// unchanged. Output is identical on every compiler and platform
/// (dasmig-core).
///
/// Can be used as a singleton via instance() or constructed
/// independently.
///
/// @par Thread safety
/// Each instance is independent.  Concurrent calls to get_weather(request) on the
/// **same** instance require external synchronisation.
class wthg
{
  public:
    /// @brief Default constructor — creates an empty generator with no
    ///        data.
    wthg() = default;

    wthg(const wthg&) = delete;            ///< Not copyable.
    wthg& operator=(const wthg&) = delete; ///< Not copyable.
    wthg(wthg&&) noexcept = default;       ///< Movable.
    wthg& operator=(wthg&&) noexcept = default; ///< Movable.
    ~wthg() = default;                       ///< Destructor.

    /// @brief Access the global singleton instance.
    ///
    /// Loads the `lite` tier (or `full` if `lite` is missing) on first
    /// access, from compiled-in data or an auto-probed `resources/`.
    /// @return The singleton.
    static wthg& instance()
    {
        static wthg inst{auto_probe_tag{}};
        return inst;
    }

    // -- Generation -------------------------------------------------------

    /// @brief Generate a random weather.
    ///        Use weather_request{} for the defaults.
    /// @param request What to generate.
    /// @return The weather.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If every loaded entry has weight 0.
    [[nodiscard]] weather get_weather([[maybe_unused]] const weather_request& request)
    {
        return get_weather(request, draw_seed_());
    }

    /// @brief Generate a deterministic weather.
    /// @param request What to generate.
    /// @param call_seed Seed; the same request, seed and data give the same
    ///        result. Does not touch the instance engine.
    /// @return The weather.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If every loaded entry has weight 0.
    [[nodiscard]] weather get_weather([[maybe_unused]] const weather_request& request,
                                    std::uint64_t call_seed) const
    {
        require_data_();
        const core::rng call{call_seed};
        auto r = call.split("pick");
        const auto& e = _entries[_weights.pick(r)];

        weather v;
        v._signature = {.seed = call_seed, .data = data_hash()};
        v.key = e.key;
        v.label = e.label;
        return v;
    }

    // -- Batch generation -------------------------------------------------

    /// @brief Generate a batch of weather results from a seed drawn like
    ///        get_weather() draws its own.
    ///
    /// Item `i` is `get_weather(request, core::item_seed(batch_seed, options.first + i))`
    /// for the drawn batch seed; see the seeded overload.
    ///
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @return The results, in index order.
    [[nodiscard]] std::vector<weather> get_weather_batch(const weather_request& request,
                                                  const core::batch_options& options)
    {
        return get_weather_batch(request, options, draw_seed_());
    }

    /// @brief Generate a batch of weather results, deterministically.
    ///
    /// Item `i` is exactly `get_weather(request, core::item_seed(batch_seed,
    /// options.first + i))`, so an item replays on its own, a range can be
    /// generated directly, two batches that follow each other equal one, and
    /// the output does not depend on `options.threads`.
    ///
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness (see
    ///        core::unique_rule; the rule must be made for `weather`).
    /// @param batch_seed Seed of the whole batch.
    /// @return The results, in index order.
    /// @throws std::invalid_argument If the request or options are invalid.
    /// @throws std::runtime_error If no data has been loaded, or a unique
    ///         batch runs out of attempts.
    [[nodiscard]] std::vector<weather> get_weather_batch(const weather_request& request,
                                                  const core::batch_options& options,
                                                  std::uint64_t batch_seed) const
    {
        return core::run_batch(options, batch_seed, [&](std::uint64_t item_seed)
                               { return get_weather(request, item_seed); });
    }

    /// @brief Stream a batch of weather results to a sink without holding the
    ///        whole batch.
    ///
    /// Generates the same items as get_weather_batch() and calls
    /// `sink(index, item)` in index order on the calling thread; `index`
    /// includes `options.first`.
    ///
    /// @tparam Sink Callable taking `(std::uint64_t, weather&)`; for example
    ///         core::jsonl_writer or core::csv_writer.
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @param batch_seed Seed of the whole batch.
    /// @param sink Receives the items.
    template <typename Sink>
    void for_each_weather(const weather_request& request, const core::batch_options& options,
                        std::uint64_t batch_seed, Sink&& sink) const
    {
        core::for_each_batch(options, batch_seed,
                             [&](std::uint64_t item_seed) { return get_weather(request, item_seed); },
                             std::forward<Sink>(sink));
    }

    /// @brief Stream a batch of weather results with a batch seed drawn like
    ///        get_weather() draws its own.
    /// @tparam Sink Callable taking `(std::uint64_t, weather&)`.
    /// @param request What every item is generated from.
    /// @param options Count, first index, threads and uniqueness.
    /// @param sink Receives the items.
    template <typename Sink>
    void for_each_weather(const weather_request& request, const core::batch_options& options,
                        Sink&& sink)
    {
        for_each_weather(request, options, draw_seed_(), std::forward<Sink>(sink));
    }

    // -- Data management --------------------------------------------------

    /// @brief Check whether any data has been loaded.
    /// @return True when at least one entry is loaded.
    [[nodiscard]] bool has_data() const { return !_entries.empty(); }

    /// @brief Return the number of loaded entries.
    /// @return The count.
    [[nodiscard]] std::size_t count() const { return _entries.size(); }

    /// @brief Load data from a resource directory, replacing all data and
    ///        layers loaded before.
    ///
    /// Expects the directory to contain `weather.tsv`. A path that is not
    /// a directory, or a directory without the file, is ignored.
    ///
    /// @param dir Directory such as `resources/lite`.
    /// @throws core::tsv_error If the file is malformed.
    void load(const std::filesystem::path& dir)
    {
        std::error_code ec;
        const auto file = dir / data_file_;
        if (!std::filesystem::is_directory(dir, ec) ||
            !std::filesystem::is_regular_file(file, ec))
        {
            return;
        }
        (void)extend(core::tsv_document::load(file), core::layer_mode::replace);
    }

    /// @brief Load a dataset tier, replacing all data and layers loaded
    ///        before.
    ///
    /// Uses the compiled-in copy when the library is built with
    /// `DASMIG_WEATHER_EMBED`, otherwise searches `resources/`, `../resources`,
    /// `weather-generator/resources` and `../weather-generator/resources`.
    ///
    /// @param tier The tier.
    /// @return False when the tier was not found.
    /// @throws core::tsv_error If the file is malformed.
    [[nodiscard]] bool load(dataset tier)
    {
        const std::string path = std::string{core::tier_name(tier)} + "/" + std::string{data_file_};
        auto doc = core::load_resource(path, embedded_files_(),
                                       core::find_resources(library_name_, path));
        if (!doc) return false;
        (void)extend(std::move(*doc), core::layer_mode::replace);
        return true;
    }

    /// @brief Add a data layer from a TSV file with the `weather.tsv`
    ///        columns.
    ///
    /// Rows are keyed by `key`. In the default override mode a row
    /// replaces the loaded row with the same key and new keys are added;
    /// a row with `disabled` set to 1 removes its key.
    ///
    /// @param file The layer file.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did (rows added, replaced, removed, warnings).
    /// @throws core::tsv_error If the file cannot be read or a row is
    ///         invalid; the data is then unchanged.
    const core::layer_info& extend(const std::filesystem::path& file,
                                   core::layer_mode mode = core::layer_mode::override)
    {
        return extend(core::tsv_document::load(file), mode);
    }

    /// @brief Add a data layer from TSV text (for example from a
    ///        WebAssembly host).
    /// @param text TSV with the `weather.tsv` columns.
    /// @param source Name used in errors.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did.
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend_from_string(std::string_view text, std::string source,
                                               core::layer_mode mode = core::layer_mode::override)
    {
        return extend(core::tsv_document::parse(text, std::move(source)), mode);
    }

    /// @brief Add a data layer.
    /// @param doc TSV with the `weather.tsv` columns.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did.
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend(core::tsv_document doc,
                                   core::layer_mode mode = core::layer_mode::override)
    {
        const auto& info = _table.extend(std::move(doc), mode, [](const core::tsv_row& row) {
            (void)entry::parse(row);
        });
        rebuild_();
        return info;
    }

    /// @brief The data layers loaded, in order.
    /// @return The layer records.
    [[nodiscard]] const std::vector<core::layer_info>& layers() const
    {
        return _table.layers();
    }

    /// @brief Hash of the loaded data, recorded in each result's
    ///        signature().
    /// @return The hash; 0 when nothing is loaded.
    [[nodiscard]] std::uint64_t data_hash() const { return _table.data_hash(); }

  private:
    // -- Internal data structures -----------------------------------------

    static constexpr std::string_view library_name_ = "weather-generator";
    static constexpr std::string_view data_file_ = "weather.tsv";

    static core::table_schema schema_()
    {
        return {.key = {"key"}, .required = {"label", "weight"}, .optional = {}};
    }

    struct entry
    {
        std::string key;
        std::string label;
        double weight{0.0};

        // Parse and check a row; throws core::tsv_error located at the row.
        static entry parse(const core::tsv_row& row)
        {
            entry e;
            e.key = row.get<std::string>("key");
            e.label = row.get<std::string>("label");
            if (e.label.empty()) throw row.error("label", "must not be empty");
            e.weight = row.get<double>("weight");
            if (!(e.weight >= 0.0)) throw row.error("weight", "must not be negative");
            return e;
        }
    };

    core::layered_table _table{schema_()};
    std::vector<entry> _entries; // in table row order
    core::weighted_table<std::size_t> _weights; // entry index, by weight

    core::rng _engine{core::rng::from_entropy()};

    struct auto_probe_tag {};

    explicit wthg(auto_probe_tag /*tag*/)
    {
        if (!load(dataset::lite)) (void)load(dataset::full);
    }

    // -- Helpers ----------------------------------------------------------

    static std::span<const core::embedded_file> embedded_files_()
    {
#ifdef DASMIG_WEATHER_EMBED
        return embedded::weathergen::files;
#else
        return {};
#endif
    }

    std::uint64_t draw_seed_() { return _engine(); }

    void rebuild_()
    {
        std::vector<entry> entries;
        core::weighted_table<std::size_t> weights;
        entries.reserve(_table.size());
        for (const auto& row : _table.rows())
        {
            entries.push_back(entry::parse(row));
            weights.add(entries.size() - 1, entries.back().weight);
        }
        _entries = std::move(entries);
        _weights = std::move(weights);
    }

    void require_data_() const
    {
        if (_entries.empty())
        {
            throw std::runtime_error("No weather data loaded. Call load() first.");
        }
    }
};

} // namespace dasmig

#endif // DASMIG_WEATHERGEN_HPP
