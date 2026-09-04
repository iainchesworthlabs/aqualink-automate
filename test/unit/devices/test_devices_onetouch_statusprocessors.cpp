#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "devices/capabilities/screen.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_devices/heater_status.h"
#include "kernel/auxillary_devices/pump_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"

#include "support/onetouch_test_device.h"
#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// The Equipment Status LINE processors (devices/onetouch/onetouch_statusprocessors.cpp).
//
// PageProcessor_EquipmentStatus runs all nine line processors across every non-empty
// line of the page, so a test simply renders an "EQUIPMENT STATUS" screen carrying the
// rows of interest and asserts the resulting DataHub state. Each processor has three
// rejection arms - the two-character hint pre-filter, the full regex, and (for the
// value rows) the numeric conversion - plus the "ENA" vs bare-label status split.
//=============================================================================

namespace
{
	namespace ATT = Kernel::AuxillaryTraitsTypes;

	using TestDevice = Test::SeamedOneTouchDevice;

	struct StatusFixture : public Test::HubLocatorInjector
	{
		StatusFixture() :
			device_type(std::make_shared<JandyDeviceType>(JandyDeviceId(0x40))),
			data_hub(Find<Kernel::DataHub>()),
			device(device_type, *this, /*emulated*/ false)
		{
		}

		// Render an Equipment Status screen carrying 'rows' on lines 1..N (line 0 is the
		// title that routes the page to PageProcessor_EquipmentStatus) and run the page
		// processors over it, exactly as the Status frame at the end of a MessageLong burst
		// would.
		void ShowEquipmentStatus(const std::vector<std::string>& rows)
		{
			device.RenderScreenLineForTest(0, "EQUIPMENT STATUS");
			for (uint8_t line = 1; line < 12; ++line)
			{
				const std::size_t index = line - 1u;
				device.RenderScreenLineForTest(line, (index < rows.size()) ? rows[index] : std::string(16, ' '));
			}

			device.ScreenMode(Capabilities::ScreenModes::UpdateComplete);
			device.ProcessScreenUpdates();

			BOOST_REQUIRE(device.DisplayedPageType() == Utility::ScreenDataPageTypes::Page_EquipmentStatus);
		}

		std::optional<Kernel::HeaterStatuses> HeaterStatusOf(const std::string& label) const
		{
			for (const auto& heater : data_hub->Heaters())
			{
				auto heater_label = heater->AuxillaryTraits.TryGet(ATT::LabelTrait{});
				if (heater_label.has_value() && (heater_label.value() == label))
				{
					return heater->AuxillaryTraits.TryGet(ATT::HeaterStatusTrait{});
				}
			}
			return std::nullopt;
		}

		std::shared_ptr<JandyDeviceType> device_type;
		std::shared_ptr<Kernel::DataHub> data_hub;
		TestDevice device;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_StatusProcessors, StatusFixture)

//=============================================================================
// StatusProcessor_SolarHeat
//=============================================================================

BOOST_AUTO_TEST_CASE(SolarHeat_BareLabel_CreatesHeaterAndReportsHeating)
{
	ShowEquipmentStatus({ "Solar Heat      " });

	auto status = HeaterStatusOf("Solar Heat");
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Heating);
}

BOOST_AUTO_TEST_CASE(SolarHeat_EnaSuffix_ReportsEnabled)
{
	ShowEquipmentStatus({ "Solar Heat   ENA" });

	auto status = HeaterStatusOf("Solar Heat");
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Enabled);
}

BOOST_AUTO_TEST_CASE(SolarHeat_HintMatchesButRegexDoesNot_CreatesNothing)
{
	// "So..." passes the two-character hint pre-filter but is not a solar-heat row.
	ShowEquipmentStatus({ "Solar Heater 12 " });

	BOOST_CHECK(!HeaterStatusOf("Solar Heat").has_value());
	BOOST_CHECK(data_hub->Heaters().empty());
}

//=============================================================================
// StatusProcessor_HeatPump
//=============================================================================

BOOST_AUTO_TEST_CASE(HeatPump_BareLabel_CreatesHeaterAndReportsHeating)
{
	ShowEquipmentStatus({ "Heat Pump       " });

	auto status = HeaterStatusOf("Heat Pump");
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Heating);
}

BOOST_AUTO_TEST_CASE(HeatPump_EnaSuffix_ReportsEnabled)
{
	ShowEquipmentStatus({ "Heat Pump    ENA" });

	auto status = HeaterStatusOf("Heat Pump");
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Enabled);
}

BOOST_AUTO_TEST_CASE(HeatPump_HintMatchesButRegexDoesNot_CreatesNothing)
{
	ShowEquipmentStatus({ "Heat  Pumping   " });

	BOOST_CHECK(!HeaterStatusOf("Heat Pump").has_value());
	BOOST_CHECK(data_hub->Heaters().empty());
}

//=============================================================================
// StatusProcessor_Chiller
//=============================================================================

