#include <chrono>
#include <memory>

#include "logging/logging.h"
#include "profiling/profiling.h"

#include "kernel/data_hub.h"
#include "devices/iaq_device.h"
#include "devices/keypad_device.h"
#include "devices/onetouch_device.h"
#include "devices/pda_device.h"
#include "devices/serial_adapter_device.h"
#include "devices/spaside_remote_device.h"
#include "options/options_developer_options.h"
#include "options/options_jandy.h"
#include "equipment/jandy_equipment.h"
#include "jandy.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Jandy
{

	void Initialise(Kernel::HubLocator& /*hub_locator*/)
	{
		// Intentionally empty: the Jandy subsystem performs no work at the
		// Initialise phase; all setup happens in Configure() once settings
		// are available.
	}

	void Configure(Kernel::HubLocator& hub_locator, const AqualinkAutomate::Options::Settings& settings)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("Jandy::Initialise", std::source_location::current());

		auto jandy_settings_result = settings.Get<Jandy::Options::JandySettings>();
		if (!jandy_settings_result)
		{
			LogError(Channel::Main, "Failed to retrieve Jandy settings; Jandy equipment will not be configured");
			return;
		}

		const auto& jandy_settings = jandy_settings_result.value().get();

		auto equipment_hub = hub_locator.Find<Kernel::EquipmentHub>();
		if (nullptr == equipment_hub)
		{
			LogError(Channel::Main, "Failed to locate EquipmentHub; Jandy equipment will not be configured");
			return;
		}

		//---------------------------------------------------------------------
		// JANDY EQUIPMENT
		//---------------------------------------------------------------------

		LogInfo(Channel::Main, "Starting AqualinkAutomate::JandyEquipment...");

		bool decode_to_master = false;
		if (auto dev_settings_result = settings.Get<AqualinkAutomate::Options::Developer::DeveloperSettings>(); dev_settings_result)
		{
			decode_to_master = dev_settings_result.value().get().decode_to_master_enabled;
			if (decode_to_master)
			{
				LogInfo(Channel::Main, "Developer: decode-to-master enabled; frames addressed to the master (0x00) will be decoded and logged on Channel::Messages (observe-only)");
			}
		}

		equipment_hub->AddEquipment(std::make_unique<Equipment::JandyEquipment>(hub_locator, decode_to_master));

		if (jandy_settings.disable_emulation)
		{
			auto data_hub = hub_locator.Find<Kernel::DataHub>();
			if (nullptr != data_hub)
			{
				data_hub->EmulationDisabled = true;
				LogInfo(Channel::Main, "Emulation disabled; skipping equipment discovery phase");
			}
		}

		if (jandy_settings.disable_presence_gating)
		{
			auto data_hub = hub_locator.Find<Kernel::DataHub>();
			if (nullptr != data_hub)
			{
				data_hub->PresenceGatingDisabled = true;
				LogInfo(Channel::Main, "RSSA presence-gating disabled; an emulated Serial Adapter will not auto-suppress on a suspected real adapter");
			}
		}

		if (jandy_settings.auto_startup)
		{
			// The JandyStartupService (wired in aqualink-automate.cpp on the io_context) detects
			// the controller from the bus and stands up the emulation dynamically, so skip the
			// static, CLI-configured device set here.
			LogInfo(Channel::Main, "Jandy auto-startup enabled; deferring emulated-device selection to the start-up coordinator");
		}

		if (!jandy_settings.disable_emulation && !jandy_settings.auto_startup)
		{
			for (const auto& [controller_type, device_type] : jandy_settings.emulated_devices)
			{
				LogInfo(Channel::Main, std::format("Enabling controller emulation; type: {}, id: {}", magic_enum::enum_name(controller_type), device_type.Id()));

				auto device_id = std::make_shared<Devices::JandyDeviceType>(device_type);

				using enum Devices::JandyEmulatedDeviceTypes;

				switch (controller_type)
				{
				case OneTouch:
					{
						auto onetouch = std::make_unique<Devices::OneTouchDevice>(device_id, hub_locator, true);
						onetouch->EnableChlorinatorSetpointRefresh(std::chrono::seconds{ jandy_settings.chlorinator_setpoint_refresh_interval });
						equipment_hub->AddDevice(std::move(onetouch));
					}
					break;

				case RS_Keypad:
					equipment_hub->AddDevice(std::make_unique<Devices::KeypadDevice>(device_id, hub_locator, true));
					break;

				case IAQ:
					equipment_hub->AddDevice(std::make_unique<Devices::IAQDevice>(device_id, hub_locator, true));
					break;

				case PDA:
					equipment_hub->AddDevice(std::make_unique<Devices::PDADevice>(device_id, hub_locator, true));
					break;

				case SerialAdapter:
					equipment_hub->AddDevice(std::make_unique<Devices::SerialAdapterDevice>(device_id, hub_locator, true));
					break;

				case SpasideRemote:
					equipment_hub->AddDevice(std::make_unique<Devices::SpasideRemoteDevice>(device_id, hub_locator, true));
					break;

				case Unknown:
				default:
					LogWarning(Channel::Main, "Unknown emulated device type; cannot create controller device");
				}
			}
		}
	}

}
// namespace AqualinkAutomate::Jandy
