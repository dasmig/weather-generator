// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Diego Dasso Migotto

#ifndef DASMIG_WEATHERGEN_HPP
#define DASMIG_WEATHERGEN_HPP

#include "core.hpp"
#include <algorithm>
#include <array>
#include <bit>
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

DASMIG_CORE_REQUIRE(1, 3);

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
    fog,           ///< Dry, with heavy fog (visibility down to about 400 m).
    drizzle,       ///< Rain below 2.5 mm that falls as drizzle.
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
    case weather_condition::fog: return "fog";
    case weather_condition::drizzle: return "drizzle";
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
    case weather_condition::fog: return "fog";
    case weather_condition::drizzle: return "drizzle";
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

/// @brief The units of a result's weather values.
enum class unit_system : std::uint8_t
{
    metric,  ///< °C, mm of precipitation, cm of snow, m/s.
    imperial ///< °F, inches of precipitation and of snow, mph.
};

/// @brief The key of a unit system (`metric`, `imperial`).
/// @param u The unit system.
/// @return The key.
[[nodiscard]] constexpr std::string_view to_string(unit_system u) noexcept
{
    return u == unit_system::imperial ? "imperial" : "metric";
}

/// @cond INTERNAL
namespace weather_detail
{

// An instant as ISO 8601 UTC to the minute (`2024-01-15T07:42Z`).
inline std::string time_string(std::chrono::sys_time<std::chrono::minutes> t)
{
    const auto day = std::chrono::floor<std::chrono::days>(t);
    const std::chrono::year_month_day ymd{day};
    const auto minutes = (t - day).count();
    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}Z", static_cast<int>(ymd.year()),
                       static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), minutes / 60,
                       minutes % 60);
}

} // namespace weather_detail
/// @endcond

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

    /// @brief The units of the weather values (metric by default). The
    ///        place keeps metres and km either way, and `warming` and
    ///        `elevation` are always in °C and metres.
    unit_system units{unit_system::metric};

    /// @brief Also give each day's 24 hours in weather::hours. The days
    ///        themselves are the same with or without them.
    bool hourly{false};

    /// @brief Draw each month around the month as it was (CRU TS, January
    ///        1991 to December 2020) instead of around the normals: a cold
    ///        winter is cold, a drought month is dry. Every day asked for
    ///        must fall in 1991-2020, and load_history() must have loaded
    ///        the months.
    bool historical{false};

    /// @brief Share the weather between nearby places on the same seed (on
    ///        by default): a seed is one world, where a front that brings
    ///        rain to one town brings it to the next one too, and a cold
    ///        month is cold across a region. Correlations fall with distance
    ///        as between weather stations (doc/data.md). Set false to give
    ///        each climate cell weather of its own.
    bool spatial{true};
};

/// @brief One hour of a day's weather (weather::hours, when the request asks
///        for them).
///
/// The hours are those of the place's mean solar day: hour 0 starts at local
/// mean midnight, `4 * longitude` minutes before 00:00 UTC (east is
/// earlier), so the day's lowest temperature comes near sunrise and the
/// highest in the afternoon wherever the place is. In metric units the
/// precipitation and snowfall of the 24 hours add up to the day's exactly.
/// Values are rounded like the day's.
struct weather_hour
{
    std::chrono::sys_time<std::chrono::minutes> time; ///< Start of the hour, UTC.
    int hour{};              ///< Hour of the local mean solar day, 0 to 23.
    double temperature{};    ///< Temperature at the start of the hour, °C (°F).
    double precipitation{};  ///< Precipitation in the hour, mm (in).
    double snowfall{};       ///< Fresh snow in the hour, cm (in).
    precipitation_type type{precipitation_type::none}; ///< The form it falls in (none in a dry hour).
    bool thunder{};          ///< Thunder in the hour.
    bool fog{};              ///< Fog in the hour (a foggy day's hours around sunrise).
    double cloud_cover{};    ///< Cloud cover, 0 to 1.
    double humidity{};       ///< Relative humidity, %.
    double wind{};           ///< Mean wind speed at 10 m, m/s (mph).
    double daylight_hours{}; ///< The part of the hour the sun is up, 0 to 1.
    double sunshine_hours{}; ///< Bright sunshine in the hour, 0 to 1.
    weather_condition condition{weather_condition::clear}; ///< The hour in a word.

