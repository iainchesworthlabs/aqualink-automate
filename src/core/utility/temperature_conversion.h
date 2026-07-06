#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "kernel/temperature.h"

namespace AqualinkAutomate::Utility
{

	/// @brief Convert a Celsius temperature into a Fahrenheit temperature.
	///
	/// Centralises the C->F formula that was previously open-coded with bare magic
	/// numbers in both the HTTP setpoint route and the MQTT integration.
	inline constexpr double CelsiusToFahrenheit(double celsius) noexcept
	{
		return (celsius * 9.0 / 5.0) + 32.0;
	}

	/// @brief Convert a Celsius setpoint into the byte value placed on the wire.
	///
	/// The Jandy/Zodiac controllers carry setpoints as a single unsigned byte in the
	/// system's configured display unit. This helper:
	///   1. converts the incoming Celsius value into the system unit (Celsius is sent
	///      as-is; otherwise it is converted to Fahrenheit), then
	///   2. rounds to the nearest whole degree, then
	///   3. clamps to the valid uint8_t domain [0, 255] BEFORE the narrowing cast so an
	///      out-of-range double can never trigger undefined behaviour.
	///
	/// Callers are still expected to range-check the human-meaningful Celsius input
	/// (rejecting non-finite / absurd values) before reaching this conversion; the clamp
	/// here is defence-in-depth, not a substitute for input validation.
	///
	/// @param celsius        The setpoint in degrees Celsius.
	/// @param system_units   The unit the controller expects on the wire.
	/// @returns the clamped, rounded wire byte.
	// NB: not constexpr — std::round is not a constant expression on MSVC's STL (C3615);
	// this conversion only runs at runtime when building a wire setpoint.
	inline std::uint8_t CelsiusToWireSetpoint(double celsius, Kernel::TemperatureUnits system_units) noexcept
	{
		const double wire_value = (Kernel::TemperatureUnits::Celsius == system_units)
			? std::round(celsius)
			: std::round(CelsiusToFahrenheit(celsius));

		return static_cast<std::uint8_t>(std::clamp(wire_value, 0.0, 255.0));
	}

}
// namespace AqualinkAutomate::Utility
