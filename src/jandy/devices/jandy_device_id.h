#pragma once

#include <cstdint>
#include <functional>

namespace AqualinkAutomate::Devices
{

	class JandyDeviceId
	{
	public:
		using value_type = uint8_t;

	public:
		constexpr JandyDeviceId() : m_DeviceId(0) {}
		constexpr JandyDeviceId(value_type device_id) : m_DeviceId(device_id) {}

	public:
		constexpr value_type operator()() const { return m_DeviceId; }

	public:
		constexpr bool operator==(const JandyDeviceId& other) const { return (m_DeviceId == other.m_DeviceId); }
		constexpr bool operator!=(const JandyDeviceId& other) const { return !operator==(other); }

	private:
		value_type m_DeviceId;
	};

}
// namespace AqualinkAutomate::Devices

namespace std
{

	template<>
	struct hash<AqualinkAutomate::Devices::JandyDeviceId>
	{
		size_t operator()(AqualinkAutomate::Devices::JandyDeviceId const& device_id) const noexcept
		{
			return hash<uint8_t>{}(device_id());
		}
	};

}
// namespace std
