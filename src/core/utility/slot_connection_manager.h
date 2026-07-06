#pragma once

#include <format>
#include <functional>
#include <memory>
#include <typeinfo>
#include <variant>
#include <vector>

#include <boost/signals2.hpp>

#include "logging/logging.h"

namespace AqualinkAutomate::Utility
{

	// Empty base for type-erased "filtered" slot wrappers.  Protocol-specific
	// libraries (e.g. Jandy's FilteredSlot_ByDeviceId) derive from this so the
	// connection manager can own a heterogeneous mix of plain connections and
	// filtered-slot objects without knowing their concrete type.
	class FilteredSlotBase
	{
	public:
		virtual ~FilteredSlotBase() = default;
	};

	// Protocol-agnostic owner of message-signal slot connections.
	//
	// A device mixes this in (directly, or via a protocol-specific subclass that
	// adds filtered registrations) and uses RegisterSlot<T>() to connect a
	// handler to a message type's static receive-signal.  On destruction every
	// owned connection is disconnected, so a device's slots never outlive it.
	//
	// The connection storage is exposed to subclasses (protected) so a
	// protocol-specific manager can append its own filtered-slot wrappers while
	// reusing the same teardown semantics.
	class SlotConnectionManager
	{
	protected:
		using ConnectionVariant = std::variant<boost::signals2::connection, std::shared_ptr<FilteredSlotBase>>;

	public:
		SlotConnectionManager();
		virtual ~SlotConnectionManager();

		// This type is a unique RAII owner of live signal-slot connections: its
		// destructor disconnects every owned connection.  Copying would duplicate
		// the connection handles so a copy's destruction would tear down the
		// original's live connections; moving is likewise not needed because the
		// manager is embedded by value in a (non-movable) device.  Delete both to
		// keep the special-member set regular and prevent an unsafe duplicate.
		SlotConnectionManager(const SlotConnectionManager&) = delete;
		SlotConnectionManager& operator=(const SlotConnectionManager&) = delete;
		SlotConnectionManager(SlotConnectionManager&&) = delete;
		SlotConnectionManager& operator=(SlotConnectionManager&&) = delete;

		// Connect handler to MESSAGE_TYPE's static receive-signal.  Every message
		// of that type (regardless of source/destination) is delivered.
		template<typename MESSAGE_TYPE>
		bool RegisterSlot(std::function<void(const MESSAGE_TYPE& msg)> handler)
		{
			LogDebug(Logging::Channel::Signals, std::format("Registering general handler for messages of type: {}", typeid(MESSAGE_TYPE*).name()));

			ConnectionVariant connection = MESSAGE_TYPE::GetSignal()->connect(handler);
			auto it = m_Connections.insert(m_Connections.cend(), connection);

			return (m_Connections.end() != it);
		}

	protected:
		std::vector<ConnectionVariant> m_Connections;
	};

}
// namespace AqualinkAutomate::Utility
