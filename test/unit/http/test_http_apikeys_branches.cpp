#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/buffer_traits.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "auth/api_key_store.h"
#include "auth/audit_log.h"
#include "auth/entitlement.h"
#include "auth/subject.h"
#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "http/webroute_apikey.h"
#include "http/webroute_apikeys.h"

using namespace AqualinkAutomate;

//=============================================================================
// Branch coverage for the API-key admin surface THROUGH the routing layer:
// every validation rejection of POST /api/apikeys (method, malformed body,
// missing/empty label, bad entitlement list, non-integer expiry), the optional
// expiry_unix leg, the shown-once secret, and the item route's 405/404/204
// outcomes.  The subject is stubbed directly (a resolver keyed on the bearer
// string) so no argon2 login is needed to reach the SYSTEM_ADMIN-gated routes.
//=============================================================================

namespace
{
	namespace fs = std::filesystem;

	constexpr auto GET = boost::beast::http::verb::get;
	constexpr auto POST = boost::beast::http::verb::post;
	constexpr auto PUT = boost::beast::http::verb::put;
	constexpr auto DELETE_ = boost::beast::http::verb::delete_;

	constexpr std::string_view ADMIN_BEARER{ "admin-credential" };
	constexpr std::string_view VIEWER_BEARER{ "viewer-credential" };

	struct ApiKeysBranchesFixture
	{
		ApiKeysBranchesFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-apikeys-branches-{}", counter++);
			fs::create_directories(Dir);

			ApiKeys = std::make_shared<Auth::ApiKeyStore>(Auth::ApiKeyStore::Load(Dir / "api-keys.json"));
			Audit = std::make_unique<Auth::AuditLog>(Auth::AuditLog::Config{});

			HTTP::Routing::Clear();
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_ApiKeys>(*ApiKeys, *Audit));
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_ApiKey>(*ApiKeys, *Audit));

			HTTP::Routing::SecurityConfig security;
			security.AuthModeEnabled = true;
			HTTP::Routing::SetSecurityConfig(std::move(security));

			// Stub resolver: the bearer string selects the subject shape.
			HTTP::Routing::SetSubjectResolver([](const HTTP::Request& req, bool) -> Auth::Subject
				{
					const auto it = req.find(boost::beast::http::field::authorization);
					const std::string header = (req.end() != it) ? std::string{ it->value() } : std::string{};

					if (header == std::format("Bearer {}", ADMIN_BEARER))
					{
						Auth::Subject admin;
						admin.Id = "admin-1";
						admin.Username = "admin";
						admin.Authenticated = true;
						admin.Provider = Auth::SubjectProvider::Local;
						admin.Entitlements = Auth::EntitlementSet::Parse({ "system.admin" });
						return admin;
					}

					if (header == std::format("Bearer {}", VIEWER_BEARER))
					{
						Auth::Subject viewer;
						viewer.Id = "viewer-1";
						viewer.Username = "viewer";
						viewer.Authenticated = true;
						viewer.Provider = Auth::SubjectProvider::Local;
						viewer.Entitlements = Auth::EntitlementSet::Parse({ "equipment.view" });
						return viewer;
					}

					return Auth::Subject::Anonymous();
				});
		}

		~ApiKeysBranchesFixture()
		{
			HTTP::Routing::Clear();

			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		HTTP::Request MakeRequest(boost::beast::http::verb method, std::string_view target, std::string_view raw_body = {}, std::string_view bearer = ADMIN_BEARER)
		{
			HTTP::Request req;
			req.version(11);
			req.method(method);
			req.target(target);
			req.set(boost::beast::http::field::host, "localhost.localdomain");

			if (!bearer.empty())
			{
				req.set(boost::beast::http::field::authorization, "Bearer " + std::string{ bearer });
			}

			if (!raw_body.empty())
			{
				req.set(boost::beast::http::field::content_type, "application/json");
				req.body() = std::string{ raw_body };
				req.prepare_payload();
			}

			return req;
		}

		HTTP::Request MakeJsonRequest(boost::beast::http::verb method, std::string_view target, const nlohmann::json& body, std::string_view bearer = ADMIN_BEARER)
		{
			return MakeRequest(method, target, body.dump(), bearer);
		}

		// Route through the synchronous facade and render the type-erased
		// message_generator back into an inspectable Response.
		HTTP::Response Dispatch(const HTTP::Request& req)
		{
			auto msg = HTTP::Routing::HTTP_OnRequest(req, "192.168.1.50");

			boost::beast::error_code ec;
			std::string wire;

			while (!msg.is_done())
			{
				const auto buffers = msg.prepare(ec);
				BOOST_REQUIRE(!ec);

				for (const auto b : boost::beast::buffers_range_ref(buffers))
				{
					wire.append(static_cast<const char*>(b.data()), b.size());
				}

				msg.consume(boost::beast::buffer_bytes(buffers));
			}

			boost::beast::http::response_parser<boost::beast::http::string_body> parser;
			parser.eager(true);
			parser.put(boost::asio::buffer(wire), ec);
			BOOST_REQUIRE_MESSAGE(!ec, "Could not parse serialised response: " + ec.message());

			if (!parser.is_done())
			{
				parser.put_eof(ec);
				BOOST_REQUIRE_MESSAGE(!ec, "Could not finish parsing serialised response: " + ec.message());
			}

			return parser.release();
		}

		nlohmann::json BodyOf(const HTTP::Response& resp)
		{
			return nlohmann::json::parse(resp.body(), nullptr, false);
		}

		fs::path Dir;
		std::shared_ptr<Auth::ApiKeyStore> ApiKeys;
		std::unique_ptr<Auth::AuditLog> Audit;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_HttpApiKeysBranches)

