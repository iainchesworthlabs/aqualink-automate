#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "devices/onetouch/onetouch_keypad.h"
#include "navigation/menu_model.h"
#include "scheduling/controller_schedule.h"

namespace AqualinkAutomate::Devices::OneTouch
{

	// Toggle a single pool device (DeviceActuator): drive the Navigator to the Equipment ON/OFF
	// page, find the row whose label matches the target, and Select it in place. The select_target
	// is the Equipment ON/OFF page itself because an in-place toggle keeps us on that page (the row
	// re-renders) rather than transitioning elsewhere.
	class ToggleGoal : public IKeypadGoal
	{
	public:
		explicit ToggleGoal(std::string label);

		GoalStatus Step(KeypadContext& ctx) override;
		std::string_view Description() const override { return m_Desc; }

	private:
		// Frame backstop so a mis-detected page can never wedge NormalOperation (the Navigator's
		// own timeouts normally drive it to Failed first).
		static constexpr uint32_t STEP_LIMIT{ 500 };

		std::string m_Label;
		std::string m_Desc;
		bool m_Started{ false };
		uint32_t m_StepCount{ 0 };
	};

	// On-screen VALUE EDITOR (SetpointController + chlorinator %): the Navigator positions the
	// cursor on the goal's value row (no Select), then this goal drives the in-place editor
	// directly - Select to begin, arrow keys to step the displayed value toward the target, Select
	// to commit. Used for heater setpoints (Set Temperature, 1-degree steps) and chlorinator %
	// (Set AquaPure, 5% steps) alike - only the page/row/target/units differ.
	class ValueEditGoal : public IKeypadGoal
	{
	public:
		ValueEditGoal(Navigation::PageId page, uint8_t line, std::string label, int target, std::string desc);

		GoalStatus Step(KeypadContext& ctx) override;
		std::string_view Description() const override { return m_Desc; }

	private:
		enum class Phase
		{
			Navigating,   // Navigator positioning the cursor on the value row
			BeginEdit,    // Select pressed to enter the in-place value editor
			Stepping,     // arrow keys stepping the displayed value toward the target
			Commit        // Select pressed again to commit the value and leave the editor
		};

		static constexpr uint32_t STEP_LIMIT{ 500 };

		Navigation::PageId m_Page;
		uint8_t m_Line;         // row index of the value (cursor target + value read-back)
		std::string m_Label;    // row label for content-based cursor positioning
		int m_Target;           // target value in the on-screen units
		std::string m_Desc;
		Phase m_Phase{ Phase::Navigating };
		bool m_Started{ false };
		uint32_t m_StepCount{ 0 };
	};

	// Chlorinator BOOST start/stop (ChlorinatorController) via the Boost Pool page: Select on the
	// idle "Operate at 100%" page starts it; on a running page, navigate to the "Stop" item and
	// Select. The page state ("Time Remaining" vs "Operate ... at 100%") decides whether an action
	// is actually needed.
	class BoostGoal : public IKeypadGoal
	{
	public:
		explicit BoostGoal(bool start);

		GoalStatus Step(KeypadContext& ctx) override;
		std::string_view Description() const override { return m_Desc; }

	private:
		enum class Phase
		{
			Navigating,   // Navigator driving to the Boost Pool page
			Acting,       // walking the cursor to the Stop item and Selecting (stop path)
			Settle        // the start Select has been queued; one-shot, done
		};

		static constexpr uint32_t STEP_LIMIT{ 500 };

		// The Boost Pool page shows "Time Remaining" while a boost is running.
		static bool BoostIsRunning(const KeypadContext& ctx);
		// Per-phase handlers: nullopt = keep running (fall through to GoalStatus::Running).
		std::optional<GoalStatus> HandleNavigating(KeypadContext& ctx);

		bool m_Start;   // true = start boost, false = stop
		std::string m_Desc;
		Phase m_Phase{ Phase::Navigating };
		bool m_Started{ false };
		uint32_t m_StepCount{ 0 };
	};

	// SPA-SWITCH ASSIGNMENT edit (SpaSwitchConfigurator): walk the Spa Switch config menu (System
	// Setup -> Spa Switch -> the number-of-switches page -> Button Setup list -> the "S:B" row ->
	// the function picker) and cycle the picker until it shows the target function, then Select.
	// Screen-driven after System Setup because the number-of-switches page must be passed with a
	// bare Select (no cursor move) so the switch count is preserved.
	class SpaSwitchGoal : public IKeypadGoal
	{
	public:
		SpaSwitchGoal(uint8_t switch_number, uint8_t button_number, std::string function);

		GoalStatus Step(KeypadContext& ctx) override;
		std::string_view Description() const override { return m_Desc; }

	private:
		enum class Phase
		{
			ToSystemSetup,    // Navigator drives to the System Setup menu
			SelectSpaSwitch,  // cursor to the "Spa Switch" item, then Select -> number page
			PassNumberPage,   // bare Select on the number-of-switches page -> Button Setup list
			ToRow,            // cursor to the "S:B" row, then Select -> function picker
			CyclePicker,      // LineUp-cycle the picker until it shows the target function
			Commit            // Select to write the function and leave the picker
		};

		static constexpr uint32_t STEP_LIMIT{ 800 };   // menu walk + up to a full picker cycle
		static constexpr uint32_t MAX_SCROLL{ 40 };
		static constexpr uint8_t PICKER_FUNCTION_LINE{ 3 };

		// Per-phase handlers: nullopt = keep running (fall through to GoalStatus::Running).
		std::optional<GoalStatus> HandleToSystemSetup(KeypadContext& ctx);
		std::optional<GoalStatus> HandleSelectSpaSwitch(KeypadContext& ctx);
		std::optional<GoalStatus> HandlePassNumberPage(KeypadContext& ctx);
		std::optional<GoalStatus> HandleToRow(KeypadContext& ctx);
		std::optional<GoalStatus> HandleCyclePicker(KeypadContext& ctx);

