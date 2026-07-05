#pragma once

#include <boost/units/quantity.hpp>
#include <boost/units/systems/si/temperature.hpp>
#include <boost/units/systems/temperature/celsius.hpp>
#include <boost/units/systems/temperature/fahrenheit.hpp>

namespace AqualinkAutomate::Kernel
{

	enum class TemperatureUnits
	{
		Celsius,
		Fahrenheit
	};

	class Temperature
	{
	public:
		Temperature(const boost::units::quantity<boost::units::absolute<boost::units::celsius::temperature>>& degrees_celsius);
		Temperature(const boost::units::quantity<boost::units::absolute<boost::units::fahrenheit::temperature>>& degrees_fahrenheit);
		Temperature(const Temperature& other) = default;
		Temperature& operator=(const Temperature& other) = default;
		Temperature(Temperature&& other) noexcept = default;
		Temperature& operator=(Temperature&& other) noexcept = default;

	public:
		// Compares the underlying Kelvin quantity directly (no unit-conversion round-trip), so two
		// temperatures built from the same reading compare equal.
		bool operator==(const Temperature& other) const = default;

		boost::units::quantity<boost::units::absolute<boost::units::celsius::temperature>> InCelsius() const;
		boost::units::quantity<boost::units::absolute<boost::units::fahrenheit::temperature>> InFahrenheit() const;

	public:
		static Temperature ConvertToTemperatureInCelsius(double degrees_celsius);
		static Temperature ConvertToTemperatureInFahrenheit(double degrees_fahrenheit);

	private:
		boost::units::quantity<boost::units::absolute<boost::units::si::temperature>> m_TempInKelvin;
	};

}
// namespace AqualinkAutomate::Kernel
