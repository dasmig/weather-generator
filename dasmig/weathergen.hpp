// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Dasso Migotto

#ifndef DASMIG_WEATHERGEN_HPP
#define DASMIG_WEATHERGEN_HPP

#include "core.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <numbers>
#include <optional>
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
/// @brief Weather Generator library — daily weather for any place and date
///        from climate normals for C++23.
/// @author Diego Dasso Migotto (diegomigotto at hotmail dot com)
/// @see See doc/usage.md for the narrative tutorial and doc/data.md for the
///      data and the model.

DASMIG_CORE_REQUIRE(1, 1);

namespace dasmig
{

/// @brief The form precipitation falls in.
enum class precipitation_type : std::uint8_t
{
    none,  ///< A dry day.
    rain,  ///< Rain.
    sleet, ///< Rain and snow mixed.
    snow   ///< Snow.
};

/// @brief The day's weather in one word or two.
enum class weather_condition : std::uint8_t
{
    clear,         ///< Dry, cloud cover below 20%.
    partly_cloudy, ///< Dry, cloud cover 20% to 50%.
    mostly_cloudy, ///< Dry, cloud cover 50% to 85%.
    overcast,      ///< Dry, cloud cover 85% or more.
    light_rain,    ///< Rain below 2.5 mm.
    rain,          ///< Rain from 2.5 mm to 10 mm.
    heavy_rain,    ///< Rain of 10 mm or more (a heavy precipitation day).
    thunderstorm,  ///< Rain or sleet with thunder.
    sleet,         ///< Rain and snow mixed.
    light_snow,    ///< Snow below 1 mm of water (about 1 cm).
    snow,          ///< Snow from 1 mm to 10 mm of water.
    heavy_snow     ///< Snow of 10 mm of water (about 10 cm) or more.
};

/// @brief The key of a precipitation type (`none`, `rain`, `sleet`, `snow`).
/// @param t The type.
/// @return The key.
[[nodiscard]] constexpr std::string_view to_string(precipitation_type t) noexcept
{
    switch (t)
    {
    case precipitation_type::rain: return "rain";
    case precipitation_type::sleet: return "sleet";
    case precipitation_type::snow: return "snow";
    case precipitation_type::none: break;
    }
    return "none";
}

/// @brief The key of a condition (`clear`, `partly_cloudy`, ...,
///        `heavy_snow`).
/// @param c The condition.
/// @return The key.
[[nodiscard]] constexpr std::string_view to_string(weather_condition c) noexcept
{
    switch (c)
    {
    case weather_condition::partly_cloudy: return "partly_cloudy";
    case weather_condition::mostly_cloudy: return "mostly_cloudy";
    case weather_condition::overcast: return "overcast";
    case weather_condition::light_rain: return "light_rain";
    case weather_condition::rain: return "rain";
    case weather_condition::heavy_rain: return "heavy_rain";
    case weather_condition::thunderstorm: return "thunderstorm";
    case weather_condition::sleet: return "sleet";
    case weather_condition::light_snow: return "light_snow";
    case weather_condition::snow: return "snow";
    case weather_condition::heavy_snow: return "heavy_snow";
    case weather_condition::clear: break;
    }
    return "clear";
}

/// @brief The English label of a condition (`clear`, `partly cloudy`, ...).
/// @param c The condition.
/// @return The label.
[[nodiscard]] constexpr std::string_view label(weather_condition c) noexcept
{
    switch (c)
    {
    case weather_condition::partly_cloudy: return "partly cloudy";
    case weather_condition::mostly_cloudy: return "mostly cloudy";
    case weather_condition::overcast: return "overcast";
    case weather_condition::light_rain: return "light rain";
    case weather_condition::rain: return "rain";
    case weather_condition::heavy_rain: return "heavy rain";
    case weather_condition::thunderstorm: return "thunderstorm";
    case weather_condition::sleet: return "sleet";
    case weather_condition::light_snow: return "light snow";
    case weather_condition::snow: return "snow";
    case weather_condition::heavy_snow: return "heavy snow";
    case weather_condition::clear: break;
    }
    return "clear";
}

/// @brief What a weather call asks for.
///
/// Every generation call takes one; `weather_request{}` asks for the
/// defaults (a random place on land, on the default date). Choices are never
/// stored on the generator, so one call cannot change the next.
struct weather_request
{
    /// @brief The default date, 2023-07-01 (birth-generator's, user-generator's
    ///        and device-generator's reference date); the library never reads
    ///        the clock.
    static constexpr std::chrono::year_month_day default_date{
        std::chrono::year{2023}, std::chrono::month{7}, std::chrono::day{1}};

    /// @brief Degrees north, -90 to 90. Set with longitude, or leave both
    ///        unset to draw a place on land by area.
    std::optional<double> latitude{}; // NOLINT(readability-redundant-member-init)

    /// @brief Degrees east, -180 to 180. Set with latitude.
    std::optional<double> longitude{}; // NOLINT(readability-redundant-member-init)

    /// @brief Metres above sea level, -500 to 9000. Temperatures move by the
    ///        standard lapse rate (6.5 °C per km) from the climate cell's mean
    ///        elevation; unset uses the cell's.
    std::optional<double> elevation{}; // NOLINT(readability-redundant-member-init)

    /// @brief The day (any valid date). A series starts here.
    std::chrono::year_month_day date{default_date};

    /// @brief Degrees Celsius added to every temperature, -10 to 10. The
    ///        normals are 1991-2020 means (centred on 2005); land has warmed
    ///        about 0.3 °C a decade since.
    double warming{0.0};
};

/// @brief The monthly climate normals of a place (climate_at()).
///
/// Twelve values per element, January to December, as loaded: the means the
/// daily weather is drawn around.
struct climate_normals
{
    std::string cell;  ///< The climate cell's key (its block centre, or a layer's own key).
    double latitude{};  ///< The cell's point, degrees north (a block's land centroid).
    double longitude{}; ///< The cell's point, degrees east.
    double elevation{}; ///< The cell's mean elevation, metres.
    std::array<double, 12> tmean{};         ///< Mean temperature, °C.
    std::array<double, 12> dtr{};           ///< Mean diurnal range (Tmax - Tmin), °C.
    std::array<double, 12> precipitation{}; ///< Precipitation, mm per month.
    std::array<double, 12> wet_days{};      ///< Days with at least 0.1 mm.
    std::array<double, 12> humidity{};      ///< Mean relative humidity, %.
    std::array<double, 12> cloud{};         ///< Cloud cover, %.
    std::array<double, 12> wind{};          ///< Mean wind speed at 10 m, m/s.

    /// @brief Write the normals as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object()
            .field("cell", cell)
            .field("latitude", latitude)
            .field("longitude", longitude)
            .field("elevation", elevation);
        const auto months = [&](std::string_view name, const std::array<double, 12>& v)
        {
            w.key(name).begin_array();
            for (const double x : v) w.value(x);
            w.end_array();
        };
        months("tmean", tmean);
        months("dtr", dtr);
        months("precipitation", precipitation);
        months("wet_days", wet_days);
        months("humidity", humidity);
        months("cloud", cloud);
        months("wind", wind);
        w.end_object();
    }

    /// @brief The normals as a JSON document.
    /// @return Compact JSON.
    [[nodiscard]] std::string to_json() const
    {
        core::json_writer w;
        to_json(w);
        return w.str();
    }
};

/// @brief One day's weather at one place.
///
/// Values are rounded for output: temperatures, precipitation, snowfall and
/// wind to 0.1, cloud cover to 0.01, humidity to whole percent. Supports
/// implicit conversion to std::string (a one-line summary), streaming via
/// operator<< and JSON output via to_json().
class weather
{
  public:
    std::chrono::year_month_day date{}; ///< The day.
    std::string cell;      ///< Key of the climate cell drawn from.
    double latitude{};     ///< Degrees north: the requested point, or the drawn cell's.
    double longitude{};    ///< Degrees east: the requested point, or the drawn cell's.
    double elevation{};    ///< Metres: the requested elevation, or the cell's.
    double distance_km{};  ///< Distance from the point to land, km (0 on land; at most 100).
    double tmin{};         ///< Lowest temperature, °C.
    double tmean{};        ///< Mean temperature, °C: halfway between tmin and tmax.
    double tmax{};         ///< Highest temperature, °C.
    bool frost{};          ///< tmin below 0 °C.
    double precipitation{}; ///< Precipitation, mm of water (0 on a dry day).
    double snowfall{};     ///< Fresh snow, cm (10 cm per 10 mm of water).
    precipitation_type type{precipitation_type::none}; ///< The form it falls in.
    bool thunder{};        ///< Thunder heard (on rain or sleet days only).
    double cloud_cover{};  ///< Mean cloud cover, 0 to 1.
    double humidity{};     ///< Mean relative humidity, %.
    double wind{};         ///< Mean wind speed at 10 m, m/s.
    double daylight_hours{}; ///< Time from sunrise to sunset, hours (0 in polar night, 24 in polar day).
    double sunshine_hours{}; ///< Bright sunshine, hours: the daylight not covered by cloud.
    /// @brief Sunrise, UTC (unset in polar day and polar night). It can fall
    ///        on the date before in UTC, far east of Greenwich.
    std::optional<std::chrono::sys_time<std::chrono::minutes>> sunrise{}; // NOLINT(readability-redundant-member-init)
    /// @brief Sunset, UTC (unset in polar day and polar night).
    std::optional<std::chrono::sys_time<std::chrono::minutes>> sunset{}; // NOLINT(readability-redundant-member-init)
    weather_condition condition{weather_condition::clear}; ///< The day in a word.

