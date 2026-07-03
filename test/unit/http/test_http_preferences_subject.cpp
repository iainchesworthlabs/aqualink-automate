#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/verb.hpp>
#include <nlohmann/json.hpp>

#include "auth/group_store.h"
#include "auth/jwt_codec.h"
#include "auth/jwt_key_store.h"
#include "auth/subject_resolver.h"
#include "auth/user_store.h"
#include "http/server/routing/routing.h"
#include "http/server/server_types.h"
#include "http/webroute_preferences.h"
#include "kernel/preferences_hub.h"
#include "options/options_preferences_options.h"
#include "preferences/preferences_service.h"
#include "preferences/user_preferences_store.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;

//=============================================================================
// Subject-aware /api/preferences (docs/auth-redesign.md §8, D7): per-user
// units/theme/accent/chemistry_bands routed to the caller's slice; system
// fields admin-gated; global defaults show through until a user overrides.
//=============================================================================

namespace
{
	namespace fs = std::filesystem;

	struct PreferencesFixture : Test::HubLocatorInjector
	{
		PreferencesFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-prefs-subject-{}", counter++);
			fs::create_directories(Dir);

			Options::Preferences::PreferencesSettings settings;   // in-memory global prefs
			Service = std::make_shared<Preferences::PreferencesService>(*this, settings);
			Service->Seed(2600, 60, "", 90);   // global defaults (units default = Celsius)

			UserPrefs = std::make_shared<Preferences::UserPreferencesStore>(Preferences::UserPreferencesStore::Load(Dir / "user_preferences.json"));

			Groups = std::make_shared<Auth::GroupStore>(Auth::GroupStore::Load(Dir / "groups.json"));
			Users = std::make_shared<Auth::UserStore>(Auth::UserStore::Load(Dir / "users.json"));

			Keys = std::make_shared<Auth::JwtKeyStore>(Auth::JwtKeyStore::LoadOrCreate(Dir / "jwt.key"));
			Codec = std::make_shared<Auth::JwtCodec>(Keys, Auth::JwtCodec::Config{});

			std::string error;
			Auth::UserRecord alice;
			alice.Username = "alice";
			alice.PasswordHash = "$argon2id$fake";
			alice.Groups = { std::string{ Auth::BuiltInGroups::ADMINISTRATORS } };
			BOOST_REQUIRE(Users->Create(std::move(alice), error));
			AliceId = Users->FindByUsername("alice")->Id;

			Auth::UserRecord bob;   // a plain user: view only, no admin
			bob.Username = "bob";
			bob.PasswordHash = "$argon2id$fake";
			bob.DirectEntitlements = Auth::EntitlementSet::Parse({ "equipment.view" });
			BOOST_REQUIRE(Users->Create(std::move(bob), error));
			BobId = Users->FindByUsername("bob")->Id;

			HTTP::Routing::Clear();
			HTTP::Routing::Add(std::make_unique<HTTP::WebRoute_Preferences>(Service, UserPrefs));

			HTTP::Routing::SecurityConfig security;
			security.AuthModeEnabled = true;
			HTTP::Routing::SetSecurityConfig(std::move(security));

			HTTP::Routing::SetSubjectResolver(Auth::MakeSubjectResolver(Auth::SubjectResolverDeps{
				.Groups = Groups->SharedRegistry(),
				.Codec = Codec,
				.Users = Users }));
		}

