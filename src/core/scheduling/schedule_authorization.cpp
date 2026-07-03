#include <format>
#include <string_view>
#include <vector>

#include <boost/uuid/uuid_io.hpp>

#include "auth/entitlement_vocabulary.h"
#include "auth/policy_engine.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/data_hub.h"
#include "scheduling/schedule_authorization.h"

namespace AqualinkAutomate::Scheduling
{

	namespace
	{
		// The control-type entitlement a non-button action requires.
		std::string_view ActionEntitlement(ActionType type)
		{
			switch (type)
			{
			case ActionType::PoolSetpoint:
			case ActionType::SpaSetpoint:
				return Auth::Vocabulary::EQUIPMENT_CONTROL_SETPOINTS;

			case ActionType::ChlorinatorPercent:
				return Auth::Vocabulary::EQUIPMENT_CONTROL_CHLORINATOR;

			case ActionType::CirculationMode:
				return Auth::Vocabulary::EQUIPMENT_CONTROL_CIRCULATION;

			default:
				return Auth::Vocabulary::EQUIPMENT_CONTROL_AUX;
			}
		}

		bool IsButtonAction(ActionType type)
		{
			return (ActionType::ButtonOn == type) || (ActionType::ButtonOff == type) || (ActionType::ButtonToggle == type);
		}
	}
	// anonymous namespace

	bool AuthorizeScheduleAction(const Auth::Subject& subject, const Schedule& schedule, const Kernel::DataHub& data_hub, bool auth_enabled, std::string& error)
	{
		const Auth::Environment environment{ .AuthEnabled = auth_enabled };

		const auto& action = schedule.action;
		const auto entitlement = ActionEntitlement(action.type);

		if (IsButtonAction(action.type))
		{
			// Resolve the target label to the device id(s) it names — the same
			// identifier the live button route gates on — and require the
			// subject to be entitled for EVERY match.  An unresolved label is a
			// schedule targeting an unknown device: fail closed.
			const auto devices = data_hub.Devices.FindByLabel(action.target);

			if (devices.empty())
			{
				// Posture off still means "no gate"; only reject under auth-mode.
				if (!auth_enabled)
				{
					return true;
				}

				error = std::format("schedule targets an unknown device '{}'", action.target);
				return false;
			}

			for (const auto& device : devices)
			{
				const auto device_id = boost::uuids::to_string(device->Id());
				const Auth::ResourceRef resource{ .Kind = "aux", .Id = device_id };

				if (Auth::Decision::Permit != Auth::PolicyEngine::Decide(subject, entitlement, resource, environment))
				{
					error = std::format("not entitled to control '{}' — a schedule may only perform actions you can perform directly", action.target);
					return false;
				}
			}

			return true;
		}

		// Non-button actions carry no per-resource grain in v1.
		if (Auth::Decision::Permit != Auth::PolicyEngine::Decide(subject, entitlement, {}, environment))
		{
			error = std::format("not entitled to '{}' — a schedule may only perform actions you can perform directly", entitlement);
			return false;
		}

		return true;
	}

}
// namespace AqualinkAutomate::Scheduling
