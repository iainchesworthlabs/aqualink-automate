#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

#include <boost/beast/http/verb.hpp>

#include "http/webroute_auth_check.h"
#include "http/webroute_auth_me.h"
#include "http/webroute_diagnostics_actualdevices.h"
#include "http/webroute_diagnostics_devices.h"
#include "http/webroute_diagnostics_logging.h"
#include "http/webroute_diagnostics_matter.h"
#include "http/webroute_diagnostics_mqtt.h"
#include "http/webroute_diagnostics_options.h"
#include "http/webroute_diagnostics_profiling.h"
#include "http/webroute_diagnostics_recording.h"
#include "http/webroute_equipment.h"
#include "http/webroute_equipment_button.h"
#include "http/webroute_equipment_buttons.h"
#include "http/webroute_equipment_chlorinator.h"
#include "http/webroute_equipment_circulation.h"
#include "http/webroute_equipment_devices.h"
#include "http/webroute_equipment_heater.h"
#include "http/webroute_equipment_iaq.h"
#include "http/webroute_equipment_setpoints.h"
#include "http/webroute_equipment_version.h"
#include "http/webroute_health.h"
#include "http/webroute_health_detailed.h"
#include "http/webroute_metrics.h"
#include "http/webroute_version.h"
#include "http/websocket_equipment.h"
#include "http/websocket_equipment_stats.h"
#include "interfaces/iwebroute.h"
#include "interfaces/iwebsocket.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

//=============================================================================
// FULL-SURFACE ENFORCEMENT (docs/auth-redesign.md §12): every registered API
// route must declare its RequiredAccess so the PolicyEngine gates it when the
// identity system is enabled.  A new route added WITHOUT a declaration fails
// here unless it is on the EXPLICIT exemption list below (deliberately-open
// endpoints only).  Do not "fix" a failure by extending the exemption list —
// declare the route's access instead.
//
// Routes whose constructors need heavier services (preferences, history,
// schedules, spaside) are compile-verified by their headers' overrides and
// exercised through TestSuite_RoutingAuthz; the registrable set below covers
// every hub-constructible route.
//=============================================================================

namespace
{
	constexpr auto GET = boost::beast::http::verb::get;
	constexpr auto POST = boost::beast::http::verb::post;

	void CheckDeclared(const Interfaces::IWebRouteBase& route)
	{
		const bool declared = route.RequiredAccess(GET).IsSpecified() || route.RequiredAccess(POST).IsSpecified();

		BOOST_CHECK_MESSAGE(declared, std::string{ route.Route() } + " declares no RequiredAccess for GET or POST - every non-exempt route must be entitlement-gated");
	}

	void CheckExempt(const Interfaces::IWebRouteBase& route)
	{
		const bool open = !route.RequiredAccess(GET).IsSpecified() && !route.RequiredAccess(POST).IsSpecified();

		BOOST_CHECK_MESSAGE(open, std::string{ route.Route() } + " is on the deliberate-exemption list but declares RequiredAccess - update the list");
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_AccessEnforcement)

BOOST_AUTO_TEST_CASE(Test_Enforcement_EveryGatedRouteDeclaresAccess)
{
	Test::HubLocatorInjector hub_locator;

	// Hub-constructible gated routes: MUST declare access.
	CheckDeclared(HTTP::WebRoute_Diagnostics_ActualDevices{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Diagnostics_Devices{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Diagnostics_Logging{});
	CheckDeclared(HTTP::WebRoute_Diagnostics_Matter{ false, 0 });
	CheckDeclared(HTTP::WebRoute_Diagnostics_Mqtt{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Diagnostics_Options{});
	CheckDeclared(HTTP::WebRoute_Diagnostics_Profiling{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Diagnostics_Recording{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Button{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Buttons{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Chlorinator{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Circulation{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Devices{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Heater{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_IAQ{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Setpoints{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Equipment_Version{ hub_locator });
	CheckDeclared(HTTP::WebRoute_HealthDetailed{ hub_locator });
	CheckDeclared(HTTP::WebRoute_Metrics{ hub_locator });
}

BOOST_AUTO_TEST_CASE(Test_Enforcement_ButtonRouteCarriesAuxResourceKind)
{
	Test::HubLocatorInjector hub_locator;

	// The per-aux route must declare the resource kind so the router extracts
	// the aux id from the path and enforces selector-scoped grants.
	const HTTP::WebRoute_Equipment_Button route{ hub_locator };

	BOOST_CHECK_EQUAL(std::string{ route.RequiredAccess(POST).ResourceKind }, "aux");
}

BOOST_AUTO_TEST_CASE(Test_Enforcement_DeliberatelyOpenRoutesStayOpen)
{
	// The ONLY endpoints allowed to skip the entitlement gate: liveness probe,
	// auth-state probes, and the version banner.
	CheckExempt(HTTP::WebRoute_AuthCheck{});
	CheckExempt(HTTP::WebRoute_AuthMe{});
	CheckExempt(HTTP::WebRoute_Health{});
	CheckExempt(HTTP::WebRoute_Version{});
}

BOOST_AUTO_TEST_CASE(Test_Enforcement_WebSocketsDeclareAccess)
{
	Test::HubLocatorInjector hub_locator;

	BOOST_CHECK(HTTP::WebSocket_Equipment{ hub_locator }.RequiredAccess().IsSpecified());
	BOOST_CHECK(HTTP::WebSocket_Equipment_Stats{ hub_locator }.RequiredAccess().IsSpecified());
}

BOOST_AUTO_TEST_SUITE_END()
