#include <format>
#include <functional>
#include <source_location>
#include <utility>

#include "logging/logging.h"
#include "devices/onetouch/onetouch_scraper.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	OneTouchScraper::OneTouchScraper(
		std::shared_ptr<Devices::JandyDeviceType> device_id,
		std::shared_ptr<Kernel::DataHub> data_hub,
		std::shared_ptr<Scheduling::ControllerScheduleStore> schedule_store,
		std::shared_ptr<Kernel::PreferencesHub> preferences_hub) :
		m_DeviceId(std::move(device_id)),
		m_DataHub(std::move(data_hub)),
		m_ControllerScheduleStore(std::move(schedule_store)),
		m_PreferencesHub(std::move(preferences_hub))
	{
	}

	std::list<Utility::ScreenDataPage_Processor> OneTouchScraper::MakeProcessors()
	{
		using enum Utility::ScreenDataPageTypes;

		return {
			Utility::ScreenDataPage_Processor(Page_System, { 9, "Equipment ON/OFF" }, std::bind(&OneTouchScraper::PageProcessor_System, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_Service, { 3, "Service Mode" }, std::bind(&OneTouchScraper::PageProcessor_Service, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_TimeOut, { 3, "Timeout Mode" }, std::bind(&OneTouchScraper::PageProcessor_TimeOut, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_OneTouch, { 11, "System" }, std::bind(&OneTouchScraper::PageProcessor_OneTouch, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_EquipmentOnOff, { 11, "More" }, std::bind(&OneTouchScraper::PageProcessor_EquipmentOnOff, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_EquipmentOnOff, { 0, "Filter Pump" }, std::bind(&OneTouchScraper::PageProcessor_EquipmentOnOff, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_EquipmentStatus, { 0, "EQUIPMENT STATUS" }, std::bind(&OneTouchScraper::PageProcessor_EquipmentStatus, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SelectSpeed, { 0, "Select Speed" }, std::bind(&OneTouchScraper::PageProcessor_SelectSpeed, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_MenuHelp, { 0, "Menu" }, std::bind(&OneTouchScraper::PageProcessor_MenuHelp, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_HelpSubmenu, { 1, "Keys" }, std::bind(&OneTouchScraper::PageProcessor_HelpSubmenu, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SetTemperature, { 0, "Set Temp" }, std::bind(&OneTouchScraper::PageProcessor_SetTemperature, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SetTime, { 0, "Set Time" }, std::bind(&OneTouchScraper::PageProcessor_SetTime, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SystemSetup, { 0, "System Setup" }, std::bind(&OneTouchScraper::PageProcessor_SystemSetup, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_FreezeProtect, { 0, "Freeze Protect" }, std::bind(&OneTouchScraper::PageProcessor_FreezeProtect, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_Boost, { 0, "Boost Pool" }, std::bind(&OneTouchScraper::PageProcessor_Boost, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SetAquapure, { 0, "Set AQUAPURE" }, std::bind(&OneTouchScraper::PageProcessor_SetAquapure, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_Version, { 7, "REV " }, std::bind(&OneTouchScraper::PageProcessor_Version, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_DiagnosticsSensors, { 6, "Sensors" }, std::bind(&OneTouchScraper::PageProcessor_DiagnosticsSensors, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_DiagnosticsRemotes, { 0, "Remotes" }, std::bind(&OneTouchScraper::PageProcessor_DiagnosticsRemotes, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_DiagnosticsErrors, { 0, "Errors" }, std::bind(&OneTouchScraper::PageProcessor_DiagnosticsErrors, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_LabelAuxList, { 0, "Label Aux" }, std::bind(&OneTouchScraper::PageProcessor_LabelAuxList, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_LabelAux, { 2, "Current Label" }, std::bind(&OneTouchScraper::PageProcessor_LabelAux, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SetPoolHeat, { 0, "Pool Heat" }, std::bind(&OneTouchScraper::PageProcessor_SetPoolHeat, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SetSpaHeat, { 0, "Spa Heat" }, std::bind(&OneTouchScraper::PageProcessor_SetSpaHeat, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_SpaSwitch, { 0, "Spa Switch" }, std::bind(&OneTouchScraper::PageProcessor_SpaSwitch, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_MoreOneTouch, { 10, "OneTouch ON/OFF" }, std::bind(&OneTouchScraper::PageProcessor_MoreOneTouch, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_Program, { 0, "Program" }, std::bind(&OneTouchScraper::PageProcessor_Program, this, std::placeholders::_1)),
			// The per-equipment Program DETAIL page has the EQUIPMENT NAME on line 0 (e.g.
			// "Filter Pump"), NOT "Program", so the { 0, "Program" } matcher above misses it.
			// Detect it by a STABLE row instead: line 2 always carries "Pgm N of M". (Its
			// line-0 name also trips the Page_EquipmentOnOff { 0, "Filter Pump" } matcher, but
			// that processor rejects every detail-page row - none end in ON/OFF/ENA or *** - so it
			// is a harmless no-op while THIS processor does the real parse.)
			Utility::ScreenDataPage_Processor(Page_Program, { 2, "Pgm " }, std::bind(&OneTouchScraper::PageProcessor_Program, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_DisplayLight, { 0, "Display Light" }, std::bind(&OneTouchScraper::PageProcessor_DisplayLight, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_Lockouts, { 0, "Lockout" }, std::bind(&OneTouchScraper::PageProcessor_Lockouts, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_PasswordSettings, { 0, "Password" }, std::bind(&OneTouchScraper::PageProcessor_PasswordSettings, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_ProgramGroup, { 0, "Program Group" }, std::bind(&OneTouchScraper::PageProcessor_ProgramGroup, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_GeneralLabels, { 0, "General" }, std::bind(&OneTouchScraper::PageProcessor_GeneralLabels, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_LightLabels, { 0, "Light" }, std::bind(&OneTouchScraper::PageProcessor_LightLabels, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_WaterfallLabels, { 0, "Wtrfall" }, std::bind(&OneTouchScraper::PageProcessor_WaterfallLabels, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_CustomLabel, { 0, "Custom" }, std::bind(&OneTouchScraper::PageProcessor_CustomLabel, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_EnterPassword, { 0, "Enter Password" }, std::bind(&OneTouchScraper::PageProcessor_EnterPassword, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_HelpKeys, { 0, "Key Help" }, std::bind(&OneTouchScraper::PageProcessor_HelpKeys, this, std::placeholders::_1)),
			Utility::ScreenDataPage_Processor(Page_StartUp, { 5, "-" }, std::bind(&OneTouchScraper::PageProcessor_StartUp, this, std::placeholders::_1))
		};
	}

	void OneTouchScraper::PageProcessor_HelpSubmenu([[maybe_unused]] const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, std::format("OneTouch ({}): PageProcessor_HelpSubmenu invoked", DeviceId()));
	}

	void OneTouchScraper::PageProcessor_StartUp(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_StartUp", std::source_location::current());

		LogDebug(Channel::Devices, std::format("OneTouch ({}): Processing cold start splash screen", DeviceId()));

		// The splash carries the same model/type/revision + pool configuration as the REV page, so
		// the decode (and body-of-water build) is shared with PageProcessor_Version.
		DecodePanelConfiguration(page);
	}

}
// namespace AqualinkAutomate::Devices