		uint8_t m_SwitchNumber;
		uint8_t m_ButtonNumber;
		std::string m_Function;   // target function name (as the controller's picker lists it)
		std::string m_RowTag;     // "<switch>:<button>" -- the Button Setup row label
		std::string m_Desc;
		Phase m_Phase{ Phase::ToSystemSetup };
		bool m_Started{ false };
		uint32_t m_StepCount{ 0 };
		uint32_t m_CursorStuck{ 0 };
		std::optional<std::string> m_PickerFirstSeen;   // wrap detection while cycling the picker
	};

	// Which controller-schedule write a ScheduleWriteGoal performs.
	enum class ScheduleWriteOp
	{
		Create,   // add a new program on an equipment (Add Program -> editor)
		Delete,   // remove an equipment's program (Delete Program row -> immediate, no confirm)
		Edit,     // change an equipment's program (Change Program -> editor, pre-filled)
	};

	// Controller-schedule WRITE (ControllerScheduleWriter): walk the Program menu to the target
	// equipment's detail page, then drive the Add/Change editor (closed-loop field stepping, the
	// active field tracked by the phase progression since the editor has NO field cursor) or Select
	// the Delete row, and finally re-parse the returned detail page to Verify. Screen-driven after
	// the Program menu. RE'd from captures/onetouch_program.cap; see docs/onetouch_schedule_protocol.md.
	class ScheduleWriteGoal : public IKeypadGoal
	{
	public:
		ScheduleWriteGoal(ScheduleWriteOp op, Scheduling::ControllerSchedule program, std::string desc);

		GoalStatus Step(KeypadContext& ctx) override;
		std::string_view Description() const override { return m_Desc; }

	private:
		enum class Phase
		{
			ToProgramMenu,   // Navigator drives to the Program equipment-list page
			SelectEquipment, // scroll the list to the target equipment, cursor onto it, Select
			ChooseAction,    // on the detail page, cursor onto Add/Change and Select, or Delete row
			EnterEditor,     // wait for the editor to render, then begin field entry
			SetOnHour,       // closed-loop step the ON hour, Select to advance
			SetOnMinute,     // closed-loop step the ON minute, Select to advance
			SetOffHour,      // closed-loop step the OFF hour, Select to advance
			SetOffMinute,    // closed-loop step the OFF minute, Select to advance
			SetDays,         // step the days wheel, Select -> SAVES + returns to detail
			Verify,          // re-parse the returned detail page; confirm the program is present
			VerifyGone,      // (delete) confirm the detail page now shows "No Programs"
		};

		static constexpr uint32_t STEP_LIMIT{ 900 };   // menu walk + list scroll + full field entry
		static constexpr uint32_t MAX_STEP{ 40 };      // per-field wheel-step / list-scroll bound
		// Editor line layout (verified vs captures/onetouch_program.cap): title on 1, ON on 3, OFF
		// on 4, days on 5. Detail-page action rows: Add on 9, Delete on 10, Change on 11.
		static constexpr uint8_t TITLE_LINE{ 1 };
		static constexpr uint8_t ON_LINE{ 3 };
		static constexpr uint8_t OFF_LINE{ 4 };
		static constexpr uint8_t DAYS_LINE{ 5 };
		static constexpr uint8_t ADD_ROW{ 9 };
		static constexpr uint8_t DELETE_ROW{ 10 };
		static constexpr uint8_t CHANGE_ROW{ 11 };

		// True when the current page is the per-equipment Program detail page (vs the equipment LIST).
		bool OnDetailPage(const KeypadContext& ctx) const;
		// True when the current page is the Add/Change editor.
		bool OnEditorPage(const KeypadContext& ctx) const;
		// Read the on-screen ON/OFF time row into 24-hour (hour, minute); reuses the read-path parser.
		std::optional<std::pair<int, int>> DisplayedTime(const KeypadContext& ctx, uint8_t line) const;
		// Read the on-screen days row into a DayMask value; reuses the read-path parser.
		std::optional<uint8_t> DisplayedDays(const KeypadContext& ctx, uint8_t line) const;
		// Closed-loop hour/minute wheel steppers: nullopt = keep running, Failed = abandon.
		std::optional<GoalStatus> StepHour(KeypadContext& ctx, uint8_t line, int target_hour, Phase next);
		std::optional<GoalStatus> StepMinute(KeypadContext& ctx, uint8_t line, int target_minute, Phase next);

		// Per-phase handlers: nullopt = keep running (fall through to GoalStatus::Running).
		std::optional<GoalStatus> HandleToProgramMenu(KeypadContext& ctx);
		std::optional<GoalStatus> HandleSelectEquipment(KeypadContext& ctx);
		std::optional<GoalStatus> HandleChooseAction(KeypadContext& ctx);
		std::optional<GoalStatus> HandleSetDays(KeypadContext& ctx);
		std::optional<GoalStatus> HandleVerify(KeypadContext& ctx);
		std::optional<GoalStatus> HandleVerifyGone(KeypadContext& ctx);

		ScheduleWriteOp m_Op;
		Scheduling::ControllerSchedule m_Program;   // the program to write (create/edit) / locate (delete)
		std::string m_Desc;
		Phase m_Phase{ Phase::ToProgramMenu };
		bool m_Started{ false };
		uint32_t m_StepCount{ 0 };   // overall frame backstop
		uint32_t m_FieldStep{ 0 };   // per-field wheel-step / list-scroll bound
	};

}
// namespace AqualinkAutomate::Devices::OneTouch
