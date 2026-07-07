#pragma once

#include "types/units_flow_rate.h"

namespace AqualinkAutomate::Kernel
{

	class FlowRate
	{
	public:
		explicit FlowRate(const Units::gallons_per_minute& rate_in_gpm);
		explicit FlowRate(const Units::liters_per_minute& rate_in_lpm);

		Units::gallons_per_minute InGPM() const;
		Units::liters_per_minute InLPM() const;

		static FlowRate ConvertToFlowRateInGPM(double flow_rate_in_gpm);
		static FlowRate ConvertToFlowRateInLPM(double flow_rate_in_lpm);

	private:
		Units::liters_per_minute m_FlowRateInLPM;
	};

}
// namespace AqualinkAutomate::Kernel
