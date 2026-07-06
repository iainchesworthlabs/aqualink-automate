#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "devices/onetouch/onetouch_keypad.h"

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

}
// namespace AqualinkAutomate::Devices::OneTouch
