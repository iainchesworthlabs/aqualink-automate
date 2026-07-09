#include <functional>
#include <source_location>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "devices/keypad_device.h"
#include "formatters/screen_data_page_formatter.h"
#include "profiling/factories/profiling_unit_factory.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{
	void KeypadDevice::Slot_Keypad_Ack([[maybe_unused]] const Messages::JandyMessage_Ack& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("KeypadDevice::Slot_Keypad_Ack", std::source_location::current());

		LogDebug(Channel::Devices, "RS Keypad device received a JandyMessage_Ack signal.");

		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void KeypadDevice::Slot_Keypad_Probe([[maybe_unused]] const Messages::JandyMessage_Probe& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("KeypadDevice::Slot_Keypad_Probe", std::source_location::current());

		LogDebug(Channel::Devices, "RS Keypad device received a JandyMessage_Probe signal.");

		Signal_AckMessage(Messages::AckTypes::V2_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void KeypadDevice::Slot_Keypad_Message([[maybe_unused]] const Messages::JandyMessage_Message& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("KeypadDevice::Slot_Keypad_Message", std::source_location::current());

		LogDebug(Channel::Devices, "RS Keypad device received a JandyMessage_Message signal.");

		Signal_AckMessage(Messages::AckTypes::V2_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void KeypadDevice::Slot_Keypad_MessageLong([[maybe_unused]] const Messages::JandyMessage_MessageLong& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("KeypadDevice::Slot_Keypad_MessageLong", std::source_location::current());

		LogDebug(Channel::Devices, "RS Keypad device received a JandyMessage_MessageLong signal.");

		Signal_AckMessage(Messages::AckTypes::V2_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void KeypadDevice::Slot_Keypad_Status([[maybe_unused]] const Messages::JandyMessage_Status& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("KeypadDevice::Slot_Keypad_Status", std::source_location::current());

		LogDebug(Channel::Devices, "RS Keypad device received a JandyMessage_Status signal.");

		Signal_AckMessage(Messages::AckTypes::V2_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

}
// namespace AqualinkAutomate::Devices
