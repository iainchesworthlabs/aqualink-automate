#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "kernel/body_of_water_ids.h"
#include "kernel/temperature.h"

namespace AqualinkAutomate::Kernel
{

	class BodyOfWater
	{
	public:
		BodyOfWater(BodyOfWaterIds id, std::string_view label);
		BodyOfWater(const BodyOfWater& other) = default;
		BodyOfWater& operator=(const BodyOfWater& other) = default;
		BodyOfWater(BodyOfWater&& other) noexcept = default;
		BodyOfWater& operator=(BodyOfWater&& other) noexcept = default;

		BodyOfWaterIds Id() const;
		const std::string& Label() const;

		bool IsActive() const;
		void IsActive(bool active);

		std::optional<Temperature> CurrentTemp() const;
		void CurrentTemp(const Temperature& temp);

		std::optional<Temperature> TempSetpoint() const;
		void TempSetpoint(const Temperature& setpoint);

	private:
		BodyOfWaterIds m_Id;
		std::string m_Label;
		bool m_IsActive{ false };
		std::optional<Temperature> m_CurrentTemp;
		std::optional<Temperature> m_TempSetpoint;
	};

}
// namespace AqualinkAutomate::Kernel
