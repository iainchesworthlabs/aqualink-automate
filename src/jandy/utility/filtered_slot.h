#pragma once

#include <cstdint>
#include <format>
#include <functional>

#include <boost/signals2.hpp>

#include "devices/jandy_device_id.h"
#include "formatters/jandy_device_formatters.h"
#include "utility/slot_connection_manager.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Utility
{

	// FilteredSlotBase now lives in the shared core slot-connection manager
	// (utility/slot_connection_manager.h).  FilteredSlot_ByDeviceId is the
	// Jandy-specific filtered slot that matches on a JandyDeviceId destination.

	template<typename MESSAGE_TYPE>
	class FilteredSlot_ByDeviceId : public FilteredSlotBase
	{
	public:
		using HandlerType = std::function<void(const MESSAGE_TYPE&)>;

	public:
		FilteredSlot_ByDeviceId(HandlerType handler, Devices::JandyDeviceId device_id) :
			m_Handler(handler),
			m_DeviceId(device_id)
		{
			auto filtered_slot_handler = [this](const MESSAGE_TYPE& msg) -> void
			{
				if (m_DeviceId != msg.Destination().Id())
				{
					LogTrace(Channel::Signals, std::format("Filtering message (type: {}) as device's id ({}) does not match destination ({})", typeid(MESSAGE_TYPE*).name(), m_DeviceId, msg.Destination().Id()));
				}
				else
				{
					LogDebug(Channel::Signals, std::format("Handling message (type: {}) as device's id ({}) matches destination ({})", typeid(MESSAGE_TYPE*).name(), m_DeviceId, msg.Destination().Id()));
					m_Handler(msg);
				}
			};

			LogDebug(Channel::Signals, std::format("Connecting slot handler for message  (type: {}) for device id ({}) ", typeid(MESSAGE_TYPE*).name(), m_DeviceId));
			m_Connection = MESSAGE_TYPE::GetSignal()->connect(filtered_slot_handler);
		}

		~FilteredSlot_ByDeviceId()
		{
			// The diagnostic log formats a string (std::format can throw); a destructor
			// must not let that escape (cpp:S1048). The disconnect itself is kept
			// outside the guard so the slot is always torn down.
			try
			{
				LogDebug(Channel::Signals, std::format("Disconnecting slot handler for message  (type: {}) for device id ({}) ", typeid(MESSAGE_TYPE*).name(), m_DeviceId));
			}
			catch (...)   // NOSONAR(cpp:S1181) — destructor boundary: nothing may escape a dtor (S1048)
			{
				// Swallow — the diagnostic log itself must not escape the destructor.
			}

			m_Connection.disconnect();
		}

	private:
		HandlerType m_Handler;
		Devices::JandyDeviceId m_DeviceId;
		boost::signals2::connection m_Connection;
	};

}
// namespace AqualinkAutomate::Utility
