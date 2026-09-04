#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include <nlohmann/json.hpp>

#include "devices/capabilities/screen.h"
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "kernel/preferences_hub.h"
#include "kernel/system_boards.h"
#include "utility/screen_data_page_processor.h"

#include "support/onetouch_test_device.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// OneTouch page processors (devices/onetouch/onetouch_pageprocessors.cpp) not reached
// by the existing page-processor suite: the recognised-but-inert menu pages (whose only
// observable effect is the page TYPE the Screen capability records), the mode pages, the
// home-page water temperatures, the freeze-protect setpoint, the spa-switch assignment
// scrape and every rejection arm of the Label Aux custom-label writer.
//
// Each test renders a complete 12-line screen and runs the real processor chain over it,
// exactly as the Status frame at the end of a MessageLong burst does.
//=============================================================================

namespace
{
	namespace ATT = Kernel::AuxillaryTraitsTypes;
	namespace SDP = AqualinkAutomate::Utility;

	using TestDevice = Test::SeamedOneTouchDevice;
	using PageLine = std::pair<uint8_t, std::string>;

	constexpr uint8_t PAGE_LINES{ 12 };

	struct PageFixture : public Test::HubLocatorInjector
	{
		PageFixture() :
			device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x40))),
			data_hub(Find<Kernel::DataHub>()),
			preferences_hub(Find<Kernel::PreferencesHub>()),
			device(device_type, *this, /*emulated*/ false)
		{
		}

		// Blank the whole screen, render 'lines' onto it, then complete the update so the
		// page processors run.
		void ShowPage(const std::vector<PageLine>& lines)
		{
			for (uint8_t line = 0; line < PAGE_LINES; ++line)
			{
				device.RenderScreenLineForTest(line, std::string(16, ' '));
			}
			for (const auto& [line, text] : lines)
			{
				device.RenderScreenLineForTest(line, text);
			}

			device.ScreenMode(Capabilities::ScreenModes::UpdateComplete);
			device.ProcessScreenUpdates();
		}

		// The page type the processor chain settled on for a screen carrying a single row.
		SDP::ScreenDataPageTypes TypeOfPageWith(uint8_t line, const std::string& text)
		{
			ShowPage({ { line, text } });
			return device.DisplayedPageType();
		}

		// The "Label Aux<n>" sub-page: the title names the aux, line 3 carries the custom label.
		void ShowLabelAuxPage(const std::string& title, const std::string& custom_label)
		{
			ShowPage({ { 0, title }, { 2, " Current Label  " }, { 3, custom_label } });
		}

		std::shared_ptr<JandyDeviceType> device_type;
		std::shared_ptr<Kernel::DataHub> data_hub;
		std::shared_ptr<Kernel::PreferencesHub> preferences_hub;
		TestDevice device;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_PageProcessors_More, PageFixture)

//=============================================================================
// Recognised-but-inert menu pages
//
// These processors only log; their observable contract is that the page is RECOGNISED
// (the Screen capability records the processor's page type instead of Page_Unknown), which
// is what the navigator and the diagnostics screen view depend on.
//=============================================================================

BOOST_AUTO_TEST_CASE(InertPages_OneTouchAndEquipmentMenus_AreRecognised)
{
	BOOST_CHECK(TypeOfPageWith(11, "     System     ") == SDP::ScreenDataPageTypes::Page_OneTouch);
	BOOST_CHECK(TypeOfPageWith(10, "OneTouch ON/OFF ") == SDP::ScreenDataPageTypes::Page_MoreOneTouch);
	BOOST_CHECK(TypeOfPageWith(0, "  Select Speed  ") == SDP::ScreenDataPageTypes::Page_SelectSpeed);
	BOOST_CHECK(TypeOfPageWith(0, "   Menu / Help  ") == SDP::ScreenDataPageTypes::Page_MenuHelp);
	BOOST_CHECK(TypeOfPageWith(1, "  Help  Keys    ") == SDP::ScreenDataPageTypes::Page_HelpSubmenu);
	BOOST_CHECK(TypeOfPageWith(0, "    Key Help    ") == SDP::ScreenDataPageTypes::Page_HelpKeys);
}

