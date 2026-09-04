#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_utility.hpp>
#include <nlohmann/json.hpp>

#include "equipment_cache/equipment_cache_service.h"
#include "http/capture_directory.h"
#include "http/server/server_types.h"
#include "http/webroute_diagnostics_auxrediscovery.h"
#include "http/webroute_diagnostics_captures.h"
#include "http/webroute_diagnostics_logging.h"
#include "http/webroute_diagnostics_matter.h"
#include "http/webroute_diagnostics_mqtt.h"
#include "interfaces/iequipmentdiscoverycontroller.h"
#include "kernel/data_hub.h"
#include "kernel/hub_locator.h"
#include "logging/logging_channels.h"
#include "logging/logging_severity_filter.h"
#include "logging/logging_severity_levels.h"
#include "mqtt/mqtt_client.h"
#include "mqtt/mqtt_hub.h"
#include "mqtt/mqtt_integration.h"
#include "options/options_equipment_options.h"
#include "options/options_mqtt_options.h"

using namespace AqualinkAutomate;

//=============================================================================
// Branch coverage for the diagnostics routes that had no (or partial) unit
// coverage: the MQTT status snapshot with a live (unstarted) integration, the
// Matter sidecar proxy in its disabled / unreachable / reachable states, the
// logging-level POST surface, the aux-rediscovery GET/POST outcomes, and the
// capture download/listing corners (percent-encoded names, ordering, bad
// targets).
//=============================================================================

namespace
{
	constexpr auto GET = boost::beast::http::verb::get;
	constexpr auto POST = boost::beast::http::verb::post;
	constexpr auto PUT = boost::beast::http::verb::put;

	HTTP::Request MakeRequest(boost::beast::http::verb method, std::string_view target, std::string_view body = {})
	{
		HTTP::Request req;
		req.version(11);
		req.method(method);
		req.target(target);
		req.set(boost::beast::http::field::host, "localhost.localdomain");

		if (!body.empty())
		{
			req.set(boost::beast::http::field::content_type, "application/json");
			req.body() = std::string{ body };
			req.prepare_payload();
		}

		return req;
	}

	nlohmann::json BodyOf(const HTTP::Response& resp)
	{
		return nlohmann::json::parse(resp.body(), nullptr, false);
	}

	// A unique, empty scratch directory per test.
	std::filesystem::path MakeTempDir(const std::string& tag)
	{
		auto dir = std::filesystem::temp_directory_path() / ("aqualink-diagbranches-" + tag);
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		std::filesystem::create_directories(dir, ec);
		return dir;
	}

	void WriteFile(const std::filesystem::path& path, const std::string& content)
	{
		std::ofstream out{ path, std::ios::out | std::ios::binary | std::ios::trunc };
		out << content;
	}

	class FakeDiscoveryController final : public Interfaces::IEquipmentDiscoveryController
	{
	public:
		bool RequestFullRediscovery() override
		{
			++request_calls;

			if (refuse)
			{
				return false;
			}

			status.in_progress = true;
			status.last_cleared_count = 3;
			return true;
		}

		DiscoveryStatusSnapshot DiscoveryStatus() const override
		{
			return status;
		}

	public:
		bool refuse{ false };
		int request_calls{ 0 };
		DiscoveryStatusSnapshot status{};
	};

	// Snapshot + restore of every channel's filter level, so the logging POSTs
	// below cannot leak a changed verbosity into other suites.
	struct LoggingLevelsGuard
	{
		LoggingLevelsGuard()
		{
			magic_enum::enum_for_each<Logging::Channel>([this](auto channel_entry)
				{
					Saved[channel_entry.value] = Logging::SeverityFiltering::GetChannelFilterLevel(channel_entry.value);
				});
		}

		~LoggingLevelsGuard()
		{
			for (const auto& [channel, severity] : Saved)
			{
				Logging::SeverityFiltering::SetChannelFilterLevel(channel, severity);
			}
		}

