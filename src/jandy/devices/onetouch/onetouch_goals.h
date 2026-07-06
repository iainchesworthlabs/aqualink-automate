#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "devices/onetouch/onetouch_keypad.h"
#include "navigation/menu_model.h"

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

		bool m_Start;   // true = start boost, false = stop
		std::string m_Desc;
		Phase m_Phase{ Phase::Navigating };
		bool m_Started{ false };
		uint32_t m_StepCount{ 0 };
	};

}
// namespace AqualinkAutomate::Devices::OneTouch
