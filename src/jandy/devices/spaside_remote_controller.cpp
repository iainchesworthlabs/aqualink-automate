#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "devices/spaside_remote_controller.h"
#include "devices/spaside_remote_device.h"
#include "devices/capabilities/spa_switch_configurator.h"
#include "interfaces/idevice.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	SpasideRemoteController::SpasideRemoteController(std::shared_ptr<Kernel::EquipmentHub> equipment_hub) :
		m_EquipmentHub(std::move(equipment_hub))
	{
	}

	std::vector<Interfaces::ISpasideRemoteController::RemoteState> SpasideRemoteController::Remotes() const
	{
		std::vector<RemoteState> remotes;

		if (nullptr == m_EquipmentHub)
		{
			return remotes;
		}

		m_EquipmentHub->ForEachDevice([&remotes](Interfaces::IDevice& device)
		{
			const auto* spaside = dynamic_cast<const SpasideRemoteDevice*>(&device);
			if (nullptr == spaside)
			{
				return;
			}

			RemoteState state;
			state.address = spaside->DeviceId().Id()();
			state.device_class = std::string(magic_enum::enum_name(spaside->DeviceId().Class()));
			state.emulated = spaside->IsEmulated();
			state.button_count = spaside->ButtonCount();
			state.poll_count = spaside->PollCount();
			state.last_button = spaside->LastButton();
			state.led_image_seen = spaside->LedImageSeen();
			state.leds = spaside->LedStates();
			state.led_image = spaside->LedImageHex();

			// The device owns the protocol knowledge of how each physical key maps to a config
			// switch:button (the web layer must not encode it). Copy it into the plain interface type.
			for (const auto& button : spaside->ButtonLayout())
			{
				Interfaces::ISpasideRemoteController::Button b;
				b.index = button.index;
				b.switch_number = button.switch_number;
				b.button_number = button.button_number;
				b.assignable = button.assignable;
				state.buttons.push_back(b);
			}

			remotes.push_back(std::move(state));
		});

		return remotes;
	}

	Interfaces::ISpasideRemoteController::PressResult SpasideRemoteController::PressButton(uint8_t address, uint8_t button_index)
	{
		if (nullptr == m_EquipmentHub)
		{
			return PressResult::RemoteNotFound;
		}

		SpasideRemoteDevice* target{ nullptr };
		m_EquipmentHub->ForEachDevice([&target, address](Interfaces::IDevice& device)
		{
			auto* spaside = dynamic_cast<SpasideRemoteDevice*>(&device);
			if ((nullptr != spaside) && (spaside->DeviceId().Id()() == address))
			{
				target = spaside;
			}
		});

		if (nullptr == target)
		{
			return PressResult::RemoteNotFound;
		}

		// Only an emulated remote can be actuated: for a passively-observed real remote WE are not
		// the transmitter (the press would be silently dropped by Capabilities::Emulated), so reject
		// it explicitly rather than pretend it worked.
		if (!target->IsEmulated())
		{
			return PressResult::NotEmulated;
		}

		// Buttons are 1..button_count (0 is the idle/release code, not a press).
		if ((button_index < 1) || (button_index > target->ButtonCount()))
		{
			return PressResult::InvalidButton;
		}

		LogInfo(Channel::Web, std::format("SpasideRemoteController: injecting press of button {} on emulated remote 0x{:02x}", button_index, address));
		target->PressButton(button_index);
		return PressResult::Success;
	}

	Interfaces::ISpasideRemoteController::AssignResult SpasideRemoteController::SetButtonAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function)
	{
		// Programming is a CONTROLLER-config operation keyed by the controller's own switch
		// numbering (1..3) and button numbering (1..N), independent of which spa switches are
		// bus-attached -- so validate against those, not against any decoded remote.
		if ((switch_number < 1) || (switch_number > MAX_SWITCH_NUMBER) ||
			(button_number < 1) || (button_number > MAX_BUTTON_NUMBER) ||
			function.empty())
		{
			return AssignResult::InvalidRequest;
		}

		if (nullptr == m_EquipmentHub)
		{
			return AssignResult::NotAvailable;
		}

		// Collect every controller that can program assignments (OneTouch / iAQ) and order them by
		// ControllerPriority (lowest value wins, mirroring CommandDispatcher::FindCapable). We then
		// try them in order and FALL THROUGH past any that returns NotSupported: a higher-priority
		// controller that is present but cannot perform THIS action (e.g. an iAQ whose per-button
		// function-write isn't decoded) must not shadow a lower-priority one that can (the OneTouch).
		std::vector<std::pair<Capabilities::ActuationPriority, Capabilities::SpaSwitchConfigurator*>> configurators;
		m_EquipmentHub->ForEachDevice([&configurators](Interfaces::IDevice& device)
		{
			if (auto* candidate = dynamic_cast<Capabilities::SpaSwitchConfigurator*>(&device); nullptr != candidate)
			{
				configurators.emplace_back(candidate->ControllerPriority(), candidate);
			}
		});

		if (configurators.empty())
		{
			LogWarning(Channel::Web, std::format("SpasideRemoteController: no controller can program spa-switch assignments (switch {} button {} -> '{}')", switch_number, button_number, function));
			return AssignResult::NotAvailable;
		}

		std::ranges::stable_sort(configurators,
			[](const auto& a, const auto& b) { return static_cast<int>(a.first) < static_cast<int>(b.first); });

		LogInfo(Channel::Web, std::format("SpasideRemoteController: programming switch {} button {} -> '{}' ({} candidate controller(s))", switch_number, button_number, function, configurators.size()));
		for (const auto& [priority, configurator] : configurators)
		{
			switch (configurator->SetSpaSwitchAssignment(switch_number, button_number, function))
			{
			case Capabilities::ActuationResult::Accepted:      return AssignResult::Accepted;
			case Capabilities::ActuationResult::InvalidValue:
			case Capabilities::ActuationResult::MappingFailed: return AssignResult::InvalidRequest;
			case Capabilities::ActuationResult::NotSupported:
			default:
				// This controller can't do it; try the next-lower-priority configurator.
				continue;
			}
		}

		// Every present controller reported NotSupported (e.g. an iAQ-only system whose function-write
		// isn't decoded, or no emulated controller to transmit) -- the action is genuinely unavailable.
		LogWarning(Channel::Web, std::format("SpasideRemoteController: no controller could program switch {} button {} -> '{}' (all reported NotSupported)", switch_number, button_number, function));
		return AssignResult::NotAvailable;
	}

	std::vector<std::string> SpasideRemoteController::AvailableFunctions() const
	{
		// Union the assignable-function lists of every connected configurator (OneTouch / iAQ),
		// ordered by ControllerPriority (lowest first, as in SetButtonAssignment), deduping while
		// preserving first-seen order. Both currently return the same canonical list, so in practice
		// this yields that list; the dedup keeps it stable if a controller ever reports a richer set.
		std::vector<std::string> functions;

		if (nullptr == m_EquipmentHub)
		{
			return functions;
		}

		std::vector<std::pair<Capabilities::ActuationPriority, Capabilities::SpaSwitchConfigurator*>> configurators;
		m_EquipmentHub->ForEachDevice([&configurators](Interfaces::IDevice& device)
		{
			if (auto* candidate = dynamic_cast<Capabilities::SpaSwitchConfigurator*>(&device); nullptr != candidate)
			{
				configurators.emplace_back(candidate->ControllerPriority(), candidate);
			}
		});

		std::ranges::stable_sort(configurators,
			[](const auto& a, const auto& b) { return static_cast<int>(a.first) < static_cast<int>(b.first); });

		for (const auto& [priority, configurator] : configurators)
		{
			for (auto& fn : configurator->AvailableFunctions())
			{
				if (std::ranges::find(functions, fn) == functions.end())
				{
					functions.push_back(std::move(fn));
				}
			}
		}

		return functions;
	}

}
// namespace AqualinkAutomate::Devices