    /// @brief Retrieve the random seed used to generate this weather.
    /// @return The call seed; pass it to get_weather(request, seed) with the
    ///         same request and data to replay this day.
    [[nodiscard]] std::uint64_t seed() const { return _signature.seed; }

    /// @brief The seed and the data this weather was generated from.
    /// @return The replay signature; `data` is wtg::data_hash() at the
    ///         time of generation.
    [[nodiscard]] const core::seed_signature& signature() const { return _signature; }

    /// @brief ISO 8601 date string (YYYY-MM-DD).
    /// @return The date.
    [[nodiscard]] std::string date_string() const
    {
        return std::format("{:04d}-{:02d}-{:02d}", static_cast<int>(date.year()),
                           static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
    }

    /// @brief An instant as ISO 8601 UTC to the minute (`2024-01-15T07:42Z`).
    /// @param t The instant.
    /// @return The string.
    [[nodiscard]] static std::string time_string(std::chrono::sys_time<std::chrono::minutes> t)
    {
        const auto day = std::chrono::floor<std::chrono::days>(t);
        const std::chrono::year_month_day ymd{day};
        const auto minutes = (t - day).count();
        return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}Z", static_cast<int>(ymd.year()),
                           static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), minutes / 60,
                           minutes % 60);
    }

    /// @brief Human-readable summary string.
    /// @return For example `2023-07-01: light rain, 14.2 to 21.9 °C, 1.3 mm`.
    [[nodiscard]] std::string to_string() const
    {
        std::string s = std::format("{}: {}, {:.1f} to {:.1f} °C", date_string(), label(condition), tmin, tmax);
        if (type == precipitation_type::snow || type == precipitation_type::sleet)
        {
            s += std::format(", {:.1f} cm of snow", snowfall);
        }
        else if (type == precipitation_type::rain)
        {
            s += std::format(", {:.1f} mm", precipitation);
        }
        return s;
    }

    /// @brief Write this weather as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object()
            .field("date", date_string())
            .key("place")
            .begin_object()
            .field("cell", cell)
            .field("latitude", latitude)
            .field("longitude", longitude)
            .field("elevation", elevation)
            .field("distance_km", distance_km)
            .end_object()
            .field("condition", dasmig::to_string(condition))
            .field("tmin", tmin)
            .field("tmean", tmean)
            .field("tmax", tmax)
            .field("frost", frost)
            .field("precipitation", precipitation)
            .field("snowfall", snowfall)
            .field("type", dasmig::to_string(type))
            .field("thunder", thunder)
            .field("cloud_cover", cloud_cover)
            .field("humidity", humidity)
            .field("wind", wind)
            .field("daylight_hours", daylight_hours)
            .field("sunshine_hours", sunshine_hours);
        const auto instant = [&](std::string_view name, const auto& t)
        {
            w.key(name);
            if (t) w.value(time_string(*t));
            else w.value(nullptr);
        };
        instant("sunrise", sunrise);
        instant("sunset", sunset);
        w.key("signature");
        _signature.to_json(w);
        w.end_object();
    }

    /// @brief This weather as a JSON document.
    /// @return Compact JSON, for example `{"date":"2023-07-01","place":{...},
    ///   "condition":"light_rain","tmin":14.2,...,"signature":{...}}`.
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
    friend class wtg;
};

/// @brief Weather Generator: daily weather drawn around monthly climate
///        normals.
///
/// A Richardson (1981) type weather generator: rain falls by a two-state
/// Markov chain, wet-day amounts are gamma distributed, the daily mean
/// temperature and the daily range follow the season with residuals from a
/// lag-1 autoregressive process, months carry their own anomalies, and cloud,
/// humidity and wind follow the wet and dry days. Its parameters come from
/// the place's monthly normals (CRU TS 4.10, 1991-2020) and from fits on
/// GHCN-Daily stations (doc/data.md).
///
/// A day's weather depends only on the seed, the place and the date: the
/// draws of each day come from that day's own stream, and the chains are
/// read back to the day where their state no longer matters. So the weather
/// of 10 March is the same whether it is asked for alone or inside a series
/// that starts in January. Output is identical on every compiler and
/// platform (dasmig-core).
///
/// Can be used as a singleton via instance() or constructed independently.
///
/// @par Thread safety
/// Each instance is independent. Concurrent calls to get_weather(request)
/// on the **same** instance require external synchronisation.
class wtg
{
  public:
    /// @brief The longest series one call returns, in days (about 100 years).
    static constexpr int max_series_days = 36600;

    /// @brief Default constructor — creates an empty generator with no
    ///        data.
    wtg() = default;

    wtg(const wtg&) = delete;            ///< Not copyable.
    wtg& operator=(const wtg&) = delete; ///< Not copyable.
    wtg(wtg&&) noexcept = default;       ///< Movable.
    wtg& operator=(wtg&&) noexcept = default; ///< Movable.
    ~wtg() = default;                     ///< Destructor.

    /// @brief Access the global singleton instance.
    ///
    /// Loads the `lite` tier (or `full` if `lite` is missing) on first
    /// access, from compiled-in data or an auto-probed `resources/`.
    /// @return The singleton.
    static wtg& instance()
    {
        static wtg inst{auto_probe_tag{}};
        return inst;
    }

    // -- Generation -------------------------------------------------------

    /// @brief Generate one day's weather.
    ///        Use weather_request{} for the defaults.
    /// @param request What to generate.
    /// @return The weather.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the request is invalid.
    [[nodiscard]] weather get_weather(const weather_request& request)
    {
        return get_weather(request, draw_seed_());
    }

    /// @brief Generate one day's weather, deterministically.
    /// @param request What to generate.
    /// @param call_seed Seed; the same request, seed and data give the same
    ///        result. Does not touch the instance engine.
    /// @return The weather; equal to day 0 of get_weather_series() with the
    ///         same request and seed.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the request is invalid.
    [[nodiscard]] weather get_weather(const weather_request& request, std::uint64_t call_seed) const
    {
        return std::move(simulate_(request, 1, call_seed).front());
    }

    /// @brief Generate the weather of consecutive days.
    /// @param request The place and the first day (`date`).
    /// @param days How many days, 1 to max_series_days.
    /// @return The days, in order.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the request or @p days is invalid.
    [[nodiscard]] std::vector<weather> get_weather_series(const weather_request& request, int days)
    {
        return get_weather_series(request, days, draw_seed_());
    }

    /// @brief Generate the weather of consecutive days, deterministically.
    ///
    /// Day `i` equals get_weather() for the same place and seed with `date`
    /// moved `i` days on, so series that overlap agree on the days they
    /// share.
    ///
    /// @param request The place and the first day (`date`).
    /// @param days How many days, 1 to max_series_days.
    /// @param call_seed Seed of the series; every day carries it.
    /// @return The days, in order.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the request or @p days is invalid.
    [[nodiscard]] std::vector<weather> get_weather_series(const weather_request& request, int days,
                                                          std::uint64_t call_seed) const
    {
        if (days < 1 || days > max_series_days)
        {
            throw std::invalid_argument(std::format("weather-generator: days must be 1 to {}", max_series_days));
        }
        return simulate_(request, days, call_seed);
    }

    /// @brief The monthly normals of the climate cell nearest a point.
    /// @param latitude Degrees north, -90 to 90.
    /// @param longitude Degrees east, -180 to 180.
    /// @return The cell's normals.
    /// @throws std::runtime_error If no data has been loaded.
    /// @throws std::invalid_argument If the point is out of range or more
    ///         than 100 km from land.
    [[nodiscard]] climate_normals climate_at(double latitude, double longitude) const
    {
        require_data_();
        check_point_(latitude, longitude);
        return _cells[locate_(latitude, longitude).cell].normals;
    }

    // -- Batch generation -------------------------------------------------

