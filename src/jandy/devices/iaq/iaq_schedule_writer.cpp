#include "devices/iaq/iaq_schedule_writer.h"

#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

#include "devices/iaq/iaq_command_sink.h"
#include "devices/iaq/iaq_page_model.h"
#include "devices/iaq/iaq_schedule_parser.h"
#include "devices/jandy_device_types.h"
#include "formatters/jandy_device_formatters.h"
#include "logging/logging.h"
#include "profiling/profiling.h"
#include "scheduling/promotion_constraints.h"
#include "utility/case_insensitive_comparision.h"
#include "utility/screen_data_page.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Devices::IAQ
{

	namespace
	{
		// IAQ page ids (mirror IAQDevice's class constants) and the fixed touch-grid command bytes the
		// Program flow uses (Command = 0x11 + screen position). RE'd from
		// captures/iaq_schedule_{session,clean}.cap + iaq_editdelete.cap; see docs/iaq_schedule_protocol.md.
		constexpr uint8_t IAQ_PAGE_HOME{ 0x01 };
		constexpr uint8_t IAQ_PAGE_MENU{ 0x0f };
		constexpr uint8_t IAQ_SCHEDULE_PAGE_ID{ 0x28 };        // the Schedule list
		constexpr uint8_t IAQ_DEVICE_PICKER_PAGE_ID{ 0x38 };   // the schedule editor's device picker
		constexpr uint8_t IAQ_TIME_PICKER_PAGE_ID{ 0x29 };     // the time picker (ON/OFF field)
		constexpr uint8_t IAQ_TIME_PICKER_AMPM_LINE{ 2 };      // PageMessage line 2 carries "AM"/"PM"

		constexpr char IAQ_BUTTON_INDEX_POOL{ '1' };           // ASCII value-field prefix ("1" + value)

		constexpr uint8_t IAQ_CMD_BACK{ 0x02 };                // navigate back / unwind toward the menu
		constexpr uint8_t IAQ_CMD_SUBMIT_VALUE{ 0x80 };        // submit the entered value
		constexpr uint8_t IAQ_CMD_MENU_TO_SCHEDULE{ 0x11 };    // on the menu (0x0f): pos0 -> Schedule list
		constexpr uint8_t IAQ_CMD_ADD_PROGRAM{ 0x11 };         // on the list (0x28): pos0 -> Add Program -> picker
		// Device picker (0x38): click a visible row R (1..7) with (base + R); scroll down / confirm.
		constexpr uint8_t IAQ_SCHEDULE_PICK_ROW_BASE{ 0x13 };  // click visible row R -> 0x13 + R
		constexpr uint8_t IAQ_CMD_PICKER_SCROLL{ 0x12 };       // scroll the picker down one page
		constexpr uint8_t IAQ_CMD_PICKER_OK{ 0x13 };           // confirm the highlighted device -> back to list
		// Time fields on the schedule list (0x28) open the time picker (0x29); on the picker, 0x11
		// toggles AM/PM and 0x80 submits (the value rides the control-data response as "1"+HH:MM).
		constexpr uint8_t IAQ_CMD_OPEN_ON_FIELD{ 0x21 };
		constexpr uint8_t IAQ_CMD_OPEN_OFF_FIELD{ 0x22 };
		constexpr uint8_t IAQ_CMD_AMPM_TOGGLE{ 0x11 };
		// Editing an existing program on the list (0x28): click program row R -> 0x22 + R (row1 0x23,
		// ...); Delete = 0x13 -> confirm dialog; Ok = 0x01. Verified against iaq_editdelete.cap.
		constexpr uint8_t IAQ_SCHEDULE_ROW_BASE{ 0x22 };   // click program row R -> 0x22 + R
		constexpr uint8_t IAQ_CMD_EDIT_PROGRAM{ 0x12 };    // Edit -> enter the highlighted row's edit mode
		constexpr uint8_t IAQ_CMD_DELETE_PROGRAM{ 0x13 };  // Delete -> confirm dialog
		constexpr uint8_t IAQ_CMD_CONFIRM_OK{ 0x01 };      // Ok on the confirm dialog
		// Day keys on the schedule list (0x28): the day-selection touch cells.
		constexpr uint8_t IAQ_CMD_DAY_ALL{ 0x17 };
		constexpr uint8_t IAQ_CMD_DAY_MON{ 0x18 };  // then Tue..Sun are consecutive
		constexpr uint8_t IAQ_CMD_DAY_WKDAYS{ 0x1f };
		constexpr uint8_t IAQ_CMD_DAY_WKENDS{ 0x20 };
		constexpr uint32_t IAQ_SCHEDULE_SETTLE_POLLS{ 4 };    // polls to let the master render after a command
		constexpr uint32_t IAQ_SCHEDULE_MAX_SCROLLS{ 12 };    // bound the device-picker scroll search
		constexpr uint32_t IAQ_SCHEDULE_POLL_LIMIT{ 400 };    // overall backstop (abandon the goal)

		// Map a controller-expressible day-of-week bitmask (bit0=Mon..bit6=Sun) to the schedule list's
		// day touch-cell command. The caller only ever passes a selection the controller can represent
		// (guaranteed by Scheduling::CheckControllerCandidate): all / weekdays / weekends / a single day.
		// Single days are consecutive from Monday (0x18) in Mon..Sun order.
		constexpr uint8_t DayCommandFor(uint8_t days_of_week)
		{
			const uint8_t days = days_of_week & 0x7f;
			if (days == 0x7f) { return IAQ_CMD_DAY_ALL; }
			if (days == 0x1f) { return IAQ_CMD_DAY_WKDAYS; }
			if (days == 0x60) { return IAQ_CMD_DAY_WKENDS; }
			if (std::popcount(days) == 1)
			{
				return static_cast<uint8_t>(IAQ_CMD_DAY_MON + std::countr_zero(days));
			}
			return IAQ_CMD_DAY_ALL;   // unreachable for a validated candidate
		}

		// The submit value for a schedule time field: the field-index prefix ('1') + the 12-hour clock
		// time, zero-padded (e.g. 09:00 -> "109:00", 17:00 -> "105:00"). AM/PM is set on the picker by a
		// separate toggle and never rides this value (docs/iaq_schedule_protocol.md).
		std::string ScheduleTimeValue(int hour24, int minute)
		{
			int hour12 = hour24 % 12;
			if (hour12 == 0) { hour12 = 12; }
			return std::format("{}{:02d}:{:02d}", IAQ_BUTTON_INDEX_POOL, hour12, minute);
		}
	}
	// namespace

	Capabilities::ActuationResult ScheduleWriter::QueueCreate(const Scheduling::ControllerSchedule& program,
		bool emulated, bool channel_busy, const JandyDeviceType& device_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ScheduleWriter::QueueCreate", std::source_location::current());

		// A passive (non-emulated) iAQ never transmits, so it cannot drive the panel.
		if (!emulated)
		{
			LogDebug(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): CreateControllerProgram rejected -- device is passive (not emulated)", device_id); });
			return Capabilities::ActuationResult::NotSupported;
		}

		// One goal at a time on the shared panel UI.
		if (m_Pending.has_value() || channel_busy)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): busy - rejecting controller-schedule write", device_id); });
			return Capabilities::ActuationResult::Busy;
		}

		// The controller can only represent a constrained subset -- reject anything it can't.
		if (const auto feasibility = Scheduling::CheckControllerCandidate(program); !feasibility.promotable)
		{
			LogWarning(Channel::Devices, [&device_id, &program]() { return std::format("IAQ ({}): CreateControllerProgram rejected -- program is not controller-representable (target='{}')", device_id, program.target); });
			return Capabilities::ActuationResult::InvalidValue;
		}

		Goal goal;
		goal.op = Op::Create;
		goal.program = program;
		goal.desc = std::format("create controller program '{}'", program.target);
		Arm(std::move(goal), device_id);
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult ScheduleWriter::QueueDelete(const Scheduling::ControllerSchedule& program,
		bool emulated, bool channel_busy, const JandyDeviceType& device_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ScheduleWriter::QueueDelete", std::source_location::current());

		if (!emulated)
		{
			LogDebug(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): DeleteControllerProgram rejected -- device is passive (not emulated)", device_id); });
			return Capabilities::ActuationResult::NotSupported;
		}
		if (m_Pending.has_value() || channel_busy)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): busy - rejecting controller-schedule delete", device_id); });
			return Capabilities::ActuationResult::Busy;
		}
		if (program.target.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		Goal goal;
		goal.op = Op::Delete;
		goal.program = program;
		goal.match = program;   // SelectRow locates the row by matching `match`
		goal.desc = std::format("delete controller program '{}'", program.target);
		Arm(std::move(goal), device_id);
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult ScheduleWriter::QueueEdit(const Scheduling::ControllerSchedule& existing,
		const Scheduling::ControllerSchedule& desired, bool emulated, bool channel_busy, const JandyDeviceType& device_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ScheduleWriter::QueueEdit", std::source_location::current());

		if (!emulated)
		{
			LogDebug(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): EditControllerProgram rejected -- device is passive (not emulated)", device_id); });
			return Capabilities::ActuationResult::NotSupported;
		}
		if (m_Pending.has_value() || channel_busy)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): busy - rejecting controller-schedule edit", device_id); });
			return Capabilities::ActuationResult::Busy;
		}
		if (existing.target.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		// The desired program must be one the controller can represent -- same feasibility gate as create.
		if (const auto feasibility = Scheduling::CheckControllerCandidate(desired); !feasibility.promotable)
		{
			LogWarning(Channel::Devices, [&device_id, &desired]() { return std::format("IAQ ({}): EditControllerProgram rejected -- desired program is not controller-representable (target='{}')", device_id, desired.target); });
			return Capabilities::ActuationResult::InvalidValue;
		}

		Goal goal;
		goal.op = Op::Edit;
		goal.program = desired;    // the field phases set from goal.program; Verify matches it
		goal.match = existing;     // SelectRow locates the current row by matching `match`
		goal.desc = std::format("edit controller program '{}'", existing.target);
		Arm(std::move(goal), device_id);
		return Capabilities::ActuationResult::Accepted;
	}

	void ScheduleWriter::Arm(Goal goal, const JandyDeviceType& device_id)
	{
		m_Pending = std::move(goal);
		m_Phase = Phase::NavigateToList;
		m_PollCount = 0;
		m_SettleCount = 0;
		m_ScrollCount = 0;
		m_ProgramAdded = false;
		m_DeviceClicked = false;
		m_TimeFieldOpened = false;

		LogInfo(Channel::Devices, [this, &device_id]() { return std::format("IAQ ({}): queued {}", device_id, m_Pending->desc); });
	}

	void ScheduleWriter::IssueAndSettle(ICommandSink& sink, uint8_t cmd)
	{
		sink.IssueCommand(cmd);
		m_SettleCount = IAQ_SCHEDULE_SETTLE_POLLS;
	}

	void ScheduleWriter::FinishGoal(ICommandSink& sink, const JandyDeviceType& device_id, bool ok)
	{
		const Goal& goal = m_Pending.value();
		if (ok) { LogInfo(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} completed", device_id, goal.desc); }); }
		else    { LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} abandoned", device_id, goal.desc); }); }
		sink.IssueCommand(0x00);
		m_Pending.reset();
		m_Phase = Phase::NavigateToList;
		m_ProgramAdded = false;
		m_DeviceClicked = false;
		m_TimeFieldOpened = false;
		m_ScrollCount = 0;
		m_SettleCount = 0;
	}

	// Set one time field of the highlighted program: open it (from the list) -> on the time
	// picker, toggle AM/PM to match then submit the value via the control-data handshake. Emits
	// at most one command per poll and advances to `next` once the submit is issued.
	void ScheduleWriter::SetTimeField(const PageModel& page, const Utility::ScreenDataPage& status_page,
		ICommandSink& sink, uint8_t open_cmd, int hour, int minute, Phase next)
	{
		if (!m_TimeFieldOpened)
		{
			if (page.PageId() == IAQ_SCHEDULE_PAGE_ID) { IssueAndSettle(sink, open_cmd); m_TimeFieldOpened = true; }
			else { sink.IssueCommand(0x00); }   // dwell until the list is up
			return;
		}
		if (page.PageId() != IAQ_TIME_PICKER_PAGE_ID)
		{
			sink.IssueCommand(0x00);   // dwell until the time picker renders
			return;
		}

		// Match AM/PM before submitting (picker line 2 carries the current meridiem).
		const std::string meridiem = Utility::TrimWhitespace(status_page[IAQ_TIME_PICKER_AMPM_LINE].Text);
		const bool is_am = Utility::EqualsCaseInsensitive(meridiem, "AM");
		const bool is_pm = Utility::EqualsCaseInsensitive(meridiem, "PM");
		if (!is_am && !is_pm)
		{
			sink.IssueCommand(0x00);   // picker not fully rendered yet; wait for the meridiem line
			return;
		}
		if (const bool want_pm = hour >= 12; is_pm != want_pm)
		{
			IssueAndSettle(sink, IAQ_CMD_AMPM_TOGGLE);   // flip AM<->PM, then re-read next poll
			return;
		}

		// Meridiem matches: submit. The value ("1"+HH:MM) rides the control-data response the
		// master requests with IAQ_ControlReady (Slot_IAQ_ControlReady) after the 0x80 submit.
		sink.ArmControlValue(ScheduleTimeValue(hour, minute));
		IssueAndSettle(sink, IAQ_CMD_SUBMIT_VALUE);
		m_TimeFieldOpened = false;
		m_Phase = next;
	}

	// Does a parsed list row match the program to LOCATE (delete/edit use `match`, the existing
	// program; create is never in a row-locating phase so `match` is unset there)?
	bool ScheduleWriter::MatchesProgram(const Scheduling::ControllerSchedule& row) const
	{
		const Goal& goal = m_Pending.value();
		return Utility::EqualsCaseInsensitive(row.target, goal.match.target)
			&& row.days_of_week == goal.match.days_of_week
			&& row.on_hour == goal.match.on_hour && row.on_minute == goal.match.on_minute
			&& row.off_hour == goal.match.off_hour && row.off_minute == goal.match.off_minute;
	}

	void ScheduleWriter::StepNavigateToList(const PageModel& page, ICommandSink& sink, const Goal& goal)
	{
		// Page-gated walk to the Schedule list (0x28).
		switch (page.PageId())
		{
		case IAQ_SCHEDULE_PAGE_ID:
			// Arrived on the list: create adds a program; delete/edit find the target row first.
			m_Phase = (goal.op == Op::Create) ? Phase::AddProgram : Phase::SelectRow;
			return;

		case IAQ_PAGE_MENU:
			IssueAndSettle(sink, IAQ_CMD_MENU_TO_SCHEDULE);           // menu pos0 -> Schedule list
			return;

		case IAQ_PAGE_HOME:
		default:
			IssueAndSettle(sink, IAQ_CMD_BACK);                       // unwind toward the menu
			return;
		}
	}

	void ScheduleWriter::StepAddProgram(const PageModel& page, ICommandSink& sink)
	{
		if (page.PageId() != IAQ_SCHEDULE_PAGE_ID)
		{
			m_Phase = Phase::NavigateToList;   // lost the page; re-navigate
			return;
		}
		if (!m_ProgramAdded)
		{
			IssueAndSettle(sink, IAQ_CMD_ADD_PROGRAM);   // pos0 on the list -> Add Program -> device picker (0x38)
			m_ProgramAdded = true;
		}
		m_Phase = Phase::SelectDevice;
	}

	void ScheduleWriter::StepSelectDevice(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id, const Goal& goal)
	{
		if (page.PageId() != IAQ_DEVICE_PICKER_PAGE_ID)
		{
			sink.IssueCommand(0x00);   // dwell until the picker renders
			return;
		}

		// Two-step select once the target is on-screen: click its visible row (0x13 + row) to
		// highlight it, then confirm with the OK key (0x13) -> returns to the list.
		if (m_DeviceClicked)
		{
			IssueAndSettle(sink, IAQ_CMD_PICKER_OK);
			m_Phase = Phase::SetOnTime;
			return;
		}

		// Is the target device visible in the current picker page? (Attribute = the visible row.)
		for (const auto& [row, label] : page.DevicePickerRows())
		{
			if (Utility::EqualsCaseInsensitive(label, goal.program.target))
			{
				IssueAndSettle(sink, static_cast<uint8_t>(IAQ_SCHEDULE_PICK_ROW_BASE + row));   // click the row
				m_DeviceClicked = true;
				return;
			}
		}

		// Not visible: scroll down one page, bounded by IAQ_SCHEDULE_MAX_SCROLLS.
		if (++m_ScrollCount > IAQ_SCHEDULE_MAX_SCROLLS)
		{
			LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} -- target device '{}' not found after scrolling the picker", device_id, goal.desc, goal.program.target); });
			FinishGoal(sink, device_id, false);
			return;
		}
		IssueAndSettle(sink, IAQ_CMD_PICKER_SCROLL);
	}

	void ScheduleWriter::StepSetDay(const PageModel& page, ICommandSink& sink, const Goal& goal)
	{
		if (page.PageId() != IAQ_SCHEDULE_PAGE_ID)
		{
			sink.IssueCommand(0x00);   // dwell until the master renders the list with the new program
			return;
		}
		IssueAndSettle(sink, DayCommandFor(goal.program.days_of_week));
		m_Phase = Phase::Verify;
	}

	void ScheduleWriter::StepVerify(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id, const Goal& goal)
	{
		// The new program is present once the parsed schedule list carries a row for the target
		// device on the requested day. (Times are set by a later increment; a freshly-created
		// program defaults to 1:00 PM / 1:00 PM until then.)
		for (const auto& [ordinal, text] : page.ScheduleRows())
		{
			if (const auto parsed = IAQ::ParseScheduleRow(text);
				parsed.has_value()
				&& Utility::EqualsCaseInsensitive(parsed->target, goal.program.target)
				&& parsed->days_of_week == goal.program.days_of_week
				&& parsed->on_hour == goal.program.on_hour && parsed->on_minute == goal.program.on_minute
				&& parsed->off_hour == goal.program.off_hour && parsed->off_minute == goal.program.off_minute)
			{
				FinishGoal(sink, device_id, true);
				return;
			}
		}
		sink.IssueCommand(0x00);   // dwell until the list re-renders (or the poll backstop fires)
	}

	void ScheduleWriter::StepSelectRow(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id, const Goal& goal)
	{
		if (page.PageId() != IAQ_SCHEDULE_PAGE_ID)
		{
			sink.IssueCommand(0x00);   // dwell until the list renders
			return;
		}
		if (page.ScheduleRows().empty())
		{
			sink.IssueCommand(0x00);   // list not populated yet; wait (poll backstop bounds it)
			return;
		}
		// Click the row whose parsed contents match the program to locate, then branch: delete
		// presses Delete on the highlighted row, edit presses Edit to enter its field editor.
		for (const auto& [ordinal, text] : page.ScheduleRows())
		{
			if (const auto parsed = IAQ::ParseScheduleRow(text); parsed.has_value() && MatchesProgram(parsed.value()))
			{
				IssueAndSettle(sink, static_cast<uint8_t>(IAQ_SCHEDULE_ROW_BASE + ordinal));   // highlight the target row
				m_Phase = (goal.op == Op::Edit) ? Phase::PressEdit : Phase::PressDelete;
				return;
			}
		}
		LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} -- no matching program row to {} (target='{}')", device_id, goal.desc, (goal.op == Op::Edit) ? "edit" : "delete", goal.match.target); });
		FinishGoal(sink, device_id, false);
	}

	void ScheduleWriter::StepPressEdit(const PageModel& page, ICommandSink& sink)
	{
		if (page.PageId() != IAQ_SCHEDULE_PAGE_ID)
		{
			sink.IssueCommand(0x00);   // dwell until the list (with the highlighted row) renders
			return;
		}
		IssueAndSettle(sink, IAQ_CMD_EDIT_PROGRAM);   // Edit -> enter the highlighted row's field-edit mode
		// Re-set the fields from the desired program with the same phases the create flow uses.
		m_Phase = Phase::SetOnTime;
	}

	void ScheduleWriter::StepPressDelete(const PageModel& page, ICommandSink& sink)
	{
		if (page.PageId() != IAQ_SCHEDULE_PAGE_ID)
		{
			sink.IssueCommand(0x00);
			return;
		}
		IssueAndSettle(sink, IAQ_CMD_DELETE_PROGRAM);   // Delete -> the master raises the confirm dialog
		m_Phase = Phase::ConfirmDelete;
	}

	void ScheduleWriter::StepConfirmDelete(ICommandSink& sink)
	{
		IssueAndSettle(sink, IAQ_CMD_CONFIRM_OK);   // Ok on the confirm dialog -> removes the program
		m_Phase = Phase::VerifyGone;
	}

	void ScheduleWriter::StepVerifyGone(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id)
	{
		// Complete once the target program is no longer present in the parsed list.
		for (const auto& [ordinal, text] : page.ScheduleRows())
		{
			if (const auto parsed = IAQ::ParseScheduleRow(text); parsed.has_value() && MatchesProgram(parsed.value()))
			{
				sink.IssueCommand(0x00);   // still listed; dwell until the list re-renders
				return;
			}
		}
		FinishGoal(sink, device_id, true);
	}

	void ScheduleWriter::ProcessStep(const PageModel& page, const Utility::ScreenDataPage& status_page,
		ICommandSink& sink, const JandyDeviceType& device_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("ScheduleWriter::ProcessStep", std::source_location::current());

		if (!m_Pending.has_value())
		{
			return;
		}
		const Goal& goal = m_Pending.value();

		// Overall backstop.
		if (++m_PollCount > IAQ_SCHEDULE_POLL_LIMIT)
		{
			FinishGoal(sink, device_id, false);
			return;
		}

		// Settle: dwell a few polls after a command so the master renders the new page.
		if (m_SettleCount > 0)
		{
			--m_SettleCount;
			sink.IssueCommand(0x00);
			return;
		}

		switch (m_Phase)
		{
		case Phase::NavigateToList:  StepNavigateToList(page, sink, goal); return;
		case Phase::AddProgram:      StepAddProgram(page, sink); return;
		case Phase::SelectDevice:    StepSelectDevice(page, sink, device_id, goal); return;

		case Phase::SetOnTime:
			SetTimeField(page, status_page, sink, IAQ_CMD_OPEN_ON_FIELD, goal.program.on_hour, goal.program.on_minute, Phase::SetOffTime);
			return;

		case Phase::SetOffTime:
			SetTimeField(page, status_page, sink, IAQ_CMD_OPEN_OFF_FIELD, goal.program.off_hour, goal.program.off_minute, Phase::SetDay);
			return;

		case Phase::SetDay:          StepSetDay(page, sink, goal); return;
		case Phase::Verify:          StepVerify(page, sink, device_id, goal); return;
		case Phase::SelectRow:       StepSelectRow(page, sink, device_id, goal); return;
		case Phase::PressEdit:       StepPressEdit(page, sink); return;
		case Phase::PressDelete:     StepPressDelete(page, sink); return;
		case Phase::ConfirmDelete:   StepConfirmDelete(sink); return;
		case Phase::VerifyGone:      StepVerifyGone(page, sink, device_id); return;

		case Phase::Done:
		case Phase::Failed:
		default:
			FinishGoal(sink, device_id, m_Phase == Phase::Done);
			return;
		}
	}

}
// namespace AqualinkAutomate::Devices::IAQ
