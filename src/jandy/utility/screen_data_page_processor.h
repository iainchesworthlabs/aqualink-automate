#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "utility/screen_data_page.h"

namespace AqualinkAutomate::Utility
{
	enum class ScreenDataPageTypes
	{
		Page_System,
		Page_Service,
		Page_TimeOut,
		Page_OneTouch,
		Page_EquipmentOnOff,
		Page_EquipmentStatus,
		Page_SelectSpeed,
		Page_MenuHelp,
		Page_HelpSubmenu,
		Page_SetPoolHeat,
		Page_SetSpaHeat,
		Page_SetTemperature,
		Page_SetTime,
		Page_SystemSetup,
		Page_FreezeProtect,
		Page_Boost,
		Page_SetAquapure,
		Page_Version,
		Page_DiagnosticsSensors,
		Page_DiagnosticsRemotes,
		Page_DiagnosticsErrors,
		Page_DiagnosticsIAQStatus,
		Page_DiagnosticsIAQRSSI,
		Page_LabelAuxList,
		Page_LabelAux,
		Page_MoreOneTouch,
		Page_Program,
		Page_DisplayLight,
		Page_Lockouts,
		Page_PasswordSettings,
		Page_ProgramGroup,
		Page_GeneralLabels,
		Page_LightLabels,
		Page_WaterfallLabels,
		Page_CustomLabel,
		Page_EnterPassword,
		Page_HelpKeys,
		Page_StartUp,
		Page_SystemStatus,
		Page_CloudLink,
		Page_SpaSwitch,
		Page_Unknown
	};

	class ScreenDataPage_Processor
	{
	public:
		using MenuMatcherDetails = std::pair<uint8_t, std::string>;
		using MenuMatcherProcessor = std::function<void(const ScreenDataPage&)>;

	public:
		ScreenDataPage_Processor(ScreenDataPageTypes page_type, const MenuMatcherDetails& menu_matcher, MenuMatcherProcessor menu_processor);

		ScreenDataPageTypes PageType() const;

		bool CanProcess(const ScreenDataPage& page) const;
		void Process(const ScreenDataPage& page) const;

	private:
		const ScreenDataPageTypes m_PageType;
		const MenuMatcherDetails m_MenuMatcher;
		MenuMatcherProcessor m_MenuProcessor;
	};

}
// namespace AqualinkAutomate::Devices
