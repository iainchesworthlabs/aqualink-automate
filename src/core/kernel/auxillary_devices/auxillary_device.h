#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

#include <boost/uuid/uuid.hpp>

#include "kernel/auxillary_devices/auxillary_health_states.h"
#include "kernel/auxillary_traits/auxillary_traits.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

namespace AqualinkAutomate::Kernel
{
	
	class AuxillaryDevice
	{
	public:
		AuxillaryDevice();
		explicit AuxillaryDevice(std::string_view label);
		// Reconstruct with a specific id (used to restore stable ids from the
		// equipment cache so device UUIDs survive a restart).
		explicit AuxillaryDevice(boost::uuids::uuid id);

		boost::uuids::uuid Id() const;

		bool operator==(const AuxillaryDevice& other) const;
		bool operator==(const boost::uuids::uuid id) const;
		bool operator==(const std::string& id) const;

		bool operator!=(const AuxillaryDevice& other) const;

		Traits AuxillaryTraits{};

	public:
		AuxillaryHealthStates Health() const;

	private:
		boost::uuids::uuid m_Id;
	};

}
// namespace AqualinkAutomate::Kernel
