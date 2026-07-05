#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace AqualinkAutomate::Interfaces
{
	/// @brief Runtime control surface for spa-side remotes (Dual Spa Switch / Spa Link).
	///
	/// Implemented in the Jandy layer over the EquipmentHub (it must dynamic_cast to the
	/// concrete spa-side device, which core cannot see). Registered in the HubLocator so the
	/// /api/equipment/spaside-remotes HTTP route can resolve it via TryFind<> and read the
	/// decoded LED/last-press state and -- for an EMULATED remote -- inject a button press.
	///
	/// All types here are plain data so the interface stays free of any Jandy dependency.
	class ISpasideRemoteController
	{
	public:
		/// @brief One physical key on a spa-side remote: its wire press index and -- where the
		///        device class's mapping is decoded -- its controller config switch:button coordinate.
		struct Button
		{
			uint8_t index{ 0 };          ///< 1..button_count -- wire code used to PRESS this key.
			uint8_t switch_number{ 0 };  ///< Controller config switch for ASSIGN; 0 if mapping unknown.
			uint8_t button_number{ 0 };  ///< Controller config button within the switch; 0 if unknown.
			bool assignable{ false };    ///< True iff switch/button are a known mapping (so it is programmable).
		};

		/// @brief Snapshot of one spa-side remote on the bus.
		struct RemoteState
		{
			uint8_t address{ 0 };            ///< Bus address (e.g. 0x10 Dual Spa Switch, 0x20 Spa Link).
			std::string device_class;        ///< Class name, e.g. "DualSpaSwitch" / "SpaRemote".
			bool emulated{ false };          ///< True when WE emulate it (so button presses can be injected).
			uint8_t button_count{ 0 };       ///< Number of physical keys (8 Dual Spa Switch / 9 Spa Link).
			uint32_t poll_count{ 0 };        ///< Times the master has polled it (alive-on-the-bus proxy).
			uint8_t last_button{ 0 };        ///< Last decoded pressed-button index (0 = none seen).
			bool led_image_seen{ false };    ///< True once the master has pushed an LED image.
			std::vector<std::string> leds;   ///< Per-indicator state: "off" / "on" / "blink" (empty until seen).
			std::string led_image;           ///< Raw LED image bytes as a hex string (empty until seen).
			std::vector<Button> buttons;     ///< The remote's physical keys (press index + config coordinate).
		};

		/// @brief Outcome of a PressButton request.
		enum class PressResult
		{
			Success,         ///< The press was queued for injection on the next poll.
			RemoteNotFound,  ///< No spa-side remote is present at the requested address.
			NotEmulated,     ///< The remote exists but is only passively observed -- we cannot actuate it.
			InvalidButton    ///< The button index is outside 1..button_count for that remote.
		};

		/// @brief Outcome of a SetButtonAssignment request (programming a button's function over the bus).
		enum class AssignResult
		{
			Accepted,        ///< Queued: a controller is driving its config menu to set the assignment.
			InvalidRequest,  ///< Out-of-range switch/button, or an empty/unrecognised function name.
			NotAvailable,    ///< No connected controller can program spa-switch assignments (no configurator).
			Busy             ///< A controller is mid-operation; retry shortly.
		};

	public:
		virtual ~ISpasideRemoteController() = default;

		/// @brief Snapshot of every spa-side remote currently known on the bus.
		virtual std::vector<RemoteState> Remotes() const = 0;

		/// @brief Inject a momentary press of @p button_index on the emulated remote at @p address.
		virtual PressResult PressButton(uint8_t address, uint8_t button_index) = 0;

		/// @brief Program switch @p switch_number button @p button_number to @p function over the bus
		///        (drives the controller's Spa Remotes / Spa Switch config menu). 1-based indices.
		virtual AssignResult SetButtonAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function) = 0;

		/// @brief The functions a connected controller can assign to a spa-switch button -- the valid
		///        choices for SetButtonAssignment, so the UI can offer only what the controller can do.
		///        Empty when no controller able to program assignments is present.
		virtual std::vector<std::string> AvailableFunctions() const = 0;
	};
}
// namespace AqualinkAutomate::Interfaces