		std::map<Logging::Channel, Logging::Severity> Saved;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_HttpDiagnosticsBranches)

//-----------------------------------------------------------------------------
// /api/diagnostics/mqtt
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Mqtt_MethodNotAllowed)
{
	Kernel::HubLocator hub_locator;
	HTTP::WebRoute_Diagnostics_Mqtt route{ hub_locator };

	const auto resp = route.OnRequest(MakeRequest(POST, "/api/diagnostics/mqtt"));

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == resp.result());
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Mqtt_NotConstructedReportsDisabled)
{
	Kernel::HubLocator hub_locator;
	HTTP::WebRoute_Diagnostics_Mqtt route{ hub_locator };

	const auto resp = route.OnRequest(MakeRequest(GET, "/api/diagnostics/mqtt"));

	BOOST_CHECK(boost::beast::http::status::ok == resp.result());
	const auto body = BodyOf(resp);
	BOOST_CHECK(!body.value("enabled", true));
	BOOST_CHECK(!body.contains("running"));
	BOOST_CHECK(!body.contains("broker_host"));
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Mqtt_ReportsClientSnapshot)
{
	boost::asio::io_context ioc;

	Options::Mqtt::MqttSettings settings;
	settings.enabled = true;
	settings.broker_host = "broker.example.test";
	settings.broker_port = 18883;
	settings.topic_prefix = "diag";
	settings.home_assistant_enabled = false;
	settings.ha_discovery_prefix = "hass";

	// Constructed but never started: the client exists (so every field is
	// reported) without ever touching the network.
	Kernel::HubLocator hub_locator;
	hub_locator.Register(std::make_shared<Mqtt::MqttIntegration>(ioc, settings));

	HTTP::WebRoute_Diagnostics_Mqtt route{ hub_locator };

	const auto resp = route.OnRequest(MakeRequest(GET, "/api/diagnostics/mqtt"));
	BOOST_REQUIRE(boost::beast::http::status::ok == resp.result());

	const auto body = BodyOf(resp);
	BOOST_CHECK(body.value("enabled", false));
	BOOST_CHECK(!body.value("running", true));
	BOOST_CHECK(!body.value("connected", true));
	BOOST_CHECK(!body.value("state", "").empty());
	BOOST_CHECK_EQUAL(body.value("broker_host", ""), "broker.example.test");
	BOOST_CHECK_EQUAL(body.value("broker_port", 0), 18883);
	BOOST_CHECK(!body.value("tls", true));
	BOOST_CHECK(!body.value("protocol_version", "").empty());
	BOOST_CHECK_EQUAL(body.value("topic_prefix", ""), "diag");
	BOOST_CHECK(!body.value("client_id", "").empty());
	BOOST_CHECK(!body.value("home_assistant_enabled", true));
	BOOST_CHECK_EQUAL(body.value("ha_discovery_prefix", ""), "hass");
	BOOST_CHECK_EQUAL(body.value("queue_depth", 99), 0);
	BOOST_CHECK_EQUAL(body.value("reconnect_attempts", 99), 0);
	BOOST_CHECK_EQUAL(body.value("published", 99), 0);
	BOOST_CHECK_EQUAL(body.value("dropped", 99), 0);
	BOOST_CHECK(body.contains("last_error"));
}

