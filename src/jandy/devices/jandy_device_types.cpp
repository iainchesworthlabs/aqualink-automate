#include <algorithm>

#include "devices/jandy_device_types.h"

namespace AqualinkAutomate::Devices
{

	JandyDeviceType::JandyDeviceType() :
		m_DeviceId(0xFF)
	{
	}

	JandyDeviceType::JandyDeviceType(DeviceId device_id) :
		m_DeviceClass(DeviceClasses::Unknown),
		m_DeviceId(device_id)
	{
		for (const auto& device_class : m_KnownDeviceIdsList)
		{
			const auto& ids = device_class.second;
			if (std::ranges::find(ids, device_id) != ids.cend())
			{
				m_DeviceClass = device_class.first;
				// A given raw id maps to exactly one class; stop once matched.
				break;
			}
		}
	}

	bool JandyDeviceType::operator==(const JandyDeviceType& other) const
	{
		return m_DeviceId == other.m_DeviceId;
	}

	bool JandyDeviceType::operator!=(const JandyDeviceType& other) const
	{
		return !(*this == other);
	}

	bool JandyDeviceType::Equals(const Interfaces::IDeviceIdentifier& other) const
	{
		if (const auto ptr = dynamic_cast<const JandyDeviceType*>(&other); nullptr != ptr)
		{
			return operator==(*ptr);
		}

		// Could not convert to a JandyDeviceType; objects _must_ be different.
		return false;
	}

	JandyDeviceType::DeviceId JandyDeviceType::operator()() const
	{
		return m_DeviceId;
	}

	DeviceClasses JandyDeviceType::Class() const
	{
		return m_DeviceClass;
	}

	JandyDeviceType::DeviceId JandyDeviceType::Id() const
	{
		return m_DeviceId;
	}

	std::vector<std::uint8_t> JandyDeviceType::InstanceAddressesForClass(DeviceClasses device_class)
	{
		for (const auto& [known_class, ids] : m_KnownDeviceIdsList)
		{
			if (known_class == device_class)
			{
				std::vector<std::uint8_t> addresses;
				addresses.reserve(ids.size());
				for (const auto& id : ids)
				{
					addresses.push_back(id());   // JandyDeviceId -> raw uint8_t
				}
				return addresses;
			}
		}
		return {};
	}

}
// namespace AqualinkAutomate::Devices