    /// @brief Generate a batch of weather results from a seed drawn like
    ///        get_weather() draws its own.
    ///
    /// Item `i` is `get_weather(request, core::item_seed(batch_seed,
    /// options.first + i))` for the drawn batch seed; see the seeded
    /// overload.
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
    /// the output does not depend on `options.threads`. With no place set,
    /// each item draws its own place.
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
        return core::run_batch(options, batch_seed,
                               [&](std::uint64_t item_seed) { return get_weather(request, item_seed); });
    }

    /// @brief Stream a batch of weather results to a sink without holding
    ///        the whole batch.
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
    void for_each_weather(const weather_request& request, const core::batch_options& options, Sink&& sink)
    {
        for_each_weather(request, options, draw_seed_(), std::forward<Sink>(sink));
    }

    // -- Data management --------------------------------------------------

    /// @brief Check whether any data has been loaded.
    /// @return True when at least one climate cell is loaded.
    [[nodiscard]] bool has_data() const { return !_cells.empty(); }

    /// @brief Return the number of loaded climate cells.
    /// @return The count.
    [[nodiscard]] std::size_t count() const { return _cells.size(); }

    /// @brief Load data from a resource directory, replacing all data and
    ///        layers loaded before.
    ///
    /// Expects the directory to contain `climate.tsv`, and reads the land
    /// mask from `land.tsv` when it is there (without one, a point must be
    /// within 100 km of a cell). A path that is not a directory, or a
    /// directory without `climate.tsv`, is ignored.
    ///
    /// @param dir Directory such as `resources/lite`.
    /// @throws core::tsv_error If the file is malformed.
    void load(const std::filesystem::path& dir)
    {
        std::error_code ec;
        const auto file = dir / data_file_;
        if (!std::filesystem::is_directory(dir, ec) || !std::filesystem::is_regular_file(file, ec))
        {
            return;
        }
        (void)extend(core::tsv_document::load(file), core::layer_mode::replace);
        const auto mask = dir / land_file_;
        _land = std::filesystem::is_regular_file(mask, ec) ? read_land_(core::tsv_document::load(mask)) : land_mask{};
    }

    /// @brief Load a dataset tier, replacing all data and layers loaded
    ///        before.
    ///
    /// `lite` is a 2-degree grid (5,163 cells), `full` a half-degree grid
    /// (67,213 cells). Uses the compiled-in copy when the library is built
    /// with `DASMIG_WEATHER_EMBED`, otherwise searches `resources/`,
    /// `../resources`, `weather-generator/resources` and
    /// `../weather-generator/resources`.
    ///
    /// @param tier The tier.
    /// @return False when the tier was not found.
    /// @throws core::tsv_error If the file is malformed.
    [[nodiscard]] bool load(dataset tier)
    {
        const std::string path = std::string{core::tier_name(tier)} + "/" + std::string{data_file_};
        auto doc = core::load_resource(path, embedded_files_(), core::find_resources(library_name_, path));
        if (!doc) return false;
        (void)extend(std::move(*doc), core::layer_mode::replace);
        const std::string mask = std::string{core::tier_name(tier)} + "/" + std::string{land_file_};
        auto land = core::load_resource(mask, embedded_files_(), core::find_resources(library_name_, mask));
        _land = land ? read_land_(*land) : land_mask{};
        return true;
    }

    /// @brief Add a data layer from a TSV file with the `climate.tsv`
    ///        columns.
    ///
    /// Rows are keyed by `key`. In the default override mode a row replaces
    /// the loaded row with the same key and new keys are added (a station's
    /// own normals, say, which then serve the points nearest it); a row with
    /// `disabled` set to 1 removes its key. `elevation`, `area`, `humidity`,
    /// `cloud` and `wind` may be left out (0 m, never drawn as a random
    /// place, 70%, 50% and 3.5 m/s).
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
    /// @param text TSV with the `climate.tsv` columns.
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
    /// @param doc TSV with the `climate.tsv` columns.
    /// @param mode How the layer combines with the data loaded before.
    /// @return What the layer did.
    /// @throws core::tsv_error If a row is invalid; the data is unchanged.
    const core::layer_info& extend(core::tsv_document doc, core::layer_mode mode = core::layer_mode::override)
    {
        const auto& info =
            _table.extend(std::move(doc), mode, [](const core::tsv_row& row) { (void)cell::parse(row); });
        rebuild_();
        return info;
    }

    /// @brief The data layers loaded, in order.
    /// @return The layer records.
    [[nodiscard]] const std::vector<core::layer_info>& layers() const { return _table.layers(); }

    /// @brief Hash of the loaded data, recorded in each result's
    ///        signature().
    /// @return The hash; 0 when nothing is loaded.
    [[nodiscard]] std::uint64_t data_hash() const { return _table.data_hash(); }

  private:
    // -- The model's constants --------------------------------------------
    //
    // Fitted on GHCN-Daily stations, 1991-2020, by scripts/calibrate.py
    // unless marked tuned (adjusted from the fit with scripts/validate.py)
    // or curated; doc/data.md has the fits.

    struct model
    {
        // Rain persistence d = P(wet | wet) - P(wet | dry) = d0 + d1 f
        // (Geng et al. 1986 found 0.25). Tuned: the fit gives d0 = 0.277,
        // lowered because the month anomalies also cluster wet days.
        static constexpr double persistence0 = 0.27;
        static constexpr double persistence1 = 0.00647;

        // Gamma shape of wet-day amounts = a0 + a1 ln(mean wet-day mm).
        static constexpr double shape0 = 1.14578;
        static constexpr double shape1 = -0.22430;
        static constexpr double shape_min = 0.4;
        static constexpr double shape_max = 1.5;

        // Dry-day minus wet-day daily mean temperature and daily range, as
        // shares of the diurnal range: wet days are a little cooler and have
        // a much smaller range (cooler afternoons, warmer nights). The mean's
        // is tuned from the fit's 0.032 so the generated dry-minus-wet Tmax
        // matches the stations once the residuals and anomalies are added;
        // the range's is the fit's.
        static constexpr double wet_mean = 0.016;
        static constexpr double wet_range = 0.2204;

        // Day-to-day spread (°C) of the daily mean temperature and of the
        // daily range = c . (1, annual range, dtr, f, season, season * range,
        // |latitude|), where season is the day's mean temperature against
        // the year's, -1 (coldest) to 1.
        static constexpr std::array<double, 7> spread_mean{-1.21055, 0.05030, 0.15209, 1.31636, -0.19543, -0.04674, 0.03630};
        static constexpr std::array<double, 7> spread_range{-0.37573, 0.02161, 0.20729, 0.85934, -0.25677, -0.00543, 0.01475};
        static constexpr double spread_min = 0.2;
        static constexpr double spread_max = 12.0;

        // Lag-1 autoregression of the standardized (mean, range) residuals:
        // x_t = A x_{t-1} + B e_t (Richardson 1981), row-major.
        static constexpr std::array<double, 4> a{0.69497378, 0.06841554, -0.05222351, 0.30667295};
        static constexpr std::array<double, 4> b{0.71084994, 0.0, 0.09968858, 0.94429324};

        // Weibull shape of the daily mean wind, and Gamma(1 + 1/k).
        static constexpr double wind_shape = 2.902;
        static constexpr double wind_gamma = 0.89171496;

        // Snow on a wet day: logit p = s0 + s1 T, T the daily mean
        // temperature (p = 0.5 at 0.76 °C). Curated: snow above sleet_above
        // falls as sleet.
        static constexpr double snow0 = 0.4632;
        static constexpr double snow1 = -0.6058;
        static constexpr double sleet_above = 1.0;

        // Thunder on a rain or sleet day: logit p = t0 + t1 Tmax + t2 ln(mm)
        // (fitted on US stations, whose reports are consistent).
        static constexpr std::array<double, 3> thunder{-5.0964, 0.1213, 0.3431};

        // Curated (doc/data.md): wet days have
        // cloud_wet of the clear sky left covered on top of the mean, and
        // daily cover is Beta with concentration cloud_kappa.
        static constexpr double cloud_wet = 0.6;
        static constexpr double cloud_kappa = 4.0;
        // Curated: wet days are humidity_wet points more humid than dry
        // days; daily humidity has a spread of humidity_sd points.
        static constexpr double humidity_wet = 10.0;
        static constexpr double humidity_sd = 7.0;

        // Month-to-month anomalies (tuned with scripts/validate.py so the
        // year-to-year spread of monthly means matches the stations): the
        // temperature anomaly's share of the daily spread, and the log
        // spread of the odds of a wet day and of the wet-day amount, both
        // moved by one precipitation anomaly.
        static constexpr double temperature_month = 0.48;
        static constexpr double wet_month = 0.32;
        static constexpr double amount_month = 0.28;

        static constexpr double lapse_rate = 0.0065; // °C per metre (the standard atmosphere)

        // Days read back: the temperature residual sums this many days of
        // innovations (A^window is below 1e-7), and the rain chain looks
        // back at most this far for the day its state stops mattering.
        static constexpr int window = 48;
    };

    // -- Internal data structures -----------------------------------------

    static constexpr std::string_view library_name_ = "weather-generator";
    static constexpr std::string_view data_file_ = "climate.tsv";
    static constexpr std::string_view land_file_ = "land.tsv";

    // The land cells of a 0.5-degree grid, as runs of columns per row: row
    // i is centred at -89.75 + 0.5 i, column j at -179.75 + 0.5 j.
    static constexpr int land_rows = 360;
    static constexpr int land_columns = 720;
    static constexpr double sea_limit_km = 100.0;
    static constexpr double earth_radius_km = 6371.0;
    using land_mask = std::vector<std::vector<std::pair<int, int>>>; // per row: [first, last] columns

    static land_mask read_land_(const core::tsv_document& doc)
    {
        land_mask mask(land_rows);
        for (const auto& row : doc.rows())
        {
            const auto i = row.get<int>("row");
            if (i < 0 || i >= land_rows) throw row.error("row", "must be 0 to 359");
            for (const auto run : row.list("columns"))
            {
                const auto dash = run.find('-');
                const auto a = core::detail::parse_number<int>(run.substr(0, dash));
                const auto b = dash == std::string_view::npos ? a : core::detail::parse_number<int>(run.substr(dash + 1));
                if (!a || !b || *a < 0 || *b < *a || *b >= land_columns)
                {
                    throw row.error("columns", "must be runs such as 12-40 of columns 0 to 719");
                }
                mask[static_cast<std::size_t>(i)].emplace_back(*a, *b);
            }
            std::ranges::sort(mask[static_cast<std::size_t>(i)]);
        }
        return mask;
    }

    static core::table_schema schema_()
    {
        return {.key = {"key"},
                .required = {"latitude", "longitude", "tmean", "dtr", "precipitation", "wet_days"},
                .optional = {"elevation", "area", "humidity", "cloud", "wind"}};
    }

    // True when x is not a number in [lo, hi].
    static bool outside_(double x, double lo, double hi) { return !std::isfinite(x) || x < lo || x > hi; }

    // Twelve control points per element: linear interpolation between them
    // (at mid-month) gives back each month's mean, so a day's value follows
    // the season smoothly instead of stepping at month ends.
    using months = std::array<double, 12>;

    struct cell
    {
        climate_normals normals;
        double area{0.0};
        std::array<double, 3> xyz{};
        // Controls for the daily parameters.
        months tmean{}, dtr{}, wet{}, amount{}, humidity{}, cloud{}, wind{};
        double annual_range{0.0};
        double annual_mean{0.0};

        static months read_months_(const core::tsv_row& row, std::string_view column, double lo, double hi)
        {
            const auto items = row.list(column);
            if (items.size() != 12) throw row.error(column, "must hold 12 values separated by ';'");
            months out{};
            for (std::size_t i = 0; i < 12; ++i)
            {
                const auto v = core::detail::parse_number<double>(items[i]);
                if (!v || !std::isfinite(*v)) throw row.error(column, "holds a value that is not a number");
                if (*v < lo || *v > hi)
                {
                    throw row.error(column, std::format("values must be {} to {}", lo, hi));
                }
                out.at(i) = *v;
            }
            return out;
        }

        static months constant_(double v)
        {
            months m{};
            m.fill(v);
            return m;
        }

        // Parse and check a row; throws core::tsv_error located at the row.
        static cell parse(const core::tsv_row& row)
        {
            cell c;
            auto& n = c.normals;
            n.cell = row.get<std::string>("key");
            n.latitude = row.get<double>("latitude");
            n.longitude = row.get<double>("longitude");
            if (outside_(n.latitude, -90.0, 90.0)) throw row.error("latitude", "must be -90 to 90");
            if (outside_(n.longitude, -180.0, 180.0)) throw row.error("longitude", "must be -180 to 180");
            n.elevation = row.get_or<double>("elevation", 0.0);
            if (outside_(n.elevation, -500.0, 9000.0)) throw row.error("elevation", "must be -500 to 9000");
            c.area = row.get_or<double>("area", 0.0);
            if (outside_(c.area, 0.0, std::numeric_limits<double>::max()))
            {
                throw row.error("area", "must not be negative");
            }
            n.tmean = read_months_(row, "tmean", -80.0, 60.0);
            n.dtr = read_months_(row, "dtr", 0.0, 50.0);
            n.precipitation = read_months_(row, "precipitation", 0.0, 5000.0);
            n.wet_days = read_months_(row, "wet_days", 0.0, 31.0);
            n.humidity = row.has("humidity") ? read_months_(row, "humidity", 0.0, 100.0) : constant_(70.0);
            n.cloud = row.has("cloud") ? read_months_(row, "cloud", 0.0, 100.0) : constant_(50.0);
            n.wind = row.has("wind") ? read_months_(row, "wind", 0.0, 60.0) : constant_(3.5);
            return c;
        }
    };

    // The parameters of one day at one cell.
    struct day_params
    {
        double tmean, dtr, wet, amount, humidity, cloud, wind;
        double anomaly;  // the months' temperature anomaly, standard normal
        double residual; // the share of the spread left to the daily residual
    };

    // One day's draws that later days read back.
    struct day_draws
    {
        day_params p{};
        double u_wet{};
        std::array<double, 2> e{}; // temperature innovations
    };

    core::layered_table _table{schema_()};
    std::vector<cell> _cells; // in table row order
    std::vector<std::uint32_t> _tree; // k-d tree over the cells' unit vectors (implicit, median-split)
    core::weighted_table<std::size_t> _area; // cell index, by land area
    land_mask _land;                         // empty when the data has no land.tsv

    core::rng _engine{core::rng::from_entropy()};

    struct auto_probe_tag
    {
    };

    explicit wtg(auto_probe_tag /*tag*/)
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

    void require_data_() const
    {
        if (_cells.empty()) throw std::runtime_error("No weather data loaded. Call load() first.");
    }

    static void check_point_(double latitude, double longitude) // NOLINT(bugprone-easily-swappable-parameters)
    {
        if (outside_(latitude, -90.0, 90.0))
        {
            throw std::invalid_argument("weather-generator: latitude must be -90 to 90");
        }
        if (outside_(longitude, -180.0, 180.0))
        {
            throw std::invalid_argument("weather-generator: longitude must be -180 to 180");
        }
    }

    static double fma_free_(double a, double b, double c) { return core::math::fenced(a * b) + c; }

    // sin and cos of an angle in degrees, from + - * only, so the bits are
    // the same everywhere (std::sin and std::cos are not correctly rounded).
    static std::pair<double, double> sin_cos_deg_(double degrees)
    {
        const double q = std::round(degrees / 90.0);
        const double x = (degrees - (90.0 * q)) * (std::numbers::pi / 180.0); // |x| <= pi/4
        const double x2 = x * x;
        // Taylor series to x^17 and x^16; the error is below 1e-17 on |x| <= pi/4.
        double s = 1.0 / 355687428096000.0;
        double c = 1.0 / 20922789888000.0;
        constexpr std::array<double, 8> sin_terms{-1.0 / 1307674368000.0, 1.0 / 6227020800.0,
                                                  -1.0 / 39916800.0,      1.0 / 362880.0,
                                                  -1.0 / 5040.0,          1.0 / 120.0,
                                                  -1.0 / 6.0,             1.0};
        constexpr std::array<double, 8> cos_terms{-1.0 / 87178291200.0, 1.0 / 479001600.0,
                                                  -1.0 / 3628800.0,     1.0 / 40320.0,
                                                  -1.0 / 720.0,         1.0 / 24.0,
                                                  -1.0 / 2.0,           1.0};
        for (const double term : sin_terms) s = fma_free_(s, x2, term);
        for (const double term : cos_terms) c = fma_free_(c, x2, term);
        s *= x;
        const int quadrant = ((static_cast<int>(q) % 4) + 4) % 4;
        switch (quadrant)
        {
        case 1: return {c, -s};
        case 2: return {-s, -c};
        case 3: return {-c, s};
        default: return {s, c};
        }
    }

    static double sin_deg_(double degrees) { return sin_cos_deg_(degrees).first; }
    static double cos_deg_(double degrees) { return sin_cos_deg_(degrees).second; }

    // atan from + - * / only, for the same reason: odd Taylor series on
    // |x| <= tan(pi / 8) after two reductions (error below 1e-17).
    static double atan_(double x)
    {
        const bool negative = x < 0.0;
        if (negative) x = -x;
        const bool inverted = x > 1.0;
        if (inverted) x = 1.0 / x;
        double base = 0.0;
        if (x > 0.41421356237309503) // tan(pi / 8)
        {
            x = (x - 1.0) / (x + 1.0);
            base = std::numbers::pi / 4.0;
        }
        const double x2 = core::math::fenced(x * x);
        double sum = 0.0;
        for (int n = 25; n >= 0; --n) sum = fma_free_(sum, x2, (n % 2 == 0 ? 1.0 : -1.0) / ((2.0 * n) + 1.0));
        double r = base + core::math::fenced(sum * x);
        if (inverted) r = (std::numbers::pi / 2.0) - r;
        return negative ? -r : r;
    }

    // atan2, asin and acos in degrees.
    static double atan2_deg_(double y, double x)
    {
        constexpr double deg = 180.0 / std::numbers::pi;
        if (x > 0.0) return atan_(y / x) * deg;
        if (x < 0.0) return (atan_(y / x) * deg) + (y >= 0.0 ? 180.0 : -180.0);
        if (y == 0.0) return 0.0;
        return y > 0.0 ? 90.0 : -90.0;
    }

    static double asin_deg_(double x)
    {
        return atan2_deg_(x, std::sqrt(std::max(0.0, 1.0 - core::math::fenced(x * x))));
    }

    static double acos_deg_(double x)
    {
        return atan2_deg_(std::sqrt(std::max(0.0, 1.0 - core::math::fenced(x * x))), x);
    }

    // Great-circle distance in km between two unit vectors.
    static double km_(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        constexpr double rad = std::numbers::pi / 180.0;
        return 2.0 * earth_radius_km * asin_deg_(std::min(1.0, std::sqrt(distance2_(a, b)) / 2.0)) * rad;
    }

    static std::array<double, 3> unit_vector_(double latitude, double longitude)
    {
        const auto [slat, clat] = sin_cos_deg_(latitude);
        const auto [slon, clon] = sin_cos_deg_(longitude);
        return {clat * clon, clat * slon, slat};
    }

    static double distance2_(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        const double dx = a[0] - b[0];
        const double dy = a[1] - b[1];
        const double dz = a[2] - b[2];
        return core::math::fenced(dx * dx) + core::math::fenced(dy * dy) + core::math::fenced(dz * dz);
    }

    // Controls whose linear interpolation between mid-months gives back the
    // monthly means: (c[m-1] + 6 c[m] + c[m+1]) / 8 = v[m], solved by
    // iteration (each step shrinks the error threefold).
    static months controls_(const months& v)
    {
        months c = v;
        for (int it = 0; it < 40; ++it)
        {
            months next{};
            for (std::size_t m = 0; m < 12; ++m)
            {
                next.at(m) = ((8.0 * v.at(m)) - c.at((m + 11) % 12) - c.at((m + 1) % 12)) / 6.0;
            }
            c = next;
        }
        return c;
    }

    void rebuild_()
    {
        std::vector<cell> cells;
        core::weighted_table<std::size_t> area;
        cells.reserve(_table.size());
        for (const auto& row : _table.rows())
        {
            cell c = cell::parse(row);
            const auto& n = c.normals;
            c.xyz = unit_vector_(n.latitude, n.longitude);
            months wet{};
            months amount{};
            months cloud{};
            double total = 0.0;
            double total_wet = 0.0;
            for (std::size_t m = 0; m < 12; ++m)
            {
                const auto ml = static_cast<double>(month_length_(2001, static_cast<unsigned>(m + 1)));
                wet.at(m) = std::min(n.wet_days.at(m), ml) / ml;
                total += n.precipitation.at(m);
                total_wet += n.wet_days.at(m);
                cloud.at(m) = n.cloud.at(m) / 100.0;
            }
            const double annual_amount = total_wet > 0.0 ? total / total_wet : 0.0;
            for (std::size_t m = 0; m < 12; ++m)
            {
                amount.at(m) = n.wet_days.at(m) >= 0.1 ? n.precipitation.at(m) / n.wet_days.at(m) : annual_amount;
            }
            c.tmean = controls_(n.tmean);
            c.dtr = controls_(n.dtr);
            c.wet = controls_(wet);
            c.amount = controls_(amount);
            c.humidity = controls_(n.humidity);
            c.cloud = controls_(cloud);
            c.wind = controls_(n.wind);
            const auto [lo, hi] = std::ranges::minmax(n.tmean);
            c.annual_range = hi - lo;
            double sum = 0.0;
            for (const double t : n.tmean) sum += t;
            c.annual_mean = sum / 12.0;
            cells.push_back(std::move(c));
            if (cells.back().area > 0.0) area.add(cells.size() - 1, cells.back().area);
        }
        _cells = std::move(cells);
        _area = std::move(area);
        build_tree_();
    }

    // -- Nearest cell -----------------------------------------------------

    void build_tree_()
    {
        _tree.resize(_cells.size());
        for (std::size_t i = 0; i < _tree.size(); ++i) _tree[i] = static_cast<std::uint32_t>(i);
        build_tree_(0, _tree.size(), 0);
    }

    // Recursion depth is log2 of the cell count (17 for the full tier).
    // NOLINTNEXTLINE(misc-no-recursion)
    void build_tree_(std::size_t lo, std::size_t hi, int axis)
    {
        if (hi - lo <= 1) return;
        const std::size_t mid = lo + ((hi - lo) / 2);
        const auto a = static_cast<std::size_t>(axis);
        std::nth_element(_tree.begin() + static_cast<std::ptrdiff_t>(lo), _tree.begin() + static_cast<std::ptrdiff_t>(mid),
                         _tree.begin() + static_cast<std::ptrdiff_t>(hi),
                         [&](std::uint32_t x, std::uint32_t y)
                         {
                             const double cx = _cells[x].xyz.at(a);
                             const double cy = _cells[y].xyz.at(a);
                             return cx < cy || (cx == cy && x < y);
                         });
        build_tree_(lo, mid, (axis + 1) % 3);
        build_tree_(mid + 1, hi, (axis + 1) % 3);
    }

    // The nearest cell by chord distance; ties go to the earlier row, so the
    // answer does not depend on the tree's shape.
    [[nodiscard]] std::size_t nearest_(const std::array<double, 3>& q) const
    {
        std::size_t best = _cells.size();
        double best_d = std::numeric_limits<double>::infinity();
        search_(q, 0, _tree.size(), 0, best, best_d);
        return best;
    }

    // NOLINTNEXTLINE(misc-no-recursion, bugprone-easily-swappable-parameters): depth log2 of the cells
    void search_(const std::array<double, 3>& q, std::size_t lo, std::size_t hi, int axis, std::size_t& best,
                 double& best_d) const
    {
        if (lo >= hi) return;
        const std::size_t mid = lo + ((hi - lo) / 2);
        const std::uint32_t i = _tree[mid];
        const double d = distance2_(q, _cells[i].xyz);
        if (d < best_d || (d == best_d && i < best))
        {
            best_d = d;
            best = i;
        }
        const auto a = static_cast<std::size_t>(axis);
        const double diff = q.at(a) - _cells[i].xyz.at(a);
        const int next = (axis + 1) % 3;
        const bool left_first = diff < 0.0;
        if (left_first) search_(q, lo, mid, next, best, best_d);
        else search_(q, mid + 1, hi, next, best, best_d);
        if (core::math::fenced(diff * diff) <= best_d)
        {
            if (left_first) search_(q, mid + 1, hi, next, best, best_d);
            else search_(q, lo, mid, next, best, best_d);
        }
    }

    // -- Days -------------------------------------------------------------

    static unsigned month_length_(int year, unsigned month)
    {
        return static_cast<unsigned>(
            std::chrono::year_month_day_last{std::chrono::year{year} / std::chrono::month{month} / std::chrono::last}
                .day());
    }

    // The anomalies of one calendar month at one place: (temperature,
    // precipitation), standard normal, from the month's own stream.
    static std::array<double, 2> month_anomaly_(std::uint64_t month_seed, std::int64_t month_index)
    {
        core::rng r{core::rng::child_seed_of(month_seed, static_cast<std::uint64_t>(month_index))};
        const double t = core::normal(r, 0.0, 1.0);
        return {t, core::normal(r, 0.0, 1.0)};
    }

    // The month anomalies one call reads, each drawn once. Days are read in
    // order, so the months asked for never go back before the first.
    class month_anomalies
    {
      public:
        explicit month_anomalies(std::uint64_t seed) : _seed{seed} {}

        std::array<double, 2> operator()(std::int64_t index)
        {
            if (_values.empty()) _first = index;
            while (_first + static_cast<std::int64_t>(_values.size()) <= index)
            {
                _values.push_back(month_anomaly_(_seed, _first + static_cast<std::int64_t>(_values.size())));
            }
            return _values[static_cast<std::size_t>(index - _first)];
        }

      private:
        std::uint64_t _seed;
        std::int64_t _first{0};
        std::vector<std::array<double, 2>> _values;
    };

    // A day's parameters, interpolated between the controls of the months
    // around it, with the months' anomalies interpolated the same way.
    static day_params params_(const cell& c, std::chrono::year_month_day ymd, month_anomalies& anomalies)
    {
        const auto month = static_cast<unsigned>(ymd.month());
        const int year = static_cast<int>(ymd.year());
        const auto length = static_cast<double>(month_length_(year, month));
        // Months after the middle of the year's January (negative before).
        const double pos = static_cast<double>(month - 1) +
                           ((static_cast<double>(static_cast<unsigned>(ymd.day())) - 0.5) / length) - 0.5;
        const double lower = std::floor(pos); // the month before or the month itself
        const double t = pos - lower;
        const std::int64_t index = (static_cast<std::int64_t>(year) * 12) + static_cast<std::int64_t>(lower);
        const auto i = static_cast<std::size_t>(((index % 12) + 12) % 12);
        const std::size_t j = (i + 1) % 12;
        const auto lerp = [&](const months& v) { return v.at(i) + core::math::fenced(t * (v.at(j) - v.at(i))); };

        const auto a = anomalies(index);
        const auto b = anomalies(index + 1);
        const double za = a[0] + core::math::fenced(t * (b[0] - a[0]));
        const double zp = a[1] + core::math::fenced(t * (b[1] - a[1]));
        // Variance of the interpolated anomaly, to keep means and spreads.
        const double v = core::math::fenced((1.0 - t) * (1.0 - t)) + core::math::fenced(t * t);
        // The precipitation anomaly scales the odds of a wet day, so the
        // fraction stays below 1 however wet the month, and the wet-day amount
        // by a log-normal factor. Each correction keeps its expected value:
        // the odds' moves from -1/2 to +1/2 of the variance with the fraction
        // (a dry month's fraction scales like the odds, a wet month's dry
        // fraction like their inverse), and the amount's also takes its
        // covariance with the fraction, f (1 - f) sf zp, so the expected
        // total stays.
        const auto factor = [&](double sigma, double correction)
        { return core::math::exp(core::math::fenced(sigma * zp) - core::math::fenced(0.5 * correction * v)); };
        constexpr double sf = model::wet_month;
        constexpr double sa = model::amount_month;
        const double f = std::clamp(lerp(c.wet), 0.0, 1.0);
        const double odds = factor(sf, sf * sf * (1.0 - (2.0 * f)));

        day_params p{};
        p.tmean = lerp(c.tmean);
        p.dtr = std::max(lerp(c.dtr), 0.0);
        p.wet = std::min((f * odds) / ((f * odds) + (1.0 - f)), 0.995);
        p.amount = std::max(lerp(c.amount) * factor(sa, (sa * sa) + (2.0 * sf * sa * (1.0 - f))), 0.1);
        p.humidity = std::clamp(lerp(c.humidity), 1.0, 100.0);
        p.cloud = std::clamp(lerp(c.cloud), 0.0, 1.0);
        p.wind = std::max(lerp(c.wind), 0.0);
        p.anomaly = za;
        p.residual = std::sqrt(1.0 - core::math::fenced(model::temperature_month * model::temperature_month * v));
        return p;
    }

    static double persistence_(double f)
    {
        return std::clamp(model::persistence0 + core::math::fenced(model::persistence1 * f), 0.0, 0.9);
    }

    // P(wet | dry) and P(wet | wet) that keep the wet-day fraction f.
    static std::pair<double, double> transitions_(double f)
    {
        const double d = persistence_(f);
        const double p01 = core::math::fenced(f * (1.0 - d));
        return {p01, p01 + d};
    }

    // Marsaglia and Tsang (2000) for a gamma variate; a shape below 1 uses the
    // shape + 1 variate times u^(1 / shape).
    static double gamma_(core::rng& r, double shape, double scale) // NOLINT(bugprone-easily-swappable-parameters)
    {
        double boost = 1.0;
        if (shape < 1.0)
        {
            const double u = 1.0 - core::uniform01(r); // (0, 1]
            boost = core::math::exp(core::math::log(u) / shape);
            shape += 1.0;
        }
        const double d = shape - (1.0 / 3.0);
        const double c = 1.0 / std::sqrt(9.0 * d);
        for (;;)
        {
            const double x = core::normal(r, 0.0, 1.0);
            const double t = 1.0 + core::math::fenced(c * x);
            if (t <= 0.0) continue;
            const double v = t * t * t;
            const double u = core::uniform01(r);
            if (u <= 0.0) continue;
            const double bound = core::math::fenced(0.5 * x * x) + d - core::math::fenced(d * v) +
                                 core::math::fenced(d * core::math::log(v));
            if (core::math::log(u) < bound) return d * v * scale * boost;
        }
    }

    static double beta_(core::rng& r, double mean, double kappa)
    {
        const double x = gamma_(r, mean * kappa, 1.0);
        const double y = gamma_(r, (1.0 - mean) * kappa, 1.0);
        return x / (x + y);
    }

    static double logistic_(double z) { return 1.0 / (1.0 + core::math::exp(-z)); }

    static double spread_(const std::array<double, 7>& c, const cell& cl, const day_params& p)
    {
        const double season = (p.tmean - cl.annual_mean) / std::max(cl.annual_range / 2.0, 0.5);
        const std::array<double, 7> x{1.0, cl.annual_range, p.dtr, p.wet, season, season * cl.annual_range,
                                      std::abs(cl.normals.latitude)};
        double s = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) s += core::math::fenced(c.at(i) * x.at(i));
        return std::clamp(s, model::spread_min, model::spread_max);
    }

    // A^k B for k = 0 .. window - 1: the temperature residual of a day is
    // the sum of these times the innovations of that day and the ones
    // before it.
    static const std::array<std::array<double, 4>, model::window>& impulse_()
    {
        static const auto table = []
        {
            std::array<std::array<double, 4>, model::window> out{};
            std::array<double, 4> m = model::b;
            for (auto& k : out)
            {
                k = m;
                const auto& a = model::a;
                m = {core::math::fenced(a[0] * m[0]) + core::math::fenced(a[1] * m[2]),
                     core::math::fenced(a[0] * m[1]) + core::math::fenced(a[1] * m[3]),
                     core::math::fenced(a[2] * m[0]) + core::math::fenced(a[3] * m[2]),
                     core::math::fenced(a[2] * m[1]) + core::math::fenced(a[3] * m[3])};
            }
            return out;
        }();
        return table;
    }

    struct sun
    {
        double daylight_minutes{};
        std::optional<double> rise; // minutes after 00:00 UTC of the date (may be negative or past 1440)
        std::optional<double> set;
    };

    // Sunrise, sunset and day length on a date at a point, by NOAA's solar
    // calculator equations (Meeus): the sun's position at local noon, the
    // equation of time, and the hour angle at which the sun's centre is
    // 0.833 degrees below the horizon (refraction and the sun's radius).
    static sun sun_(std::chrono::sys_days day, double latitude, double longitude) // NOLINT(bugprone-easily-swappable-parameters)
    {
        using core::math::fenced;
        const double days = static_cast<double>(day.time_since_epoch().count());
        // Julian centuries since J2000.0 at local mean noon.
        const double jd = days + 2440587.5 + 0.5 - (longitude / 360.0);
        const double t = (jd - 2451545.0) / 36525.0;
        const auto reduce = [](double x) { return x - (360.0 * std::floor(x / 360.0)); };
        const double l0 = reduce(280.46646 + fenced(t * (36000.76983 + fenced(t * 0.0003032))));
        const double m = reduce(357.52911 + fenced(t * (35999.05029 - fenced(0.0001537 * t))));
        const double e = 0.016708634 - fenced(t * (0.000042037 + fenced(0.0000001267 * t)));
        const double c = fenced(sin_deg_(m) * (1.914602 - fenced(t * (0.004817 + fenced(0.000014 * t))))) +
                         fenced(sin_deg_(2.0 * m) * (0.019993 - fenced(0.000101 * t))) +
                         fenced(sin_deg_(3.0 * m) * 0.000289);
        const double omega = reduce(125.04 - fenced(1934.136 * t));
        const double lambda = l0 + c - 0.00569 - fenced(0.00478 * sin_deg_(omega));
        const double eps0 =
            23.0 + ((26.0 + ((21.448 - fenced(t * (46.815 + fenced(t * (0.00059 - fenced(t * 0.001813)))))) / 60.0)) /
                    60.0);
        const double eps = eps0 + fenced(0.00256 * cos_deg_(omega));
        const double decl = asin_deg_(fenced(sin_deg_(eps) * sin_deg_(lambda)));
        const double ty = sin_deg_(eps / 2.0) / cos_deg_(eps / 2.0);
        const double y = fenced(ty * ty);
        constexpr double deg = 180.0 / std::numbers::pi;
        const double eq_time =
            4.0 * deg *
            (fenced(y * sin_deg_(2.0 * l0)) - fenced(2.0 * e * sin_deg_(m)) +
             fenced(4.0 * e * y * sin_deg_(m) * cos_deg_(2.0 * l0)) - fenced(0.5 * y * y * sin_deg_(4.0 * l0)) -
             fenced(1.25 * e * e * sin_deg_(2.0 * m)));
        const double noon = 720.0 - fenced(4.0 * longitude) - eq_time;

        const double denominator = fenced(cos_deg_(latitude) * cos_deg_(decl));
        const double numerator = cos_deg_(90.833) - fenced(sin_deg_(latitude) * sin_deg_(decl));
        // cos(hour angle) = numerator / denominator: above 1 the sun never
        // rises, below -1 it never sets.
        if (std::abs(denominator) < 1e-12 || numerator >= denominator || numerator <= -denominator)
        {
            const bool polar_day = (latitude > 0.0) == (decl > 0.0);
            return {.daylight_minutes = polar_day ? 1440.0 : 0.0, .rise = std::nullopt, .set = std::nullopt};
        }
        const double hour_angle = acos_deg_(numerator / denominator);
        return {.daylight_minutes = 8.0 * hour_angle, .rise = noon - (4.0 * hour_angle), .set = noon + (4.0 * hour_angle)};
    }

    // x rounded to 1 / per (round_to_(x, 10) to 0.1): the division gives the
    // double nearest the decimal, so JSON prints -1.2, not -1.2000000000000002;
    // adding 0 turns -0 into 0.
    static double round_to_(double x, double per) { return (std::round(x * per) / per) + 0.0; }

    static void check_request_(const weather_request& request)
    {
        if (!request.date.ok()) throw std::invalid_argument("weather-generator: date is not a valid date");
        if (request.latitude.has_value() != request.longitude.has_value())
        {
            throw std::invalid_argument("weather-generator: set latitude and longitude together");
        }
        if (request.latitude) check_point_(*request.latitude, *request.longitude);
        if (request.elevation && outside_(*request.elevation, -500.0, 9000.0))
        {
            throw std::invalid_argument("weather-generator: elevation must be -500 to 9000 metres");
        }
        if (outside_(request.warming, -10.0, 10.0))
        {
            throw std::invalid_argument("weather-generator: warming must be -10 to 10 °C");
        }
    }

    // Where a call's weather is: the requested point and its nearest cell,
    // or a cell drawn by land area.
    struct place
    {
        std::size_t cell{0};
        double latitude{0.0};
        double longitude{0.0};
        double distance_km{0.0};
    };

    // Distance in km from a point to the nearest land cell of the mask
    // (0 inside one), searched within the rows and columns that can be
    // within the limit; infinity when there is no mask or no land near.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    [[nodiscard]] double land_km_(double latitude, double longitude, const std::array<double, 3>& q) const
    {
        if (_land.empty()) return std::numeric_limits<double>::infinity();
        const auto row_of = [](double lat) { return std::clamp(static_cast<int>(std::floor((lat + 90.0) * 2.0)), 0, land_rows - 1); };
        const auto column_of = [](double lon)
        { return ((static_cast<int>(std::floor((lon + 180.0) * 2.0)) % land_columns) + land_columns) % land_columns; };
        const auto is_land = [&](int i, int j)
        {
            for (const auto& [a, b] : _land[static_cast<std::size_t>(i)])
            {
                if (j < a) return false;
                if (j <= b) return true;
            }
            return false;
        };
        const int i0 = row_of(latitude);
        const int j0 = column_of(longitude);
        if (is_land(i0, j0)) return 0.0;

        double best = std::numeric_limits<double>::infinity();
        for (int i = std::max(0, i0 - 2); i <= std::min(land_rows - 1, i0 + 2); ++i)
        {
            const double south = -90.0 + (0.5 * i);
            const double north = south + 0.5;
            const double nearest_lat = std::clamp(latitude, south, north);
            // Columns that can lie within the limit at this row's widest
            // latitude, plus one for the cell's own width.
            const double widest = std::max(std::abs(south), std::abs(north));
            const double km_per_degree = std::max(1e-6, cos_deg_(std::min(widest, 89.999)) * 111.19);
            const int reach = std::min(land_columns / 2, static_cast<int>(sea_limit_km / km_per_degree * 2.0) + 2);
            for (int dj = -reach; dj <= reach; ++dj)
            {
                const int j = (((j0 + dj) % land_columns) + land_columns) % land_columns;
                if (!is_land(i, j)) continue;
                const double west = -180.0 + (0.5 * j);
                // The cell's nearest longitude to the point, across the date line.
                double offset = longitude - west;
                offset -= 360.0 * std::floor(offset / 360.0); // 0 to 360 east of the cell's west edge
                double nearest_lon = west; // the point lies west of the cell
                if (offset <= 0.5) nearest_lon = longitude;
                else if (offset < 180.25) nearest_lon = west + 0.5;
                best = std::min(best, km_(q, unit_vector_(nearest_lat, nearest_lon)));
            }
        }
        return best;
    }

    // The cell a point uses and its distance to land: the nearest cell by
    // chord, if the point is within 100 km of the mask's land or of a cell.
    [[nodiscard]] place locate_(double latitude, double longitude) const
    {
        const auto q = unit_vector_(latitude, longitude);
        const std::size_t index = nearest_(q);
        const double km = std::min(land_km_(latitude, longitude, q), km_(q, _cells[index].xyz));
        if (!(km <= sea_limit_km))
        {
            throw std::invalid_argument(std::format(
                "weather-generator: ({}, {}) is {} km from land; the data covers land only, without Antarctica",
                latitude, longitude, std::isfinite(km) ? std::format("{:.0f}", km) : std::string{"far"}));
        }
        return {.cell = index, .latitude = latitude, .longitude = longitude, .distance_km = km};
    }

    [[nodiscard]] place place_(const weather_request& request, const core::rng& call) const
    {
        if (request.latitude) return locate_(*request.latitude, *request.longitude);
        if (_area.empty()) throw std::invalid_argument("weather-generator: no cell has an area to draw a place from");
        auto r = call.split("place");
        const std::size_t index = _area.pick(r);
        return {.cell = index,
                .latitude = _cells[index].normals.latitude,
                .longitude = _cells[index].normals.longitude,
                .distance_km = 0.0};
    }

    static std::uint64_t day_index_(std::chrono::sys_days day)
    {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(day.time_since_epoch().count()));
    }

    // Wet or dry on day k: read back to the latest day whose draw settles the
    // state whatever the day before was, then run forward.
    static bool wet_on_(const std::vector<day_draws>& draws, int k)
    {
        const auto at = [&](int i) -> const day_draws& { return draws[static_cast<std::size_t>(i)]; };
        int s = k;
        int known = -1; // 1 wet, 0 dry
        for (; s > k - model::window && known < 0; --s)
        {
            const auto [p01, p11] = transitions_(at(s).p.wet);
            if (at(s).u_wet < p01) known = 1;
            else if (at(s).u_wet >= p11) known = 0;
        }
        bool wet = false;
        if (known < 0)
        {
            // Not settled within the window (probability below 1e-20): start
            // from the stationary chance.
            wet = at(s).u_wet < at(s).p.wet;
        }
        else
        {
            ++s; // the loop stepped past the settling day
            wet = known == 1;
        }
        for (int t = s + 1; t <= k; ++t)
        {
            const auto [p01, p11] = transitions_(at(t).p.wet);
            wet = at(t).u_wet < (wet ? p11 : p01);
        }
        return wet;
    }

    struct temperatures
    {
        double mean;
        double tmax;
        double tmin;
    };

    // The day's mean temperature (the season, the wet or dry day, and a
    // spread split between the month's anomaly and the day's residual, so
    // the total spread is unchanged) and its range (log-normal around its
    // mean, so it is never negative and keeps the mean and spread).
    static temperatures temperatures_(const std::vector<day_draws>& draws, int k, bool wet, const cell& c,
                                      double shift)
    {
        // Residuals of the daily mean and range: the sum over the window,
        // oldest first.
        const auto& impulse = impulse_();
        double rm = 0.0;
        double rr = 0.0;
        for (int j = model::window - 1; j >= 0; --j)
        {
            const auto& e = draws[static_cast<std::size_t>(k - j)].e;
            const auto& m = impulse.at(static_cast<std::size_t>(j));
            rm += core::math::fenced(m[0] * e[0]) + core::math::fenced(m[1] * e[1]);
            rr += core::math::fenced(m[2] * e[0]) + core::math::fenced(m[3] * e[1]);
        }

        const auto& p = draws[static_cast<std::size_t>(k)].p;
        const double f = p.wet;
        const auto wet_shift = [&](double share)
        { return wet ? -core::math::fenced(share * p.dtr * (1.0 - f)) : core::math::fenced(share * p.dtr * f); };
        const double sm = spread_(model::spread_mean, c, p);
        const double anomaly =
            core::math::fenced(p.residual * rm) + core::math::fenced(model::temperature_month * p.anomaly);
        const double mean = p.tmean + wet_shift(model::wet_mean) + core::math::fenced(sm * anomaly) + shift;
        const double base_range = std::max(p.dtr + wet_shift(model::wet_range), 0.1);
        const double cv = spread_(model::spread_range, c, p) / base_range;
        const double s2 = core::math::log(1.0 + core::math::fenced(cv * cv));
        const double range = base_range * core::math::exp(core::math::fenced(std::sqrt(s2) * rr) - (0.5 * s2));
        return {.mean = mean, .tmax = mean + (0.5 * range), .tmin = mean - (0.5 * range)};
    }

    // Amount, form and thunder of a wet day.
    static void precipitation_(weather& w, const day_params& p, const temperatures& t, const core::rng& day)
    {
        auto r = day.split("amount");
        const double shape = std::clamp(model::shape0 + core::math::fenced(model::shape1 * core::math::log(p.amount)),
                                        model::shape_min, model::shape_max);
        const double amount = std::max(gamma_(r, shape, p.amount / shape), 0.1);
        auto phase = day.split("phase");
        const double snow = logistic_(model::snow0 + core::math::fenced(model::snow1 * t.mean));
        if (core::uniform01(phase) >= snow) w.type = precipitation_type::rain;
        else if (t.mean > model::sleet_above) w.type = precipitation_type::sleet;
        else w.type = precipitation_type::snow;

        w.precipitation = round_to_(amount, 10.0);
        // A cm of snow per mm of water; sleet is half snow.
        if (w.type == precipitation_type::snow) w.snowfall = w.precipitation;
        else if (w.type == precipitation_type::sleet) w.snowfall = round_to_(amount / 2.0, 10.0);

        if (w.type != precipitation_type::snow)
        {
            auto th = day.split("thunder");
            const auto& c = model::thunder;
            const double z =
                c[0] + core::math::fenced(c[1] * t.tmax) + core::math::fenced(c[2] * core::math::log(amount));
            w.thunder = core::uniform01(th) < logistic_(z);
        }
    }

    // Cloud cover, humidity and wind, which follow the wet and dry days.
    static void sky_(weather& w, const day_params& p, bool wet, const core::rng& day)
    {
        const double f = p.wet;
        {
            const double cbar = p.cloud;
            const double cw = cbar + core::math::fenced(model::cloud_wet * (1.0 - cbar));
            const double cd = f < 1.0 ? (cbar - core::math::fenced(f * cw)) / (1.0 - f) : cbar;
            const double mean = std::clamp(wet ? cw : cd, 0.02, 0.98);
            auto r = day.split("cloud");
            w.cloud_cover = round_to_(beta_(r, mean, model::cloud_kappa), 100.0);
        }
        {
            const double hw = model::humidity_wet * (1.0 - f);
            const double hd = -(model::humidity_wet * f);
            auto r = day.split("humidity");
            const double h = core::normal(r, p.humidity + (wet ? hw : hd), model::humidity_sd);
            w.humidity = round_to_(std::clamp(h, 5.0, 100.0), 1.0);
        }
        {
            auto r = day.split("wind");
            const double scale = p.wind / model::wind_gamma;
            const double u = 1.0 - core::uniform01(r); // (0, 1]
            const double e = -core::math::log(u);
            const double v = e > 0.0 ? scale * core::math::exp(core::math::log(e) / model::wind_shape) : 0.0;
            w.wind = round_to_(v, 10.0);
        }
    }

    // The weather of `days` days from request.date, each from its own
    // stream, with `window` days before the first read for the chains.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): days and seed differ in type and meaning
    [[nodiscard]] std::vector<weather> simulate_(const weather_request& request, int days,
                                                 std::uint64_t call_seed) const
    {
        require_data_();
        check_request_(request);
        const core::rng call{call_seed};
        const place where = place_(request, call);
        const cell& c = _cells[where.cell];
        const double elevation = request.elevation.value_or(c.normals.elevation);
        const double shift =
            request.warming - core::math::fenced(model::lapse_rate * (elevation - c.normals.elevation));

        // The place's stream: the same seed gives unrelated weather at
        // different cells, and the same weather at points of one cell.
        const std::uint64_t place_seed = call.split(c.normals.cell).seed();
        month_anomalies anomalies{core::rng{place_seed}.split("month").seed()};
        const auto first = std::chrono::sys_days{request.date} - std::chrono::days{model::window};
        const int span = model::window + days;

        std::vector<day_draws> draws(static_cast<std::size_t>(span));
        for (int k = 0; k < span; ++k)
        {
            const auto day = first + std::chrono::days{k};
            const core::rng day_rng{core::rng::child_seed_of(place_seed, day_index_(day))};
            auto& d = draws[static_cast<std::size_t>(k)];
            d.p = params_(c, std::chrono::year_month_day{day}, anomalies);
            auto u = day_rng.split("wet");
            d.u_wet = core::uniform01(u);
            auto e = day_rng.split("temperature");
            d.e = {core::normal(e, 0.0, 1.0), core::normal(e, 0.0, 1.0)};
        }

        std::vector<weather> out;
        out.reserve(static_cast<std::size_t>(days));
        for (int k = model::window; k < span; ++k)
        {
            const auto day = first + std::chrono::days{k};
            const core::rng day_rng{core::rng::child_seed_of(place_seed, day_index_(day))};
            const auto& p = draws[static_cast<std::size_t>(k)].p;
            const bool wet = wet_on_(draws, k);
            const auto t = temperatures_(draws, k, wet, c, shift);

            weather w;
            w._signature = {.seed = call_seed, .data = data_hash()};
            w.date = std::chrono::year_month_day{day};
            w.cell = c.normals.cell;
            w.latitude = where.latitude;
            w.longitude = where.longitude;
            w.elevation = round_to_(elevation, 1.0);
            w.distance_km = round_to_(where.distance_km, 10.0);
            if (wet) precipitation_(w, p, t, day_rng);
            sky_(w, p, wet, day_rng);
            w.tmin = round_to_(t.tmin, 10.0);
            w.tmean = round_to_(t.mean, 10.0);
            w.tmax = round_to_(t.tmax, 10.0);
            w.frost = w.tmin < 0.0;
            const auto sun = sun_(day, where.latitude, where.longitude);
            w.daylight_hours = round_to_(sun.daylight_minutes / 60.0, 100.0);
            // Sunshine: the daylight left uncovered by the day's cloud
            // (curated; doc/data.md).
            w.sunshine_hours = round_to_((sun.daylight_minutes / 60.0) * (1.0 - w.cloud_cover), 10.0);
            const auto instant = [&](double minutes)
            { return std::chrono::sys_time<std::chrono::minutes>{day} + std::chrono::minutes{std::llround(minutes)}; };
            if (sun.rise) w.sunrise = instant(*sun.rise);
            if (sun.set) w.sunset = instant(*sun.set);
            w.condition = condition_(w);
            out.push_back(std::move(w));
        }
        return out;
    }

    static weather_condition condition_(const weather& w)
    {
        switch (w.type)
        {
        case precipitation_type::none:
            if (w.cloud_cover < 0.2) return weather_condition::clear;
            if (w.cloud_cover < 0.5) return weather_condition::partly_cloudy;
            if (w.cloud_cover < 0.85) return weather_condition::mostly_cloudy;
            return weather_condition::overcast;
        case precipitation_type::snow:
            if (w.precipitation < 1.0) return weather_condition::light_snow;
            if (w.precipitation < 10.0) return weather_condition::snow;
            return weather_condition::heavy_snow;
        case precipitation_type::sleet:
            return w.thunder ? weather_condition::thunderstorm : weather_condition::sleet;
        case precipitation_type::rain: break;
        }
        if (w.thunder) return weather_condition::thunderstorm;
        if (w.precipitation < 2.5) return weather_condition::light_rain;
        if (w.precipitation < 10.0) return weather_condition::rain;
        return weather_condition::heavy_rain;
    }
};

} // namespace dasmig

#endif // DASMIG_WEATHERGEN_HPP
