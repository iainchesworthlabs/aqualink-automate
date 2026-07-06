#include <format>
#include <functional>
#include <utility>

#include <nlohmann/json.hpp>

#include "logging/logging.h"
#include "devices/spaside_remote_device.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	SpasideRemoteDevice::SpasideRemoteDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated) :
		JandyController(device_id, hub_locator),
		Capabilities::Restartable(SPASIDE_TIMEOUT_DURATION),
		Capabilities::Emulated(is_emulated)
	{
		if (Capabilities::Emulated::IsEmulated())
		{
			// We ARE the remote: ACK the master's discovery probe and its cmd-0x02 LED-image
			// poll, injecting any pending button press into the [0x01][0x00][button] reply.
			m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Probe>(
				std::bind_front(&SpasideRemoteDevice::Slot_Spaside_EmulatedProbe, this), (*device_id)());
			m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Status>(
				std::bind_front(&SpasideRemoteDevice::Slot_Spaside_EmulatedPoll, this), (*device_id)());
		}
		else
		{
			// Passively decode a real remote. The master's cmd-0x02 LED-image poll decodes as a
			// Status addressed to our id; use it purely as the "we were just polled" trigger that
			// arms the button-Ack correlation.
			m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Status>(
				std::bind_front(&SpasideRemoteDevice::Slot_Spaside_Status, this), (*device_id)());

			// Button presses are reported as a generic Ack (cmd 0x01) addressed to the MASTER
			// (0x00), so we CANNOT filter by our own id -- register unfiltered and correlate.
			m_SlotManager.RegisterSlot<Messages::JandyMessage_Ack>(
				std::bind_front(&SpasideRemoteDevice::Slot_Spaside_Ack, this));
		}
	}

	void SpasideRemoteDevice::PressButton(uint8_t button_index)
	{
		m_PendingButton = button_index;
		LogInfo(Channel::Devices, [this, button_index]() { return std::format("SpasideRemote (0x{:02x}): queued emulated press of button {}", DeviceId().Id()(), button_index); });
	}

	void SpasideRemoteDevice::Slot_Spaside_EmulatedProbe(const Messages::JandyMessage_Probe& /*msg*/)
	{
		SendButtonAck();
	}

	void SpasideRemoteDevice::Slot_Spaside_EmulatedPoll(const Messages::JandyMessage_Status& msg)
	{
		// We are emulating the remote: the master's cmd-0x02 poll is the LED-image push telling us
		// what to display, AND the cue to report any pending button. Decode the image first.
		DecodeLedImage(msg);
		SendButtonAck();
	}

	void SpasideRemoteDevice::SendButtonAck()
	{
		++m_PollCount;

		// Reply [0x01][0x00][button]: ack_type 0x00, command = the pending button (0 = idle).
		// Signal_AckMessage is gated by Capabilities::Emulated, so it only transmits when emulated.
		Signal_AckMessage(static_cast<uint8_t>(0x00), m_PendingButton);

		if (m_PendingButton != 0x00)
		{
			LogInfo(Channel::Devices, [this]() { return std::format("SpasideRemote (0x{:02x}): reported emulated button {} to the master", DeviceId().Id()(), m_PendingButton); });
			m_PendingButton = 0x00;   // momentary: release after one report
		}
	}

	void SpasideRemoteDevice::ProcessControllerUpdates()
	{
		// Intentionally empty: the spa-side remote is driven entirely by inbound slots (probe/poll/
		// ack); it has no periodic controller-update work to perform on the poll loop.
	}

	void SpasideRemoteDevice::WatchdogTimeoutOccurred()
	{
		// Intentionally empty: a spa-side remote may legitimately be absent/idle for long periods,
		// so a watchdog timeout carries no recovery action for this device.
	}

	void SpasideRemoteDevice::Slot_Spaside_Status(const Messages::JandyMessage_Status& msg)
	{
		// The master addressed us (the LED-image poll). Arm the one-shot window: the next Ack to
		// the master is our reply, and decode the indicator image this frame carries.
		m_AwaitingButtonAck = true;
		++m_PollCount;
		DecodeLedImage(msg);
	}

	void SpasideRemoteDevice::DecodeLedImage(const Messages::JandyMessage_Status& msg)
	{
		// The cmd-0x02 LED image is carried as the Status payload. data[1] of the 6-byte image is
		// payload byte [0] on the wire (the cmd byte is the message type, not payload). It packs up
		// to four 2-bit indicator fields: bits [1:0]=LED0, [3:2]=LED1, [5:4]=LED2, [7:6]=LED3, each
		// 0=off, 1=on, 2/3=blink (docs/alwin32/spaside-remotes.md, Ghidra-confirmed).
		const auto& payload = msg.RawPayload();
		if (payload.empty())
		{
			return;
		}

		m_LedImage = payload;
		m_LedImageSeen = true;

		using enum SpasideLedState;

		const auto led_byte = payload[0];
		for (std::size_t i = 0; i < m_Leds.size(); ++i)
		{
			const uint8_t bits = static_cast<uint8_t>((led_byte >> (2 * i)) & 0x03);
			if (0x00 == bits)
			{
				m_Leds[i] = Off;
			}
			else if (0x01 == bits)
			{
				m_Leds[i] = On;
			}
			else
			{
				m_Leds[i] = Blink;
			}
		}
	}

	void SpasideRemoteDevice::Slot_Spaside_Ack(const Messages::JandyMessage_Ack& msg)
	{
		if (!m_AwaitingButtonAck)
		{
			// Not the Ack that immediately followed the master polling us.
			return;
		}

		// Consume the one-shot window: a present remote answers every poll, so the first Ack
		// after our poll is ours. Clearing here also bounds any mis-attribution if we ever go
		// silent (the very next unrelated Ack would be ignored, not chained).
		m_AwaitingButtonAck = false;

		// Spa-side button reports carry ack_type 0x00 ([0x01][0x00][button]); OneTouch acks use
		// 0x80 and PDA 0x40, so this distinguishes our reply from those other devices' acks.
		if (std::to_underlying(msg.AckType()) != 0x00)
		{
			return;
		}

		const uint8_t button = msg.Command();
		if (button != 0x00)
		{
			m_LastButton = button;
			m_LastButtonAt = std::chrono::system_clock::now();
			LogInfo(Channel::Devices, [this, button]() { return std::format("SpasideRemote (0x{:02x}): button {} pressed", DeviceId().Id()(), button); });
		}
	}

	uint8_t SpasideRemoteDevice::ButtonCount() const
	{
		switch (DeviceId().Class())
		{
		case DeviceClasses::DualSpaSwitch: return 8;   // 2x4rem: 2 columns x 4 rows
		case DeviceClasses::SpaRemote:     return 9;   // 8button: 8 keys + a 9th extra/menu key
		default:                           return 0;
		}
	}

	std::vector<SpasideButton> SpasideRemoteDevice::ButtonLayout() const
	{
		const uint8_t count = ButtonCount();
		std::vector<SpasideButton> layout;
		layout.reserve(count);

		// The 6588 Dual Spa Side Interface board (class DualSpaSwitch at 0x10) bridges two physical
		// 4-button switches onto the bus: wire codes 1-4 = Switch 2, codes 5-8 = Switch 3
		// (CONFIRMED against real hardware + Sheet #6873; docs/alwin32/spaside-remotes.md). So each
		// key's config coordinate is (switch, button-within-switch) with 4 buttons per switch.
		const bool dual = (DeviceClasses::DualSpaSwitch == DeviceId().Class());

		for (uint8_t index = 1; index <= count; ++index)
		{
			SpasideButton button;
			button.index = index;
			if (dual)
			{
				static constexpr uint8_t BUTTONS_PER_SWITCH{ 4 };
				static constexpr uint8_t FIRST_SWITCH{ 2 };   // the 6588 bridges Switch 2 and Switch 3
				button.switch_number = static_cast<uint8_t>(FIRST_SWITCH + (index - 1) / BUTTONS_PER_SWITCH);
				button.button_number = static_cast<uint8_t>((index - 1) % BUTTONS_PER_SWITCH + 1);
				button.assignable = true;
			}
			// Spa Link / unrecognised classes: mapping undecoded -> leave switch/button at 0 and
			// assignable=false. The key is still listed (so it can be pressed when emulated); it
			// simply cannot be programmed until the mapping is captured.
			layout.push_back(button);
		}

		return layout;
	}

	std::vector<std::string> SpasideRemoteDevice::LedStates() const
	{
		std::vector<std::string> states;
		if (!m_LedImageSeen)
		{
			return states;   // nothing decoded yet
		}

		using enum SpasideLedState;

		states.reserve(m_Leds.size());
		for (const auto state : m_Leds)
		{
			switch (state)
			{
			case On:    states.emplace_back("on");    break;
			case Blink: states.emplace_back("blink"); break;
			case Off:
			default:    states.emplace_back("off");   break;
			}
		}
		return states;
	}

	std::string SpasideRemoteDevice::LedImageHex() const
	{
		std::string image_hex;
		image_hex.reserve(m_LedImage.size() * 3);
		for (const auto byte : m_LedImage)
		{
			image_hex += std::format("{:02x} ", byte);
		}
		if (!image_hex.empty())
		{
			image_hex.pop_back();   // drop trailing space
		}
		return image_hex;
	}

	nlohmann::json SpasideRemoteDevice::DescribeDiagnostics() const
	{
		nlohmann::json j;

		j["device_type"] = "SpasideRemote";
		j["device_id"] = std::format("0x{:02x}", DeviceId().Id()());
		j["poll_count"] = static_cast<int64_t>(m_PollCount);
		j["last_button"] = static_cast<int>(m_LastButton);

		// Indicator LEDs the master is pushing to this remote (visualise the master's LED state).
		j["led_image_seen"] = m_LedImageSeen;
		if (m_LedImageSeen)
		{
			j["leds"] = LedStates();
			j["led_image"] = LedImageHex();
		}

		if (m_LastButtonAt.has_value())
		{
			const auto age = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now() - m_LastButtonAt.value()).count();
			j["last_button_age_seconds"] = static_cast<int64_t>(age);
		}
		else
		{
			j["last_button_age_seconds"] = nullptr;
		}

		j["is_emulated"] = IsEmulated();
		j["emulation_suppressed"] = IsEmulationSuppressed();

		return j;
	}

}
// namespace AqualinkAutomate::Devices
