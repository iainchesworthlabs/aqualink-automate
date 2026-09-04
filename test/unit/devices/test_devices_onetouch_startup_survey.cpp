#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include <boost/test/unit_test.hpp>

#include "devices/onetouch/onetouch_startup_survey.h"
#include "jandy/auxillaries/jandy_auxillary_id.h"
#include "jandy/auxillaries/jandy_auxillary_traits_types.h"
#include "jandy/devices/jandy_device_id.h"
#include "jandy/devices/jandy_device_types.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/data_hub.h"
#include "navigation/menu_model.h"
#include "navigation/navigator.h"
#include "navigation/onetouch_menu_model.h"
#include "navigation/spider_engine.h"
#include "navigation/visit_policies.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// The OneTouch startup-crawl analysis helpers (devices/onetouch/onetouch_startup_survey.cpp).
// They are pure functions of their inputs, so each is driven directly:
//
//   * DataHubChlorinatorOnline    - the offline/online predicate that gates the
//                                   setpoint-refresh recovery re-scrape.
//   * ValidateDiscoveredEquipment - the model-vs-discovered cross-check written back
//                                   onto DataHub::EquipmentValidationResult.
//   * BuildMenuSurvey             - the reached / expected-absent / notable-failure
//                                   classification of a completed SpiderEngine crawl.
//=============================================================================

namespace
{
	namespace ATT = Kernel::AuxillaryTraitsTypes;

	using Navigation::MenuModel;
	using Navigation::Navigator;
	using Navigation::PageId;
	using Navigation::SpiderEngine;
	using Navigation::TargetedVisitPolicy;

	struct SurveyFixture : public Test::HubLocatorInjector
	{
		SurveyFixture() :
			data_hub(Find<Kernel::DataHub>()),
			device_id(JandyDeviceId(0x40)),
			model(Navigation::CreateOneTouchMenuModel()),
			nav(model),
			engine(model, nav)
		{
		}

		std::shared_ptr<Kernel::AuxillaryDevice> AddAux(Auxillaries::JandyAuxillaryIds aux_id)
		{
			auto aux = std::make_shared<Kernel::AuxillaryDevice>();
			aux->AuxillaryTraits.Set(ATT::AuxillaryTypeTrait{}, ATT::AuxillaryTypes::Auxillary);
			aux->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, aux_id);
			data_hub->Devices.Add(aux);
			return aux;
		}

		std::shared_ptr<Kernel::AuxillaryDevice> AddChlorinator()
		{
			auto swg = std::make_shared<Kernel::AuxillaryDevice>();
			swg->AuxillaryTraits.Set(ATT::AuxillaryTypeTrait{}, ATT::AuxillaryTypes::Chlorinator);
			swg->AuxillaryTraits.Set(ATT::LabelTrait{}, std::string{ "AquaPure" });
			data_hub->Devices.Add(swg);
			return swg;
		}

		// The screen the controller would show while sitting on 'page_id': every one of that
		// page's detector patterns rendered onto its detector line, so MenuModel::DetectPage
		// resolves the page unambiguously.
		Utility::ScreenDataPage ContentFor(PageId page_id) const
		{
			Utility::ScreenDataPage page(12);
			if (const auto* menu_page = model.GetPage(page_id); nullptr != menu_page)
			{
				for (const auto& detector : menu_page->detectors)
				{
					if (detector.line < page.Size())
					{
						page[detector.line].Text = detector.pattern;
					}
				}
			}
			return page;
		}

		// Park the Navigator on a page (its position is simply the last detected page).
		void SyncNavigatorTo(PageId page_id)
		{
			const auto content = ContentFor(page_id);
			for (uint32_t i = 0; i <= Navigator::SYNC_REQUIRED_CONSISTENT_COUNT; ++i)
			{
				nav.OnPageUpdate(content, 0);
			}
			BOOST_REQUIRE(nav.IsSynced());
			BOOST_REQUIRE(nav.GetCurrentPage() == page_id);
		}

