#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "devices/capabilities/actuation_types.h"
#include "scheduling/controller_schedule.h"

namespace AqualinkAutomate::Devices { class JandyDeviceType; }
namespace AqualinkAutomate::Utility { class ScreenDataPage; }

namespace AqualinkAutomate::Devices::IAQ
{
	class PageModel;
	class ICommandSink;

	// The controller-schedule WRITE state machine for the AqualinkTouch (0x33) -- create, delete or
	// edit a program in the controller's active schedule group -- extracted from IAQDevice
	// (SonarCloud S1820). Drives the Program pages one command per poll, page-gated so a navigation
	// miss can never act on the wrong page: navigate to the Schedule list (0x28) -> Add Program /
	// locate row -> device picker (0x38) -> set ON/OFF times (time picker 0x29 + value-submit
	// handshake) and day. RE'd from captures/iaq_schedule_{session,clean,picker}.cap +
	// iaq_editdelete.cap; see docs/iaq_schedule_protocol.md and docs/iaq_device_decomposition.md.
	class ScheduleWriter
	{
	public:
		// Arm create/delete/edit (one goal at a time). `emulated` = the panel actively emulates (a
		// passive decoder never transmits -> NotSupported); `channel_busy` = the command channel or
		// another writer is occupied (-> NotSupported). Create/Edit gate feasibility with
		// Scheduling::CheckControllerCandidate (-> InvalidValue). Accepted == queued, not done.
		Capabilities::ActuationResult QueueCreate(const Scheduling::ControllerSchedule& program,
			bool emulated, bool channel_busy, const JandyDeviceType& device_id);
		Capabilities::ActuationResult QueueDelete(const Scheduling::ControllerSchedule& program,
			bool emulated, bool channel_busy, const JandyDeviceType& device_id);
		Capabilities::ActuationResult QueueEdit(const Scheduling::ControllerSchedule& existing,
			const Scheduling::ControllerSchedule& desired, bool emulated, bool channel_busy,
			const JandyDeviceType& device_id);

		// Is a goal currently in flight?
		bool HasPendingGoal() const { return m_Pending.has_value(); }

		// Service the pending goal: inspect the current page + decoded rows and emit at most one
		// command into `sink` this poll, page-gated on `page`. `status_page` supplies the time
		// picker's AM/PM line (the meridiem the writer matches before submitting a time).
		void ProcessStep(const PageModel& page, const Utility::ScreenDataPage& status_page,
			ICommandSink& sink, const JandyDeviceType& device_id);

	private:
		enum class Op
		{
			Create,   // add a new program (device -> times -> day)
			Delete,   // remove an existing program (click its row -> Delete -> Ok)
			Edit,     // change an existing program (click its row -> Edit -> times -> day)
		};

		enum class Phase
		{
			NavigateToList,  // page-gated walk to the Schedule list (0x28)
			AddProgram,      // press Add Program (0x11) -> device picker (0x38)
			SelectDevice,    // scroll the picker until the target device is visible, click its row, confirm
			SetOnTime,       // open the ON field (0x21) -> time picker -> AM/PM toggle + submit "1"+HH:MM
			SetOffTime,      // open the OFF field (0x22) -> time picker -> AM/PM toggle + submit
			SetDay,          // on the list, press the day key (0x17-0x20) for the desired selection
			Verify,          // confirm the new program is present in the parsed list
			SelectRow,       // (delete/edit) click the target program's row (0x22 + ordinal) to highlight it
			PressDelete,     // (delete) press Delete (0x13) -> the confirm dialog
			ConfirmDelete,   // (delete) press Ok (0x01) on the confirm dialog
			VerifyGone,      // (delete) confirm the program is no longer in the parsed list
			PressEdit,       // (edit) press Edit (0x12) -> enter the highlighted row's edit mode
			Done,
			Failed,
		};

		struct Goal
		{
			Op op{ Op::Create };
			Scheduling::ControllerSchedule program;  // desired device + on/off + day mask to write (create/edit)
			Scheduling::ControllerSchedule match;    // the existing program to locate (delete/edit): its row is clicked
			std::string desc;
		};

		// Arm the goal and reset the per-goal state.
		void Arm(Goal goal, const JandyDeviceType& device_id);

		// Emit `cmd` and dwell IAQ_SCHEDULE_SETTLE_POLLS polls so the master can render (was the
		// `issue` lambda in ProcessStep).
		void IssueAndSettle(ICommandSink& sink, uint8_t cmd);

		// Finish the pending goal: log ok/abandoned, clear the ACK and reset the per-goal state (was
		// the `finish` lambda in ProcessStep).
		void FinishGoal(ICommandSink& sink, const JandyDeviceType& device_id, bool ok);

		// Set one time field of the highlighted program: open it (from the list) then, on the time
		// picker, match AM/PM and submit the value via the control-data handshake, advancing to `next`
		// once the submit is issued (was the `set_time` lambda in ProcessStep).
		void SetTimeField(const PageModel& page, const Utility::ScreenDataPage& status_page,
			ICommandSink& sink, uint8_t open_cmd, int hour, int minute, Phase next);

		// Does a parsed list row match the pending goal's `match` program (delete/edit row locate)?
		bool MatchesProgram(const Scheduling::ControllerSchedule& row) const;

		// Per-phase step handlers (each was a `case` body of ProcessStep's `switch (m_Phase)`); each
		// emits at most one command into `sink` this poll and advances `m_Phase`.
		void StepNavigateToList(const PageModel& page, ICommandSink& sink, const Goal& goal);
		void StepAddProgram(const PageModel& page, ICommandSink& sink);
		void StepSelectDevice(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id, const Goal& goal);
		void StepSetDay(const PageModel& page, ICommandSink& sink, const Goal& goal);
		void StepVerify(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id, const Goal& goal);
		void StepSelectRow(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id, const Goal& goal);
		void StepPressEdit(const PageModel& page, ICommandSink& sink);
		void StepPressDelete(const PageModel& page, ICommandSink& sink);
		void StepConfirmDelete(ICommandSink& sink);
		void StepVerifyGone(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id);

		std::optional<Goal> m_Pending;
		Phase m_Phase{ Phase::NavigateToList };
		uint32_t m_PollCount{ 0 };        // overall backstop
		uint32_t m_SettleCount{ 0 };      // polls to let the master render after a command
		uint32_t m_ScrollCount{ 0 };      // bound the device-picker scroll search
		bool m_ProgramAdded{ false };     // the Add-Program press has been issued
		bool m_DeviceClicked{ false };    // the target device row has been clicked (awaiting the OK confirm)
		bool m_TimeFieldOpened{ false };  // the current time field's open press (0x21/0x22) has been issued
	};

}
// namespace AqualinkAutomate::Devices::IAQ