		~PreferencesFixture()
		{
			HTTP::Routing::Clear();
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		std::string TokenFor(const std::string& user_id)
		{
			const auto user = Users->FindById(user_id);
			Auth::TokenClaims claims;
			claims.Subject = user->Id;
			claims.Provider = Auth::SubjectProvider::Local;
			claims.TokenVersion = user->TokenVersion;
			claims.Groups = user->Groups;
			claims.Entitlements = Groups->Registry().ResolveEffectiveEntitlements(user->DirectEntitlements, user->Groups).ToStrings();
			return Codec->Sign(claims);
		}

		HTTP::Request MakeRequest(boost::beast::http::verb method, const nlohmann::json& body, std::string_view bearer)
		{
			HTTP::Request req;
			req.version(11);
			req.method(method);
			req.target("/api/preferences");
			req.set(boost::beast::http::field::host, "localhost.localdomain");
			req.set(boost::beast::http::field::authorization, "Bearer " + std::string{ bearer });

			if (!body.empty())
			{
				req.set(boost::beast::http::field::content_type, "application/json");
				req.body() = body.dump();
				req.prepare_payload();
			}

			return req;
		}

		HTTP::Response Dispatch(HTTP::Request req)
		{
			auto guard = boost::asio::make_work_guard(IoContext);
			std::optional<HTTP::Message> serialised;

			HTTP::Routing::HTTP_OnRequestDispatch(std::move(req), "10.0.0.5",
				[&](HTTP::Message&& msg) { serialised = std::move(msg); guard.reset(); });

			IoContext.restart();
			IoContext.run();

			BOOST_REQUIRE(serialised.has_value());

			boost::beast::error_code ec;
			std::string wire;
			while (!serialised->is_done())
			{
				const auto buffers = serialised->prepare(ec);
				BOOST_REQUIRE(!ec);
				for (const auto b : boost::beast::buffers_range_ref(buffers))
				{
					wire.append(static_cast<const char*>(b.data()), b.size());
				}
				serialised->consume(boost::beast::buffer_bytes(buffers));
			}

			boost::beast::http::response_parser<boost::beast::http::string_body> parser;
			parser.eager(true);
			parser.put(boost::asio::buffer(wire), ec);
			if (!parser.is_done()) { parser.put_eof(ec); }
			return parser.release();
		}

		nlohmann::json GetAs(const std::string& user_id)
		{
			const auto resp = Dispatch(MakeRequest(boost::beast::http::verb::get, {}, TokenFor(user_id)));
			BOOST_REQUIRE(boost::beast::http::status::ok == resp.result());
			return nlohmann::json::parse(resp.body());
		}

		fs::path Dir;
		boost::asio::io_context IoContext;
		std::shared_ptr<Preferences::PreferencesService> Service;
		std::shared_ptr<Preferences::UserPreferencesStore> UserPrefs;
		std::shared_ptr<Auth::GroupStore> Groups;
		std::shared_ptr<Auth::UserStore> Users;
		std::shared_ptr<Auth::JwtKeyStore> Keys;
		std::shared_ptr<Auth::JwtCodec> Codec;
		std::string AliceId, BobId;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_PreferencesSubject)

BOOST_FIXTURE_TEST_CASE(Test_Prefs_GetShowsGlobalDefaultsWithPerUserFields, PreferencesFixture)
{
	const auto view = GetAs(BobId);

	// Global default until overridden, plus the per-user display fields.
	BOOST_CHECK_EQUAL(view.value("temperature_units", ""), "Celsius");
	BOOST_CHECK_EQUAL(view.value("theme", ""), "system");
	BOOST_CHECK_EQUAL(view.value("accent", ""), "teal");
	BOOST_CHECK(view.contains("chemistry_bands"));
}

BOOST_FIXTURE_TEST_CASE(Test_Prefs_PerUserWriteIsIsolatedAndDoesNotTouchGlobal, PreferencesFixture)
{
	// Bob sets his own units + theme.
	const auto put = Dispatch(MakeRequest(boost::beast::http::verb::put, { { "temperature_units", "Fahrenheit" }, { "theme", "dark" } }, TokenFor(BobId)));
	BOOST_REQUIRE(boost::beast::http::status::ok == put.result());

	// Bob sees his overrides...
	const auto bob_view = GetAs(BobId);
	BOOST_CHECK_EQUAL(bob_view.value("temperature_units", ""), "Fahrenheit");
	BOOST_CHECK_EQUAL(bob_view.value("theme", ""), "dark");

	// ...Alice is unaffected (isolation)...
	const auto alice_view = GetAs(AliceId);
	BOOST_CHECK_EQUAL(alice_view.value("temperature_units", ""), "Celsius");
	BOOST_CHECK_EQUAL(alice_view.value("theme", ""), "system");

	// ...and the GLOBAL hub default is untouched (backend/MQTT still Celsius).
	BOOST_CHECK(Find<Kernel::PreferencesHub>()->Temperature_DisplayUnits == Kernel::TemperatureUnits::Celsius);
}

BOOST_FIXTURE_TEST_CASE(Test_Prefs_NonAdminCannotWriteSystemFields, PreferencesFixture)
{
	// A system field (label_overrides) from a non-admin is refused...
	const auto denied = Dispatch(MakeRequest(boost::beast::http::verb::put, { { "label_overrides", { { "aux1", "Pool Light" } } } }, TokenFor(BobId)));
	BOOST_CHECK(boost::beast::http::status::forbidden == denied.result());

	// ...and a mixed document is refused ATOMICALLY: the per-user field does
	// not sneak through when a system field is present.
	const auto mixed = Dispatch(MakeRequest(boost::beast::http::verb::put, { { "theme", "dark" }, { "label_overrides", { { "aux1", "X" } } } }, TokenFor(BobId)));
	BOOST_CHECK(boost::beast::http::status::forbidden == mixed.result());
	BOOST_CHECK(!UserPrefs->HasOverrides(BobId));
}

BOOST_FIXTURE_TEST_CASE(Test_Prefs_AdminWritesSystemAndOwnPerUser, PreferencesFixture)
{
	// Admin sets a system field...
	const auto sys = Dispatch(MakeRequest(boost::beast::http::verb::put, { { "label_overrides", { { "aux1", "Pool Light" } } } }, TokenFor(AliceId)));
	BOOST_REQUIRE(boost::beast::http::status::ok == sys.result());
	BOOST_CHECK_EQUAL(Find<Kernel::PreferencesHub>()->LabelOverrides.value("aux1", ""), "Pool Light");

	// ...and her own per-user field, which stays hers.
	const auto usr = Dispatch(MakeRequest(boost::beast::http::verb::put, { { "accent", "violet" } }, TokenFor(AliceId)));
	BOOST_REQUIRE(boost::beast::http::status::ok == usr.result());
	BOOST_CHECK_EQUAL(GetAs(AliceId).value("accent", ""), "violet");
	BOOST_CHECK_EQUAL(GetAs(BobId).value("accent", ""), "teal");
}

BOOST_FIXTURE_TEST_CASE(Test_Prefs_InvalidPerUserValueRejected, PreferencesFixture)
{
	const auto resp = Dispatch(MakeRequest(boost::beast::http::verb::put, { { "temperature_units", "Kelvin" } }, TokenFor(BobId)));
	BOOST_CHECK(boost::beast::http::status::bad_request == resp.result());
}

BOOST_AUTO_TEST_SUITE_END()