    /// @brief Write this hour as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object()
            .field("time", weather_detail::time_string(time))
            .field("hour", hour)
            .field("condition", dasmig::to_string(condition))
            .field("temperature", temperature)
            .field("precipitation", precipitation)
            .field("snowfall", snowfall)
            .field("type", dasmig::to_string(type))
            .field("thunder", thunder)
            .field("fog", fog)
            .field("cloud_cover", cloud_cover)
            .field("humidity", humidity)
            .field("wind", wind)
            .field("daylight_hours", daylight_hours)
            .field("sunshine_hours", sunshine_hours)
            .end_object();
    }
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
/// Values are rounded for output: temperatures, precipitation, snowfall,
/// snow depth, wind, gusts and solar radiation to 0.1 (inches of
/// precipitation to 0.01), cloud cover to 0.01, humidity to whole percent.
/// The units are metric unless the request asked for imperial ones. Supports
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
    double tmin{};         ///< Lowest temperature, °C (°F).
    double tmean{};        ///< Mean temperature, °C (°F): halfway between tmin and tmax.
    double tmax{};         ///< Highest temperature, °C (°F).
    bool frost{};          ///< tmin below 0 °C (32 °F).
    double precipitation{}; ///< Precipitation, mm (in) of water (0 on a dry day).
    double snowfall{};     ///< Fresh snow, cm (in): 10 cm per 10 mm of water.
    /// @brief Snow on the ground at the end of the day, cm (in): the
    ///        season's snowfall less what has melted, settled as it ages.
    double snow_depth{};
    precipitation_type type{precipitation_type::none}; ///< The form it falls in.
    bool thunder{};        ///< Thunder heard (on rain or sleet days only).
    bool fog{};            ///< Fog at some time of the day.
    double cloud_cover{};  ///< Mean cloud cover, 0 to 1.
    double humidity{};     ///< Mean relative humidity, %.
    double wind{};         ///< Mean wind speed at 10 m, m/s (mph).
    double gust{};         ///< Strongest gust (a 5-second mean) at 10 m, m/s (mph).
    double daylight_hours{}; ///< Time from sunrise to sunset, hours (0 in polar night, 24 in polar day).
    double sunshine_hours{}; ///< Bright sunshine, hours.
    /// @brief Solar energy reaching the ground, MJ per square metre (in
    ///        either unit system): global radiation on level ground.
    double solar_radiation{};
    /// @brief Sunrise, UTC (unset in polar day and polar night). It can fall
    ///        on the date before in UTC, far east of Greenwich.
    std::optional<std::chrono::sys_time<std::chrono::minutes>> sunrise{}; // NOLINT(readability-redundant-member-init)
    /// @brief Sunset, UTC (unset in polar day and polar night).
    std::optional<std::chrono::sys_time<std::chrono::minutes>> sunset{}; // NOLINT(readability-redundant-member-init)
    weather_condition condition{weather_condition::clear}; ///< The day in a word.
    unit_system units{unit_system::metric}; ///< The units of the values above.
    /// @brief The day's 24 hours, when the request set `hourly` (empty
    ///        otherwise).
    std::vector<weather_hour> hours;

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
        return weather_detail::time_string(t);
    }

    /// @brief Human-readable summary string.
    /// @return For example `2023-07-01: light rain, 14.2 to 21.9 °C, 1.3 mm`
    ///         (`57.6 to 71.4 °F, 0.05 in` in imperial units).
    [[nodiscard]] std::string to_string() const
    {
        const bool imperial = units == unit_system::imperial;
        std::string s = std::format("{}: {}, {:.1f} to {:.1f} {}", date_string(), label(condition), tmin, tmax,
                                    imperial ? "°F" : "°C");
        if (type == precipitation_type::snow || type == precipitation_type::sleet)
        {
            s += std::format(", {:.1f} {} of snow", snowfall, imperial ? "in" : "cm");
        }
        else if (type == precipitation_type::rain)
        {
            s += imperial ? std::format(", {:.2f} in", precipitation) : std::format(", {:.1f} mm", precipitation);
        }
        return s;
    }

    /// @brief Write this weather as a JSON object.
    /// @param w Writer positioned where a value is allowed.
    void to_json(core::json_writer& w) const
    {
        w.begin_object().field("date", date_string()).field("units", dasmig::to_string(units));
        w.key("place")
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
            .field("snow_depth", snow_depth)
            .field("type", dasmig::to_string(type))
            .field("thunder", thunder)
            .field("fog", fog)
            .field("cloud_cover", cloud_cover)
            .field("humidity", humidity)
            .field("wind", wind)
            .field("gust", gust)
            .field("daylight_hours", daylight_hours)
            .field("sunshine_hours", sunshine_hours)
            .field("solar_radiation", solar_radiation);
        const auto instant = [&](std::string_view name, const auto& t)
        {
            w.key(name);
            if (t) w.value(time_string(*t));
            else w.value(nullptr);
        };
        instant("sunrise", sunrise);
        instant("sunset", sunset);
        if (!hours.empty())
        {
            w.key("hours").begin_array();
            for (const auto& h : hours) h.to_json(w);
            w.end_array();
        }
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
/// A Richardson (1981) type weather generator: rain falls by a semi-Markov
/// chain, wet-day amounts are Weibull distributed, the daily mean
/// temperature and the daily range follow the season and the rain around
/// the day with residuals from a lag-1 autoregressive process, months carry
/// their own anomalies, and cloud, humidity, wind, gusts and fog follow the
/// wet and dry days. Snow lies on the ground until it melts. Its parameters
/// come from the place's monthly normals (CRU TS 4.10, 1991-2020) and from
/// fits on GHCN-Daily stations (doc/data.md).
///
/// A day's weather depends only on the seed, the place and the date: the
/// draws of each day come from that day's own stream, and the chains are
/// read back to the day where their state no longer matters. So the weather
/// of 10 March is the same whether it is asked for alone or inside a series
/// that starts in January. On one seed, nearby places share their weather
/// through regional fields (weather_request::spatial). Output is identical
/// on every compiler and platform (dasmig-core).
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
    /// @throws std::runtime_error If no data has been loaded, or the request
    ///         is historical and no history is loaded.
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
        return std::move(run_(request, 1, call_seed).front());
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
        return run_(request, days, call_seed);
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

    /// @brief Load the months of 1991-2020 for historical requests, from the
    ///        compiled-in copy or `resources/history/history.tsv`.
    ///
    /// Not loaded by instance() or load(): the table is large (23 MB)
    /// and only historical requests read it. It holds, for each 2-degree
    /// block of land, each month's temperature, precipitation and wet days
    /// against the 1991-2020 normals (CRU TS 4.10); a cell takes the
    /// block its point lies in.
    ///
    /// @return False when the file was not found.
    /// @throws core::tsv_error If the file is malformed.
    [[nodiscard]] bool load_history()
    {
        auto doc = core::load_resource(std::string{history_file_}, embedded_files_(),
                                       core::find_resources(library_name_, std::string{history_file_}));
        if (!doc) return false;
        read_history_(*doc);
        return true;
    }

    /// @brief Load the months of 1991-2020 from a file.
    /// @param file A `history.tsv`.
    /// @throws core::tsv_error If the file cannot be read or is malformed;
    ///         the months loaded before are then kept.
    void load_history(const std::filesystem::path& file) { read_history_(core::tsv_document::load(file)); }

    /// @brief Check whether historical requests can be served.
    /// @return True when load_history() has loaded the months.
    [[nodiscard]] bool has_history() const { return !_history.empty(); }

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

        // Dry spells (semi-Markov): after L dry days the chance of rain is
        // q h(L), h(L) = 1 - c (1 - exp(-(L - 1) / tau)), so rain comes back
        // sooner after one dry day than after ten; q keeps the mean dry
        // spell, 1 / P(wet | dry), of the first-order chain above. Runs of
        // dry_states days or more share h(dry_states).
        static constexpr double dry_hazard_drop = 0.1; // c, tuned
        static constexpr double dry_hazard_days = 2.0; // tau, tuned
        static constexpr int dry_states = 16;

        // Weibull shape of wet-day amounts = a0 + a1 ln(mean wet-day mm).
        static constexpr double shape0 = 0.9997;
        static constexpr double shape1 = -0.1191;
        static constexpr double shape_min = 0.4;
        static constexpr double shape_max = 1.5;

        // The rain classes of a day by its own and its neighbours' rain:
        // wet after a dry day, wet after a wet day, dry after a wet day,
        // dry before a wet day, dry between wet days (a dry day between dry
        // days is the reference). Each shifts the daily mean by a share of
        // the diurnal range and scales the range by 1 + a share, each share
        // c . (1, season, f, |latitude| / 90, annual range / 30, dtr / 10,
        // cloud, humidity / 100): fronts bring warm days before the rain and
        // cold days after it, wet days have warm nights, and a cloudy
        // winter's wet days are warmer than its dry ones.
        static constexpr int classes = 5;
        static constexpr std::array<std::array<double, 8>, classes> class_mean{{
            {-0.8567, -0.2014, 0.4379, 0.4119, 0.1073, 0.0691, -0.1620, 0.6504},
            {-1.3251, -0.3310, 0.6678, 0.6942, 0.1441, 0.0054, -0.5106, 1.2427},
            {-0.6406, -0.1142, 0.2561, 0.2294, -0.0135, -0.0094, -0.1986, 0.6811},
            {-0.2103, -0.0638, 0.1055, 0.0571, 0.0215, 0.0727, 0.0472, 0.0989},
            {-0.8014, -0.1782, 0.4068, 0.3643, 0.0065, 0.0556, -0.1826, 0.6812},
        }};
        static constexpr std::array<std::array<double, 8>, classes> class_range{{
            {-0.0854, -0.0814, 0.3890, 0.0992, 0.0556, -0.2476, -0.5605, 0.3597},
            {0.0851, -0.0880, 0.2882, 0.0080, 0.0431, -0.3305, -0.5637, 0.2484}, // NOLINT(modernize-use-std-numbers): fitted
            {-0.0335, -0.0418, 0.1646, -0.0016, 0.0524, -0.1326, -0.2734, 0.1771},
            {-0.1274, -0.0210, 0.1694, 0.0654, 0.0428, -0.0417, -0.1770, 0.1407},
            {-0.1248, -0.0469, 0.2682, 0.0144, 0.0897, -0.1526, -0.3572, 0.2531},
        }};

        // Day-to-day spread (°C) of the daily mean temperature and of the
        // daily range = c . (1, annual range, dtr, f, season, season * range,
        // |latitude|), where season is the day's mean temperature against
        // the year's, -1 (coldest) to 1; spreads about the rain classes'
        // shifts. The intercepts are tuned (the fit's -1.201 and -0.120):
        // the classes' shifts, predicted rather than each station's own,
        // add a little spread.
        static constexpr std::array<double, 7> spread_mean{-1.26, 0.05072, 0.14930, 1.25918, -0.36100, -0.04355, 0.03593};
        static constexpr std::array<double, 7> spread_range{-0.17, 0.02164, 0.19810, 0.50738, -0.19703, -0.00827, 0.01325};
        static constexpr double spread_min = 0.2;
        static constexpr double spread_max = 12.0;

        // Lag-1 autoregression of the standardized (mean, range) residuals:
        // x_t = A x_{t-1} + B e_t (Richardson 1981), row-major.
        static constexpr std::array<double, 4> a{0.70386418, 0.06393189, -0.04861423, 0.29847693};
        static constexpr std::array<double, 4> b{0.70319415, 0.0, 0.09214192, 0.94913958};

        // Weibull shape of the daily mean wind, and Gamma(1 + 1/k).
        static constexpr double wind_shape = 2.902;
        static constexpr double wind_gamma = 0.89171496;

        // The day's strongest gust: ln(gust / wind) = g0 + g1 ln(wind) +
        // g2 thunder, plus a normal residual of sd gust_sd (fitted on the
        // 5-second gusts of US stations); at least gust_min times the wind.
        static constexpr std::array<double, 3> gust{1.6787, -0.4902, 0.2273};
        static constexpr double gust_sd = 0.2306;
        static constexpr double gust_min = 1.2;

        // Fog at some time of the day, and heavy fog: logit p = c . (1,
        // humidity / 100 (the month's), range / the season's range, wet,
        // ln(1 + mm), winter (cosine of the season, 1 at midwinter),
        // ln(wind / the season's wind)), fitted on US stations' fog reports
        // where they also report wind, with the intercepts refitted on all
        // fog-reporting stations (those with wind, mostly automated
        // airports, report fog more often).
        static constexpr std::array<double, 7> fog{-5.0962, 4.8243, -0.5113, 1.0134, 0.5929, 0.1539, -0.4476};
        static constexpr std::array<double, 7> heavy_fog{-7.5642, 4.9412, 0.0683, 0.1133, 0.1912, 0.1028, -0.6007};

        // Drizzle on a rain day without thunder above 0 °C: logit p = c .
        // (1, Tmax / 10, (Tmax / 10)^2, ln mm, humidity / 100), fitted on US
        // stations' drizzle reports; a light rain day with drizzle is
        // called drizzle.
        static constexpr std::array<double, 5> drizzle{-4.8649, 0.4193, -0.1169, -0.0731, 4.1903};

        // Sunshine: the share of the daylight that is sunny is
        // (1 - cloud)^sunshine_power, fitted so a month of days averages
        // CRU CL 2.0's sunshine against CRU TS cloud. Solar radiation by
        // Angstrom-Prescott with FAO-56's coefficients: Rs = (a + b n / N)
        // Ra (curated).
        static constexpr double sunshine_power = 0.637;
        static constexpr double angstrom_a = 0.25;
        static constexpr double angstrom_b = 0.50;

        // Snow on the ground (fitted on GHCN snow depths with the stations'
        // own weather): the snow's water melts by melt_rate mm a day per
        // °C of daily mean above 0 °C; new snow has density snow_new
        // kg/m3 (10 cm per 10 mm) and settles towards snow_old at settle a
        // day (settle_melting while it melts). No snow lies at the end of a
        // day whose normal mean temperature (after elevation and warming) is
        // snow_free or more; where no day is that warm, a season's snow
        // starts at the middle of the warmest month.
        static constexpr double melt_rate = 3.5;
        static constexpr double snow_new = 100.0;
        static constexpr double snow_old = 350.0;
        static constexpr double settle = 0.01;
        static constexpr double settle_melting = 0.3;
        static constexpr double snow_free = 10.0;

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
        static constexpr double temperature_month = 0.40;
        static constexpr double wet_month = 0.32;
        static constexpr double amount_month = 0.28;
        // The diurnal range's month anomaly (its share of the range's
        // daily spread, tuned) and its correlation with the precipitation
        // anomaly (cloudy wet months have small ranges).
        static constexpr double range_month = 0.32;
        static constexpr double range_precipitation = -0.3;

        // Spatial correlation (weather_request::spatial): each random part
        // of the weather is a regional field plus a local part. A field is
        // the sum of up to two scales, each the smooth interpolation of
        // standard normals on nodes `spacing` degrees apart, times a weight;
        // fitted on the correlation by distance between GHCN stations
        // (doc/data.md).
        struct scale
        {
            double spacing; // degrees between nodes
            double weight;
        };
        static constexpr std::array<scale, 2> field_rain{{{.spacing = 3.5, .weight = 0.47}, {.spacing = 8.6, .weight = 0.738}}};
        static constexpr std::array<scale, 2> field_amount{{{.spacing = 2.9, .weight = 0.36}, {.spacing = 7.8, .weight = 0.33}}};
        static constexpr std::array<scale, 2> field_mean{{{.spacing = 5.0, .weight = 0.4}, {.spacing = 16.0, .weight = 0.86}}};
        static constexpr std::array<scale, 2> field_range{{{.spacing = 4.2, .weight = 0.49}, {.spacing = 10.3, .weight = 0.615}}};
        static constexpr std::array<scale, 2> field_month_temperature{{{.spacing = 6.0, .weight = 0.3}, {.spacing = 24.0, .weight = 0.95}}};
        // How much of a wet day's amount follows how deep in the rain the day
        // is (the rain's uniform against the day's chance of rain, as a
        // normal): neighbours inside one rain area all get more, which the
        // monthly totals of nearby stations show (tuned).
        static constexpr double amount_rain = 0.7;
        static constexpr std::array<scale, 2> field_month_precipitation{{{.spacing = 3.0, .weight = 0.6}, {.spacing = 9.8, .weight = 0.78}}};

        static constexpr double lapse_rate = 0.0065; // °C per metre (the standard atmosphere)

        // Hours (curated; doc/data.md). Temperature after Cesaraccio et
        // al. (2001): a sine from tmin at sunrise up to tmax at the warmest
        // hour, sunset - 4 h (or midway in a short day), a sine down to the
        // sunset temperature, tmax - sunset_share (tmax - the next tmin),
        // and a square-root fall to the next tmin at the next sunrise.
        static constexpr double sunset_share = 0.39;
        static constexpr double warmest_before_sunset = 4.0;
        // Wet hours: 2 + 2.5 ln(1 + mm) on average (half that with
        // thunder), gamma distributed with shape 2, in one spell or, with
        // two_spells, two.
        static constexpr double wet_hours0 = 2.0;
        static constexpr double wet_hours1 = 2.5;
        static constexpr double two_spells = 0.3;
        static constexpr double thunder_hour = 15.0; // a thunder spell is centred on 15:00 local
        // Wind: a 25% swing peaking at 14:00 local, times log-normal noise;
        // cloud: logit noise around the day's cover. Both lag-1 correlated.
        static constexpr double wind_swing = 0.25;
        static constexpr double windiest_hour = 14.0;
        static constexpr double wind_noise = 0.2;
        static constexpr double cloud_noise = 1.0;
        static constexpr double hour_lag = 0.8;
        static constexpr double wet_hour_cloud = 0.85;
        static constexpr double wet_hour_humidity = 90.0;

        // A foggy day's fog lasts from fog_before hours before sunrise to
        // fog_after hours after it (curated).
        static constexpr double fog_before = 3.0;
        static constexpr double fog_after = 3.0;

        // Days read back: the temperature residual sums this many days of
        // innovations (A^window is below 1e-7). The rain chain looks back
        // chain_window days for the day its state stops mattering (the
        // window's days, already drawn), then twice as far, up to
        // chain_window_max; the state found is the same however far it looked.
        static constexpr int window = 48;
        static constexpr int chain_window = window;
        static constexpr int chain_window_max = 4096;
    };

    // -- Internal data structures -----------------------------------------

    static constexpr std::string_view library_name_ = "weather-generator";
    static constexpr std::string_view data_file_ = "climate.tsv";
    static constexpr std::string_view land_file_ = "land.tsv";

    // The months of history.tsv: 2-degree blocks (90 rows of 180), each with
    // history_months months from January of history_first_year.
    static constexpr std::string_view history_file_ = "history/history.tsv";
    static constexpr int history_first_year = 1991;
    static constexpr int history_last_year = 2020;
    static constexpr int history_months = 12 * (history_last_year - history_first_year + 1);
    static constexpr int history_rows = 90;
    static constexpr int history_columns = 180;
    // Per month: temperature anomaly (°C), precipitation (% of normal) and
    // wet-day anomaly (days).
    using history_month = std::array<float, 3>;

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

    // The index of the 2-degree block a point lies in, row-major.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): latitude, longitude as everywhere
    static std::size_t history_block_of_(double latitude, double longitude)
    {
        const auto row = std::clamp(static_cast<int>(std::floor((latitude + 90.0) / 2.0)), 0, history_rows - 1);
        const auto column =
            std::clamp(static_cast<int>(std::floor((longitude + 180.0) / 2.0)), 0, history_columns - 1);
        return (static_cast<std::size_t>(row) * history_columns) + static_cast<std::size_t>(column);
    }

    void read_history_(const core::tsv_document& doc)
    {
        std::vector<std::int32_t> block(static_cast<std::size_t>(history_rows * history_columns), -1);
        std::vector<history_month> months;
        for (const auto& row : doc.rows())
        {
            // The key is the block's centre, "latitude,longitude".
            const auto key = row.get<std::string>("key");
            const auto comma = key.find(',');
            const auto lat = core::detail::parse_number<double>(std::string_view{key}.substr(0, comma));
            const auto lon = comma == std::string::npos
                                 ? std::nullopt
                                 : core::detail::parse_number<double>(std::string_view{key}.substr(comma + 1));
            if (!lat || !lon || outside_(*lat, -90.0, 90.0) || outside_(*lon, -180.0, 180.0))
            {
                throw row.error("key", "must be a block centre such as 49.00,3.00");
            }
            if (row.get<int>("first_year") != history_first_year)
            {
                throw row.error("first_year", std::format("must be {}", history_first_year));
            }
            auto& slot = block[history_block_of_(*lat, *lon)];
            if (slot >= 0) throw row.error("key", "repeats a block");
            slot = static_cast<std::int32_t>(months.size());
            months.resize(months.size() + history_months);
            const std::array<std::pair<std::string_view, std::pair<double, double>>, 3> columns{
                {{"tmean", {-40.0, 40.0}}, {"precipitation", {0.0, 100000.0}}, {"wet_days", {-31.0, 31.0}}}};
            for (std::size_t v = 0; v < columns.size(); ++v)
            {
                const auto& [name, range] = columns.at(v);
                const auto items = row.list(name);
                if (items.size() != static_cast<std::size_t>(history_months))
                {
                    throw row.error(name, std::format("must hold {} values separated by ';'", history_months));
                }
                for (std::size_t m = 0; m < items.size(); ++m)
                {
                    const auto x = core::detail::parse_number<double>(items[m]);
                    if (!x || outside_(*x, range.first, range.second))
                    {
                        throw row.error(name, std::format("values must be {} to {}", range.first, range.second));
                    }
                    months[static_cast<std::size_t>(slot) + m].at(v) = static_cast<float>(*x);
                }
            }
        }
        _history_block = std::move(block);
        _history = std::move(months);
        _history_hash = doc.content_hash();
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
        unsigned warmest{7}; // the warmest month, 1 to 12: snow seasons start in its middle

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
        double normal_wet;     // the wet-day fraction the rain classes centre on: the normals' (historical: the month's)
        double anomaly;        // the months' temperature anomaly, standard normal
        double residual;       // the share of the spread left to the daily residual
        double range_anomaly;  // the months' diurnal range anomaly, standard normal
        double range_residual; // the share of the range's spread left to the daily residual
        double p11;            // P(wet | wet)
        double q;              // P(wet | dry) after one dry day; q h(L) after L
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

    std::vector<std::int32_t> _history_block; // block row * 180 + column -> first month in _history, or -1
    std::vector<history_month> _history;
    std::uint64_t _history_hash{0};

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

    // Great-circle distance in km between two unit vectors.
    static double km_(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        constexpr double rad = std::numbers::pi / 180.0;
        return 2.0 * earth_radius_km * core::math::asin_deg(std::min(1.0, std::sqrt(distance2_(a, b)) / 2.0)) * rad;
    }

    static std::array<double, 3> unit_vector_(double latitude, double longitude)
    {
        const auto [slat, clat] = core::math::sin_cos_deg(latitude);
        const auto [slon, clon] = core::math::sin_cos_deg(longitude);
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
            c.warmest = static_cast<unsigned>(std::ranges::max_element(n.tmean) - n.tmean.begin()) + 1;
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

    // -- Distributions ----------------------------------------------------

    // P(Z > x) for a standard normal Z, by Hart's (1968) rational
    // approximation as West (2005) gives it, good to about 1e-14.
    static double normal_tail_(double x)
    {
        using core::math::fenced;
        const double a = std::abs(x);
        double c = 0.0; // P(Z > |x|)
        if (a < 37.0)
        {
            const double e = core::math::exp(-fenced(a * a) / 2.0);
            if (a < 7.07106781186547)
            {
                double n = fenced(3.52624965998911e-02 * a) + 0.700383064443688;
                n = fenced(n * a) + 6.37396220353165;
                n = fenced(n * a) + 33.912866078383;
                n = fenced(n * a) + 112.079291497871;
                n = fenced(n * a) + 221.213596169931;
                n = fenced(n * a) + 220.206867912376;
                double d = fenced(8.83883476483184e-02 * a) + 1.75566716318264;
                d = fenced(d * a) + 16.064177579207;
                d = fenced(d * a) + 86.7807322029461;
                d = fenced(d * a) + 296.564248779674;
                d = fenced(d * a) + 637.333633378831;
                d = fenced(d * a) + 793.826512519948;
                d = fenced(d * a) + 440.413735824752;
                c = e * n / d;
            }
            else
            {
                double b = a + 0.65;
                b = a + (4.0 / b);
                b = a + (3.0 / b);
                b = a + (2.0 / b);
                b = a + (1.0 / b);
                c = e / b / 2.506628274631;
            }
        }
        return x >= 0.0 ? c : 1.0 - c;
    }

    // Gamma(1 + 1 / c), the mean of a Weibull of shape c and scale 1, for
    // shape_min <= c <= shape_max: tabulated and read by linear
    // interpolation (good to about 1e-7).
    static double weibull_mean_factor_(double shape)
    {
        constexpr int steps = 1024;
        constexpr double lo = model::shape_min;
        constexpr double width = model::shape_max - model::shape_min;
        static const auto table = []
        {
            std::array<double, steps + 1> out{};
            for (int i = 0; i <= steps; ++i)
            {
                const double c = lo + (width * static_cast<double>(i) / steps);
                out.at(static_cast<std::size_t>(i)) = gamma_function_(1.0 + (1.0 / c));
            }
            return out;
        }();
        const double x = std::clamp((shape - lo) / width, 0.0, 1.0) * steps;
        const auto i = std::min(static_cast<std::size_t>(x), static_cast<std::size_t>(steps - 1));
        const double t = x - static_cast<double>(i);
        return table.at(i) + core::math::fenced(t * (table.at(i + 1) - table.at(i)));
    }

    // The z with P(Z > z) = p for a standard normal Z: Acklam's rational
    // approximation, good to about 1e-9.
    static double normal_upper_(double p)
    {
        using core::math::fenced;
        p = std::clamp(p, 1e-300, 1.0 - 1e-16);
        constexpr std::array<double, 6> a{-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                                          1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
        constexpr std::array<double, 5> b{-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                                          6.680131188771972e+01,  -1.328068155288572e+01};
        constexpr std::array<double, 6> c{-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                                          -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
        constexpr std::array<double, 4> d{7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                                          3.754408661907416e+00};
        // The tail: z for the smaller of p and 1 - p, as a lower quantile.
        const auto tail = [&](double t)
        {
            const double q = std::sqrt(-2.0 * core::math::log(t));
            double n = c[0];
            for (std::size_t i = 1; i < c.size(); ++i) n = fenced(n * q) + c.at(i);
            double m = d[0];
            for (std::size_t i = 1; i < d.size(); ++i) m = fenced(m * q) + d.at(i);
            m = fenced(m * q) + 1.0;
            return n / m;
        };
        constexpr double low = 0.02425;
        double x = 0.0; // the lower quantile of 1 - p, which is the z asked for
        if (p < low)
        {
            x = -tail(p);
        }
        else if (p > 1.0 - low)
        {
            x = tail(1.0 - p);
        }
        else
        {
            const double q = 0.5 - p;
            const double r = fenced(q * q);
            double n = a[0];
            for (std::size_t i = 1; i < a.size(); ++i) n = fenced(n * r) + a.at(i);
            double m = b[0];
            for (std::size_t i = 1; i < b.size(); ++i) m = fenced(m * r) + b.at(i);
            m = fenced(m * r) + 1.0;
            x = n * q / m;
        }
        return x;
    }

    // The gamma function for x above 0.5 (Lanczos, as in Numerical
    // Recipes, good to about 2e-10).
    static double gamma_function_(double x)
    {
        static constexpr std::array<double, 6> coefficients{76.18009172947146,   -86.50532032941677,
                                                            24.01409824083091,   -1.231739572450155,
                                                            0.1208650973866179e-2, -0.5395239384953e-5};
        const double t = x + 5.5;
        double series = 1.000000000190015;
        double y = x;
        for (const double c : coefficients) series += c / (y += 1.0);
        return core::math::exp(core::math::fenced((x + 0.5) * core::math::log(t)) - t +
                               core::math::log(2.5066282746310005 * series / x));
    }

    // Two independent standard normals from a seed alone, for the nodes of
    // the spatial fields: each half of a hash of the seed read through a
    // table of 4096 normal quantiles (at the middles of equal-probability
    // bins) by linear interpolation. The tails beyond 3.7 are cut (the
    // variance is 0.996); a place's own local part keeps them.
    static std::array<double, 2> hashed_normals_(std::uint64_t seed)
    {
        constexpr int bins = 4096;
        static const auto table = []
        {
            std::array<double, bins> out{};
            for (int k = 0; k < bins; ++k)
            {
                out.at(static_cast<std::size_t>(k)) = -normal_upper_((static_cast<double>(k) + 0.5) / bins);
            }
            return out;
        }();
        const std::uint64_t h = core::rng::child_seed_of(seed, 0);
        const auto read = [&](std::uint32_t bits)
        {
            const double x = std::clamp((static_cast<double>(bits) * 0x1.0p-32 * bins) - 0.5, 0.0, bins - 1.0);
            const auto k = std::min(static_cast<std::size_t>(x), static_cast<std::size_t>(bins - 2));
            const double t = x - static_cast<double>(k);
            return table.at(k) + core::math::fenced(t * (table.at(k + 1) - table.at(k)));
        };
        return {read(static_cast<std::uint32_t>(h >> 32U)), read(static_cast<std::uint32_t>(h))};
    }

    // -- Spatial fields ---------------------------------------------------

    // One scale of a field at one point: the four nodes around it and their
    // weights. Nodes lie on rows `spacing` degrees apart, each row with as
    // many columns as keep them about `spacing` apart along it; the weights
    // are smoothstep-bilinear, scaled so their squares add up to 1, so the
    // field is a standard normal everywhere.
    struct lattice_point
    {
        std::array<std::uint64_t, 4> node{};
        std::array<double, 4> weight{};
    };

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): latitude, longitude as everywhere
    static lattice_point lattice_(double latitude, double longitude, double spacing, std::uint64_t seed)
    {
        const int rows = std::max(1, static_cast<int>(std::lround(180.0 / spacing)));
        const double row_height = 180.0 / rows;
        const double y = ((latitude + 90.0) / row_height) - 0.5;
        int row = static_cast<int>(std::floor(y));
        double ty = y - row;
        if (row < 0)
        {
            row = 0;
            ty = 0.0;
        }
        else if (row >= rows - 1)
        {
            row = rows - 1;
            ty = 0.0;
        }
        const auto smooth = [](double t) { return core::math::fenced(t * t) * (3.0 - (2.0 * t)); };
        lattice_point out;
        double total = 0.0;
        for (int k = 0; k < 2; ++k)
        {
            const int r = std::min(row + k, rows - 1);
            const double row_weight = k == 0 ? 1.0 - smooth(ty) : smooth(ty);
            const double row_latitude = -90.0 + ((r + 0.5) * row_height);
            const int columns = std::max(
                1, static_cast<int>(std::lround(360.0 * core::math::cos_deg(row_latitude) / row_height)));
            const double x = ((longitude + 180.0) / (360.0 / columns)) - 0.5;
            const double base = std::floor(x);
            const double sx = smooth(x - base);
            const int column = ((static_cast<int>(base) % columns) + columns) % columns;
            for (int m = 0; m < 2; ++m)
            {
                const auto i = (2 * static_cast<std::size_t>(k)) + static_cast<std::size_t>(m);
                const auto j = static_cast<std::uint64_t>((column + m) % columns);
                out.node.at(i) = core::rng::child_seed_of(seed, (static_cast<std::uint64_t>(r) << 16U) | j);
                out.weight.at(i) = core::math::fenced(row_weight * (m == 0 ? 1.0 - sx : sx));
                total += core::math::fenced(out.weight.at(i) * out.weight.at(i));
            }
        }
        const double norm = std::sqrt(total);
        for (double& w : out.weight) w /= norm;
        return out;
    }

    // A random part of the weather at one place: up to two regional scales
    // plus a local part, so field(index) + local * (a local normal) is a
    // standard normal that nearby places share in part. Without spatial
    // correlation it has no scales and local is 1.
    struct field
    {
        std::array<lattice_point, 2> scale{};
        std::array<double, 2> weight{};
        std::size_t scales{0};
        double local{1.0};

        // The regional part for a day or a month. A node's normals come in
        // pairs, one pair for indices 2i and 2i + 1, kept for the next
        // index (days are read in order).
        [[nodiscard]] double operator()(std::uint64_t index) const
        {
            const std::uint64_t pair = index >> 1U;
            const auto half = static_cast<std::size_t>(index & 1U);
            double total = 0.0;
            for (std::size_t s = 0; s < scales; ++s)
            {
                const auto& p = scale.at(s);
                double v = 0.0;
                for (std::size_t k = 0; k < 4; ++k)
                {
                    if (_pair.at(s).at(k) != pair)
                    {
                        _pair.at(s).at(k) = pair;
                        _normals.at(s).at(k) = hashed_normals_(core::rng::child_seed_of(p.node.at(k), pair));
                    }
                    v += core::math::fenced(p.weight.at(k) * _normals.at(s).at(k).at(half));
                }
                total += core::math::fenced(weight.at(s) * v);
            }
            return total;
        }

      private:
        static constexpr std::uint64_t none = ~std::uint64_t{0}; // no pair index is this
        mutable std::array<std::array<std::uint64_t, 4>, 2> _pair{{{none, none, none, none}, {none, none, none, none}}};
        mutable std::array<std::array<std::array<double, 2>, 4>, 2> _normals{};
    };

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): latitude, longitude as everywhere
    static field field_(const std::array<model::scale, 2>& scales, double latitude, double longitude,
                        const core::rng& seeds, std::string_view name, bool spatial)
    {
        field f;
        if (!spatial) return f;
        const auto root = seeds.split(name);
        double shared = 0.0;
        for (std::size_t s = 0; s < scales.size(); ++s)
        {
            if (scales.at(s).weight <= 0.0) continue;
            const std::size_t i = f.scales++;
            f.scale.at(i) = lattice_(latitude, longitude, scales.at(s).spacing, root.split(s).seed());
            f.weight.at(i) = scales.at(s).weight;
            shared += core::math::fenced(scales.at(s).weight * scales.at(s).weight);
        }
        f.local = std::sqrt(std::max(0.0, 1.0 - shared));
        return f;
    }

    // The fields of one call: one set per seed, evaluated at the cell.
    struct fields
    {
        field rain, amount, mean, range, month_temperature, month_precipitation;
    };

    // The anomalies of one calendar month at one place: (temperature,
    // precipitation, diurnal range), standard normal, from the month's own
    // stream and the regional fields.
    static std::array<double, 3> month_anomaly_(std::uint64_t month_seed, const fields& f, std::int64_t month_index)
    {
        const auto index = static_cast<std::uint64_t>(month_index);
        core::rng r{core::rng::child_seed_of(month_seed, index)};
        const double t = f.month_temperature(index) + core::math::fenced(f.month_temperature.local * core::normal(r, 0.0, 1.0));
        const double p =
            f.month_precipitation(index) + core::math::fenced(f.month_precipitation.local * core::normal(r, 0.0, 1.0));
        constexpr double rp = model::range_precipitation;
        const double own = std::sqrt(1.0 - (rp * rp));
        return {t, p, core::math::fenced(rp * p) + core::math::fenced(own * core::normal(r, 0.0, 1.0))};
    }

    // The month anomalies one call reads, each drawn once. Days are read in
    // order, so the months asked for never go back before the first.
    class month_anomalies
    {
      public:
        month_anomalies(std::uint64_t seed, const fields& f) : _seed{seed}, _fields{&f} {}

        std::array<double, 3> operator()(std::int64_t index)
        {
            if (_values.empty()) _first = index;
            while (_first + static_cast<std::int64_t>(_values.size()) <= index)
            {
                _values.push_back(month_anomaly_(_seed, *_fields, _first + static_cast<std::int64_t>(_values.size())));
            }
            return _values[static_cast<std::size_t>(index - _first)];
        }

      private:
        std::uint64_t _seed;
        const fields* _fields;
        std::int64_t _first{0};
        std::vector<std::array<double, 3>> _values;
    };

    // The months as they were at one cell, for historical requests: per
    // month index (year * 12 + month - 1) the controls of the temperature
    // anomaly and of the log ratios of the wet-day odds and the wet-day
    // amount to the normals'. A control is the inverse of the
    // interpolation's (1, 6, 1) / 8 filter applied to the months around it,
    // over a fixed reach, so it does not depend on the days a call asks for.
    class month_history
    {
      public:
        month_history(const cell& c, std::span<const history_month> months)
            : _wet_days{c.normals.wet_days}, _months{months}
        {
        }

        std::array<double, 3> operator()(std::int64_t index)
        {
            if (_values.empty()) _first = index;
            while (_first + static_cast<std::int64_t>(_values.size()) <= index)
            {
                _values.push_back(control_(_first + static_cast<std::int64_t>(_values.size())));
            }
            return _values[static_cast<std::size_t>(index - _first)];
        }

      private:
        static constexpr int reach = 12; // r^12 is below 1e-9
        static constexpr double root = -0.17157287525380990;  // 2 sqrt(2) - 3
        static constexpr double gain = std::numbers::sqrt2;   // 8 / sqrt(32)

        // The month's own values; zero outside the record or the data.
        [[nodiscard]] std::array<double, 3> month_(std::int64_t index) const
        {
            const std::int64_t first = static_cast<std::int64_t>(history_first_year) * 12;
            if (_months.empty() || index < first || index >= first + history_months) return {0.0, 0.0, 0.0};
            const auto& h = _months[static_cast<std::size_t>(index - first)];
            const auto m = static_cast<std::size_t>(((index % 12) + 12) % 12);
            const auto year = static_cast<int>(index / 12);
            const double normal_length = month_length_(2001, static_cast<unsigned>(m + 1));
            const double length = month_length_(year, static_cast<unsigned>(m + 1));
            const double wet = _wet_days.at(m);
            const double wet_now = std::max(wet + static_cast<double>(h[2]), 0.0);
            const auto log_odds = [](double f)
            {
                const double g = std::clamp(f, 0.005, 0.995);
                return core::math::log(g / (1.0 - g));
            };
            const double odds = log_odds(wet_now / length) - log_odds(std::min(wet, normal_length) / normal_length);
            // The amount per wet day: the precipitation's ratio over the
            // wet days'.
            double amount = core::math::log(std::max(static_cast<double>(h[1]) / 100.0, 0.05));
            if (wet >= 0.1 && wet_now >= 0.1) amount -= core::math::log(wet_now / wet);
            return {static_cast<double>(h[0]), odds, std::clamp(amount, -3.0, 3.0)};
        }

        [[nodiscard]] std::array<double, 3> control_(std::int64_t index) const
        {
            std::array<double, 3> out{};
            double weight = gain;
            for (int k = 0; k <= reach; ++k)
            {
                const auto a = month_(index + k);
                const auto b = k == 0 ? std::array<double, 3>{} : month_(index - k);
                for (std::size_t v = 0; v < 3; ++v) out.at(v) += core::math::fenced(weight * (a.at(v) + b.at(v)));
                weight *= root;
            }
            return out;
        }

        months _wet_days;
        std::span<const history_month> _months;
        std::int64_t _first{0};
        std::vector<std::array<double, 3>> _values;
    };

    // A day's parameters, interpolated between the controls of the months
    // around it, with the months' anomalies interpolated the same way (or,
    // when @p history is set, the months as they were).
    // Where a day falls between the middles of two months: the earlier
    // month's index (year * 12 + month - 1), its calendar month i and the
    // next j (0 to 11), and the share t of the way to the next.
    struct month_position
    {
        std::int64_t index;
        std::size_t i, j;
        double t;
    };

    static month_position month_position_(std::chrono::year_month_day ymd)
    {
        const auto month = static_cast<unsigned>(ymd.month());
        const int year = static_cast<int>(ymd.year());
        const auto length = static_cast<double>(month_length_(year, month));
        // Months after the middle of the year's January (negative before).
        const double pos = static_cast<double>(month - 1) +
                           ((static_cast<double>(static_cast<unsigned>(ymd.day())) - 0.5) / length) - 0.5;
        const double lower = std::floor(pos); // the month before or the month itself
        const std::int64_t index = (static_cast<std::int64_t>(year) * 12) + static_cast<std::int64_t>(lower);
        const auto i = static_cast<std::size_t>(((index % 12) + 12) % 12);
        return {.index = index, .i = i, .j = (i + 1) % 12, .t = pos - lower};
    }

    // The normals' daily mean temperature on a day, without the month's
    // anomaly.
    static double normal_tmean_(const cell& c, std::chrono::sys_days day)
    {
        const auto [index, i, j, t] = month_position_(std::chrono::year_month_day{day});
        return c.tmean.at(i) + core::math::fenced(t * (c.tmean.at(j) - c.tmean.at(i)));
    }

    static day_params params_(const cell& c, std::chrono::year_month_day ymd, month_anomalies& anomalies,
                              month_history* history)
    {
        const auto [index, i, j, t] = month_position_(ymd);
        const auto lerp = [&](const months& v) { return v.at(i) + core::math::fenced(t * (v.at(j) - v.at(i))); };

        if (history != nullptr) return historical_params_(c, *history, index, t, lerp);

        const auto a = anomalies(index);
        const auto b = anomalies(index + 1);
        const double za = a[0] + core::math::fenced(t * (b[0] - a[0]));
        const double zp = a[1] + core::math::fenced(t * (b[1] - a[1]));
        const double zr = a[2] + core::math::fenced(t * (b[2] - a[2]));
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

        const double wet_odds = core::math::fenced(f * odds);

        day_params p{};
        p.tmean = lerp(c.tmean);
        p.dtr = std::max(lerp(c.dtr), 0.0);
        p.wet = std::min(wet_odds / (wet_odds + (1.0 - f)), 0.995);
        p.normal_wet = std::min(f, 0.995);
        p.amount =
            std::max(lerp(c.amount) * factor(sa, (sa * sa) + core::math::fenced(2.0 * sf * sa * (1.0 - f))), 0.1);
        p.humidity = std::clamp(lerp(c.humidity), 1.0, 100.0);
        p.cloud = std::clamp(lerp(c.cloud), 0.0, 1.0);
        p.wind = std::max(lerp(c.wind), 0.0);
        p.anomaly = za;
        p.residual = std::sqrt(1.0 - core::math::fenced(model::temperature_month * model::temperature_month * v));
        p.range_anomaly = zr;
        p.range_residual = std::sqrt(1.0 - core::math::fenced(model::range_month * model::range_month * v));
        chain_(p);
        return p;
    }

    // A historical day: the normals moved by the months as they were, with
    // no drawn month anomaly. The residual keeps the share it has on a
    // normal day, which is the spread within a month.
    template <typename Lerp>
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a month index and a share of it
    static day_params historical_params_(const cell& c, month_history& history, std::int64_t index, double t,
                                         const Lerp& lerp)
    {
        const auto a = history(index);
        const auto b = history(index + 1);
        const auto at = [&](std::size_t k) { return a.at(k) + core::math::fenced(t * (b.at(k) - a.at(k))); };
        const double v = core::math::fenced((1.0 - t) * (1.0 - t)) + core::math::fenced(t * t);
        const double f = std::clamp(lerp(c.wet), 0.0, 1.0);
        const double wet_odds = core::math::fenced(f * core::math::exp(at(1)));

        day_params p{};
        p.tmean = lerp(c.tmean) + at(0);
        p.dtr = std::max(lerp(c.dtr), 0.0);
        p.wet = std::min(wet_odds / (wet_odds + (1.0 - f)), 0.995);
        // The rain classes centre on the month's own wet chance: the
        // history's temperature already has what its wet days did to it.
        p.normal_wet = p.wet;
        p.amount = std::max(lerp(c.amount) * core::math::exp(at(2)), 0.1);
        p.humidity = std::clamp(lerp(c.humidity), 1.0, 100.0);
        p.cloud = std::clamp(lerp(c.cloud), 0.0, 1.0);
        p.wind = std::max(lerp(c.wind), 0.0);
        p.anomaly = 0.0;
        p.residual = std::sqrt(1.0 - core::math::fenced(model::temperature_month * model::temperature_month * v));
        p.range_anomaly = 0.0;
        p.range_residual = std::sqrt(1.0 - core::math::fenced(model::range_month * model::range_month * v));
        chain_(p);
        return p;
    }

    static double persistence_(double f)
    {
        return std::clamp(model::persistence0 + core::math::fenced(model::persistence1 * f), 0.0, 0.9);
    }

    // h(L), the chance of rain after L dry days relative to after one, for
    // L = 1 .. dry_states (index 0 unused).
    static const std::array<double, model::dry_states + 1>& hazard_()
    {
        static const auto table = []
        {
            std::array<double, model::dry_states + 1> h{};
            for (int L = 1; L <= model::dry_states; ++L)
            {
                const double decay = core::math::exp(-static_cast<double>(L - 1) / model::dry_hazard_days);
                h.at(static_cast<std::size_t>(L)) = 1.0 - core::math::fenced(model::dry_hazard_drop * (1.0 - decay));
            }
            return h;
        }();
        return table;
    }

    // The q whose dry spells, ending after L dry days with probability
    // q h(L), last 1 / p01 days on average, as the first-order chain's do;
    // by Newton's method from p01, where the mean is too long (the mean is
    // convex and falling in q, so the steps approach from below).
    static double dry_scale_(double p01)
    {
        using core::math::fenced;
        if (p01 <= 0.0) return 0.0;
        if (p01 >= 1.0) return 1.0;
        const auto& h = hazard_();
        constexpr auto states = static_cast<std::size_t>(model::dry_states);
        const double target = 1.0 / p01;
        double q = p01;
        for (int it = 0; it < 12; ++it)
        {
            // The mean spell D = sum of S(L), S(1) = 1, S(L + 1) = S(L) (1 - q h(L)),
            // whose runs from dry_states on end with q h(dry_states) a day; and dD/dq.
            double s = 1.0;
            double ds = 0.0;
            double mean = 0.0;
            double slope = 0.0;
            for (std::size_t L = 1; L < states; ++L)
            {
                mean += s;
                slope += ds;
                const double stay = 1.0 - fenced(q * h.at(L));
                ds = fenced(ds * stay) - fenced(s * h.at(L));
                s = fenced(s * stay);
            }
            const double end = fenced(q * h.at(states));
            mean += s / end;
            slope += (ds / end) - (s / fenced(q * end));
            const double next = q - ((mean - target) / slope);
            if (!(next > q)) break;
            q = std::min(next, 1.0);
        }
        return q;
    }

    // dry_scale_ tabulated on p01 and read by linear interpolation, which
    // is much cheaper than solving it every day.
    static double dry_scale_table_(double p01)
    {
        constexpr int steps = 1024;
        static const auto table = []
        {
            std::array<double, steps + 1> out{};
            for (int i = 0; i <= steps; ++i) out.at(static_cast<std::size_t>(i)) = dry_scale_(static_cast<double>(i) / steps);
            return out;
        }();
        const double x = std::clamp(p01, 0.0, 1.0) * steps;
        const auto i = std::min(static_cast<std::size_t>(x), static_cast<std::size_t>(steps - 1));
        const double t = x - static_cast<double>(i);
        return table.at(i) + core::math::fenced(t * (table.at(i + 1) - table.at(i)));
    }

    // P(wet | wet) and the dry-spell scale q that keep the wet-day fraction.
    static void chain_(day_params& p)
    {
        const double d = persistence_(p.wet);
        const double p01 = core::math::fenced(p.wet * (1.0 - d));
        p.p11 = p01 + d;
        p.q = dry_scale_table_(p01);
    }

    // The rain state: 0 after a wet day, L after L dry days (at most
    // dry_states). The chance of rain in a state, and the next state.
    static double rain_chance_(int state, const day_params& p)
    {
        return state == 0 ? p.p11 : core::math::fenced(p.q * hazard_().at(static_cast<std::size_t>(state)));
    }

    static int next_state_(int state, bool wet) { return wet ? 0 : std::min(state + 1, model::dry_states); }

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
        double radiation{}; // above the atmosphere, MJ per m2 over the day (FAO-56's Ra)
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
        const double c = fenced(core::math::sin_deg(m) * (1.914602 - fenced(t * (0.004817 + fenced(0.000014 * t))))) +
                         fenced(core::math::sin_deg(2.0 * m) * (0.019993 - fenced(0.000101 * t))) +
                         fenced(core::math::sin_deg(3.0 * m) * 0.000289);
        const double omega = reduce(125.04 - fenced(1934.136 * t));
        const double lambda = l0 + c - 0.00569 - fenced(0.00478 * core::math::sin_deg(omega));
        const double eps0 =
            23.0 + ((26.0 + ((21.448 - fenced(t * (46.815 + fenced(t * (0.00059 - fenced(t * 0.001813)))))) / 60.0)) /
                    60.0);
        const double eps = eps0 + fenced(0.00256 * core::math::cos_deg(omega));
        const double decl = core::math::asin_deg(fenced(core::math::sin_deg(eps) * core::math::sin_deg(lambda)));
        const double ty = core::math::sin_deg(eps / 2.0) / core::math::cos_deg(eps / 2.0);
        const double y = fenced(ty * ty);
        constexpr double deg = 180.0 / std::numbers::pi;
        const double eq_time =
            4.0 * deg *
            (fenced(y * core::math::sin_deg(2.0 * l0)) - fenced(2.0 * e * core::math::sin_deg(m)) +
             fenced(4.0 * e * y * core::math::sin_deg(m) * core::math::cos_deg(2.0 * l0)) - fenced(0.5 * y * y * core::math::sin_deg(4.0 * l0)) -
             fenced(1.25 * e * e * core::math::sin_deg(2.0 * m)));
        const double noon = 720.0 - fenced(4.0 * longitude) - eq_time;

        // Radiation above the atmosphere (FAO-56 eq. 21): the solar constant
        // over the day, by the sun's distance (from its true anomaly) and
        // the geometric sunset hour angle.
        const double ss = fenced(core::math::sin_deg(latitude) * core::math::sin_deg(decl));
        const double cc = fenced(core::math::cos_deg(latitude) * core::math::cos_deg(decl));
        const double radius = (1.000001018 * (1.0 - fenced(e * e))) / (1.0 + fenced(e * core::math::cos_deg(m + c)));
        double set_angle = ss > 0.0 ? 180.0 : 0.0; // at a pole
        if (cc > 1e-12)
        {
            const double x = -ss / cc; // cos(sunset hour angle)
            if (x <= -1.0) set_angle = 180.0;
            else if (x >= 1.0) set_angle = 0.0;
            else set_angle = core::math::acos_deg(x);
        }
        constexpr double rad = std::numbers::pi / 180.0;
        const double extraterrestrial =
            std::max(0.0, (24.0 * 60.0 / std::numbers::pi) * 0.0820 / fenced(radius * radius) *
                              (fenced(set_angle * rad * ss) + fenced(cc * core::math::sin_deg(set_angle))));

        const double numerator = core::math::cos_deg(90.833) - ss;
        // cos(hour angle) = numerator / cc: above 1 the sun never rises,
        // below -1 it never sets.
        if (std::abs(cc) < 1e-12 || numerator >= cc || numerator <= -cc)
        {
            const bool polar_day = (latitude > 0.0) == (decl > 0.0);
            return {.daylight_minutes = polar_day ? 1440.0 : 0.0,
                    .rise = std::nullopt,
                    .set = std::nullopt,
                    .radiation = extraterrestrial};
        }
        const double hour_angle = core::math::acos_deg(numerator / cc);
        return {.daylight_minutes = 8.0 * hour_angle,
                .rise = noon - (4.0 * hour_angle),
                .set = noon + (4.0 * hour_angle),
                .radiation = extraterrestrial};
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
        if (request.units != unit_system::metric && request.units != unit_system::imperial)
        {
            throw std::invalid_argument("weather-generator: units must be metric or imperial");
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
            const double km_per_degree = std::max(1e-6, core::math::cos_deg(std::min(widest, 89.999)) * 111.19);
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

    // What the days of one call share: the cell, its streams, the fields and
    // the months as they were for historical requests.
    struct context
    {
        const cell* c{nullptr};
        std::uint64_t place_seed{0};
        std::uint64_t month_seed{0};
        fields f;
        std::span<const history_month> history; // empty unless historical
        bool historical{false};
    };

    // A day's parameters and draws: the rain's uniform (a standard normal of
    // the rain field turned into a uniform) and, with `temperature`, the
    // temperature innovations.
    static day_draws draw_(const context& x, std::chrono::sys_days day, month_anomalies& anomalies,
                           month_history* history, bool temperature)
    {
        day_draws d;
        d.p = params_(*x.c, std::chrono::year_month_day{day}, anomalies, history);
        const auto index = day_index_(day);
        const core::rng day_rng{core::rng::child_seed_of(x.place_seed, index)};
        auto u = day_rng.split("wet");
        const double z = x.f.rain(index) + core::math::fenced(x.f.rain.local * core::normal(u, 0.0, 1.0));
        d.u_wet = normal_tail_(-z);
        if (temperature)
        {
            auto e = day_rng.split("temperature");
            const double em = x.f.mean(index) + core::math::fenced(x.f.mean.local * core::normal(e, 0.0, 1.0));
            const double er = x.f.range(index) + core::math::fenced(x.f.range.local * core::normal(e, 0.0, 1.0));
            d.e = {em, er};
        }
        return d;
    }

    // The states the rain chain can be in after a day, from the states it
    // could be in before it (bit s for state s).
    static std::uint32_t step_states_(std::uint32_t possible, const day_draws& d)
    {
        std::uint32_t next = 0;
        for (int s = 0; s <= model::dry_states; ++s)
        {
            if (((possible >> static_cast<unsigned>(s)) & 1U) != 0U)
            {
                next |= 1U << static_cast<unsigned>(next_state_(s, d.u_wet < rain_chance_(s, d.p)));
            }
        }
        return next;
    }

    // The rain state on the day before `from`, by coupling from the past:
    // every state is run forward from `back` days before with the same
    // draws, and once they have all met the state no longer depends on
    // anything earlier, so a day's rain is the same in every call that asks
    // for it. If they have not met, the search looks twice as far back; past
    // chain_window_max days (in practice never) it takes the run that
    // started wet. `before` holds the draws of the chain_window days before
    // `from`, oldest first.
    [[nodiscard]] static int state_before_(const context& x, std::chrono::sys_days from,
                                           std::span<const day_draws> before)
    {
        constexpr int states = model::dry_states + 1;
        static_assert(states <= 32, "the states are bits of a 32-bit mask");
        for (int back = model::chain_window;; back *= 2)
        {
            std::optional<month_anomalies> anomalies;
            std::optional<month_history> history;
            if (back > model::chain_window)
            {
                anomalies.emplace(x.month_seed, x.f);
                if (x.historical) history.emplace(*x.c, x.history);
            }
            std::uint32_t possible = (states == 32) ? ~0U : ((1U << static_cast<unsigned>(states)) - 1U);
            int from_wet = 0;
            for (int k = back; k >= 1; --k)
            {
                const auto d = k <= model::chain_window
                                   ? before[before.size() - static_cast<std::size_t>(k)]
                                   : draw_(x, from - std::chrono::days{k}, *anomalies, history ? &*history : nullptr, false);
                possible = step_states_(possible, d);
                from_wet = next_state_(from_wet, d.u_wet < rain_chance_(from_wet, d.p));
            }
            if (std::popcount(possible) == 1) return std::countr_zero(possible);
            if (back >= model::chain_window_max) return from_wet;
        }
    }

    // The rain class of a day from the rain of the day before, the day and
    // the day after: 0 wet after dry, 1 wet after wet, 2 dry after wet, 3 dry
    // before wet, 4 dry between wet days, -1 dry between dry days.
    static int rain_class_(bool before, bool wet, bool after)
    {
        if (wet) return before ? 1 : 0;
        if (before) return after ? 4 : 2;
        return after ? 3 : -1;
    }

    // The day's mean shift (°C) and range factor for its rain class, less
    // their averages over the classes in the normal month, so the monthly
    // normals stay. (Centring on the month's own wet chance would warm every
    // day of a wet month, and so the wet days against the dry ones.)
    static std::pair<double, double> class_shift_(const cell& cl, const day_params& p, int cls)
    {
        using core::math::fenced;
        const double season = (p.tmean - cl.annual_mean) / std::max(cl.annual_range / 2.0, 0.5);
        const std::array<double, 8> x{1.0,
                                      season,
                                      p.normal_wet,
                                      std::abs(cl.normals.latitude) / 90.0,
                                      cl.annual_range / 30.0,
                                      p.dtr / 10.0,
                                      p.cloud,
                                      p.humidity / 100.0};
        const auto share = [&](const std::array<double, 8>& c)
        {
            double s = 0.0;
            for (std::size_t i = 0; i < x.size(); ++i) s += fenced(c.at(i) * x.at(i));
            return s;
        };
        // How often each class comes in the normal month, from the chain: a
        // wet day follows a dry one on f (1 - p11) of days, and a dry day
        // after a wet one is followed by rain with chance q.
        day_params normal = p;
        normal.wet = p.normal_wet;
        chain_(normal);
        const double f = normal.wet;
        const double turn = fenced(f * (1.0 - normal.p11));
        const std::array<double, model::classes> often{turn, fenced(f * normal.p11), fenced(turn * (1.0 - normal.q)),
                                                       fenced(turn * (1.0 - normal.q)), fenced(turn * normal.q)};
        double mean_shift = 0.0;
        double range_factor = 0.0;
        double reference = 1.0; // the share of dry days between dry days
        double shift = 0.0;
        double factor = 1.0;
        for (std::size_t k = 0; k < static_cast<std::size_t>(model::classes); ++k)
        {
            const double m = std::clamp(share(model::class_mean.at(k)), -1.0, 1.0);
            const double r = 1.0 + std::clamp(share(model::class_range.at(k)), -0.8, 0.8);
            mean_shift += fenced(often.at(k) * m);
            range_factor += fenced(often.at(k) * r);
            reference -= often.at(k);
            if (std::cmp_equal(k, cls))
            {
                shift = m;
                factor = r;
            }
        }
        range_factor += std::max(reference, 0.0);
        return {fenced((shift - mean_shift) * p.dtr), factor / range_factor};
    }

    struct temperatures
    {
        double mean;
        double tmax;
        double tmin;
    };

    // The day's mean temperature (the season, the rain class, and a spread
    // split between the month's anomaly and the day's residual, so the total
    // spread is unchanged) and its range (log-normal around the season's
    // range for the rain class, its spread split the same way, so it is
    // never negative and keeps its mean and spread).
    // With `mean_only` (the days before the first asked for, which only the
    // snow on the ground needs), the range is left out.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a day and a class
    static temperatures temperatures_(const std::vector<day_draws>& draws, int k, int cls, const cell& c, double shift,
                                      bool mean_only = false)
    {
        using core::math::fenced;
        // Residuals of the daily mean and range: the sum over the window,
        // oldest first.
        const auto& impulse = impulse_();
        double rm = 0.0;
        double rr = 0.0;
        for (int j = model::window - 1; j >= 0; --j)
        {
            const auto& e = draws[static_cast<std::size_t>(k - j)].e;
            const auto& m = impulse.at(static_cast<std::size_t>(j));
            rm += fenced(m[0] * e[0]) + fenced(m[1] * e[1]);
            rr += fenced(m[2] * e[0]) + fenced(m[3] * e[1]);
        }

        const auto& p = draws[static_cast<std::size_t>(k)].p;
        const auto [class_mean, class_range] = class_shift_(c, p, cls);
        const double sm = spread_(model::spread_mean, c, p);
        const double anomaly = fenced(p.residual * rm) + fenced(model::temperature_month * p.anomaly);
        const double mean = p.tmean + class_mean + fenced(sm * anomaly) + shift;
        if (mean_only) return {.mean = mean, .tmax = mean, .tmin = mean};
        const double base_range = std::max(p.dtr * class_range, 0.1);
        const double cv = spread_(model::spread_range, c, p) / base_range;
        const double s2 = core::math::log(1.0 + fenced(cv * cv));
        const double range_anomaly = fenced(p.range_residual * rr) + fenced(model::range_month * p.range_anomaly);
        const double range = base_range * core::math::exp(fenced(std::sqrt(s2) * range_anomaly) - (0.5 * s2));
        return {.mean = mean, .tmax = mean + (0.5 * range), .tmin = mean - (0.5 * range)};
    }

    // One simulated day: the weather in metric units, and what the hours and
    // the imperial units need before rounding.
    struct day_result
    {
        weather w;
        temperatures t{};
        double amount{0.0};     // mm (0 on a dry day)
        double snow_water{0.0}; // mm of the amount that fell as snow
        double wind{0.0};       // m/s
        double gust{0.0};       // m/s
        double snow_depth{0.0}; // cm
        bool drizzle{false};
        bool heavy_fog{false};
        sun s{};
        std::uint64_t seed{0}; // the day's stream
    };

    // Amount, form, thunder and drizzle of a wet day. The amount is Weibull,
    // drawn by inversion from a normal that is the amount field's and, in
    // part, the depth of the day in the rain (`depth`, the rain's uniform
    // over the chance of rain, uniform on wet days), so neighbouring places'
    // wet days are wet alike.
    // With `snow_only` (the days before the first asked for), only the snow
    // is worked out.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a day index and a share
    static void precipitation_(day_result& d, const day_params& p, const context& x, std::uint64_t index,
                               double depth, const core::rng& day, bool snow_only = false)
    {
        using core::math::fenced;
        weather& w = d.w;
        const auto& t = d.t;
        auto phase = day.split("phase");
        const double snow = logistic_(model::snow0 + fenced(model::snow1 * t.mean));
        if (core::uniform01(phase) >= snow) w.type = precipitation_type::rain;
        else if (t.mean > model::sleet_above) w.type = precipitation_type::sleet;
        else w.type = precipitation_type::snow;
        if (snow_only && w.type == precipitation_type::rain) return;

        auto r = day.split("amount");
        constexpr double k = model::amount_rain;
        const double own = x.f.amount(index) + fenced(x.f.amount.local * core::normal(r, 0.0, 1.0));
        const double z = fenced(std::sqrt(1.0 - (k * k)) * own) + fenced(k * normal_upper_(depth));
        const double shape = std::clamp(model::shape0 + fenced(model::shape1 * core::math::log(p.amount)),
                                        model::shape_min, model::shape_max);
        const double scale = p.amount / weibull_mean_factor_(shape);
        const double tail = normal_tail_(z); // 1 - u
        const double e = tail < 1.0 ? -core::math::log(tail) : 0.0;
        const double amount = std::max(e > 0.0 ? scale * core::math::exp(core::math::log(e) / shape) : 0.0, 0.1);

        d.amount = amount;
        w.precipitation = round_to_(amount, 10.0);
        // A cm of snow per mm of water; sleet is half snow.
        if (w.type == precipitation_type::snow) d.snow_water = amount;
        else if (w.type == precipitation_type::sleet) d.snow_water = amount / 2.0;
        if (w.type == precipitation_type::snow) w.snowfall = w.precipitation;
        else if (w.type == precipitation_type::sleet) w.snowfall = round_to_(amount / 2.0, 10.0);
        if (snow_only) return;

        if (w.type != precipitation_type::snow)
        {
            auto th = day.split("thunder");
            const auto& c = model::thunder;
            const double tz = c[0] + fenced(c[1] * t.tmax) + fenced(c[2] * core::math::log(amount));
            w.thunder = core::uniform01(th) < logistic_(tz);
        }
        if (w.type == precipitation_type::rain && !w.thunder && t.mean > 0.0)
        {
            auto dr = day.split("drizzle");
            const auto& c = model::drizzle;
            const double tx = t.tmax / 10.0;
            const double dz = c[0] + fenced(c[1] * tx) + fenced(c[2] * fenced(tx * tx)) +
                              fenced(c[3] * core::math::log(amount)) + fenced(c[4] * (p.humidity / 100.0));
            d.drizzle = core::uniform01(dr) < logistic_(dz);
        }
    }

    // Cloud cover, humidity, wind, gusts and fog, which follow the wet and
    // dry days.
    static void sky_(day_result& d, const day_params& p, bool wet, double winter, const core::rng& day)
    {
        using core::math::fenced;
        weather& w = d.w;
        const double f = p.wet;
        {
            const double cbar = p.cloud;
            const double cw = cbar + fenced(model::cloud_wet * (1.0 - cbar));
            const double cd = f < 1.0 ? (cbar - fenced(f * cw)) / (1.0 - f) : cbar;
            const double mean = std::clamp(wet ? cw : cd, 0.02, 0.98);
            auto r = day.split("cloud");
            constexpr double kappa = model::cloud_kappa;
            w.cloud_cover = round_to_(core::beta(r, mean * kappa, (1.0 - mean) * kappa), 100.0);
        }
        {
            const double hw = fenced(model::humidity_wet * (1.0 - f));
            const double hd = -fenced(model::humidity_wet * f);
            auto r = day.split("humidity");
            const double h = core::normal(r, p.humidity + (wet ? hw : hd), model::humidity_sd);
            w.humidity = round_to_(std::clamp(h, 5.0, 100.0), 1.0);
        }
        {
            auto r = day.split("wind");
            d.wind = core::weibull(r, model::wind_shape, p.wind / model::wind_gamma);
            w.wind = round_to_(d.wind, 10.0);
            auto g = day.split("gust");
            const auto& c = model::gust;
            const double z = c[0] + fenced(c[1] * core::math::log(std::max(d.wind, 0.1))) +
                             (w.thunder ? c[2] : 0.0) + fenced(model::gust_sd * core::normal(g, 0.0, 1.0));
            d.gust = std::max(d.wind * core::math::exp(z), model::gust_min * d.wind);
            w.gust = round_to_(d.gust, 10.0);
        }
        {
            const std::array<double, 7> x{1.0,
                                          p.humidity / 100.0,
                                          std::clamp((d.t.tmax - d.t.tmin) / std::max(p.dtr, 1.0), 0.0, 3.0),
                                          wet ? 1.0 : 0.0,
                                          wet ? core::math::log(1.0 + d.amount) : 0.0,
                                          winter,
                                          core::math::log(std::clamp(d.wind / std::max(p.wind, 0.1), 0.1, 5.0))};
            const auto chance = [&](const std::array<double, 7>& c)
            {
                double z = 0.0;
                for (std::size_t i = 0; i < x.size(); ++i) z += fenced(c.at(i) * x.at(i));
                return logistic_(z);
            };
            auto r = day.split("fog");
            const double u = core::uniform01(r);
            const double fog = chance(model::fog);
            w.fog = u < fog;
            // Heavy fog is fog too: no more often than fog.
            d.heavy_fog = w.fog && u < chance(model::heavy_fog);
        }
    }

    // The weather of `days` days from request.date: checks the request,
    // simulates (with the day before and after for the hours), adds the
    // hours and converts the units.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): days and seed differ in type and meaning
    [[nodiscard]] std::vector<weather> run_(const weather_request& request, int days, std::uint64_t call_seed) const
    {
        require_data_();
        check_request_(request);
        if (request.historical)
        {
            if (_history.empty())
            {
                throw std::runtime_error("No weather history loaded. Call load_history() first.");
            }
            const auto first = static_cast<int>(request.date.year());
            const auto last = static_cast<int>(
                std::chrono::year_month_day{std::chrono::sys_days{request.date} + std::chrono::days{days - 1}}.year());
            if (first < history_first_year || last > history_last_year)
            {
                throw std::invalid_argument(std::format("weather-generator: historical days must fall in {} to {}",
                                                        history_first_year, history_last_year));
            }
        }
        const int extra = request.hourly ? 1 : 0;
        auto sim = simulate_(request, -extra, days + (2 * extra), call_seed);
        std::vector<weather> out;
        out.reserve(static_cast<std::size_t>(days));
        const bool imperial = request.units == unit_system::imperial;
        for (int k = extra; k < extra + days; ++k)
        {
            const auto at = static_cast<std::size_t>(k);
            auto& d = sim[at];
            if (request.hourly) d.w.hours = hours_(sim[at - 1], d, sim[at + 1], imperial);
            if (imperial) to_imperial_(d);
            out.push_back(std::move(d.w));
        }
        return out;
    }

    // The sunny share of the daylight under a day's cloud cover (rounded to
    // hundredths, so read from a table): (1 - cloud)^sunshine_power.
    static double sunny_share_(double cloud)
    {
        static const auto table = []
        {
            std::array<double, 101> out{};
            for (std::size_t k = 0; k < 100; ++k)
            {
                const double c = static_cast<double>(k) / 100.0;
                out.at(k) = core::math::exp(core::math::fenced(model::sunshine_power * core::math::log(1.0 - c)));
            }
            return out;
        }();
        return table.at(static_cast<std::size_t>(std::clamp(std::lround(cloud * 100.0), 0L, 100L)));
    }

    // The day a snow season starts on or before `day`: the 15th of the
    // cell's warmest month.
    static std::chrono::sys_days season_start_(const cell& c, std::chrono::sys_days day)
    {
        const std::chrono::year_month_day ymd{day};
        std::chrono::sys_days start{ymd.year() / std::chrono::month{c.warmest} / 15};
        if (start > day) start = std::chrono::sys_days{(ymd.year() - std::chrono::years{1}) / std::chrono::month{c.warmest} / 15};
        return start;
    }

    // The call's context at a cell: the place's streams (the same seed gives
    // different weather at different cells and the same weather at points of
    // one cell), the fields (from the call's own stream, so shared by every
    // place) and the months as they were for historical requests.
    [[nodiscard]] context context_(const weather_request& request, const cell& c, const core::rng& call) const
    {
        context x;
        x.c = &c;
        x.place_seed = call.split(c.normals.cell).seed();
        x.month_seed = core::rng{x.place_seed}.split("month").seed();
        const auto seeds = call.split("field");
        const double lat = c.normals.latitude;
        const double lon = c.normals.longitude;
        const bool on = request.spatial;
        x.f.rain = field_(model::field_rain, lat, lon, seeds, "rain", on);
        x.f.amount = field_(model::field_amount, lat, lon, seeds, "amount", on);
        x.f.mean = field_(model::field_mean, lat, lon, seeds, "mean", on);
        x.f.range = field_(model::field_range, lat, lon, seeds, "range", on);
        x.f.month_temperature = field_(model::field_month_temperature, lat, lon, seeds, "month temperature", on);
        x.f.month_precipitation = field_(model::field_month_precipitation, lat, lon, seeds, "month precipitation", on);
        if (request.historical)
        {
            x.historical = true;
            const auto slot = _history_block[history_block_of_(lat, lon)];
            if (slot >= 0) x.history = std::span{_history}.subspan(static_cast<std::size_t>(slot), history_months);
        }
        return x;
    }

    // Snow on the ground is gone by the end of any day whose normal mean
    // temperature (after elevation and warming, `shift`) is snow_free or
    // more, so a day's snow depends only on the days since the last such
    // day; where no day of the year is that warm (`cold`), the snow season
    // starts on the 15th of the warmest month.
    static bool warm_(const cell& c, std::chrono::sys_days day, double shift)
    {
        return normal_tmean_(c, day) + shift >= model::snow_free;
    }

    struct snow_start
    {
        std::chrono::sys_days from; // the first day whose snow is simulated
        bool cold{false};           // no day of the year is warm
    };

    static snow_start snow_start_(const cell& c, std::chrono::sys_days first, double shift)
    {
        for (int back = 1; back <= 366; ++back)
        {
            if (warm_(c, first - std::chrono::days{back}, shift))
            {
                return {.from = first - std::chrono::days{back - 1}, .cold = false};
            }
        }
        return {.from = season_start_(c, first), .cold = true};
    }

    // Snow on the ground: its water (mm) and density (kg/m3).
    struct snowpack
    {
        double water{0.0};
        double density{model::snow_new};

        void clear()
        {
            water = 0.0;
            density = model::snow_new;
        }

        // A day: its snow (mm of water) falls, the pack melts by the day's
        // mean temperature and settles. Returns the depth (cm).
        double day(double snow, double tmean) // NOLINT(bugprone-easily-swappable-parameters): mm and °C
        {
            using core::math::fenced;
            if (snow > 0.0)
            {
                density = (fenced(water * density) + fenced(snow * model::snow_new)) / (water + snow);
                water += snow;
            }
            const double melt = fenced(model::melt_rate * std::max(tmean, 0.0));
            static const double settle_dry = core::math::exp(-model::settle);
            static const double settle_wet = core::math::exp(-model::settle_melting);
            density = model::snow_old - fenced((model::snow_old - density) * (melt > 0.0 ? settle_wet : settle_dry));
            water = std::max(water - melt, 0.0);
            if (water <= 0.0) clear();
            return water / density * 100.0;
        }
    };

    // The rest of a day asked for: the sky, the rounded temperatures and
    // snow, the sun, the sunshine and radiation, and the condition.
    static void finish_day_(day_result& result, const day_params& p, bool wet, std::chrono::sys_days day,
                            const place& where, const core::rng& day_rng)
    {
        using core::math::fenced;
        weather& w = result.w;
        // The season for fog: the cosine of the day of the year, 1 at
        // midwinter.
        static const auto season = []
        {
            std::array<double, 366> out{};
            for (std::size_t d = 0; d < out.size(); ++d)
            {
                out.at(d) = core::math::cos_deg(360.0 * static_cast<double>(d) / 365.25);
            }
            return out;
        }();
        const auto doy = static_cast<std::size_t>(
            (day - std::chrono::sys_days{std::chrono::year_month_day{day}.year() / 1 / 1}).count());
        const double winter = season.at(doy) * (where.latitude < 0.0 ? -1.0 : 1.0);
        sky_(result, p, wet, winter, day_rng);
        w.tmin = round_to_(result.t.tmin, 10.0);
        w.tmean = round_to_(result.t.mean, 10.0);
        w.tmax = round_to_(result.t.tmax, 10.0);
        w.frost = w.tmin < 0.0;
        w.snow_depth = round_to_(result.snow_depth, 10.0);
        const auto sun = sun_(day, where.latitude, where.longitude);
        w.daylight_hours = round_to_(sun.daylight_minutes / 60.0, 100.0);
        // Sunshine: the sunny share of the daylight under the day's cloud;
        // solar radiation from it by Angstrom-Prescott.
        const double sunny = sun.daylight_minutes > 0.0 ? sunny_share_(w.cloud_cover) : 0.0;
        w.sunshine_hours = round_to_((sun.daylight_minutes / 60.0) * sunny, 10.0);
        w.solar_radiation =
            round_to_(fenced((model::angstrom_a + fenced(model::angstrom_b * sunny)) * sun.radiation), 10.0);
        const auto instant = [&](double minutes)
        { return std::chrono::sys_time<std::chrono::minutes>{day} + std::chrono::minutes{std::llround(minutes)}; };
        if (sun.rise) w.sunrise = instant(*sun.rise);
        if (sun.set) w.sunset = instant(*sun.set);
        w.condition = condition_(w, result.heavy_fog, result.drizzle);
        result.s = sun;
    }

    // The days from request.date + offset, each from its own stream. The
    // temperature residual reads `window` days before the first; the rain
    // chain starts from the state coupling from the past gives; snow on the
    // ground is simulated from the last day too warm for it; and the rain
    // of the day after the last decides the last day's rain class.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): offset, days and seed differ in meaning
    [[nodiscard]] std::vector<day_result> simulate_(const weather_request& request, int offset, int days,
                                                    std::uint64_t call_seed) const
    {
        using core::math::fenced;
        const core::rng call{call_seed};
        const place where = place_(request, call);
        const cell& c = _cells[where.cell];
        const double elevation = request.elevation.value_or(c.normals.elevation);
        const double shift = request.warming - fenced(model::lapse_rate * (elevation - c.normals.elevation));
        const context x = context_(request, c, call);
        const std::uint64_t data = request.historical ? core::hash_combine(data_hash(), _history_hash) : data_hash();

        const auto first_day = std::chrono::sys_days{request.date} + std::chrono::days{offset};
        const auto [from, cold] = snow_start_(c, first_day, shift);
        const int lead = static_cast<int>((first_day - from).count()); // days simulated before the first asked for
        // Draws from chain_window days before `from` (the chain's search and
        // the temperature residual's window read them) to the day after the
        // last.
        constexpr int lead_in = std::max(model::window, model::chain_window);
        const auto first = from - std::chrono::days{lead_in};
        const int span = lead_in + lead + days + 1;

        month_anomalies anomalies{x.month_seed, x.f};
        std::optional<month_history> history;
        if (x.historical) history.emplace(c, x.history);
        std::vector<day_draws> draws(static_cast<std::size_t>(span));
        for (int k = 0; k < span; ++k)
        {
            draws[static_cast<std::size_t>(k)] =
                draw_(x, first + std::chrono::days{k}, anomalies, history ? &*history : nullptr, true);
        }

        // Rain from `from` on: wet[k] for the days from `from` - 1, and how
        // deep in the rain each wet day is.
        std::vector<char> wet(static_cast<std::size_t>(span), 0);
        std::vector<double> depth(static_cast<std::size_t>(span), 0.5);
        int state = state_before_(x, from, std::span{draws}.first(static_cast<std::size_t>(lead_in)));
        wet[static_cast<std::size_t>(lead_in - 1)] = state == 0 ? 1 : 0;
        for (int k = lead_in; k < span; ++k)
        {
            const auto& d = draws[static_cast<std::size_t>(k)];
            const double chance = rain_chance_(state, d.p);
            const bool w = d.u_wet < chance;
            wet[static_cast<std::size_t>(k)] = w ? 1 : 0;
            if (w) depth[static_cast<std::size_t>(k)] = d.u_wet / chance;
            state = next_state_(state, w);
        }

        std::vector<day_result> out;
        out.reserve(static_cast<std::size_t>(days));
        snowpack snow;
        for (int k = lead_in; k < span - 1; ++k)
        {
            const auto day = first + std::chrono::days{k};
            const auto index = day_index_(day);
            const std::uint64_t day_seed = core::rng::child_seed_of(x.place_seed, index);
            const core::rng day_rng{day_seed};
            const auto& p = draws[static_cast<std::size_t>(k)].p;
            const auto at = static_cast<std::size_t>(k);
            const bool is_wet = wet[at] != 0;
            const int cls = rain_class_(wet[at - 1] != 0, is_wet, wet[at + 1] != 0);

            const bool before_first = k < lead_in + lead; // only its snow matters
            day_result result;
            result.t = temperatures_(draws, k, cls, c, shift, before_first);
            result.seed = day_seed;
            if (is_wet) precipitation_(result, p, x, index, depth[at], day_rng, before_first);
            // None before a cold place's season, none at the end of a warm day.
            if (cold && day == season_start_(c, day)) snow.clear();
            result.snow_depth = snow.day(result.snow_water, result.t.mean);
            if (warm_(c, day, shift))
            {
                snow.clear();
                result.snow_depth = 0.0;
            }
            if (before_first) continue;

            weather& w = result.w;
            w._signature = {.seed = call_seed, .data = data};
            w.date = std::chrono::year_month_day{day};
            w.cell = c.normals.cell;
            w.latitude = where.latitude;
            w.longitude = where.longitude;
            w.elevation = round_to_(elevation, 1.0);
            w.distance_km = round_to_(where.distance_km, 10.0);
            finish_day_(result, p, is_wet, day, where, day_rng);
            out.push_back(std::move(result));
        }
        return out;
    }

    // -- Units ------------------------------------------------------------

    static double fahrenheit_(double celsius) { return core::math::fenced(celsius * 1.8) + 32.0; }
    static constexpr double mm_per_inch = 25.4;
    static constexpr double metres_per_second_per_mph = 0.44704;

    // The day in imperial units, from the values before rounding.
    static void to_imperial_(day_result& d)
    {
        weather& w = d.w;
        w.units = unit_system::imperial;
        w.tmin = round_to_(fahrenheit_(d.t.tmin), 10.0);
        w.tmean = round_to_(fahrenheit_(d.t.mean), 10.0);
        w.tmax = round_to_(fahrenheit_(d.t.tmax), 10.0);
        w.precipitation = round_to_(d.amount / mm_per_inch, 100.0);
        // A cm of snow per mm of water, sleet half of it, in inches.
        w.snowfall = round_to_(d.snow_water / (mm_per_inch / 10.0), 10.0);
        w.snow_depth = round_to_(d.snow_depth / (mm_per_inch / 10.0), 10.0);
        w.wind = round_to_(d.wind / metres_per_second_per_mph, 10.0);
        w.gust = round_to_(d.gust / metres_per_second_per_mph, 10.0);
    }

    // -- Hours ------------------------------------------------------------

    // Spread `tenths` over the hours by `weights`: each hour gets its share
    // rounded down, then the hours with the largest remainders (the earlier
    // first) one more, so the hours add up to `tenths` exactly; `floor` is
    // given to every hour with a positive weight first.
    static std::array<long long, 24> allocate_(long long tenths, const std::array<double, 24>& weights,
                                               long long floor)
    {
        std::array<long long, 24> out{};
        double total = 0.0;
        long long given = 0;
        for (std::size_t h = 0; h < 24; ++h)
        {
            if (weights.at(h) > 0.0)
            {
                total += weights.at(h);
                out.at(h) = floor;
                given += floor;
            }
        }
        const long long rest = tenths - given;
        if (rest <= 0 || total <= 0.0) return out;
        std::array<double, 24> remainder{};
        for (std::size_t h = 0; h < 24; ++h)
        {
            const double share = static_cast<double>(rest) * weights.at(h) / total;
            const double whole = std::floor(share);
            out.at(h) += static_cast<long long>(whole);
            given += static_cast<long long>(whole);
            remainder.at(h) = weights.at(h) > 0.0 ? share - whole : -1.0;
        }
        for (long long left = tenths - given; left > 0; --left)
        {
            std::size_t best = 0;
            for (std::size_t h = 1; h < 24; ++h)
            {
                if (remainder.at(h) > remainder.at(best)) best = h;
            }
            ++out.at(best);
            remainder.at(best) = -1.0;
        }
        return out;
    }

    // Saturation vapour pressure over water, hPa (Magnus; WMO 2008).
    static double saturation_(double celsius)
    {
        return 6.112 * core::math::exp((17.62 * celsius) / (243.12 + celsius));
    }

    // A day's 24 hours from the day itself and its neighbours (for the
    // night before sunrise and after sunset), from the day's "hours" stream.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one pass per element
    static std::vector<weather_hour> hours_(const day_result& before, const day_result& d,
                                            const day_result& after, bool imperial)
    {
        using core::math::fenced;
        const weather& w = d.w;
        // Local mean solar hours from the local midnight of the date.
        const double solar = 4.0 * w.longitude; // minutes ahead of UTC
        const auto local = [&](double utc_minutes, double day_shift)
        { return ((utc_minutes + solar) / 60.0) + day_shift; };

        // Temperature: the sunrise, warmest hour and sunset of the day, the
        // sunset before and the sunrise after.
        std::array<double, 24> temperature{};
        const double tn = w.tmin;
        const double tx = w.tmax;
        if (d.s.rise && d.s.set)
        {
            const double rise = local(*d.s.rise, 0.0);
            const double set = local(*d.s.set, 0.0);
            const double warmest = std::max(set - model::warmest_before_sunset, (rise + set) / 2.0);
            const double set_before = before.s.set ? local(*before.s.set, -24.0) : set - 24.0;
            const double rise_after = after.s.rise ? local(*after.s.rise, 24.0) : rise + 24.0;
            const double next = after.w.tmin;
            const double dusk = tx - fenced(model::sunset_share * (tx - next));
            const double dusk_before = before.w.tmax - fenced(model::sunset_share * (before.w.tmax - tn));
            for (int h = 0; h < 24; ++h)
            {
                const auto t = static_cast<double>(h);
                double v = 0.0;
                if (t < rise)
                {
                    const double share = std::max(0.0, t - set_before) / std::max(rise - set_before, 1e-6);
                    v = dusk_before + fenced((tn - dusk_before) * std::sqrt(std::min(share, 1.0)));
                }
                else if (t <= warmest)
                {
                    v = tn + fenced((tx - tn) * core::math::sin_deg(90.0 * (t - rise) / std::max(warmest - rise, 1e-6)));
                }
                else if (t <= set)
                {
                    const double angle = 90.0 + (90.0 * (t - warmest) / std::max(set - warmest, 1e-6));
                    v = dusk + fenced((tx - dusk) * core::math::sin_deg(angle));
                }
                else
                {
                    const double share = (t - set) / std::max(rise_after - set, 1e-6);
                    v = dusk + fenced((next - dusk) * std::sqrt(std::min(share, 1.0)));
                }
                temperature.at(static_cast<std::size_t>(h)) = v;
            }
        }
        else
        {
            // Polar day or night: a plain daily wave, coolest at 03:00.
            for (int h = 0; h < 24; ++h)
            {
                const double wave = core::math::cos_deg(15.0 * (static_cast<double>(h) - 15.0));
                temperature.at(static_cast<std::size_t>(h)) = ((tn + tx) / 2.0) + fenced(((tx - tn) / 2.0) * wave);
            }
        }

        // The part of each hour the sun is up.
        std::array<double, 24> daylight{};
        for (int h = 0; h < 24; ++h)
        {
            double up = w.daylight_hours >= 24.0 ? 1.0 : 0.0;
            if (d.s.rise && d.s.set)
            {
                const double from = std::max(static_cast<double>(h), local(*d.s.rise, 0.0));
                const double to = std::min(static_cast<double>(h + 1), local(*d.s.set, 0.0));
                up = std::max(0.0, to - from);
            }
            daylight.at(static_cast<std::size_t>(h)) = up;
        }

        const core::rng stream = core::rng{d.seed}.split("hours");

        // Precipitation: one or two spells of wet hours, the day's amount
        // spread over them by exponential weights.
        std::array<double, 24> weights{};
        std::array<long long, 24> water{};
        std::array<long long, 24> snow{};
        if (w.type != precipitation_type::none)
        {
            auto r = stream.split("precipitation");
            const long long tenths = std::llround(w.precipitation * 10.0);
            const double mean = (model::wet_hours0 + fenced(model::wet_hours1 * core::math::log(1.0 + w.precipitation))) *
                                (w.thunder ? 0.5 : 1.0);
            const long long drawn = std::llround(core::gamma(r, 2.0, mean / 2.0));
            const long long wet_hours = std::clamp<long long>(drawn, 1, std::min<long long>(24, tenths));
            const bool two = core::uniform01(r) < model::two_spells && !w.thunder && wet_hours >= 2;
            const long long first = two ? 1 + static_cast<long long>(core::bounded(r, static_cast<std::uint64_t>(wet_hours - 1)))
                                        : wet_hours;
            const auto spell = [&](long long length, long long start)
            {
                for (long long h = start; h < start + length; ++h)
                {
                    const double u = 1.0 - core::uniform01(r); // (0, 1]
                    weights.at(static_cast<std::size_t>(h)) += -core::math::log(u) + 1e-9;
                }
            };
            long long start = 0;
            if (w.thunder)
            {
                const long long centre = std::llround(core::normal(r, model::thunder_hour, 2.0));
                start = std::clamp<long long>(centre - (first / 2), 0, 24 - first);
            }
            else
            {
                start = core::uniform_int<long long>(r, 0, 24 - first);
            }
            spell(first, start);
            if (two) spell(wet_hours - first, core::uniform_int<long long>(r, 0, 24 - (wet_hours - first)));
            water = allocate_(tenths, weights, 1);
            const long long snow_tenths = std::llround(w.snowfall * 10.0);
            snow = w.type == precipitation_type::snow ? water : allocate_(snow_tenths, weights, 0);
        }

        // Cloud and wind: lag-1 noise around the day's values (the wind's
        // mean then scaled to the day's).
        auto rc = stream.split("cloud");
        auto rw = stream.split("wind");
        const double lag_sd = std::sqrt(1.0 - fenced(model::hour_lag * model::hour_lag));
        double xc = core::normal(rc, 0.0, 1.0);
        double xw = core::normal(rw, 0.0, 1.0);
        const double cover = std::clamp(w.cloud_cover, 0.01, 0.99);
        const double cloud_logit = core::math::log(cover / (1.0 - cover));
        std::array<double, 24> cloud{};
        std::array<double, 24> wind{};
        for (std::size_t h = 0; h < 24; ++h)
        {
            if (h > 0)
            {
                xc = fenced(model::hour_lag * xc) + fenced(lag_sd * core::normal(rc, 0.0, 1.0));
                xw = fenced(model::hour_lag * xw) + fenced(lag_sd * core::normal(rw, 0.0, 1.0));
            }
            cloud.at(h) = logistic_(cloud_logit + fenced(model::cloud_noise * xc));
            if (water.at(h) > 0) cloud.at(h) = std::max(cloud.at(h), model::wet_hour_cloud);
            const double swing =
                1.0 + fenced(model::wind_swing *
                             core::math::cos_deg(15.0 * (static_cast<double>(h) - model::windiest_hour)));
            const double noise = core::math::exp(fenced(model::wind_noise * xw) -
                                                  (0.5 * model::wind_noise * model::wind_noise));
            wind.at(h) = swing * noise;
        }
        // Scaled so the hours' mean is the day's wind.
        double wind_sum = 0.0;
        for (const double x : wind) wind_sum += x;
        for (double& x : wind) x = wind_sum > 0.0 ? d.wind * x * 24.0 / wind_sum : 0.0;

        // Sunshine: the day's hours shared by the sunny daylight of each
        // hour; an hour given more than its daylight keeps its daylight and
        // the rest goes to the others.
        std::array<double, 24> sunny{};
        for (std::size_t h = 0; h < 24; ++h)
        {
            sunny.at(h) = core::math::exp(fenced(model::sunshine_power * core::math::log(1.0 - std::min(cloud.at(h), 0.999))));
        }
        std::array<double, 24> sunshine{};
        {
            std::array<bool, 24> full{};
            double left = w.sunshine_hours;
            for (int pass = 0; pass < 24 && left > 1e-9; ++pass)
            {
                double clear = 0.0;
                double spare = 0.0;
                for (std::size_t h = 0; h < 24; ++h)
                {
                    if (full.at(h)) continue;
                    clear += fenced(daylight.at(h) * sunny.at(h));
                    spare += daylight.at(h);
                }
                if (spare <= 0.0) break;
                bool capped = false;
                for (std::size_t h = 0; h < 24; ++h)
                {
                    if (full.at(h)) continue;
                    const double share = clear > 0.0 ? left * fenced(daylight.at(h) * sunny.at(h)) / clear
                                                     : left * daylight.at(h) / spare;
                    if (share > daylight.at(h))
                    {
                        full.at(h) = true;
                        sunshine.at(h) = daylight.at(h);
                        left -= daylight.at(h);
                        capped = true;
                    }
                }
                if (capped) continue;
                for (std::size_t h = 0; h < 24; ++h)
                {
                    if (full.at(h)) continue;
                    sunshine.at(h) = clear > 0.0 ? left * fenced(daylight.at(h) * sunny.at(h)) / clear
                                                 : left * daylight.at(h) / spare;
                }
                left = 0.0;
            }
        }

        // Fog: the hours around sunrise on a foggy day, all of them without a
        // sunrise.
        std::array<bool, 24> fog{};
        if (w.fog)
        {
            const double rise = d.s.rise ? local(*d.s.rise, 0.0) : -100.0;
            for (std::size_t h = 0; h < 24; ++h)
            {
                const auto t = static_cast<double>(h) + 0.5;
                fog.at(h) = !d.s.rise || (t >= rise - model::fog_before && t < rise + model::fog_after);
            }
        }

        // Humidity: the day's vapour pressure against each hour's saturation.
        const double vapour = (w.humidity / 100.0) * saturation_(w.tmean);

        std::vector<weather_hour> out(24);
        const auto midnight = std::chrono::sys_time<std::chrono::minutes>{std::chrono::sys_days{w.date}} -
                              std::chrono::minutes{std::llround(solar)};
        for (std::size_t h = 0; h < 24; ++h)
        {
            weather_hour& o = out.at(h);
            o.hour = static_cast<int>(h);
            o.time = midnight + std::chrono::minutes{60 * static_cast<long long>(h)};
            const double mm = static_cast<double>(water.at(h)) / 10.0;
            const double cm = static_cast<double>(snow.at(h)) / 10.0;
            if (water.at(h) > 0)
            {
                o.type = w.type;
                o.thunder = w.thunder;
            }
            o.cloud_cover = round_to_(cloud.at(h), 100.0);
            double humidity = std::clamp(100.0 * vapour / saturation_(temperature.at(h)), 5.0, 100.0);
            if (water.at(h) > 0) humidity = std::max(humidity, model::wet_hour_humidity);
            o.humidity = round_to_(humidity, 1.0);
            o.daylight_hours = round_to_(daylight.at(h), 100.0);
            o.sunshine_hours = round_to_(sunshine.at(h), 100.0);
            o.fog = fog.at(h);
            o.condition = hour_condition_(o, mm, d.heavy_fog, d.drizzle);
            if (imperial)
            {
                o.temperature = round_to_(fahrenheit_(temperature.at(h)), 10.0);
                o.precipitation = round_to_(mm / mm_per_inch, 100.0);
                o.snowfall = round_to_(cm / (mm_per_inch / 10.0), 10.0);
                o.wind = round_to_(wind.at(h) / metres_per_second_per_mph, 10.0);
            }
            else
            {
                o.temperature = round_to_(temperature.at(h), 10.0);
                o.precipitation = mm;
                o.snowfall = cm;
                o.wind = round_to_(wind.at(h), 10.0);
            }
        }
        return out;
    }

    // An hour's condition: the day's thresholds for dry hours, rates per
    // hour (mm of water) for wet ones.
    static weather_condition hour_condition_(const weather_hour& o, double mm, bool heavy_fog, bool drizzle)
    {
        switch (o.type)
        {
        case precipitation_type::none:
            if (o.fog && heavy_fog) return weather_condition::fog;
            if (o.cloud_cover < 0.2) return weather_condition::clear;
            if (o.cloud_cover < 0.5) return weather_condition::partly_cloudy;
            if (o.cloud_cover < 0.85) return weather_condition::mostly_cloudy;
            return weather_condition::overcast;
        case precipitation_type::snow:
            if (mm < 0.5) return weather_condition::light_snow;
            if (mm < 2.0) return weather_condition::snow;
            return weather_condition::heavy_snow;
        case precipitation_type::sleet:
            return o.thunder ? weather_condition::thunderstorm : weather_condition::sleet;
        case precipitation_type::rain: break;
        }
        if (o.thunder) return weather_condition::thunderstorm;
        if (mm < 0.5) return drizzle ? weather_condition::drizzle : weather_condition::light_rain;
        if (mm < 4.0) return weather_condition::rain;
        return weather_condition::heavy_rain;
    }

    // The day's condition; a dry day with heavy fog is foggy, a light rain
    // day with drizzle drizzly.
    static weather_condition condition_(const weather& w, bool heavy_fog, bool drizzle)
    {
        switch (w.type)
        {
        case precipitation_type::none:
            if (heavy_fog) return weather_condition::fog;
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
        if (w.precipitation < 2.5) return drizzle ? weather_condition::drizzle : weather_condition::light_rain;
        if (w.precipitation < 10.0) return weather_condition::rain;
        return weather_condition::heavy_rain;
    }
};

} // namespace dasmig

#endif // DASMIG_WEATHERGEN_HPP
