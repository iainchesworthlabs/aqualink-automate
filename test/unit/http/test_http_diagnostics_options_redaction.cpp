#include <boost/test/unit_test.hpp>

#include <boost/beast/http/verb.hpp>

#include <nlohmann/json.hpp>

#include "http/webroute_diagnostics_options.h"
#include "options/options_option_type.h"

using namespace AqualinkAutomate;

BOOST_AUTO_TEST_SUITE(TestSuite_DiagnosticsOptionsRedaction)

//=============================================================================
// SECURITY REGRESSION (docs/auth-redesign.md §10): the options diagnostics
// surface must expose option METADATA only — never values.  Secret-bearing
// options (--api-auth-token today; --oidc-client-secret and the bootstrap
// password in later slices) would otherwise leak to any caller holding
// diagnostics access.  If this test starts failing because a "value" field
// was added, that change MUST come with a secret-redaction mechanism.
//=============================================================================

BOOST_AUTO_TEST_CASE(Test_DiagnosticsOptions_ExposesMetadataOnly)
{
	// Ensure a secret-bearing option exists in the registry for the assertion.
	auto secret_option = Options::make_appoption("test-redaction-secret-token", "a secret-bearing test option", boost::program_options::value<std::string>());

	HTTP::WebRoute_Diagnostics_Options route;

	HTTP::Request req;
	req.version(11);
	req.method(boost::beast::http::verb::get);
	req.target("/api/diagnostics/options");

	const auto resp = route.OnRequest(req);
	const auto body = nlohmann::json::parse(resp.body());

	BOOST_REQUIRE(body.contains("options"));
	BOOST_REQUIRE(!body["options"].empty());

	bool secret_option_listed = false;

	for (const auto& entry : body["options"])
	{
		// Metadata only: exactly these keys, nothing value-shaped.
		for (const auto& [key, unused] : entry.items())
		{
			BOOST_CHECK_MESSAGE(("name" == key) || ("short_name" == key) || ("description" == key),
				"Unexpected key '" + key + "' in /api/diagnostics/options entry - option VALUES must never be exposed without redaction");
		}

		if (entry.value("name", "") == "test-redaction-secret-token")
		{
			secret_option_listed = true;
		}
	}

	BOOST_CHECK(secret_option_listed);
}

BOOST_AUTO_TEST_SUITE_END()