//-----------------------------------------------------------------------------
// Access gate: the collection is SYSTEM_ADMIN-only
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_AnonymousAndNonAdminRefused, ApiKeysBranchesFixture)
{
	BOOST_CHECK(boost::beast::http::status::unauthorized == Dispatch(MakeRequest(GET, "/api/apikeys", {}, {})).result());
	BOOST_CHECK(boost::beast::http::status::forbidden == Dispatch(MakeRequest(GET, "/api/apikeys", {}, VIEWER_BEARER)).result());
	BOOST_CHECK(boost::beast::http::status::forbidden == Dispatch(MakeRequest(DELETE_, "/api/apikeys/whatever", {}, VIEWER_BEARER)).result());
	BOOST_CHECK(ApiKeys->All().empty());
}

//-----------------------------------------------------------------------------
// Collection: method + body validation
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_CollectionMethodNotAllowed, ApiKeysBranchesFixture)
{
	const auto resp = Dispatch(MakeJsonRequest(PUT, "/api/apikeys", { { "label", "x" }, { "entitlements", nlohmann::json::array() } }));

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "GET or POST required");
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_CreateMalformedBodies, ApiKeysBranchesFixture)
{
	// Not JSON.
	const auto garbage = Dispatch(MakeRequest(POST, "/api/apikeys", "not json"));
	BOOST_CHECK(boost::beast::http::status::bad_request == garbage.result());
	BOOST_CHECK_EQUAL(BodyOf(garbage).value("error", ""), "Expected JSON body with label and entitlements");

	// Missing label.
	BOOST_CHECK(boost::beast::http::status::bad_request == Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "entitlements", { "equipment.view" } } })).result());

	// Label not a string.
	BOOST_CHECK(boost::beast::http::status::bad_request == Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", 7 }, { "entitlements", { "equipment.view" } } })).result());

	// Missing entitlements.
	BOOST_CHECK(boost::beast::http::status::bad_request == Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "ha" } })).result());

	// Nothing was minted.
	BOOST_CHECK(ApiKeys->All().empty());
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_CreateEmptyLabelRejected, ApiKeysBranchesFixture)
{
	const auto resp = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "" }, { "entitlements", { "equipment.view" } } }));

	BOOST_CHECK(boost::beast::http::status::bad_request == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "Label is required");
	BOOST_CHECK(ApiKeys->All().empty());
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_CreateBadEntitlementsRejected, ApiKeysBranchesFixture)
{
	// Not an array.
	const auto not_array = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "ha" }, { "entitlements", "equipment.view" } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == not_array.result());
	BOOST_CHECK_EQUAL(BodyOf(not_array).value("error", ""), "Expected an array of entitlement strings");

	// An array with a non-string entry.
	const auto non_string = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "ha" }, { "entitlements", { 1, 2 } } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == non_string.result());

	// Unknown actions: EVERY reject is listed so the admin UI can show them all.
	const auto unknown = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "ha" }, { "entitlements", { "equipment.view", "no.such.action", "also.bogus" } } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == unknown.result());
	const auto error = BodyOf(unknown).value("error", "");
	BOOST_CHECK(error.starts_with("Unknown or malformed entitlements: "));
	BOOST_CHECK(error.find("no.such.action") != std::string::npos);
	BOOST_CHECK(error.find("also.bogus") != std::string::npos);

	BOOST_CHECK(ApiKeys->All().empty());
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_CreateExpiryMustBeInteger, ApiKeysBranchesFixture)
{
	const auto textual = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "ha" }, { "entitlements", { "equipment.view" } }, { "expiry_unix", "next week" } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == textual.result());
	BOOST_CHECK_EQUAL(BodyOf(textual).value("error", ""), "Expected expiry_unix to be a unix timestamp");

	const auto fractional = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "ha" }, { "entitlements", { "equipment.view" } }, { "expiry_unix", 12.5 } }));
	BOOST_CHECK(boost::beast::http::status::bad_request == fractional.result());

	BOOST_CHECK(ApiKeys->All().empty());
}