		std::shared_ptr<Kernel::DataHub> data_hub;
		JandyDeviceType device_id;
		MenuModel model;
		Navigator nav;
		SpiderEngine engine;
	};
}

BOOST_FIXTURE_TEST_SUITE(TestSuite_OneTouch_StartUpSurvey, SurveyFixture)

//=============================================================================
// DataHubChlorinatorOnline
//=============================================================================

BOOST_AUTO_TEST_CASE(ChlorinatorOnline_NoChlorinator_IsOffline)
{
	BOOST_CHECK(!OneTouch::DataHubChlorinatorOnline(*data_hub));
}

BOOST_AUTO_TEST_CASE(ChlorinatorOnline_NoStatusTrait_IsOffline)
{
	AddChlorinator();   // present on the hub, but has never reported a status
	BOOST_CHECK(!OneTouch::DataHubChlorinatorOnline(*data_hub));
}

BOOST_AUTO_TEST_CASE(ChlorinatorOnline_OffOrUnknown_IsOffline)
{
	auto swg = AddChlorinator();

	swg->AuxillaryTraits.Set(ATT::ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);
	BOOST_CHECK(!OneTouch::DataHubChlorinatorOnline(*data_hub));

	swg->AuxillaryTraits.Set(ATT::ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Unknown);
	BOOST_CHECK(!OneTouch::DataHubChlorinatorOnline(*data_hub));
}

BOOST_AUTO_TEST_CASE(ChlorinatorOnline_On_IsOnline)
{
	auto swg = AddChlorinator();
	swg->AuxillaryTraits.Set(ATT::ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::On);
	BOOST_CHECK(OneTouch::DataHubChlorinatorOnline(*data_hub));
}

//=============================================================================
// ValidateDiscoveredEquipment
//=============================================================================

BOOST_AUTO_TEST_CASE(Validate_ModelNotYetDecoded_RecordsEmptyExpectation)
{
	// No version/REV page scraped yet: nothing to validate against, but the result is still
	// published so consumers can see the check ran.
	AddAux(Auxillaries::JandyAuxillaryIds::Aux_1);

	OneTouch::ValidateDiscoveredEquipment(*data_hub, device_id);

	BOOST_REQUIRE(data_hub->EquipmentValidationResult.has_value());
	BOOST_CHECK_EQUAL(static_cast<int>(data_hub->EquipmentValidationResult->ExpectedAuxillaries), 0);
}

BOOST_AUTO_TEST_CASE(Validate_DiscoveredMatchesModel_Passes)
{
	data_hub->ExpectedAuxillaryCount = 4;
	data_hub->ExpectedPowerCenterCount = 1;

	AddAux(Auxillaries::JandyAuxillaryIds::Aux_1);
	AddAux(Auxillaries::JandyAuxillaryIds::Aux_2);
	AddAux(Auxillaries::JandyAuxillaryIds::Aux_3);
	AddAux(Auxillaries::JandyAuxillaryIds::Aux_4);

	OneTouch::ValidateDiscoveredEquipment(*data_hub, device_id);

	BOOST_REQUIRE(data_hub->EquipmentValidationResult.has_value());
	const auto& result = data_hub->EquipmentValidationResult.value();
	BOOST_CHECK_EQUAL(static_cast<int>(result.ExpectedAuxillaries), 4);
	BOOST_CHECK_EQUAL(static_cast<int>(result.DiscoveredAuxillaries), 4);
	BOOST_CHECK(result.Passed());
}

BOOST_AUTO_TEST_CASE(Validate_DiscoveredDisagreesWithModel_RecordsAnomalies)
{
	// The panel says 4 relays on one power centre, but a B-bank aux was discovered.
	data_hub->ExpectedAuxillaryCount = 4;
	data_hub->ExpectedPowerCenterCount = 1;

	AddAux(Auxillaries::JandyAuxillaryIds::Aux_1);
	AddAux(Auxillaries::JandyAuxillaryIds::Aux_B1);

	OneTouch::ValidateDiscoveredEquipment(*data_hub, device_id);

	BOOST_REQUIRE(data_hub->EquipmentValidationResult.has_value());
	const auto& result = data_hub->EquipmentValidationResult.value();
	BOOST_CHECK(!result.Passed());
	BOOST_CHECK(!result.Anomalies.empty());
}

