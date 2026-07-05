#pragma once

#include <cstdint>
#include <optional>

#include "auxillaries/jandy_auxillary_id.h"
#include "kernel/powercenter.h"

namespace AqualinkAutomate::Auxillaries
{

	// Map a Jandy aux id to the power centre that owns it. The id value ranges encode the
	// centre directly: 0x01-0x07 = A, 0x08-0x0F = B, 0x10-0x17 = C, 0x18-0x1F = D. ExtraAux
	// (0x00) is a dedicated shared relay that belongs to no numbered centre, so it returns
	// nullopt. Driving attribution off the id (i.e. the scraped label) rather than a count
	// is what makes it correct for dual-equipment models and DIP-repurposed relays.
	inline std::optional<Kernel::PowerCenterIds> PowerCenterForAuxId(JandyAuxillaryIds id)
	{
		if (const auto value = static_cast<uint8_t>(id); value == static_cast<uint8_t>(JandyAuxillaryIds::ExtraAux))
		{
			return std::nullopt;
		}
		else if ((value >= 0x01) && (value <= 0x07))
		{
			return Kernel::PowerCenterIds::A;
		}
		else if ((value >= 0x08) && (value <= 0x0F))
		{
			return Kernel::PowerCenterIds::B;
		}
		else if ((value >= 0x10) && (value <= 0x17))
		{
			return Kernel::PowerCenterIds::C;
		}
		else if ((value >= 0x18) && (value <= 0x1F))
		{
			return Kernel::PowerCenterIds::D;
		}

		return std::nullopt;
	}

}
// namespace AqualinkAutomate::Auxillaries
