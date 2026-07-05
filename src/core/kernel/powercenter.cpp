#include <utility>

#include "kernel/powercenter.h"

namespace AqualinkAutomate::Kernel
{

	void PowerCenters::Assign(PowerCenterIds pc, const std::string& aux_label)
	{
		m_Centers[std::to_underlying(pc)].push_back(aux_label);
	}

	const std::vector<std::string>& PowerCenters::AuxillariesIn(PowerCenterIds pc) const
	{
		return m_Centers[std::to_underlying(pc)];
	}

	bool PowerCenters::IsInstalled(PowerCenterIds pc) const
	{
		return !m_Centers[std::to_underlying(pc)].empty();
	}

	uint8_t PowerCenters::InstalledCount() const
	{
		uint8_t count = 0;
		for (const auto& centre : m_Centers)
		{
			if (!centre.empty())
			{
				++count;
			}
		}
		return count;
	}

	uint8_t PowerCenters::TotalAuxillaries() const
	{
		std::size_t total = 0;
		for (const auto& centre : m_Centers)
		{
			total += centre.size();
		}
		return static_cast<uint8_t>(total);
	}

}
// namespace AqualinkAutomate::Kernel