BOOST_AUTO_TEST_CASE(Chiller_BareLabel_CreatesChillerAndReportsHeating)
{
	ShowEquipmentStatus({ "Chiller         " });

	// The chiller is modelled as a heater device carrying the "ChillerCooling" label.
	auto status = HeaterStatusOf("ChillerCooling");
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Heating);
}

BOOST_AUTO_TEST_CASE(Chiller_EnaSuffix_ReportsEnabled)
{
	ShowEquipmentStatus({ "Chiller      ENA" });

	auto status = HeaterStatusOf("ChillerCooling");
	BOOST_REQUIRE(status.has_value());
	BOOST_CHECK(status.value() == Kernel::HeaterStatuses::Enabled);
}

//=============================================================================
// StatusProcessor_PoolHeat / StatusProcessor_SpaHeat
//=============================================================================

BOOST_AUTO_TEST_CASE(PoolAndSpaHeat_BareLabels_ReportHeating)
{
	ShowEquipmentStatus({ "Pool Heat       ", "Spa Heat        " });

	auto pool = HeaterStatusOf("Pool Heat");
	BOOST_REQUIRE(pool.has_value());
	BOOST_CHECK(pool.value() == Kernel::HeaterStatuses::Heating);

	auto spa = HeaterStatusOf("Spa Heat");
	BOOST_REQUIRE(spa.has_value());
	BOOST_CHECK(spa.value() == Kernel::HeaterStatuses::Heating);
}

BOOST_AUTO_TEST_CASE(PoolAndSpaHeat_HintMatchesButRegexDoesNot_CreatesNothing)
{
	ShowEquipmentStatus({ "Pool Heating    ", "Spa Heating     " });

	BOOST_CHECK(!HeaterStatusOf("Pool Heat").has_value());
	BOOST_CHECK(!HeaterStatusOf("Spa Heat").has_value());
	BOOST_CHECK(data_hub->Heaters().empty());
}

//=============================================================================
// StatusProcessor_FilterPump
//=============================================================================

BOOST_AUTO_TEST_CASE(FilterPump_MatchingLine_CreatesPumpAndReportsRunning)
{
	ShowEquipmentStatus({ "Filter Pump     " });

	auto pumps = data_hub->FilterPumps();
	BOOST_REQUIRE_EQUAL(pumps.size(), 1u);

	auto pump_status = pumps.front()->AuxillaryTraits.TryGet(ATT::PumpStatusTrait{});
	BOOST_REQUIRE(pump_status.has_value());
	BOOST_CHECK(pump_status.value() == Kernel::PumpStatuses::Running);
}

BOOST_AUTO_TEST_CASE(FilterPump_HintMatchesButRegexDoesNot_CreatesNothing)
{
	// "Fi..." passes the hint filter; the full-line regex rejects it.
	ShowEquipmentStatus({ "Filter Pumping  " });

	BOOST_CHECK(data_hub->FilterPumps().empty());
}

//=============================================================================
// StatusProcessor_AquaPurePercentage / StatusProcessor_SaltLevelPPM value rejection
//=============================================================================

BOOST_AUTO_TEST_CASE(AquaPurePercentage_OutOfRangeValue_IsRejected)
{
	// The row passes the "aq" hint but 150 is not a 1-2 digit value (nor exactly 100),
	// so the line is rejected outright rather than producing a nonsense duty cycle.
	ShowEquipmentStatus({ "AquaPure    150%" });

	BOOST_CHECK(data_hub->Chlorinators().empty());
}

BOOST_AUTO_TEST_CASE(SaltLevelPPM_OutOfRangeValue_IsRejected)
{
	// Five digits exceeds the {1,4} the salt row permits: no salt level is published.
	ShowEquipmentStatus({ "Salt  12345 PPM " });

	BOOST_CHECK_EQUAL(data_hub->SaltLevel().value(), 0.0);
}

//=============================================================================
// StatusProcessor_CheckAquaPure
//=============================================================================

BOOST_AUTO_TEST_CASE(CheckAquaPure_HintMatchesButRegexDoesNot_LeavesHealthAlone)
{
	// A "Ch..." row that is not the "Check AquaPure" alert must not flag the chlorinator.
	ShowEquipmentStatus({ "Chemistry OK    " });

	BOOST_CHECK(data_hub->Chlorinators().empty());
}

BOOST_AUTO_TEST_CASE(CheckAquaPure_AlertLine_CreatesChlorinatorWithGeneralFault)
{
	ShowEquipmentStatus({ "Check AquaPure  " });

	auto chlorinators = data_hub->Chlorinators();
	BOOST_REQUIRE_EQUAL(chlorinators.size(), 1u);

	auto health = chlorinators.front()->AuxillaryTraits.TryGet(ATT::ChlorinatorHealthTrait{});
	BOOST_REQUIRE(health.has_value());
	BOOST_CHECK(health.value() == Kernel::ChlorinatorHealth::GeneralFault);
}

BOOST_AUTO_TEST_SUITE_END()
