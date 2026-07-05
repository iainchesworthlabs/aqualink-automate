#include <format>
#include <functional>
#include <source_location>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include "devices/chemlink_device.h"
#include "devices/device_status.h"
#include "logging/logging.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{
	ChemlinkDevice::ChemlinkDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id) :
		JandyDevice(device_id),
		Capabilities::Restartable(CHEMLINK_TIMEOUT_DURATION),
		m_ORPMillivolts(std::make_pair(0, std::chrono::system_clock::now())),
		m_PHValue(std::make_pair(0.0, std::chrono::system_clock::now())),
		m_PHFeederRunning(std::make_pair(false, std::chrono::system_clock::now())),
		m_ORPFeederRunning(std::make_pair(false, std::chrono::system_clock::now()))
	{
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::ChemlinkMessage_Response>([this](auto&& PH1) { Slot_Chemlink_Response(std::forward<decltype(PH1)>(PH1)); }, (*device_id)());
	}

	void ChemlinkDevice::WatchdogTimeoutOccurred()
	{
		LogWarning(Channel::Devices, [this]() { return std::format("Chemlink (0x{:02x}): Watchdog timeout occurred - marking device as having lost communications.", DeviceId().Id()()); });

		Status(Devices::DeviceStatus_LostComms{});
	}

	ChemlinkDevice::TimestampedORP ChemlinkDevice::ORPMillivolts() const
	{
		return m_ORPMillivolts;
	}

	ChemlinkDevice::TimestampedPH ChemlinkDevice::PHValue() const
	{
		return m_PHValue;
	}

	ChemlinkDevice::TimestampedFeederState ChemlinkDevice::PHFeederRunning() const
	{
		return m_PHFeederRunning;
	}

	ChemlinkDevice::TimestampedFeederState ChemlinkDevice::ORPFeederRunning() const
	{
		return m_ORPFeederRunning;
	}

	void ChemlinkDevice::Slot_Chemlink_Response(const Messages::ChemlinkMessage_Response& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ChemlinkDevice::Slot_Chemlink_Response", std::source_location::current());

		LogDebug(Channel::Devices, [&msg]() { return std::format("Chemlink device received a ChemlinkMessage_Response signal. DataTag: {}, RawValue: {}", magic_enum::enum_name(msg.DataTag()), msg.RawValue()); });

		switch (msg.DataTag())
		{
		case Messages::ChemlinkDataTag::ORP:
			m_ORPMillivolts = std::make_pair(static_cast<uint16_t>(msg.RawValue() * 10), std::chrono::system_clock::now());
			break;

		case Messages::ChemlinkDataTag::pH:
			m_PHValue = std::make_pair(msg.RawValue() / 10.0, std::chrono::system_clock::now());
			break;

		case Messages::ChemlinkDataTag::pHFeeder:
			m_PHFeederRunning = std::make_pair(msg.RawValue() == 0x00, std::chrono::system_clock::now());
			break;

		case Messages::ChemlinkDataTag::ORPFeeder:
			m_ORPFeederRunning = std::make_pair(msg.RawValue() != 0x00, std::chrono::system_clock::now());
			break;

		default:
			LogDebug(Channel::Devices, [&msg]() { return std::format("Chemlink device received unknown data tag: 0x{:02x}", std::to_underlying(msg.DataTag())); });
			break;
		}

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

}
// namespace AqualinkAutomate::Devices