//-----------------------------------------------------------------------------
// Collection: successful creation (with and without expiry) + listing
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_CreateWithExpiryAndList, ApiKeysBranchesFixture)
{
	constexpr std::int64_t EXPIRY{ 4102444800 };   // 2100-01-01T00:00:00Z

	const auto created = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "home-assistant" }, { "entitlements", { "equipment.view" } }, { "expiry_unix", EXPIRY } }));
	BOOST_REQUIRE(boost::beast::http::status::created == created.result());

	const auto body = BodyOf(created);
	BOOST_CHECK_EQUAL(body.value("label", ""), "home-assistant");
	BOOST_CHECK_EQUAL(body.value("expiry_unix", std::int64_t{ 0 }), EXPIRY);
	BOOST_CHECK_EQUAL(body.value("last_used_unix", std::int64_t{ -1 }), 0);
	BOOST_CHECK(!body.value("revoked", true));
	BOOST_CHECK(body.value("secret", "").starts_with("aak_"));
	BOOST_CHECK(!body.value("warning", "").empty());
	BOOST_REQUIRE(body.contains("entitlements"));
	BOOST_CHECK_EQUAL(body["entitlements"].size(), 1u);

	const auto id = body.value("id", "");
	BOOST_REQUIRE(!id.empty());

	// The store persisted the expiry and only the digest (never the secret).
	const auto record = ApiKeys->FindById(id);
	BOOST_REQUIRE(record.has_value());
	BOOST_CHECK_EQUAL(record->ExpiryUnix, EXPIRY);
	BOOST_CHECK_EQUAL(record->SecretSha256Hex, Auth::ApiKeyStore::DigestOf(body["secret"].get<std::string>()));

	// A second key without expiry (the 0 == never-expires default).
	const auto forever = Dispatch(MakeJsonRequest(POST, "/api/apikeys", { { "label", "forever" }, { "entitlements", nlohmann::json::array() } }));
	BOOST_REQUIRE(boost::beast::http::status::created == forever.result());
	BOOST_CHECK_EQUAL(BodyOf(forever).value("expiry_unix", std::int64_t{ -1 }), 0);

	// The listing carries both keys, and NEVER the secret or its digest.
	const auto listed = Dispatch(MakeRequest(GET, "/api/apikeys"));
	BOOST_REQUIRE(boost::beast::http::status::ok == listed.result());

	const auto list = BodyOf(listed);
	BOOST_REQUIRE(list.is_array());
	BOOST_REQUIRE_EQUAL(list.size(), 2u);

	for (const auto& entry : list)
	{
		BOOST_CHECK(entry.contains("id"));
		BOOST_CHECK(entry.contains("label"));
		BOOST_CHECK(entry.contains("entitlements"));
		BOOST_CHECK(entry.contains("expiry_unix"));
		BOOST_CHECK(!entry.contains("secret"));
		BOOST_CHECK(!entry.contains("secret_sha256"));
	}
}

//-----------------------------------------------------------------------------
// Item route: DELETE revokes; other verbs / unknown ids are refused
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_ItemMethodNotAllowed, ApiKeysBranchesFixture)
{
	std::string key_id;
	ApiKeys->Create("ha", Auth::EntitlementSet::Parse({ "equipment.view" }), 0, key_id);

	const auto resp = Dispatch(MakeRequest(GET, "/api/apikeys/" + key_id));

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == resp.result());
	BOOST_CHECK_EQUAL(BodyOf(resp).value("error", ""), "DELETE required");
	BOOST_CHECK(!ApiKeys->FindById(key_id)->Revoked);
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_ItemUnknownIdIs404, ApiKeysBranchesFixture)
{
	const auto resp = Dispatch(MakeRequest(DELETE_, "/api/apikeys/no-such-key"));

	BOOST_CHECK(boost::beast::http::status::not_found == resp.result());
	BOOST_CHECK(!BodyOf(resp).value("error", "").empty());
}

BOOST_FIXTURE_TEST_CASE(Test_ApiKeysBranches_ItemDeleteRevokes, ApiKeysBranchesFixture)
{
	std::string key_id;
	ApiKeys->Create("ha", Auth::EntitlementSet::Parse({ "equipment.view" }), 0, key_id);

	const auto resp = Dispatch(MakeRequest(DELETE_, "/api/apikeys/" + key_id));
	BOOST_CHECK(boost::beast::http::status::no_content == resp.result());
	BOOST_CHECK(resp.body().empty());

	// Revoked, and reported as such in the listing.
	BOOST_CHECK(ApiKeys->FindById(key_id)->Revoked);

	const auto list = BodyOf(Dispatch(MakeRequest(GET, "/api/apikeys")));
	BOOST_REQUIRE_EQUAL(list.size(), 1u);
	BOOST_CHECK(list[0].value("revoked", false));

	// Revocation is idempotent: the key still exists, so it answers 204 again.
	BOOST_CHECK(boost::beast::http::status::no_content == Dispatch(MakeRequest(DELETE_, "/api/apikeys/" + key_id)).result());
	BOOST_CHECK(ApiKeys->FindById(key_id)->Revoked);
}

BOOST_AUTO_TEST_SUITE_END()