BOOST_AUTO_TEST_CASE(InertPages_SetupAndDiagnosticsMenus_AreRecognised)
{
	BOOST_CHECK(TypeOfPageWith(0, "    Set Time    ") == SDP::ScreenDataPageTypes::Page_SetTime);
	BOOST_CHECK(TypeOfPageWith(0, "  System Setup  ") == SDP::ScreenDataPageTypes::Page_SystemSetup);
	BOOST_CHECK(TypeOfPageWith(0, "   Boost Pool   ") == SDP::ScreenDataPageTypes::Page_Boost);
	BOOST_CHECK(TypeOfPageWith(6, "    Sensors     ") == SDP::ScreenDataPageTypes::Page_DiagnosticsSensors);
	BOOST_CHECK(TypeOfPageWith(0, "    Remotes     ") == SDP::ScreenDataPageTypes::Page_DiagnosticsRemotes);
	BOOST_CHECK(TypeOfPageWith(0, "     Errors     ") == SDP::ScreenDataPageTypes::Page_DiagnosticsErrors);
	BOOST_CHECK(TypeOfPageWith(0, "    Lockouts    ") == SDP::ScreenDataPageTypes::Page_Lockouts);
	BOOST_CHECK(TypeOfPageWith(0, "Password Setting") == SDP::ScreenDataPageTypes::Page_PasswordSettings);
	BOOST_CHECK(TypeOfPageWith(0, " Enter Password ") == SDP::ScreenDataPageTypes::Page_EnterPassword);
}

BOOST_AUTO_TEST_CASE(InertPages_HeatSetpointAndLabelMenus_AreRecognised)
{
	BOOST_CHECK(TypeOfPageWith(0, "   Pool Heat    ") == SDP::ScreenDataPageTypes::Page_SetPoolHeat);
	BOOST_CHECK(TypeOfPageWith(0, "   Spa Heat     ") == SDP::ScreenDataPageTypes::Page_SetSpaHeat);
	BOOST_CHECK(TypeOfPageWith(0, "   Label Aux    ") == SDP::ScreenDataPageTypes::Page_LabelAuxList);
	BOOST_CHECK(TypeOfPageWith(0, "General Labels  ") == SDP::ScreenDataPageTypes::Page_GeneralLabels);
	BOOST_CHECK(TypeOfPageWith(0, "Light   Labels  ") == SDP::ScreenDataPageTypes::Page_LightLabels);
	BOOST_CHECK(TypeOfPageWith(0, "Wtrfall Labels  ") == SDP::ScreenDataPageTypes::Page_WaterfallLabels);
	BOOST_CHECK(TypeOfPageWith(0, "Custom  Label   ") == SDP::ScreenDataPageTypes::Page_CustomLabel);

	// The "Display Light" title also carries the "Light" needle the light-label matcher uses,
	// so both inert processors run; either way the page is recognised rather than Unknown.
	BOOST_CHECK(TypeOfPageWith(0, "Display Light   ") != SDP::ScreenDataPageTypes::Page_Unknown);
}

//=============================================================================
// Mode pages
//=============================================================================

BOOST_AUTO_TEST_CASE(ServicePage_PutsTheEquipmentIntoServiceMode)
{
	BOOST_REQUIRE(data_hub->Mode == Kernel::EquipmentMode::Normal);

	ShowPage({ { 3, "  Service Mode  " }, { 5, "No  operations  " }, { 6, "  allowed here  " } });

	BOOST_CHECK(device.DisplayedPageType() == SDP::ScreenDataPageTypes::Page_Service);
	BOOST_CHECK(data_hub->Mode == Kernel::EquipmentMode::Service);
}

