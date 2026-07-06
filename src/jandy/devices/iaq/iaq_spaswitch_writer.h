#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "devices/capabilities/actuation_types.h"

namespace AqualinkAutomate::Kernel { class DataHub; }
namespace AqualinkAutomate::Devices { class JandyDeviceType; }

namespace AqualinkAutomate::Devices::IAQ
{
	class PageModel;
	class ICommandSink;

	// The spa-side switch button-assignment WRITE state machine for the AqualinkTouch (0x33),
	// extracted from IAQDevice (SonarCloud S1820). Drives the "4 Function Spa Switch" detail UI:
	// navigate Home -> menu -> Setup -> Spa Remotes -> open the detail, select the S:B row, scroll
	// the picker to the target function, commit, then verify via the DataHub. Serviced at most one
	// command per poll, page-gated so a navigation miss can never write the wrong cell. RE'd +
	// cross-validated from captures/iaq_spaswitch_edit{,2}.cap; see docs/alwin32/spaside-remotes.md
	// and docs/iaq_device_decomposition.md.
	class SpaSwitchWriter
	{
	public:
		// Arm a goal (one at a time). `emulation_active` = the panel is actively emulating (a passive
		// decoder cannot transmit); `channel_busy` = the command channel is mid-sequence / handshake.
		// Returns the ActuationResult (Accepted == queued; NotSupported / InvalidValue as documented).
		Capabilities::ActuationResult Queue(uint8_t switch_number, uint8_t button_number,
			const std::string& function, bool emulation_active, bool channel_busy,
			const JandyDeviceType& device_id);

		// Is a goal currently in flight?
		bool HasPendingGoal() const { return m_Pending.has_value(); }

		// Service the pending goal: inspect the current page + decoded picker rows and emit at most
		// one command into `sink` this poll, page-gated on `page`. `data_hub` may be null (Verify is
		// then skipped and the poll backstop bounds the goal).
		void ProcessStep(const PageModel& page, ICommandSink& sink, Kernel::DataHub* data_hub,
			const JandyDeviceType& device_id);

	private:
		enum class Phase
		{
			Navigate,     // page-gated walk to the 4-Function detail (0x3b)
			SelectRow,    // press the S:B assignment row (page-button (ordinal-1) + row-select base)
			FindFunction, // read the picker; scroll until F is visible, then commit at slot+commit-base
			Verify,       // confirm the DataHub assignment now reads F
			Done,
			Failed
		};

		struct Goal
		{
			uint8_t switch_number{ 0 };
			uint8_t button_number{ 0 };
			std::string function;   // target function as the picker lists it
			std::string row_tag;    // "<switch>:<button>" e.g. "1:2"
			std::string desc;
		};

		std::optional<Goal> m_Pending;
		Phase m_Phase{ Phase::Navigate };
		bool m_RowSelected{ false };                // the S:B row-select has been issued
		uint32_t m_PollCount{ 0 };                  // overall backstop
		uint32_t m_ScrollCount{ 0 };                // picker pages scrolled so far
		uint32_t m_SettleCount{ 0 };                // polls waited for a page/picker to settle after a command
		std::optional<std::string> m_FirstPickerSeen;  // wrap-detection while scrolling the picker
	};

}
// namespace AqualinkAutomate::Devices::IAQ
