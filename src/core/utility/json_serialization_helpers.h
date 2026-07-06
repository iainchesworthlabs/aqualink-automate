#pragma once

#include <chrono>
#include <cmath>
#include <optional>

#include <nlohmann/json.hpp>

#include "kernel/temperature.h"
#include "utility/latency_percentile_tracker.h"

namespace AqualinkAutomate::Utility
{

	/// Convert nanoseconds to microseconds for JSON output.
	inline constexpr double NanosToMicros(std::chrono::nanoseconds ns) noexcept
	{
		return static_cast<double>(ns.count()) / 1000.0;
	}

	/// Serialize a latency percentile snapshot to JSON.
	///
	/// `p1..p99`, `min`/`max`, `mean` and `sample_count` describe the recent sliding
	/// window; `alltime_min_us`/`alltime_max_us` are the since-restart extremes.
	inline nlohmann::json SerializeLatencySnapshot(const LatencyPercentileTracker<>::PercentileSnapshot& snapshot)
	{
		return {
			{"p1_us", NanosToMicros(snapshot.p1)},
			{"p50_us", NanosToMicros(snapshot.p50)},
			{"p95_us", NanosToMicros(snapshot.p95)},
			{"p99_us", NanosToMicros(snapshot.p99)},
			{"min_us", NanosToMicros(snapshot.min)},
			{"max_us", NanosToMicros(snapshot.max)},
			{"alltime_min_us", NanosToMicros(snapshot.alltime_min)},
			{"alltime_max_us", NanosToMicros(snapshot.alltime_max)},
			{"mean_us", NanosToMicros(snapshot.mean)},
			{"sample_count", snapshot.sample_count}
		};
	}

	/// Serialize a latency tracker to JSON: the sliding-window snapshot (percentiles,
	/// min/max, mean, sample_count) plus the window span (`window_secs`) and the
	/// cumulative since-restart aggregate (`alltime_min_us`/`alltime_max_us` from the
	/// snapshot, plus `cumulative_mean_us`). This lets the diagnostics UI honestly
	/// label its "last N minutes" window and its "since restart" figures.
	inline nlohmann::json SerializeLatency(const LatencyPercentileTracker<>& tracker)
	{
		nlohmann::json j = SerializeLatencySnapshot(tracker.GetSnapshot());
		j["window_secs"] = static_cast<std::int64_t>(tracker.WindowDuration().count());
		j["cumulative_mean_us"] = NanosToMicros(tracker.CumulativeMean());
		return j;
	}

	/// Round a double to `places` decimal places for JSON / display output.
	///
	/// nlohmann serializes a number_float as the shortest string that round-trips to that exact
	/// double, so any value carrying sub-ULP noise from a unit conversion, a float->double
	/// promotion, or fractional arithmetic leaks verbatim into the payload (e.g. a pH float32 of
	/// 7.1 promotes to 7.099999904632568). Snapping to the quantity's real resolution before
	/// emission collapses that noise. `factor` is built by repeated *10 so it is an exact power
	/// of ten (1, 10, 100, ...), never the fuzz std::pow could introduce.
	inline double RoundToDecimalPlaces(double value, int places) noexcept
	{
		double factor = 1.0;
		for (int i = 0; i < places; ++i)
		{
			factor *= 10.0;
		}
		return std::round(value * factor) / factor;
	}

	/// Round a temperature reading to one decimal place for JSON / display output.
	///
	/// Temperatures are stored internally in Kelvin (boost::units) and reconstructed on every
	/// read, so a whole-degree wire reading no longer round-trips exactly: 38.0C surfaces as
	/// 100.39999999999992F and an echoed 72F reads back as 72.00000000000006F. The controller
	/// only ever reports whole degrees (deci-Celsius at most), so 0.1 is the real resolution;
	/// rounding to the nearest tenth collapses the noise and makes the output match the documented
	/// swagger examples (e.g. spa_setpoint fahrenheit: 100.4, not 100.399...).
	inline double RoundTemperatureForDisplay(double value) noexcept
	{
		return RoundToDecimalPlaces(value, 1);
	}

	/// Serialize a Temperature to JSON with celsius and fahrenheit fields.
	inline nlohmann::json SerializeTemperature(const Kernel::Temperature& temp)
	{
		return {
			{"celsius", RoundTemperatureForDisplay(temp.InCelsius().value())},
			{"fahrenheit", RoundTemperatureForDisplay(temp.InFahrenheit().value())}
		};
	}

	/// Serialize an optional Temperature. Returns null JSON when not measured.
	inline nlohmann::json SerializeTemperature(const std::optional<Kernel::Temperature>& temp)
	{
		if (!temp.has_value())
		{
			return nullptr;
		}

		return SerializeTemperature(*temp);
	}

	/// Serialize an optional Temperature with freshness metadata. Returns null JSON when not
	/// measured; otherwise emits celsius/fahrenheit plus `last_updated` (epoch seconds, the
	/// codebase's time convention - null if never stamped) and a `stale` flag. The controller only
	/// reports temperatures while the pump runs (and only for the active body on a combo system),
	/// so a value can outlive its freshness; `stale` lets consumers reflect that without dropping
	/// the last-known reading.
	inline nlohmann::json SerializeTemperature(
		const std::optional<Kernel::Temperature>& temp,
		const std::optional<std::chrono::system_clock::time_point>& last_updated,
		bool stale)
	{
		if (!temp.has_value())
		{
			return nullptr;
		}

		nlohmann::json j = SerializeTemperature(*temp);

		if (last_updated.has_value())
		{
			j["last_updated"] = std::chrono::duration_cast<std::chrono::seconds>(last_updated->time_since_epoch()).count();
		}
		else
		{
			j["last_updated"] = nullptr;
		}

		j["stale"] = stale;

		return j;
	}

}
// namespace AqualinkAutomate::Utility
