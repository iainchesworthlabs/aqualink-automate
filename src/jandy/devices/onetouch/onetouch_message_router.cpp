#include <cmath>
#include <cstdint>
#include <format>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "devices/onetouch_device.h"
#include "devices/onetouch/onetouch_message_router.h"
#include "formatters/screen_data_page_formatter.h"
#include "messages/jandy_message_ack.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	namespace
	{
		// PDAMessage_Highlight uses 0xFF as a "clear all highlights" sentinel rather than
		// addressing a real (0-based) line on the page. It must NOT be treated as an
		// out-of-range line id.
		constexpr uint8_t HIGHLIGHT_CLEAR_ALL{ 0xFF };
	}
	// namespace

	void OneTouchMessageRouter::Slot_OneTouch_Ack(const Messages::JandyMessage_Ack& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_Ack", std::source_location::current(), Profiling::UnitColours::Red);

		LogTrace(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Received JandyMessage_Ack: raw_command=0x{:02x}", m_Device.DeviceId(), msg.Command()); });

		OneTouchDevice::KeyCommands key_press = msg.Command<OneTouchDevice::KeyCommands>([](uint8_t command_id)
			{
				return magic_enum::enum_cast<OneTouchDevice::KeyCommands>(command_id).value_or(OneTouchDevice::KeyCommands::Unknown);
			}
		);

		if (key_press == OneTouchDevice::KeyCommands::Unknown)
		{
			LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Unknown key command received in ACK: 0x{:02x}", m_Device.DeviceId(), msg.Command()); });
		}
		else
		{
			LogDebug(Channel::Devices, [this, &key_press, &msg]() { return std::format("OneTouch ({}): Decoded ACK key press: {} (0x{:02x})", m_Device.DeviceId(), magic_enum::enum_name(key_press), msg.Command()); });
		}

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (ACK)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_MessageLong(const Messages::JandyMessage_MessageLong& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_MessageLong", std::source_location::current(), Profiling::UnitColours::Red);

		LogTrace(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Received JandyMessage_MessageLong: line_id={}, content_length={}", m_Device.DeviceId(), msg.LineId(), msg.Line().length()); });

		if (OneTouchDevice::ONETOUCH_PAGE_LINES <= msg.LineId())
		{
			LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): MessageLong for unsupported line: line_id={} (max={}), content='{}'", m_Device.DeviceId(), msg.LineId(), OneTouchDevice::ONETOUCH_PAGE_LINES - 1, msg.Line()); });
		}
		else
		{
			LogDebug(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Processing MessageLong: line_id={}, content='{}'", m_Device.DeviceId(), msg.LineId(), msg.Line()); });

			m_Device.ScreenMode(Capabilities::ScreenModes::Updating);
			m_Device.ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evUpdate(msg.LineId(), msg.Line()));
			m_Device.ProcessScreenUpdates();

			m_Device.ProcessControllerUpdates();
		}

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (MessageLong)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_Probe(const Messages::JandyMessage_Probe& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_Probe", std::source_location::current(), Profiling::UnitColours::Red);

		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Received JandyMessage_Probe", m_Device.DeviceId()); });

		m_Device.ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (Probe)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_Status(const Messages::JandyMessage_Status& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_Status", std::source_location::current(), Profiling::UnitColours::Red);

		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Received JandyMessage_Status", m_Device.DeviceId()); });

		if (Capabilities::ScreenModes::Updating == m_Device.ScreenMode())
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Screen update complete - displaying page", m_Device.DeviceId()); });
			// m_Device.DisplayedPage() renders the whole 12-line screen; only build it when the
			// Debug level is actually enabled by deferring it inside the lazy lambda.
			LogDebug(Channel::Devices, [this]() { return std::format("\n{}", m_Device.DisplayedPage()); });

			// The series of JandyMessage_MessageLong messages has finished.
			m_Device.ScreenMode(Capabilities::ScreenModes::UpdateComplete);
		}

		m_Device.ProcessScreenUpdates();

		// Notify navigator and current task that a Status message was received.
		// This must happen BEFORE ProcessControllerUpdates so the navigator knows
		// to proceed with its state machine.
		if (m_Device.m_Navigator)
		{
			m_Device.m_Navigator->OnStatusMessageReceived();
		}
		if (m_Device.m_SpiderEngine)
		{
			m_Device.m_SpiderEngine->OnStatusReceived();
		}

		// Status messages are the ONLY message type where key commands can be sent.
		// The controller only processes key commands in ACKs to Status messages.
		m_Device.ProcessControllerUpdates(true);

		// All start-up messages up to (and including) the first status message have a
		// different ACKnowledgement type so now the first status message has been
		// ACKed, switch to the next type.
		if (Messages::AckTypes::V1_Normal == m_Device.m_AckType_ToSend)
		{
			LogInfo(Channel::Devices, [this]() { return std::format("OneTouch ({}): Transitioning from V1_Normal to V2_Normal ACK type", m_Device.DeviceId()); });
			m_Device.m_AckType_ToSend = Messages::AckTypes::V2_Normal;
		}

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (Status)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_Clear(const Messages::PDAMessage_Clear& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_Clear", std::source_location::current(), Profiling::UnitColours::Red);

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Received PDAMessage_Clear - clearing screen", m_Device.DeviceId()); });

		m_Device.ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evClear());
		m_Device.ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (Clear)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_Highlight(const Messages::PDAMessage_Highlight& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_Highlight", std::source_location::current(), Profiling::UnitColours::Red);

		LogDebug(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Received PDAMessage_Highlight: line_id={}", m_Device.DeviceId(), msg.LineId()); });

		if (HIGHLIGHT_CLEAR_ALL == msg.LineId())
		{
			// 0xFF is the "clear all highlights" sentinel, NOT an out-of-range line id.
			// Record it so the Navigator (which resets m_Device.m_HighlightedLine) observes that no
			// line is currently highlighted, and forward it to the screen updater unchanged.
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Highlight clear-all received (no line highlighted)", m_Device.DeviceId()); });
			m_Device.m_HighlightedLine = HIGHLIGHT_CLEAR_ALL;
		}
		else if (msg.LineId() >= OneTouchDevice::ONETOUCH_PAGE_LINES)
		{
			LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Highlight for invalid line: line_id={} (max={})", m_Device.DeviceId(), msg.LineId(), OneTouchDevice::ONETOUCH_PAGE_LINES - 1); });
		}
		else
		{
			// Track the highlighted line for navigation
			m_Device.m_HighlightedLine = msg.LineId();
		}

		m_Device.ScreenMode(Capabilities::ScreenModes::Updating);
		m_Device.ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evHighlight(msg.LineId()));

		m_Device.ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (Highlight)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_HighlightChars(const Messages::PDAMessage_HighlightChars& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_HighlightChars", std::source_location::current(), Profiling::UnitColours::Red);

		LogDebug(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Received PDAMessage_HighlightChars: line_id={}, start={}, stop={}", m_Device.DeviceId(), msg.LineId(), msg.StartIndex(), msg.StopIndex()); });

		if (msg.LineId() >= OneTouchDevice::ONETOUCH_PAGE_LINES)
		{
			LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): HighlightChars for invalid line: line_id={} (max={})", m_Device.DeviceId(), msg.LineId(), OneTouchDevice::ONETOUCH_PAGE_LINES - 1); });
		}

		if (msg.StartIndex() > msg.StopIndex())
		{
			LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): HighlightChars invalid range: start={} > stop={}", m_Device.DeviceId(), msg.StartIndex(), msg.StopIndex()); });
		}

		m_Device.ScreenMode(Capabilities::ScreenModes::Updating);
		m_Device.ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evHighlightChars(msg.LineId(), msg.StartIndex(), msg.StopIndex()));

		m_Device.ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (HighlightChars)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_ShiftLines(const Messages::PDAMessage_ShiftLines& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_ShiftLines", std::source_location::current(), Profiling::UnitColours::Red);

		auto direction = (0 > msg.LineShift()) ? Utility::ScreenDataPage::ShiftDirections::Up : Utility::ScreenDataPage::ShiftDirections::Down;
		auto lines_to_shift = std::abs(msg.LineShift());

		LogDebug(Channel::Devices, [this, &msg, &lines_to_shift, &direction]() { return std::format("OneTouch ({}): Received PDAMessage_ShiftLines: first_line={}, last_line={}, shift={}, direction={}", m_Device.DeviceId(), msg.FirstLineId(), msg.LastLineId(), lines_to_shift, magic_enum::enum_name(direction)); });

		if (msg.FirstLineId() >= OneTouchDevice::ONETOUCH_PAGE_LINES || msg.LastLineId() >= OneTouchDevice::ONETOUCH_PAGE_LINES)
		{
			LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): ShiftLines with invalid line range: first={}, last={} (max={})", m_Device.DeviceId(), msg.FirstLineId(), msg.LastLineId(), OneTouchDevice::ONETOUCH_PAGE_LINES - 1); });
		}

		if (msg.FirstLineId() > msg.LastLineId())
		{
			LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): ShiftLines with invalid range: first={} > last={}", m_Device.DeviceId(), msg.FirstLineId(), msg.LastLineId()); });
		}

		m_Device.ScreenMode(Capabilities::ScreenModes::Updating);
		m_Device.ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evShift(direction, msg.FirstLineId(), msg.LastLineId(), lines_to_shift));
		m_Device.ProcessScreenUpdates();

		m_Device.ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (ShiftLines)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_DisplayUpdate(const Messages::JandyMessage_DisplayUpdate& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_DisplayUpdate", std::source_location::current(), Profiling::UnitColours::Red);

		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Received JandyMessage_DisplayUpdate", m_Device.DeviceId()); });

		m_Device.ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (DisplayUpdate)", m_Device.DeviceId()); });
	}

	void OneTouchMessageRouter::Slot_OneTouch_Unknown(const Messages::JandyMessage_Unknown& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Slot_Unknown", std::source_location::current(), Profiling::UnitColours::Red);

		LogWarning(Channel::Devices, [this, &msg]() { return std::format("OneTouch ({}): Received unknown message type: 0x{:02x}", m_Device.DeviceId(), msg.RawId()); });

		m_Device.ProcessControllerUpdates();

		// Kick the watchdog to indicate that this device is alive.
		m_Device.Kick();
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Watchdog kicked (Unknown)", m_Device.DeviceId()); });
	}

}
// namespace AqualinkAutomate::Devices
