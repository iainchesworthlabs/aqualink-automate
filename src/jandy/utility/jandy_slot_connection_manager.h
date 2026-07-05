#pragma once

#include <format>
#include <functional>
#include <memory>

#include "devices/jandy_device_id.h"
#include "formatters/jandy_device_formatters.h"
#include "utility/filtered_slot.h"
#include "utility/slot_connection_manager.h"
#include "logging/logging.h"

namespace AqualinkAutomate::Utility
{

	// Jandy-specific extension of the shared SlotConnectionManager.  It adds a
	// filtered registration that delivers a message to the handler only when the
	// message's destination matches a given JandyDeviceId, while inheriting the
	// generic RegisterSlot<T>() and the connection-teardown semantics from the
	// shared core base.
	class JandySlotConnectionManager : public SlotConnectionManager
	{
	public:
		using SlotConnectionManager::SlotConnectionManager;

		template<typename MESSAGE_TYPE>
		bool RegisterSlot_FilterByDeviceId(std::function<void(const MESSAGE_TYPE& msg)> handler, Devices::JandyDeviceId device_id)
		{
			LogDebug(Logging::Channel::Signals, std::format("Registering filtered handler for device id {} for messages of type: {}", device_id, typeid(MESSAGE_TYPE*).name()));

			ConnectionVariant connection = std::make_unique<FilteredSlot_ByDeviceId<MESSAGE_TYPE>>(handler, device_id);
			auto it = m_Connections.insert(m_Connections.cend(), std::move(connection));

			return (m_Connections.end() != it);
		}
	};

}
// namespace AqualinkAutomate::Utility
