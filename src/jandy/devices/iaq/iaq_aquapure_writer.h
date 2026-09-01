#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "devices/capabilities/actuation_types.h"
#include "kernel/body_of_water_ids.h"

namespace AqualinkAutomate::Devices { class JandyDeviceType; }

namespace AqualinkAutomate::Devices::IAQ
{
	class PageModel;
	class ICommandSink;

	// The chlorinator (AquaPure) WRITE state machine for the AqualinkTouch (0x33).
	//
	// This is the FAST way to set the salt-water generator: the panel's own AquaPure page takes an
	// ABSOLUTE value through the control-data handshake, where the OneTouch menu path can only step
	// the value 5% at a time. AqualinkD reaches it the same way (source/iaqtouch_aq_programmer.c:
	// goto_iaqt_page -> iaqtFindButtonByLabel -> queue_iaqt_control_command), and its key table
	// (source/iaqtouch.h) is what confirms our command encoding: KEY_IAQTCH_MENU = 0x02 and
	// KEY_IAQTCH_KEY01..KEY15 = 0x11..0x1f, i.e. page button N is pressed as 0x11 + N.
	//
	// WHY A STATE MACHINE RATHER THAN A QUEUED BURST. The previous implementation queued four
	// commands blindly, one per poll, and assumed the AquaPure page opened from a FIXED page-button
	// index. Two things break that:
	//
	//   * The master lays every page out from the installed equipment, so no index is portable --
	//     a fixed one presses whatever else occupies that slot.
	//   * 0x02 is the panel's "Menu / Back" key: ONE physical button whose meaning depends on the
	//     screen it is pressed from (menu from home, back from a sub-page). Firing it blind does
	//     not reliably land on the menu.
	//
	// So every hop is page-GATED: issue one command, wait for the master to render, re-read the
	// page id, and only then decide the next command. A navigation miss can never submit a value
	// into the wrong field, and the target buttons are resolved BY LABEL on the page that owns
	// them rather than by a hardcoded index.
	//
	// CAPTURE STATUS: the whole route is CONFIRMED on a live bus by
	// captures/chlorinator_rs485.cap, recorded while an iAqualink module set the chlorinator on a
	// real RS-8 Combo:
	//
	//     0x02 -> PageStart 0x0f (Menu)
	//     0x19 -> PageStart 0x30 (AquaPure)   buttons: 0 "Pool 30%", 1 "Spa 30%",
	//                                                  2 "Quick Boost", 3 "Boost Setup"
	//     0x11 -> Btn[0] "Pool 30%" selected, PageMessage "30"
	//     0x80 -> IAQ_ControlReady -> Btn[0] "Pool 70%"
	//
	// One asymmetry that shapes the design: the MENU page advertises no buttons (PageStart 0x0f is
	// followed straight by PageEnd), so that hop cannot be label-driven and presses a known key --
	// made safe by verifying arrival on 0x30. The AquaPure page DOES advertise its buttons, so the
	// row there is resolved by label, which is also where it matters (Pool vs Spa).
	class AquaPureWriter
	{
	public:
		// Arm a "set output %" goal for ONE body. Pool and spa carry INDEPENDENT setpoints on the
		// panel -- the AquaPure page shows both rows at once ("Pool 40%" / "Spa 70%") -- so the
		// body is given explicitly and never inferred from which one is circulating.
		// `emulation_active` = the panel is actively emulating (a passive decoder cannot
		// transmit); `channel_busy` = the command channel is mid-sequence / handshake.
		Capabilities::ActuationResult QueuePercentage(uint8_t percentage, Kernel::BodyOfWaterIds body,
			bool emulation_active, bool channel_busy, const JandyDeviceType& device_id);

		// Arm a "toggle quick boost" goal.
		Capabilities::ActuationResult QueueBoost(bool enable, bool emulation_active, bool channel_busy,
			const JandyDeviceType& device_id);

		bool HasPendingGoal() const { return m_Pending.has_value(); }

		// Has a goal positively established that this panel's menu does NOT open an AquaPure page?
		// Until one has, we cannot know without navigating, so the first attempt tries the fast
		// path; once we DO know, the actuator refuses up-front so the dispatcher falls back to a
		// controller that can reach the setting by menu crawl.
		bool RouteUnavailable() const { return m_RouteUnavailable; }

		// Learn the menu -> AquaPure route whenever the menu page happens to render, for ANY reason
		// (the spa-switch writer walks through it, for instance). Cheap and a no-op off that page.
		void ObserveMenuPage(const PageModel& page);

		// Service the pending goal: emit at most one command into `sink` this poll, gated on the
		// page the master is currently rendering.
		void ProcessStep(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id);

	private:
		struct Goal;

		void StepNavigate(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id);
		void StepSelectField(const PageModel& page, ICommandSink& sink, const Goal& goal, const JandyDeviceType& device_id);
		void StepSubmit(const PageModel& page, ICommandSink& sink, const Goal& goal);
		void StepReturnHome(const PageModel& page, ICommandSink& sink, const Goal& goal, const JandyDeviceType& device_id);

		// Issue `cmd` this poll, then dwell while the master renders.
		void IssueAndSettle(ICommandSink& sink, uint8_t cmd);
		// Tear down the pending goal (reads goal.desc BEFORE resetting -- callers return immediately after).
		void FinishGoal(ICommandSink& sink, bool ok, const Goal& goal, const JandyDeviceType& device_id);

		Capabilities::ActuationResult Arm(Goal&& goal, bool emulation_active, bool channel_busy,
			const JandyDeviceType& device_id);

		enum class Phase
		{
			Navigate,     // page-gated walk to the AquaPure page (0x30)
			SelectField,  // press the Pool / Spa row (or the boost button) by label
			Submit,       // arm the control-data value and send the value-submit command
			ReturnHome,   // unwind off the settings page so the live page model tracks home again
			Done,
			Failed
		};

		struct Goal
		{
			bool is_boost{ false };
			bool boost_enable{ false };
			uint8_t percentage{ 0 };
			Kernel::BodyOfWaterIds body{ Kernel::BodyOfWaterIds::Pool };
			std::string desc;
		};

		std::optional<Goal> m_Pending;
		Phase m_Phase{ Phase::Navigate };
		uint32_t m_PollCount{ 0 };      // overall backstop
		uint32_t m_SettleCount{ 0 };    // polls to wait for a page to render after a command
		uint32_t m_UnwindCount{ 0 };    // bounded Menu/Back presses while unwinding
		uint32_t m_MenuAttempts{ 0 };   // bounded presses of the menu's AquaPure entry
		bool m_FieldSelected{ false };
		bool m_RouteUnavailable{ false };
		std::optional<uint8_t> m_MenuAquaPureIndex;   // learned menu button index, once observed
	};

}
// namespace AqualinkAutomate::Devices::IAQ