//=============================================================================
// BuildMenuSurvey
//=============================================================================

BOOST_AUTO_TEST_CASE(Survey_TargetReached_CountsThePageAndFlagsTheEquipmentPage)
{
	// Parked on Equipment ON/OFF and asked to visit exactly that page: the engine captures it
	// without navigating and completes.
	SyncNavigatorTo(PageId::EquipmentOnOff);
	engine.StartCrawl(std::make_unique<TargetedVisitPolicy>(std::set<PageId>{ PageId::EquipmentOnOff }));
	engine.ProcessStep(ContentFor(PageId::EquipmentOnOff), 0);

	BOOST_REQUIRE(engine.GetVisitedPages().contains(PageId::EquipmentOnOff));
	BOOST_REQUIRE(engine.GetFailedPages().empty());

	const auto survey = OneTouch::BuildMenuSurvey(engine, model, device_id);

	BOOST_CHECK_EQUAL(survey.PagesReached, 1u);
	BOOST_CHECK(survey.EquipmentPageReached);
	BOOST_CHECK(survey.ExpectedAbsent.empty());
	BOOST_CHECK(survey.NotableFailures.empty());
}

BOOST_AUTO_TEST_CASE(Survey_CapabilityGatedPageUnreachable_IsExpectedAbsentNotAFailure)
{
	// Parked on the home page, targeting the Boost page; the controller then drops into Service
	// Mode, which blocks navigation outright, so the crawl abandons the target.
	SyncNavigatorTo(PageId::System);
	engine.StartCrawl(std::make_unique<TargetedVisitPolicy>(std::set<PageId>{ PageId::Boost }));
	engine.ProcessStep(ContentFor(PageId::Service), Navigator::CURSOR_LINE_NONE);

	BOOST_REQUIRE(engine.GetFailedPages().contains(PageId::Boost));

	const auto survey = OneTouch::BuildMenuSurvey(engine, model, device_id);

	// Boost only exists on a panel with a salt chlorinator, so its absence is benign.
	BOOST_CHECK_EQUAL(survey.PagesReached, 0u);
	BOOST_CHECK(!survey.EquipmentPageReached);
	BOOST_REQUIRE_EQUAL(survey.ExpectedAbsent.size(), 1u);
	BOOST_CHECK(survey.ExpectedAbsent.front().find("requires a salt chlorinator") != std::string::npos);
	BOOST_CHECK(survey.ExpectedAbsent.front().find("Boost") != std::string::npos);
	BOOST_CHECK(survey.NotableFailures.empty());
}

BOOST_AUTO_TEST_CASE(Survey_UngatedPageUnreachable_IsANotableFailure)
{
	// The Set Temperature page exists on every model, so failing to reach it is notable.
	SyncNavigatorTo(PageId::System);
	engine.StartCrawl(std::make_unique<TargetedVisitPolicy>(std::set<PageId>{ PageId::SetTemperature }));
	engine.ProcessStep(ContentFor(PageId::Service), Navigator::CURSOR_LINE_NONE);

	BOOST_REQUIRE(engine.GetFailedPages().contains(PageId::SetTemperature));

	const auto survey = OneTouch::BuildMenuSurvey(engine, model, device_id);

	BOOST_CHECK(survey.ExpectedAbsent.empty());
	BOOST_REQUIRE_EQUAL(survey.NotableFailures.size(), 1u);
	BOOST_CHECK_EQUAL(survey.NotableFailures.front(), std::string{ "SetTemperature" });
	BOOST_CHECK(!survey.EquipmentPageReached);
}

BOOST_AUTO_TEST_SUITE_END()
