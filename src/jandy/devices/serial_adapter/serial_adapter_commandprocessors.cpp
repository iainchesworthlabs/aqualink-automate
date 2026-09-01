#include <format>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_reconciliation.h"
#include "auxillaries/jandy_auxillary_span.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "devices/serial_adapter_device.h"
#include "factories/jandy_auxillary_factory.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/hub_events/data_hub_config_event_button_state_change.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{
	void SerialAdapterDevice::Command_SerialAdapter_Model(const uint16_t model_type)
	{
		// Surface the panel model the RS Serial Adapter reports so the start-up coordinator (and
		// the diagnostics UI) can identify the controller without a menu scrape. The numeric
		// model-code -> human-name mapping is still TODO; store the raw code so it is at least
		// present and comparable.
		if (JandyController::m_DataHub != nullptr)
		{
			JandyController::m_DataHub->EquipmentVersions.Set("Model", std::format("0x{:04x}", model_type));
		}
	}

	void SerialAdapterDevice::Command_SerialAdapter_OpMode(const Messages::SerialAdapter_SCS_OpModes& op_mode)
	{
		switch (op_mode)
		{
		case Messages::SerialAdapter_SCS_OpModes::Auto:
			JandyController::m_DataHub->Mode = Kernel::EquipmentMode::Normal;
			break;

		case Messages::SerialAdapter_SCS_OpModes::Service:
			JandyController::m_DataHub->Mode = Kernel::EquipmentMode::Service;
			break;

		case Messages::SerialAdapter_SCS_OpModes::Timeout:
			JandyController::m_DataHub->Mode = Kernel::EquipmentMode::TimeOut;
			break; 

		case Messages::SerialAdapter_SCS_OpModes::Unknown:
		default:
			break;
		}
	}

	void SerialAdapterDevice::Command_SerialAdapter_Options([[maybe_unused]] const Messages::SerialAdapter_SCS_Options& options)
	{
		// Intentionally empty: the reported system options are not yet consumed by the DataHub.
	}

	void SerialAdapterDevice::Command_SerialAdapter_BatteryCondition([[maybe_unused]] const Messages::SerialAdapter_SCS_BatteryCondition& battery_condition)
	{
		// Intentionally empty: the reported battery condition is not yet consumed by the DataHub.
	}

	void SerialAdapterDevice::Command_SerialAdapter_AirTemperature(const Kernel::Temperature& temperature)
	{
		if (SERIALADAPTER_INVALID_TEMPERATURE_CUTOFF < temperature.InCelsius().value())
		{
			JandyController::m_DataHub->AirTemp(temperature);
		}
	}

	void SerialAdapterDevice::Command_SerialAdapter_PoolTemperature(const Kernel::Temperature& temperature)
	{
		if (SERIALADAPTER_INVALID_TEMPERATURE_CUTOFF < temperature.InCelsius().value())
		{
			JandyController::m_DataHub->PoolTemp(temperature);
		}
	}

	void SerialAdapterDevice::Command_SerialAdapter_SpaTemperature(const Kernel::Temperature& temperature)
	{
		if (SERIALADAPTER_INVALID_TEMPERATURE_CUTOFF < temperature.InCelsius().value())
		{
			JandyController::m_DataHub->SpaTemp(temperature);
		}
	}

	void SerialAdapterDevice::Command_SerialAdapter_SolarTemperature([[maybe_unused]] const Kernel::Temperature& temperature)
	{
		// Intentionally empty: solar temperature is reported by the adapter but not yet
		// surfaced to the DataHub.
	}

	void SerialAdapterDevice::Command_SerialAdapter_AuxillaryStatus(const Auxillaries::JandyAuxillaryIds& aux_id, const Auxillaries::JandyAuxillaryStatuses& status)
	{
		std::shared_ptr<Kernel::AuxillaryDevice> aux_ptr(nullptr);

		//
		// A reply about an aux the detected panel model cannot have is not evidence that the
		// relay exists -- it is the answer to a question we should not have asked. Creating a
		// device from one is what produced generically-labelled phantoms ("Aux B1", "Extra
		// Aux") on panels with a single power centre. Drop it: neither create nor update.
		//

		if (nullptr == JandyController::m_DataHub)
		{
			return;
		}

		if (const auto span = Auxillaries::AuxillaryModelSpan::FromDataHub(*JandyController::m_DataHub); !span.Contains(aux_id))
		{
			LogDebug(Channel::Devices, std::format("Ignoring auxillary status for {}; the detected panel model has no such relay", magic_enum::enum_name(aux_id)));
			return;
		}

		//
		// Find (or create) a device that matches this auxillary, reconciling by the stable id
		// derived from the aux id (matches a cache-restored placeholder regardless of label).
		//

		if (auto existing = JandyController::m_DataHub->Devices.FindById(Auxillaries::AuxStableId(aux_id)); nullptr != existing)
		{
			aux_ptr = existing;
			// Grant the aux identity to a cache-restored placeholder (which lacks it).
			Auxillaries::EnsureAuxIdentity(aux_ptr, aux_id);
		}
		else if (auto new_aux_ptr = Factory::JandyAuxillaryFactory::Instance().SerialAdapterDevice_CreateDevice(aux_id, status); new_aux_ptr.has_value())
		{
			JandyController::m_DataHub->Devices.Add(new_aux_ptr.value());
			aux_ptr = new_aux_ptr.value();
		}
		else
		{
			LogDebug(Channel::Devices, std::format("Failed to create auxillary device; error reported was: {}", new_aux_ptr.error().message()));
		}

		//
		// Update the auxillary's status
		//

		if (nullptr != aux_ptr)
		{
			switch (status)
			{
			case Auxillaries::JandyAuxillaryStatuses::Off:
				aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);
				break;

			case Auxillaries::JandyAuxillaryStatuses::On:
				aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::On);
				break;

			case Auxillaries::JandyAuxillaryStatuses::Unknown:
				[[fallthrough]];
			default:
				aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Unknown);
				break;
			}

			// Collapse any legacy random-id cache placeholder for this aux onto the live device at
			// the first touch, so it never reaches the MQTT/HA publishers as a duplicate.
			Auxillaries::RemoveOrphanAuxPlaceholders(JandyController::m_DataHub->Devices, aux_id, aux_ptr);

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(aux_ptr);
			std::string label;
			if (auto label_opt = aux_ptr->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				label = label_opt.value();
			}
			JandyController::m_DataHub->EmitButtonStateChange(aux_ptr->Id(), status_string, label);
		}
	}

}
// namespace AqualinkAutomate::Devices