BOOST_AUTO_TEST_CASE(TimeoutPage_PutsTheEquipmentIntoTimeoutMode)
{
	ShowPage({ { 3, "  Timeout Mode  " }, { 10, "    02:57:39    " } });

	BOOST_CHECK(device.DisplayedPageType() == SDP::ScreenDataPageTypes::Page_TimeOut);
	BOOST_CHECK(data_hub->Mode == Kernel::EquipmentMode::TimeOut);
}

//=============================================================================
// PageProcessor_System - the home page's WATER temperatures (the air-temp layouts are
// already covered; the Pool/Spa arms of the same by-area scan were not).
//=============================================================================

BOOST_AUTO_TEST_CASE(SystemPage_WaterTemperatures_AreRoutedByArea)
{
	ShowPage({
		{ 3, "Spa Mode     OFF" },
		{ 5, "Pool Pump     ON" },
		{ 6, "   Pool 78`F    " },
		{ 7, "   Spa  92`F    " },
		{ 9, "Equipment ON/OFF" },
		{ 10, "OneTouch  ON/OFF" },
		{ 11, "   Menu / Help  " }
	});

	BOOST_CHECK(data_hub->Mode == Kernel::EquipmentMode::Normal);

	auto pool = data_hub->PoolTemp();
	BOOST_REQUIRE(pool.has_value());
	BOOST_CHECK_CLOSE(pool.value().InFahrenheit().value(), 78.0, 1.0);

	auto spa = data_hub->SpaTemp();
	BOOST_REQUIRE(spa.has_value());
	BOOST_CHECK_CLOSE(spa.value().InFahrenheit().value(), 92.0, 1.0);
}

//=============================================================================
// PageProcessor_FreezeProtect
//=============================================================================

BOOST_AUTO_TEST_CASE(FreezeProtectPage_ScrapesTheSetpoint)
{
	ShowPage({ { 0, " Freeze Protect " }, { 3, "Temp        38`F" }, { 6, "Use Arrow Keys  " } });

	BOOST_CHECK(device.DisplayedPageType() == SDP::ScreenDataPageTypes::Page_FreezeProtect);

	auto freeze_point = data_hub->FreezeProtectPoint();
	BOOST_REQUIRE(freeze_point.has_value());
	BOOST_CHECK_CLOSE(freeze_point.value().InFahrenheit().value(), 38.0, 1.0);
}

BOOST_AUTO_TEST_CASE(FreezeProtectPage_UnparseableRow_LeavesTheSetpointUnset)
{
	ShowPage({ { 0, " Freeze Protect " }, { 3, "Temp        --  " } });

	BOOST_CHECK(device.DisplayedPageType() == SDP::ScreenDataPageTypes::Page_FreezeProtect);
	BOOST_CHECK(!data_hub->FreezeProtectPoint().has_value());
}

//=============================================================================
// PageProcessor_SpaSwitch
//=============================================================================

BOOST_AUTO_TEST_CASE(SpaSwitchPage_ScrapesEveryAssignmentRow)
{
	ShowPage({
		{ 0, "   Spa Switch   " },
		{ 3, "1:1     Spa Jets" },
		{ 4, "1:2   Pool Light" },
		{ 6, "2:1     Swim Jet" },
		{ 7, "Highlight & Sel " }
	});

	BOOST_CHECK(device.DisplayedPageType() == SDP::ScreenDataPageTypes::Page_SpaSwitch);

	auto one_one = data_hub->SpaSwitchAssignment(1, 1);
	BOOST_REQUIRE(one_one.has_value());
	BOOST_CHECK_EQUAL(one_one.value(), std::string{ "Spa Jets" });

	auto one_two = data_hub->SpaSwitchAssignment(1, 2);
	BOOST_REQUIRE(one_two.has_value());
	BOOST_CHECK_EQUAL(one_two.value(), std::string{ "Pool Light" });

	auto two_one = data_hub->SpaSwitchAssignment(2, 1);
	BOOST_REQUIRE(two_one.has_value());
	BOOST_CHECK_EQUAL(two_one.value(), std::string{ "Swim Jet" });

	// The non-assignment rows (title, help text) are rejected by the parser.
	BOOST_CHECK_EQUAL(data_hub->SpaSwitchAssignments().size(), 3u);
}

