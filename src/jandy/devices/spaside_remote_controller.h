#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "interfaces/ispasideremotecontroller.h"
#include "kernel/equipment_hub.h"

namespace AqualinkAutomate::Devices
{

	// Concrete spa-side remote control surface. Lives in the Jandy layer because it must
	// dynamic_cast the EquipmentHub's devices to the concrete SpasideRemoteDevice (core cannot
	// see Jandy types). Registered in the HubLocator as an Interfaces::ISpasideRemoteController so
	// the /api/equipment/spaside-remotes web route -- which only knows the core interface -- can
	// read decoded state and inject button presses on emulated remotes.
	class SpasideRemoteController : public Interfaces::ISpasideRemoteController
	{
	public:
		explicit SpasideRemoteController(std::shared_ptr<Kernel::EquipmentHub> equipment_hub);
		~SpasideRemoteController() override = default;

		std::vector<RemoteState> Remotes() const override;
		PressResult PressButton(uint8_t address, uint8_t button_index) override;
		AssignResult SetButtonAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function) override;
		std::vector<std::string> AvailableFunctions() const override;

	private:
		// Sanity bounds for a config request (the controllers number switches 1..3 and buttons
		// 1..N small); the chosen controller validates the precise function/button.
		static constexpr uint8_t MAX_SWITCH_NUMBER{ 3 };
		static constexpr uint8_t MAX_BUTTON_NUMBER{ 16 };

		std::shared_ptr<Kernel::EquipmentHub> m_EquipmentHub;
	};

}
// namespace AqualinkAutomate::Devices
