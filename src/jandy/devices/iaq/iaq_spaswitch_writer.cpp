#include "devices/iaq/iaq_spaswitch_writer.h"

#include <cstddef>
#include <cstdint>
#include <format>

#include "devices/iaq/iaq_command_sink.h"
#include "devices/iaq/iaq_page_model.h"
#include "devices/jandy_device_types.h"
#include "formatters/jandy_device_formatters.h"
#include "kernel/data_hub.h"
#include "logging/logging.h"
#include "profiling/profiling.h"
#include "utility/case_insensitive_comparision.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Devices::IAQ
{

	namespace
	{
		// IAQ (AqualinkTouch 0x33) UI navigation command / page bytes used to drive the 4-Function
		// Spa Switch detail edit. RE'd + cross-validated from captures/iaq_spaswitch_edit{,2}.cap;
		// see docs/alwin32/spaside-remotes.md. These ride the 0x33 poll-ACK channel.
		constexpr uint8_t IAQ_CMD_BACK{ 0x02 };                 // navigate back / unwind toward menu
		constexpr uint8_t IAQ_CMD_PAGE_BUTTON_BASE{ 0x11 };     // press on-screen PageButton index N -> 0x11 + N

		constexpr uint8_t IAQ_PAGE_HOME{ 0x01 };
		constexpr uint8_t IAQ_PAGE_MENU{ 0x0f };
		constexpr uint8_t IAQ_PAGE_SETUP{ 0x14 };
		constexpr uint8_t IAQ_PAGE_SPA_REMOTES{ 0x3a };
		constexpr uint8_t IAQ_PAGE_SPA_SWITCH_DETAIL{ 0x3b };

		constexpr uint8_t IAQ_CMD_MENU_TO_SETUP{ 0x15 };        // on the menu (0x0f): page-button idx4 -> Setup
		constexpr uint8_t IAQ_CMD_OPEN_SPASWITCH_DETAIL{ 0x16 };// on Spa Remotes (0x3a): idx5 -> 4-Function detail
		constexpr uint8_t IAQ_CMD_SCROLL_PICKER{ 0x15 };        // on the detail (0x3b): idx4 -> page the picker

		// On the 4-Function detail, the selectable cells share one page-button index space:
		//   assignment row (ordinal O = (S-1)*4 + B, 1-based) -> page-button index O + 4
		//   picker row     (visible slot A, 1-based)          -> page-button index A + 11
		// As page-button commands (0x11 + index): row-select = 0x15 + O, commit = 0x1c + A.
		constexpr uint8_t IAQ_SPASWITCH_ROWSELECT_CMD_BASE{ 0x15 };  // + ordinal
		constexpr uint8_t IAQ_SPASWITCH_COMMIT_CMD_BASE{ 0x1c };     // + picker slot
		constexpr uint8_t IAQ_SPASWITCH_MAX_VISIBLE_ROW{ 7 };        // rows 1..7 are on-screen; row 8 (2:4) needs an
		                                                            // assignment-list scroll that is not yet decoded
		constexpr uint32_t IAQ_SPASWITCH_SETTLE_POLLS{ 4 };         // polls to let the master render after a command
		constexpr uint32_t IAQ_SPASWITCH_MAX_SCROLLS{ 10 };        // bound the picker scroll search
		constexpr uint32_t IAQ_SPASWITCH_POLL_LIMIT{ 400 };        // overall backstop (abandon the goal)
	}
	// namespace

	Capabilities::ActuationResult SpaSwitchWriter::Queue(uint8_t switch_number, uint8_t button_number,
		const std::string& function, bool emulation_active, bool channel_busy, const JandyDeviceType& device_id)
	{
		// Program a spa-side switch button's function over the bus by driving the AqualinkTouch (0x33)
		// "4 Function Spa Switch" detail page (RE'd + cross-validated from iaq_spaswitch_edit{,2}.cap;
		// see docs/alwin32/spaside-remotes.md). Only an EMULATED panel transmits, so a passive decoder
		// can't program -- report NotSupported so the controller falls through to another writer.
		if (!emulation_active)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): Not actively emulating - cannot program spa-switch assignment", device_id); });
			return Capabilities::ActuationResult::NotSupported;
		}

		if ((switch_number < 1) || (button_number < 1) || (button_number > 4) || function.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		// The detail page lists assignment rows 1..7 on-screen (ordinal = (S-1)*4 + B). Row 8 (2:4)
		// and any switch >2 need an assignment-list scroll whose protocol is not yet decoded -- and
		// row 8's page-button index would collide with the picker commit range -- so reject those
		// rather than risk writing the wrong cell. The OneTouch writer still covers them.
		if (const uint32_t ordinal = static_cast<uint32_t>(switch_number - 1) * 4u + button_number; ordinal > IAQ_SPASWITCH_MAX_VISIBLE_ROW)
		{
			LogWarning(Channel::Devices, [&device_id, switch_number, button_number]() { return std::format("IAQ ({}): spa-switch row {}:{} is not directly selectable on the iAQ detail (needs an undecoded assignment-list scroll) - deferring", device_id, switch_number, button_number); });
			return Capabilities::ActuationResult::NotSupported;
		}

		// One goal at a time on the shared panel UI.
		if (m_Pending.has_value() || channel_busy)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): Busy - rejecting spa-switch assignment", device_id); });
			return Capabilities::ActuationResult::Busy;
		}

		Goal goal;
		goal.switch_number = switch_number;
		goal.button_number = button_number;
		goal.function = function;
		goal.row_tag = std::format("{}:{}", switch_number, button_number);
		goal.desc = std::format("spa-switch {}:{} -> '{}'", switch_number, button_number, function);

		LogInfo(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): Queued {}", device_id, goal.desc); });
		m_Pending = std::move(goal);
		m_Phase = Phase::Navigate;
		m_RowSelected = false;
		m_PollCount = 0;
		m_ScrollCount = 0;
		m_SettleCount = 0;
		m_FirstPickerSeen.reset();
		return Capabilities::ActuationResult::Accepted;
	}

	// Issue one command this poll, then settle.
	void SpaSwitchWriter::IssueAndSettle(ICommandSink& sink, uint8_t cmd)
	{
		sink.IssueCommand(cmd);
		m_SettleCount = IAQ_SPASWITCH_SETTLE_POLLS;
	}

	// Tear down the goal (reads goal.desc BEFORE resetting -- callers return immediately after).
	void SpaSwitchWriter::FinishGoal(ICommandSink& sink, bool ok, const Goal& goal, const JandyDeviceType& device_id)
	{
		if (ok) { LogInfo(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} completed", device_id, goal.desc); }); }
		else    { LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} abandoned", device_id, goal.desc); }); }
		sink.IssueCommand(0x00);
		m_Pending.reset();
		m_Phase = Phase::Navigate;
		m_RowSelected = false;
		m_ScrollCount = 0;
		m_SettleCount = 0;
		m_FirstPickerSeen.reset();
	}

	void SpaSwitchWriter::StepNavigate(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id)
	{
		(void)device_id;

		// Page-GATED walk to the 4-Function detail (0x3b). Each hop waits (via settle + page-id
		// re-evaluation) for the master to land on the next page before the following command.
		switch (page.PageId())
		{
		case IAQ_PAGE_SPA_SWITCH_DETAIL:
			m_Phase = Phase::SelectRow;   // arrived; act next poll
			return;

		case IAQ_PAGE_SPA_REMOTES:
			IssueAndSettle(sink, IAQ_CMD_OPEN_SPASWITCH_DETAIL);         // 0x16 -> detail
			return;

		case IAQ_PAGE_SETUP:
			if (auto idx = page.FindButtonByLabel("Spa Remotes"); idx.has_value())
			{
				IssueAndSettle(sink, static_cast<uint8_t>(IAQ_CMD_PAGE_BUTTON_BASE + idx.value()));
			}
			else
			{
				sink.IssueCommand(0x00);   // button not rendered yet; dwell one poll
			}
			return;

		case IAQ_PAGE_MENU:
			IssueAndSettle(sink, IAQ_CMD_MENU_TO_SETUP);                 // 0x15 -> Setup
			return;

		case IAQ_PAGE_HOME:
		default:
			IssueAndSettle(sink, IAQ_CMD_BACK);                          // HOME or unknown: unwind toward menu
			return;
		}
	}

	void SpaSwitchWriter::StepSelectRow(const PageModel& page, ICommandSink& sink, const Goal& goal)
	{
		if (page.PageId() != IAQ_PAGE_SPA_SWITCH_DETAIL)
		{
			m_Phase = Phase::Navigate;   // lost the page; re-navigate
			return;
		}
		if (!m_RowSelected)
		{
			const uint32_t ordinal = static_cast<uint32_t>(goal.switch_number - 1) * 4u + goal.button_number;
			IssueAndSettle(sink, static_cast<uint8_t>(IAQ_SPASWITCH_ROWSELECT_CMD_BASE + ordinal));   // 0x15 + ordinal
			m_RowSelected = true;
		}
		m_Phase = Phase::FindFunction;
		m_FirstPickerSeen.reset();
	}

	void SpaSwitchWriter::StepFindFunction(const PageModel& page, ICommandSink& sink, const Goal& goal,
		const JandyDeviceType& device_id)
	{
		if (page.PageId() != IAQ_PAGE_SPA_SWITCH_DETAIL)
		{
			m_Phase = Phase::Navigate;
			return;
		}

		// Target visible in the current picker page? Commit at its slot (0x1c + slot).
		for (const auto& [slot, function] : page.SpaSwitchPickerRows())
		{
			if (Utility::EqualsCaseInsensitive(function, goal.function))
			{
				IssueAndSettle(sink, static_cast<uint8_t>(IAQ_SPASWITCH_COMMIT_CMD_BASE + slot));
				m_Phase = Phase::Verify;
				return;
			}
		}

		// Not visible: scroll the picker, with wrap-detection (the first row repeating means we
		// have cycled the whole list without finding F) and a hard scroll bound.
		const std::string signature = page.SpaSwitchPickerRows().empty() ? std::string{} : page.SpaSwitchPickerRows().begin()->second;
		if (!m_FirstPickerSeen.has_value())
		{
			m_FirstPickerSeen = signature;
		}
		else if (!signature.empty() && (m_ScrollCount > 0) && Utility::EqualsCaseInsensitive(signature, m_FirstPickerSeen.value()))
		{
			LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): function '{}' not offered by the picker for {}", device_id, goal.function, goal.row_tag); });
			FinishGoal(sink, false, goal, device_id);
			return;
		}

		if (++m_ScrollCount > IAQ_SPASWITCH_MAX_SCROLLS)
		{
			LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): exhausted picker scroll for {}", device_id, goal.row_tag); });
			FinishGoal(sink, false, goal, device_id);
			return;
		}
		IssueAndSettle(sink, IAQ_CMD_SCROLL_PICKER);   // 0x15
	}

	void SpaSwitchWriter::StepVerify(const PageModel& page, ICommandSink& sink, Kernel::DataHub* data_hub,
		const Goal& goal, const JandyDeviceType& device_id)
	{
		(void)page;

		// The commit press IS the save -- the master re-pushes the group-0x00 row, which the read
		// path writes to the DataHub. Confirm it now reads the target function.
		if (nullptr != data_hub)
		{
			if (auto live = data_hub->SpaSwitchAssignment(goal.switch_number, goal.button_number);
				live.has_value() && Utility::EqualsCaseInsensitive(live.value(), goal.function))
			{
				FinishGoal(sink, true, goal, device_id);
				return;
			}
		}
		sink.IssueCommand(0x00);   // dwell until the row re-pushes (or the poll backstop fires)
	}

	void SpaSwitchWriter::ProcessStep(const PageModel& page, ICommandSink& sink, Kernel::DataHub* data_hub,
		const JandyDeviceType& device_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("SpaSwitchWriter::ProcessStep", std::source_location::current());

		if (!m_Pending.has_value())
		{
			return;
		}
		const Goal& goal = m_Pending.value();

		// Overall backstop: never spin forever on a bus that isn't behaving as decoded.
		if (++m_PollCount > IAQ_SPASWITCH_POLL_LIMIT)
		{
			FinishGoal(sink, false, goal, device_id);
			return;
		}

		// Settle: after issuing a command, dwell a few polls so the master renders the new page /
		// re-pushes the picker before we read state and decide the next step.
		if (m_SettleCount > 0)
		{
			--m_SettleCount;
			sink.IssueCommand(0x00);
			return;
		}

		switch (m_Phase)
		{
		case Phase::Navigate:
			StepNavigate(page, sink, device_id);
			return;

		case Phase::SelectRow:
			StepSelectRow(page, sink, goal);
			return;

		case Phase::FindFunction:
			StepFindFunction(page, sink, goal, device_id);
			return;

		case Phase::Verify:
			StepVerify(page, sink, data_hub, goal, device_id);
			return;

		case Phase::Done:
		case Phase::Failed:
		default:
			FinishGoal(sink, m_Phase == Phase::Done, goal, device_id);
			return;
		}
	}

}
// namespace AqualinkAutomate::Devices::IAQ