//-----------------------------------------------------------------------------
// /api/diagnostics/matter
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Matter_MethodNotAllowed)
{
	HTTP::WebRoute_Diagnostics_Matter route{ false, 0 };

	const auto resp = route.OnRequest(MakeRequest(POST, "/api/diagnostics/matter"));

	BOOST_CHECK(boost::beast::http::status::method_not_allowed == resp.result());
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Matter_DisabledReportsOnlyFlag)
{
	HTTP::WebRoute_Diagnostics_Matter route{ false, 5540 };

	const auto resp = route.OnRequest(MakeRequest(GET, "/api/diagnostics/matter"));

	BOOST_CHECK(boost::beast::http::status::ok == resp.result());
	const auto body = BodyOf(resp);
	BOOST_CHECK(!body.value("enabled", true));
	BOOST_CHECK(!body.contains("status_port"));
	BOOST_CHECK(!body.contains("reachable"));
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Matter_EnabledButSidecarAbsent)
{
	// Reserve a loopback port and CLOSE it again, so nothing is listening there:
	// the background poller's connect is refused and the snapshot stays empty.
	std::uint16_t free_port{ 0 };
	{
		boost::asio::io_context ioc;
		boost::asio::ip::tcp::acceptor probe{ ioc, boost::asio::ip::tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), 0 } };
		free_port = probe.local_endpoint().port();
	}

	HTTP::WebRoute_Diagnostics_Matter route{ true, free_port };

	const auto resp = route.OnRequest(MakeRequest(GET, "/api/diagnostics/matter"));

	BOOST_CHECK(boost::beast::http::status::ok == resp.result());
	const auto body = BodyOf(resp);
	BOOST_CHECK(body.value("enabled", false));
	BOOST_CHECK_EQUAL(body.value("status_port", 0), free_port);
	BOOST_CHECK(!body.value("reachable", true));
	BOOST_CHECK(!body.value("running", true));
	BOOST_CHECK(!body.value("paired", true));
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Matter_ReachableSidecarStatusIsMerged)
{
	using tcp = boost::asio::ip::tcp;

	// A minimal fake sidecar on loopback: answer ONE GET /matter/status.
	boost::asio::io_context ioc;
	tcp::acceptor acceptor{ ioc, tcp::endpoint{ boost::asio::ip::make_address("127.0.0.1"), 0 } };
	const auto port = acceptor.local_endpoint().port();

	// Constructing the enabled route starts the poller, whose first fetch is
	// immediate; the acceptor above is already listening for it.
	HTTP::WebRoute_Diagnostics_Matter route{ true, port };

	{
		tcp::socket sidecar{ ioc };
		acceptor.accept(sidecar);

		boost::beast::flat_buffer buffer;
		boost::beast::http::request<boost::beast::http::string_body> req;
		boost::beast::error_code ec;
		boost::beast::http::read(sidecar, buffer, req, ec);
		BOOST_REQUIRE_MESSAGE(!ec, "sidecar could not read the poller's request: " + ec.message());
		BOOST_CHECK_EQUAL(std::string{ req.target() }, "/matter/status");

		boost::beast::http::response<boost::beast::http::string_body> res{ boost::beast::http::status::ok, req.version() };
		res.set(boost::beast::http::field::content_type, "application/json");
		res.body() = R"({"running":true,"paired":false,"fabrics":2,"qr_payload":"MT:TEST"})";
		res.prepare_payload();
		boost::beast::http::write(sidecar, res, ec);
		BOOST_REQUIRE_MESSAGE(!ec, "sidecar could not write its status: " + ec.message());

		sidecar.shutdown(tcp::socket::shutdown_both, ec);
	}
	acceptor.close();

	// The poller publishes the snapshot on its own thread right after the read
	// completes; wait (bounded) for the cached body to appear.
	nlohmann::json body;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

	do
	{
		body = BodyOf(route.OnRequest(MakeRequest(GET, "/api/diagnostics/matter")));

		if (body.value("reachable", false))
		{
			break;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	while (std::chrono::steady_clock::now() < deadline);

	BOOST_REQUIRE_MESSAGE(body.value("reachable", false), "sidecar snapshot never became reachable");
	BOOST_CHECK(body.value("enabled", false));
	BOOST_CHECK_EQUAL(body.value("status_port", 0), port);
	BOOST_CHECK(body.value("running", false));
	BOOST_CHECK(!body.value("paired", true));
	BOOST_CHECK_EQUAL(body.value("fabrics", 0), 2);
	BOOST_CHECK_EQUAL(body.value("qr_payload", ""), "MT:TEST");
}

//-----------------------------------------------------------------------------
// /api/diagnostics/logging
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Logging_GetAndMethodNotAllowed)
{
	HTTP::WebRoute_Diagnostics_Logging route;

	const auto listed = route.OnRequest(MakeRequest(GET, "/api/diagnostics/logging"));
	BOOST_REQUIRE(boost::beast::http::status::ok == listed.result());

	const auto body = BodyOf(listed);
	BOOST_REQUIRE(body.contains("channels"));
	BOOST_CHECK(body["channels"].contains("Web"));
	BOOST_REQUIRE(body.contains("severity_levels"));
	BOOST_CHECK_EQUAL(body["severity_levels"].size(), magic_enum::enum_count<Logging::Severity>());

	const auto refused = route.OnRequest(MakeRequest(PUT, "/api/diagnostics/logging", R"({"global":"Info"})"));
	BOOST_CHECK(boost::beast::http::status::method_not_allowed == refused.result());
	BOOST_CHECK_EQUAL(BodyOf(refused).value("code", ""), "method_not_allowed");
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Logging_PostGlobal)
{
	LoggingLevelsGuard guard;
	HTTP::WebRoute_Diagnostics_Logging route;

	// A valid global level applies to every channel.
	const auto ok = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"global":"Warning"})"));
	BOOST_CHECK(boost::beast::http::status::ok == ok.result());
	BOOST_CHECK_EQUAL(BodyOf(ok).value("status", ""), "ok");
	BOOST_CHECK(Logging::Severity::Warning == Logging::SeverityFiltering::GetChannelFilterLevel(Logging::Channel::Web));
	BOOST_CHECK(Logging::Severity::Warning == Logging::SeverityFiltering::GetChannelFilterLevel(Logging::Channel::Mqtt));

	// The GET reflects the change.
	const auto listed = BodyOf(route.OnRequest(MakeRequest(GET, "/api/diagnostics/logging")));
	BOOST_CHECK_EQUAL(listed["channels"].value("Web", ""), "Warning");

	// An unknown severity name is refused and nothing changes.
	const auto bad = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"global":"Loud"})"));
	BOOST_CHECK(boost::beast::http::status::bad_request == bad.result());
	BOOST_CHECK_EQUAL(BodyOf(bad).value("code", ""), "invalid_severity");
	BOOST_CHECK(Logging::Severity::Warning == Logging::SeverityFiltering::GetChannelFilterLevel(Logging::Channel::Web));

	// A non-string "global" trips the JSON type error -> invalid_json.
	const auto typed = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"global":3})"));
	BOOST_CHECK(boost::beast::http::status::bad_request == typed.result());
	BOOST_CHECK_EQUAL(BodyOf(typed).value("code", ""), "invalid_json");
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Logging_PostChannel)
{
	LoggingLevelsGuard guard;
	HTTP::WebRoute_Diagnostics_Logging route;

	Logging::SeverityFiltering::SetChannelFilterLevel(Logging::Channel::Web, Logging::Severity::Info);
	Logging::SeverityFiltering::SetChannelFilterLevel(Logging::Channel::Mqtt, Logging::Severity::Info);

	// One channel only.
	const auto ok = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"channel":"Web","level":"Trace"})"));
	BOOST_CHECK(boost::beast::http::status::ok == ok.result());
	BOOST_CHECK(Logging::Severity::Trace == Logging::SeverityFiltering::GetChannelFilterLevel(Logging::Channel::Web));
	BOOST_CHECK(Logging::Severity::Info == Logging::SeverityFiltering::GetChannelFilterLevel(Logging::Channel::Mqtt));

	// Unknown channel.
	const auto bad_channel = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"channel":"Teapot","level":"Trace"})"));
	BOOST_CHECK(boost::beast::http::status::bad_request == bad_channel.result());
	BOOST_CHECK_EQUAL(BodyOf(bad_channel).value("code", ""), "invalid_channel_or_severity");

	// Known channel, unknown level.
	const auto bad_level = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"channel":"Web","level":"Screaming"})"));
	BOOST_CHECK(boost::beast::http::status::bad_request == bad_level.result());
	BOOST_CHECK_EQUAL(BodyOf(bad_level).value("code", ""), "invalid_channel_or_severity");
	BOOST_CHECK(Logging::Severity::Trace == Logging::SeverityFiltering::GetChannelFilterLevel(Logging::Channel::Web));
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Logging_PostMissingFieldsAndBadJson)
{
	LoggingLevelsGuard guard;
	HTTP::WebRoute_Diagnostics_Logging route;

	// A channel without a level (and vice versa) is neither form.
	const auto only_channel = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"channel":"Web"})"));
	BOOST_CHECK(boost::beast::http::status::bad_request == only_channel.result());
	BOOST_CHECK_EQUAL(BodyOf(only_channel).value("code", ""), "logging_missing_fields");

	const auto only_level = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", R"({"level":"Info"})"));
	BOOST_CHECK(boost::beast::http::status::bad_request == only_level.result());

	const auto empty = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", "{}"));
	BOOST_CHECK(boost::beast::http::status::bad_request == empty.result());

	// Unparseable body.
	const auto garbage = route.OnRequest(MakeRequest(POST, "/api/diagnostics/logging", "{not json"));
	BOOST_CHECK(boost::beast::http::status::bad_request == garbage.result());
	BOOST_CHECK_EQUAL(BodyOf(garbage).value("code", ""), "invalid_json");
}

