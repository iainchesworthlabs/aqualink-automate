#include <cmath>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "devices/pda_device.h"
#include "formatters/screen_data_page_formatter.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	void PDADevice::Slot_PDA_Ack(const Messages::JandyMessage_Ack& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a JandyMessage_Ack signal.");

		KeyCommands key_press = msg.Command<KeyCommands>([](uint8_t command_id)
			{
				return magic_enum::enum_cast<KeyCommands>(command_id).value_or(KeyCommands::Unknown);
			}
		);

		LogDebug(Channel::Devices, std::format("Decoded PDA-specific JandyMessage_Ack payload: key press -> {} (raw: 0x{:02x})", magic_enum::enum_name(key_press), msg.Command()));

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_Clear([[maybe_unused]] const Messages::PDAMessage_Clear& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a JandyMessage_Clear signal.");

		ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evClear());
		Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_Highlight(const Messages::PDAMessage_Highlight& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a PDAMessage_Highlight signal.");

		ScreenMode(Capabilities::ScreenModes::Updating);
		ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evHighlight(msg.LineId()));

		Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_HighlightChars(const Messages::PDAMessage_HighlightChars& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a PDAMessage_HighlightChars signal.");

		ScreenMode(Capabilities::ScreenModes::Updating);
		ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evHighlightChars(msg.LineId(), msg.StartIndex(), msg.StopIndex()));

		Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_MessageLong(const Messages::JandyMessage_MessageLong& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a JandyMessage_MessageLong signal.");

		static const uint8_t PDA_MESSAGE_LONG_AIRWATER{ 0x82 };
		static const uint8_t PDA_MESSAGE_LONG_TEMPERATURE{ 0x40 };
		
		if (PDA_PAGE_LINES <= msg.LineId())
		{
			// PDA_MESSAGE_LONG_AIRWATER, PDA_MESSAGE_LONG_TEMPERATURE, and other unsupported line types are handled the same way
			LogDebug(Channel::Devices, std::format("PDA device received a MessageLong update for an unsupported line; line id -> {}, content -> '{}'", msg.LineId(), msg.Line()));
			Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
		}
		else
		{
			ScreenMode(Capabilities::ScreenModes::Updating);
			ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evUpdate(msg.LineId(), msg.Line()));
			ProcessScreenUpdates();

			Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
			ProcessControllerUpdates();
		}

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_Probe([[maybe_unused]] const Messages::JandyMessage_Probe& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a JandyMessage_Probe signal.");

		Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_Status([[maybe_unused]] const Messages::JandyMessage_Status& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a JandyMessage_Status signal.");

		if (Capabilities::ScreenModes::Updating == ScreenMode())
		{
			LogInfo(Channel::Devices, std::format("\n{}", DisplayedPage()));

			// The series of JandyMessage_MessageLong messages has finished.
			ScreenMode(Capabilities::ScreenModes::UpdateComplete);
		}

		Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
		ProcessScreenUpdates();
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_ShiftLines(const Messages::PDAMessage_ShiftLines& msg)
	{
		LogDebug(Channel::Devices, "PDA device received a PDAMessage_ShiftLines signal.");

		auto direction = (0 > msg.LineShift()) ? Utility::ScreenDataPage::ShiftDirections::Up : Utility::ScreenDataPage::ShiftDirections::Down;
		auto lines_to_shift = std::abs(msg.LineShift());

		ScreenMode(Capabilities::ScreenModes::Updating);
		ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evShift(direction, msg.FirstLineId(), msg.LastLineId(), lines_to_shift));
		ProcessScreenUpdates();

		Signal_AckMessage(Messages::AckTypes::V1_Normal, KeyCommands::NoKeyCommand);
		ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		Restartable::Kick();
	}

	void PDADevice::Slot_PDA_Unknown_PDA_1B(const Messages::JandyMessage_Unknown& msg)
	{
		LogDebug(Channel::Devices, std::format("PDA device received a JandyMessage_Unknown signal: type -> 0x{:02x}", msg.RawId()));
	}

}
// namespace AqualinkAutomate::Devices
