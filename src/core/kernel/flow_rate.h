#pragma once

#include "types/units_flow_rate.h"

namespace AqualinkAutomate::Kernel
{

	class FlowRate
	{
	public:
		explicit FlowRate(const Units::gallons_per_minute& rate_in_gpm);
		explicit FlowRate(const Units::liters_per_minute& rate_in_lpm);

		// The sole member is a unit-typed double, so moving one cannot throw. Spelling the
		// copy/move operations out declares that: the implicitly-generated move was not
		// noexcept (the unit quantity's own move is unannotated), which silently forced
		// containers to fall back to copying on reallocation.
		FlowRate(const FlowRate&) = default;
		FlowRate& operator=(const FlowRate&) = default;
		FlowRate(FlowRate&&) noexcept = default;
		FlowRate& operator=(FlowRate&&) noexcept = default;
		~FlowRate() = default;

		Units::gallons_per_minute InGPM() const;
		Units::liters_per_minute InLPM() const;

		static FlowRate ConvertToFlowRateInGPM(double flow_rate_in_gpm);
		static FlowRate ConvertToFlowRateInLPM(double flow_rate_in_lpm);

	private:
		Units::liters_per_minute m_FlowRateInLPM;
	};

}
// namespace AqualinkAutomate::Kernel