//-----------------------------------------------------------------------------
// /api/diagnostics/aux-rediscovery
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_AuxRediscovery_NoController)
{
	Kernel::HubLocator hub_locator;
	HTTP::WebRoute_Diagnostics_AuxRediscovery route{ hub_locator, nullptr };

	// GET: the default picture (nothing in progress, nothing cleared).
	const auto status = route.OnRequest(MakeRequest(GET, "/api/diagnostics/aux-rediscovery"));
	BOOST_CHECK(boost::beast::http::status::ok == status.result());
	BOOST_CHECK(!BodyOf(status).value("in_progress", true));
	BOOST_CHECK_EQUAL(BodyOf(status).value("last_cleared_count", 99), 0);

	// POST: rediscovery is impossible without a OneTouch controller.
	const auto post = route.OnRequest(MakeRequest(POST, "/api/diagnostics/aux-rediscovery"));
	BOOST_CHECK(boost::beast::http::status::service_unavailable == post.result());
	BOOST_CHECK_EQUAL(BodyOf(post).value("code", ""), "aux_rediscovery_unavailable");

	// Other verbs.
	const auto put = route.OnRequest(MakeRequest(PUT, "/api/diagnostics/aux-rediscovery"));
	BOOST_CHECK(boost::beast::http::status::method_not_allowed == put.result());
	BOOST_CHECK_EQUAL(BodyOf(put).value("code", ""), "method_not_allowed");
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_AuxRediscovery_ControllerRefusesIsConflict)
{
	auto controller = std::make_shared<FakeDiscoveryController>();
	controller->refuse = true;
	controller->status.in_progress = true;
	controller->status.last_cleared_count = 7;

	Kernel::HubLocator hub_locator;
	hub_locator.Register<Interfaces::IEquipmentDiscoveryController>(controller);

	HTTP::WebRoute_Diagnostics_AuxRediscovery route{ hub_locator, nullptr };

	// GET reports the controller's own status.
	const auto status = BodyOf(route.OnRequest(MakeRequest(GET, "/api/diagnostics/aux-rediscovery")));
	BOOST_CHECK(status.value("in_progress", false));
	BOOST_CHECK_EQUAL(status.value("last_cleared_count", 0), 7);

	// POST while a crawl is running: 409, the controller was asked exactly once.
	const auto post = route.OnRequest(MakeRequest(POST, "/api/diagnostics/aux-rediscovery"));
	BOOST_CHECK(boost::beast::http::status::conflict == post.result());
	BOOST_CHECK_EQUAL(BodyOf(post).value("code", ""), "aux_rediscovery_busy");
	BOOST_CHECK_EQUAL(controller->request_calls, 1);
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_AuxRediscovery_StartsAndFlushesCache)
{
	const auto dir = MakeTempDir("auxrediscovery");
	const auto cache_file = dir / "equipment-cache.json";

	boost::asio::io_context ioc;

	auto controller = std::make_shared<FakeDiscoveryController>();

	Kernel::HubLocator hub_locator;
	hub_locator.Register(std::make_shared<Kernel::DataHub>());
	hub_locator.Register<Interfaces::IEquipmentDiscoveryController>(controller);

	Options::Equipment::EquipmentSettings settings;
	settings.equipment_cache_file = cache_file.string();

	auto cache = std::make_shared<EquipmentCache::EquipmentCacheService>(ioc, hub_locator, settings);

	HTTP::WebRoute_Diagnostics_AuxRediscovery route{ hub_locator, cache };

	BOOST_REQUIRE(!std::filesystem::exists(cache_file));

	const auto post = route.OnRequest(MakeRequest(POST, "/api/diagnostics/aux-rediscovery"));
	BOOST_REQUIRE(boost::beast::http::status::ok == post.result());

	// The response carries the post-clear status...
	const auto body = BodyOf(post);
	BOOST_CHECK(body.value("in_progress", false));
	BOOST_CHECK_EQUAL(body.value("last_cleared_count", 0), 3);
	BOOST_CHECK_EQUAL(controller->request_calls, 1);

	// ...and the cache was flushed immediately (best effort, but here it works).
	BOOST_CHECK(std::filesystem::exists(cache_file));

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

//-----------------------------------------------------------------------------
// Capture download / listing corners
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Capture_PercentEncodedNameRoundTrips)
{
	const auto dir = MakeTempDir("percentname");
	WriteFile(dir / "pool run.cap", "0x10|0x02\n");

	HTTP::WebRoute_Diagnostics_Capture route{ HTTP::CaptureDirectory{ dir } };

	// The space arrives percent-encoded on the wire; the route decodes it, finds
	// the file, and re-encodes it for the RFC 5987 filename* form.
	const auto resp = route.OnRequest(MakeRequest(GET, "/api/diagnostics/recording/captures/pool%20run.cap"));

	BOOST_REQUIRE(boost::beast::http::status::ok == resp.result());
	BOOST_CHECK_EQUAL(resp.body(), "0x10|0x02\n");

	const std::string disposition{ resp[boost::beast::http::field::content_disposition] };
	BOOST_CHECK(disposition.find("filename=\"pool run.cap\"") != std::string::npos);
	BOOST_CHECK(disposition.find("filename*=UTF-8''pool%20run.cap") != std::string::npos);
	BOOST_CHECK_EQUAL(std::string{ resp["X-Content-Type-Options"] }, "nosniff");

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_Capture_TargetWithoutFilenameIsBadRequest)
{
	const auto dir = MakeTempDir("nofilename");
	HTTP::WebRoute_Diagnostics_Capture route{ HTTP::CaptureDirectory{ dir } };

	// No non-empty segment at all.
	const auto bare = route.OnRequest(MakeRequest(GET, "/"));
	BOOST_CHECK(boost::beast::http::status::bad_request == bare.result());
	BOOST_CHECK_EQUAL(BodyOf(bare).value("code", ""), "capture_filename_invalid");

	// A target that is not even a parseable origin-form URL.
	const auto unparseable = route.OnRequest(MakeRequest(GET, "/api/diagnostics/recording/captures/%zz.cap"));
	BOOST_CHECK(boost::beast::http::status::bad_request == unparseable.result());

	// Non-GET verbs.
	const auto post = route.OnRequest(MakeRequest(POST, "/api/diagnostics/recording/captures/x.cap"));
	BOOST_CHECK(boost::beast::http::status::method_not_allowed == post.result());

	HTTP::WebRoute_Diagnostics_Captures listing{ HTTP::CaptureDirectory{ dir } };
	BOOST_CHECK(boost::beast::http::status::method_not_allowed == listing.OnRequest(MakeRequest(PUT, "/api/diagnostics/recording/captures")).result());

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_CaptureDirectory_ListsNewestFirst)
{
	const auto dir = MakeTempDir("ordering");
	WriteFile(dir / "older.cap", "a\n");
	WriteFile(dir / "newer.cap", "bb\n");
	WriteFile(dir / "same-as-newer.cap", "ccc\n");

	// Push "older" an hour into the past; give the other two an identical stamp
	// so the name tie-break is exercised as well.
	std::error_code ec;
	const auto now = std::filesystem::last_write_time(dir / "newer.cap", ec);
	BOOST_REQUIRE(!ec);
	std::filesystem::last_write_time(dir / "older.cap", now - std::chrono::hours(1), ec);
	BOOST_REQUIRE(!ec);
	std::filesystem::last_write_time(dir / "same-as-newer.cap", now, ec);
	BOOST_REQUIRE(!ec);

	const HTTP::CaptureDirectory captures{ dir };
	const auto entries = captures.List();

	BOOST_REQUIRE_EQUAL(entries.size(), 3u);
	BOOST_CHECK_EQUAL(entries[0].name, "newer.cap");
	BOOST_CHECK_EQUAL(entries[1].name, "same-as-newer.cap");
	BOOST_CHECK_EQUAL(entries[2].name, "older.cap");
	BOOST_CHECK_EQUAL(entries[0].bytes, 3u);
	BOOST_CHECK_EQUAL(entries[2].bytes, 2u);
	BOOST_CHECK(entries[0].modified_unix > entries[2].modified_unix);

	// The listing route reports the same order.
	HTTP::WebRoute_Diagnostics_Captures route{ HTTP::CaptureDirectory{ dir } };
	const auto body = BodyOf(route.OnRequest(MakeRequest(GET, "/api/diagnostics/recording/captures")));
	BOOST_REQUIRE_EQUAL(body["captures"].size(), 3u);
	BOOST_CHECK_EQUAL(body["captures"][0].value("name", ""), "newer.cap");
	BOOST_CHECK_EQUAL(body["captures"][2].value("name", ""), "older.cap");

	std::filesystem::remove_all(dir, ec);
}

BOOST_AUTO_TEST_CASE(Test_DiagnosticsBranches_CaptureDirectory_BasenameRules)
{
	std::string reason;

	// Control characters / quotes are refused before anything else.
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("bad\nname.cap", reason));
	BOOST_CHECK_EQUAL(reason, "contains a control character or quote");
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("quoted\".cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename(std::string{ "del\x7f.cap" }, reason));

	// Either platform's separators and a drive specifier.
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("a/b.cap", reason));
	BOOST_CHECK_EQUAL(reason, "contains a path separator or drive specifier");
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("a\\b.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("c:b.cap", reason));

	// Parent-directory tokens.
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("..", reason));
	BOOST_CHECK_EQUAL(reason, "contains a parent-directory token");
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("a..b.cap", reason));

	// Extension.
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("noext", reason));
	BOOST_CHECK(reason.starts_with("must end in"));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("wrong.txt", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("case.CAP", reason));

	// The happy shape.
	BOOST_CHECK(HTTP::CaptureDirectory::IsAcceptableBasename("session-2026-09-04.cap", reason));
}

BOOST_AUTO_TEST_SUITE_END()