//=============================================================================
// PageProcessor_Program / PublishControllerSchedules
//=============================================================================

BOOST_AUTO_TEST_CASE(ProgramGroupPage_CarriesNoProgram_AndPublishesNothing)
{
	// The Program Group page trips the { 0, "Program" } matcher, so the program parser runs
	// against a page that is not a per-equipment detail page and must simply decline.
	BOOST_CHECK(TypeOfPageWith(0, " Program Group  ") == SDP::ScreenDataPageTypes::Page_ProgramGroup);
}

//=============================================================================
// PageProcessor_LabelAux - the custom-label writer and each of its rejection arms
//=============================================================================

BOOST_AUTO_TEST_CASE(LabelAux_AppliesTheCustomLabelToANewAux)
{
	ShowLabelAuxPage("   Label Aux1   ", "   Pool Light   ");

	auto auxillaries = data_hub->Auxillaries();
	BOOST_REQUIRE_EQUAL(auxillaries.size(), 1u);

	auto label = auxillaries.front()->AuxillaryTraits.TryGet(ATT::LabelTrait{});
	BOOST_REQUIRE(label.has_value());
	BOOST_CHECK_EQUAL(label.value(), std::string{ "Pool Light" });

	auto aux_id = auxillaries.front()->AuxillaryTraits.TryGet(Auxillaries::JandyAuxillaryId{});
	BOOST_REQUIRE(aux_id.has_value());
	BOOST_CHECK(aux_id.value() == Auxillaries::JandyAuxillaryIds::Aux_1);
}

BOOST_AUTO_TEST_CASE(LabelAux_BlankCustomLabelRow_CreatesNothing)
{
	// The label row has not rendered yet: there is nothing to apply.
	ShowLabelAuxPage("   Label Aux1   ", "                ");

	BOOST_CHECK(data_hub->Auxillaries().empty());
}

BOOST_AUTO_TEST_CASE(LabelAux_TitleWithoutAnAuxId_CreatesNothing)
{
	// A title that does not carry a "Label Aux<n>" tag cannot identify a relay.
	ShowLabelAuxPage("  Label Heater  ", "   Pool Light   ");

	BOOST_CHECK(data_hub->Auxillaries().empty());
}

BOOST_AUTO_TEST_CASE(LabelAux_RelayOutsideTheDecodedModel_CreatesNothing)
{
	// A decoded RS-4 has four relays on one power centre; the Label Aux menu still offers a
	// slot for Aux7, and landing on it must never mint a relay the panel cannot have.
	data_hub->SystemBoard = Kernel::SystemBoards::RS4_Only;
	data_hub->ExpectedPowerCenterCount = 1;
	data_hub->ExpectedAuxillaryCount = 4;

	ShowLabelAuxPage("   Label Aux7   ", "   Waterfall    ");

	BOOST_CHECK(data_hub->Auxillaries().empty());
}

BOOST_AUTO_TEST_CASE(LabelAux_OperatorForcedAbsent_RemovesTheExistingAux)
{
	// First visit creates the aux from the label page...
	ShowLabelAuxPage("   Label Aux1   ", "   Pool Light   ");
	BOOST_REQUIRE_EQUAL(data_hub->Auxillaries().size(), 1u);

	// ...then the operator declares the slot absent, so the next visit must not resurrect it
	// (and must clear the device the earlier visit created).
	preferences_hub->AuxPresenceOverrides = nlohmann::json{ { "Aux1", "absent" } };

	ShowLabelAuxPage("   Label Aux1   ", "   Pool Light   ");

	BOOST_CHECK(data_hub->Auxillaries().empty());
}

BOOST_AUTO_TEST_SUITE_END()
