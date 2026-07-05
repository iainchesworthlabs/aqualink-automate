#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "devices/jandy_controller.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/describable.h"
#include "devices/capabilities/emulated.h"
#include "devices/capabilities/restartable.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_status.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::Devices
{

	// Spa-side remotes: the "Dual Spa Switch" (2x4rem, class 0x02 -> 0x10-0x13) and the
	// "Spa Link" (8button, class 0x04 -> 0x20-0x23). They are spa-side keypads: the master
	// polls them with a cmd-0x02 LED/indicator image and the remote reports a pressed button.
	//
	// Wire shape (see docs/alwin32/spaside-remotes.md, Ghidra-verified):
	//   master -> remote : cmd 0x02, a 6-byte LED image (decodes as a JandyMessage_Status
	//                      addressed to our id; its aux-bit fields are meaningless for us).
	//   remote -> master : [0x01][0x00][button] -- a generic Ack (cmd 0x01, ack_type 0x00)
	//                      addressed to the MASTER (0x00), with the pressed button index in
	//                      the Ack's command byte (0 = none).
	//
	// Because the button report is a generic Ack to the master (no source id on the wire), we
	// attribute it by correlation: the master's poll to us fires our (dest-filtered) Status
	// slot which arms a one-shot flag; the immediately-following Ack to the master is ours.
	// State of one spa-side indicator LED, decoded from the master's cmd-0x02 LED-image push
	// (2 bits per indicator on the wire: 0=off, 1=on, 2/3=blink -- see docs/alwin32/spaside-remotes.md).
	enum class SpasideLedState : uint8_t
	{
		Off = 0,
		On = 1,
		Blink = 2
	};

	// One physical key on a spa-side remote, tying together the two coordinate systems the rest
	// of the stack uses: the wire button INDEX (1..button_count, the code sent in a press) and the
	// controller's CONFIG coordinate (switch_number:button_number, used to program the key's
	// function). The two only coincide for classes whose mapping is decoded.
	struct SpasideButton
	{
		uint8_t index{ 0 };          // 1..button_count -- wire code used to PRESS this key
		uint8_t switch_number{ 0 };  // controller config switch for ASSIGN; 0 when the mapping is unknown
		uint8_t button_number{ 0 };  // controller config button within the switch; 0 when unknown
		bool assignable{ false };    // true iff switch_number/button_number are a known mapping for this class
	};

	class SpasideRemoteDevice : public JandyController,
								public Capabilities::Restartable,
								public Capabilities::Emulated,
								public Capabilities::Describable
	{
		inline static const std::chrono::seconds SPASIDE_TIMEOUT_DURATION{ std::chrono::seconds(30) };

		// The master's LED-image push carries 2 bits per indicator. Only the first payload byte
		// (data[1] of the 6-byte image) has a Ghidra-confirmed indicator decode -- four 2-bit
		// fields -- so we expose those four generically (the Dual Spa Switch uses 2, the Spa Link
		// uses 4); the meaning of higher image bytes is capture-unconfirmed and left raw.
		static constexpr std::size_t LED_INDICATOR_COUNT{ 4 };

	public:
		SpasideRemoteDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated);
		~SpasideRemoteDevice() override = default;

		nlohmann::json DescribeDiagnostics() const override;

		// Last decoded pressed-button index (1..N); 0 means none seen since startup.
		uint8_t LastButton() const { return m_LastButton; }

		// Number of times the master has polled us (a proxy for "this remote is alive on the bus").
		uint32_t PollCount() const { return m_PollCount; }

		// Decoded indicator-LED state the master has pushed to this remote. Index 0..3; returns
		// Off for any index until the first LED image is seen (LedImageSeen() reports that).
		SpasideLedState Led(std::size_t index) const { return (index < m_Leds.size()) ? m_Leds[index] : SpasideLedState::Off; }
		bool LedImageSeen() const { return m_LedImageSeen; }

		// Raw bytes of the most recent LED image (payload of the cmd-0x02 poll), for diagnostics
		// and future decode of the higher (capture-unconfirmed) indicator bytes.
		const std::vector<uint8_t>& LedImage() const { return m_LedImage; }

		// Number of physical keys on this spa-side class: 8 for a Dual Spa Switch (0x10-0x13),
		// 9 for a Spa Link (0x20-0x23); 0 for an unrecognised class (docs/alwin32/spaside-remotes.md).
		uint8_t ButtonCount() const;

		// The physical keys of this remote, each carrying its wire press index AND -- where the
		// class's mapping is decoded -- its controller config switch:button coordinate. A Dual Spa
		// Switch (the 6588 board at 0x10) bridges two switches: keys 1-4 = Switch 2, keys 5-8 =
		// Switch 3 (CONFIRMED, docs/alwin32/spaside-remotes.md), so those keys are `assignable`. A
		// Spa Link's key->switch:button mapping is not yet decoded, so its keys report
		// `assignable=false` (we never fabricate a coordinate) -- still pressable, just not
		// programmable until a capture pins the mapping down. Single source of truth for the web layer.
		std::vector<SpasideButton> ButtonLayout() const;

		// Per-indicator LED state as display strings ("off"/"on"/"blink"); empty until the first
		// LED image is seen. Single source of the enum->string mapping (used by diagnostics + web).
		std::vector<std::string> LedStates() const;

		// Hex string of the most recent raw LED image (space-separated bytes); empty until seen.
		std::string LedImageHex() const;

		// EMULATION (is_emulated): queue a button press to inject into our next reply to the
		// master, so the master actuates whatever that button is mapped to on the controller.
		// Momentary: cleared after one report (mimics a brief keypress). No-op if not emulated.
		void PressButton(uint8_t button_index);

	private:
		void ProcessControllerUpdates() override;
		void WatchdogTimeoutOccurred() override;

		// Master polled us (cmd-0x02 LED image, decoded as a Status to our id): arm the
		// one-shot correlation window for the next button-report Ack.
		void Slot_Spaside_Status(const Messages::JandyMessage_Status& msg);

		// A device->master Ack: if it is the one immediately following the master's poll to us
		// and carries ack_type 0x00 (spa-side, vs 0x80 OneTouch / 0x40 PDA), its command byte
		// is our pressed-button index.
		void Slot_Spaside_Ack(const Messages::JandyMessage_Ack& msg);

		// EMULATION: the master addressed us (a discovery Probe or an LED-image poll); reply with
		// our [0x01][0x00][button] status, injecting any pending button press.
		void Slot_Spaside_EmulatedProbe(const Messages::JandyMessage_Probe& msg);
		void Slot_Spaside_EmulatedPoll(const Messages::JandyMessage_Status& msg);
		void SendButtonAck();

		// Decode the master's cmd-0x02 LED-image push (carried as the Status payload) into the
		// per-indicator on/off/blink state. Driven for BOTH a passively-observed real remote and
		// an emulated one (an emulated remote also "displays" whatever the master pushes).
		void DecodeLedImage(const Messages::JandyMessage_Status& msg);

	private:
		bool m_AwaitingButtonAck{ false };
		uint8_t m_LastButton{ 0x00 };
		uint8_t m_PendingButton{ 0x00 };   // emulation: button to inject into the next reply
		uint32_t m_PollCount{ 0 };
		std::optional<std::chrono::system_clock::time_point> m_LastButtonAt;

		bool m_LedImageSeen{ false };
		std::array<SpasideLedState, LED_INDICATOR_COUNT> m_Leds{};   // value-init -> all Off
		std::vector<uint8_t> m_LedImage;                            // raw payload of the last LED push
	};

}
// namespace AqualinkAutomate::Devices
