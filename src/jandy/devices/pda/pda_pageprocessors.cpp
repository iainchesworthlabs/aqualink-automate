#include <format>

#include "logging/logging.h"
#include "devices/pda_device.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	void PDADevice::PageProcessor_System([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		LogDebug(Channel::Devices, "PDA device is processing a PageProcessor_System page.");

		/*
			Info:   PDA Menu Line 00 =
			Info:   PDA Menu Line 01 = Air 
			Info:   PDA Menu Line 02 =
			Info:   PDA Menu Line 03 =
			Info:   PDA Menu Line 04 = Pool Mode    OFF
			Info:   PDA Menu Line 05 = Pool Heater  OFF
			Info:   PDA Menu Line 06 = Spa Mode     OFF
			Info:   PDA Menu Line 07 = Spa Heater   OFF
			Info:   PDA Menu Line 08 = Menu
			Info:   PDA Menu Line 09 = Equipment ON/OFF
		*/
	}

	void PDADevice::PageProcessor_SetTemperature([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}

	void PDADevice::PageProcessor_SetTime([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}

	void PDADevice::PageProcessor_PoolHeat([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}

	void PDADevice::PageProcessor_SpaHeat([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}

	void PDADevice::PageProcessor_AquaPure([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}

	void PDADevice::PageProcessor_FreezeProtect([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}

	void PDADevice::PageProcessor_EquipmentStatus([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}

	void PDADevice::PageProcessor_Boost([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		// Placeholder: this PDA page is recognised for navigation but not yet parsed.
	}
	
	void PDADevice::PageProcessor_FirmwareVersion(const Utility::ScreenDataPage& page)
	{
		LogDebug(Channel::Devices, "PDA device is processing a PageProcessor_FirmwareVersion page.");

		/*
			Info:   PDA Menu Line 00 =
			Info:   PDA Menu Line 01 =      AquaPalm
			Info:   PDA Menu Line 02 =
			Info:   PDA Menu Line 03 =  Firmware Version
			Info:   PDA Menu Line 04 =
			Info:   PDA Menu Line 05 =    
			Info:   PDA Menu Line 06 =     REV T.0.1
			Info:   PDA Menu Line 07 =
			Info:   PDA Menu Line 08 =
			Info:   PDA Menu Line 09 =
		*/

		const auto model_number = Utility::TrimWhitespace(page[1].Text);
		const auto fw_revision = Utility::TrimWhitespace(page[6].Text);

		JandyController::m_DataHub->EquipmentVersions.Set("Model", model_number);
		JandyController::m_DataHub->EquipmentVersions.Set("Revision", fw_revision);

		LogInfo(Channel::Devices, std::format("Aqualink Power Center - Model: {}, Rev: {}", model_number, fw_revision));
	}

}
// namespace AqualinkAutomate::Devices
