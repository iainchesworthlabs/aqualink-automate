#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <boost/asio/io_context.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "http/capture_directory.h"
#include "http/webroute_diagnostics_recording.h"
#include "interfaces/irecordingcontroller.h"
#include "kernel/hub_locator.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"

using namespace AqualinkAutomate;

//=============================================================================
// Route coverage for /api/diagnostics/recording.
//
// Drives the route's OnRequest directly against a HubLocator carrying a fake
// IRecordingController so the GET (status) and POST (start/stop) behaviour is
// validated without a real serial port.  Also covers the no-controller case
// (dev-mode/replay) where toggling must be rejected with 503.
//=============================================================================

namespace
{
	// The historical (and still default) capture directory: a relative "captures"
	// under the working directory.  Tests that exercise the CONFIGURABLE root use
	// a unique temporary directory instead (see MakeTempCaptureDir).
	constexpr const char* DEFAULT_CAPTURE_DIR{ "captures" };

	// A unique, empty capture root for the configurable-directory tests.  Named
	// per-test (not randomly) so a crashed run leaves an obvious artefact behind.
	std::filesystem::path MakeTempCaptureDir(const std::string& tag)
	{
		auto dir = std::filesystem::temp_directory_path() / ("aqualink-captures-" + tag);
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
		return dir;
	}

	// Minimal in-memory recording controller for route tests.
	class FakeRecordingController : public Interfaces::IRecordingController
	{
	public:
		bool StartRecording(const std::string& filename) override
		{
			start_calls++;
			last_start_filename = filename;
			if (fail_start) { return false; }
			if (m_Recording) { return false; }
			m_Recording = true;
			m_Status.recording = true;
			m_Status.file = filename;
			m_Status.bytes_written = 0;
			return true;
		}

		bool StopRecording() override
		{
			stop_calls++;
			if (!m_Recording) { return false; }
			m_Recording = false;
			m_Status.recording = false;
			// File path + byte count retained so a post-stop GET still reports them.
			return true;
		}

		bool IsRecording() const override { return m_Recording; }

		Status RecordingStatus() const override { return m_Status; }

	public:
		// Test knobs / observation.
		void SetBytes(std::size_t n) { m_Status.bytes_written = n; }

		bool fail_start = false;
		int start_calls = 0;
		int stop_calls = 0;
		std::string last_start_filename;

	private:
		bool m_Recording = false;
		Status m_Status;
	};

	// Serialize a route's message_generator response and parse it back into an
	// inspectable HTTP::Response (status code + body).  Mirrors the technique in
	// PerformHttpRequestResponse but works for an arbitrary request/route, which
	// the GET-only shared helper does not support.
	HTTP::Response InvokeRoute(HTTP::WebRoute_Diagnostics_Recording& route, HTTP::Request& req)
	{
		HTTP::Message msg = route.OnRequest(req);

		boost::asio::io_context ioc;
		auto exec = ioc.get_executor();

		Test::MockBeastBasicStreamWithTimeout client_stream(exec);
		Test::MockBeastBasicStreamWithTimeout server_stream(exec);
		server_stream.connect(client_stream);

		boost::beast::error_code ec;
		boost::beast::write(server_stream, std::move(msg), ec);
		if (ec)
		{
			throw std::runtime_error("Failed to write response: " + ec.message());
		}
		server_stream.close();
		ioc.poll();

		HTTP::Response resp;
		boost::beast::flat_buffer read_buffer;
		boost::beast::http::read(client_stream, read_buffer, resp, ec);
		if (ec && ec != boost::beast::http::error::end_of_stream)
		{
			throw std::runtime_error("Failed to read response: " + ec.message());
		}
		return resp;
	}

	HTTP::Request MakeGet()
	{
		HTTP::Request req;
		req.version(11);
		req.method(boost::beast::http::verb::get);
		req.target("/api/diagnostics/recording");
		req.set(boost::beast::http::field::host, "localhost.localdomain");
		return req;
	}

	HTTP::Request MakePost(const std::string& body)
	{
		HTTP::Request req;
		req.version(11);
		req.method(boost::beast::http::verb::post);
		req.target("/api/diagnostics/recording");
		req.set(boost::beast::http::field::host, "localhost.localdomain");
		req.set(boost::beast::http::field::content_type, "application/json");
		req.body() = body;
		req.prepare_payload();
		return req;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_HttpDiagnosticsRecording)

//-----------------------------------------------------------------------------
// GET reports the controller's current status as {recording,file,bytes}.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_Get_ReportsStatus)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	auto req = MakeGet();
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	auto json = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE(json.contains("recording"));
	BOOST_REQUIRE(json.contains("file"));
	BOOST_REQUIRE(json.contains("bytes"));
	BOOST_CHECK_EQUAL(json["recording"].get<bool>(), false);
	BOOST_CHECK_EQUAL(json["file"].get<std::string>(), "");
	BOOST_CHECK_EQUAL(json["bytes"].get<std::size_t>(), 0u);
}

//-----------------------------------------------------------------------------
// POST start begins recording and the response reflects the new status; a
// follow-up GET reports the live byte count.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_StartsAndReportsStatus)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	auto req = MakePost(R"({"action":"start","filename":"session.cap"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK_EQUAL(controller->start_calls, 1);
	// The filename is jailed into the capture directory before reaching the
	// controller, so it receives the safe absolute path (not the bare name).
	BOOST_CHECK(controller->last_start_filename.ends_with("session.cap"));
	BOOST_CHECK(controller->last_start_filename.find("captures") != std::string::npos);
	BOOST_CHECK(controller->IsRecording());

	auto json = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(json["recording"].get<bool>(), true);
	BOOST_CHECK_EQUAL(json["file"].get<std::string>(), "session.cap");

	// Simulate bytes accruing on the wire, then GET reflects them.
	controller->SetBytes(128);
	auto get_req = MakeGet();
	auto get_resp = InvokeRoute(route, get_req);
	auto get_json = nlohmann::json::parse(get_resp.body());
	BOOST_CHECK_EQUAL(get_json["bytes"].get<std::size_t>(), 128u);
}

//-----------------------------------------------------------------------------
// POST stop ends recording.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStop_Stops)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	// Start, then stop.
	{
		auto req = MakePost(R"({"action":"start","filename":"s.cap"})");
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	}

	auto req = MakePost(R"({"action":"stop"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK_EQUAL(controller->stop_calls, 1);
	BOOST_CHECK(!controller->IsRecording());

	auto json = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(json["recording"].get<bool>(), false);
}

//-----------------------------------------------------------------------------
// Starting while already recording is a 409 conflict.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_WhenAlreadyRecording_Conflict)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	{
		auto req = MakePost(R"({"action":"start","filename":"a.cap"})");
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	}

	auto req = MakePost(R"({"action":"start","filename":"b.cap"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());
	auto json = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE(json.contains("error"));
}

//-----------------------------------------------------------------------------
// Stopping when nothing is recording is a 409 conflict.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStop_WhenNotRecording_Conflict)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	auto req = MakePost(R"({"action":"stop"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());
}

//-----------------------------------------------------------------------------
// Malformed / invalid requests are 400.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_Post_BadRequests)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	// Invalid JSON.
	{
		auto req = MakePost("not-json");
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	}
	// Missing action.
	{
		auto req = MakePost(R"({"filename":"x.cap"})");
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	}
	// Unknown action.
	{
		auto req = MakePost(R"({"action":"pause"})");
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	}
	// start without filename.
	{
		auto req = MakePost(R"({"action":"start"})");
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	}
	// start with empty filename.
	{
		auto req = MakePost(R"({"action":"start","filename":""})");
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	}

	BOOST_CHECK_EQUAL(controller->start_calls, 0);
}

//-----------------------------------------------------------------------------
// A start that the controller refuses (e.g. file could not be opened) is 409.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_ControllerRefuses_Conflict)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	controller->fail_start = true;
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	// A valid bare basename so the request passes the path jail and actually
	// reaches the controller, which then refuses (e.g. file could not be opened).
	auto req = MakePost(R"({"action":"start","filename":"x.cap"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::conflict, resp.result());
	BOOST_CHECK_EQUAL(controller->start_calls, 1);
}

//-----------------------------------------------------------------------------
// SECURITY REGRESSION: the recording filename is an unauthenticated,
// attacker-controlled value.  Path-traversal / absolute / drive-letter forms
// must be rejected with 400 BEFORE the controller is ever asked to open a file,
// so the route cannot be used as an arbitrary file-write/truncate sink.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_PathTraversal_Rejected)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	const std::string malicious[] =
	{
		R"({"action":"start","filename":"../escape.cap"})",            // parent traversal
		R"({"action":"start","filename":"../../etc/passwd.cap"})",     // deep POSIX traversal
		R"({"action":"start","filename":"sub/dir/file.cap"})",         // nested separator
		R"({"action":"start","filename":"/etc/cron.d/evil.cap"})",     // absolute POSIX
		R"({"action":"start","filename":"\\windows\\system32\\x.cap"})", // backslash separators
		R"({"action":"start","filename":"C:\\windows\\evil.cap"})",    // drive letter
		R"({"action":"start","filename":"..\\escape.cap"})",           // windows parent traversal
		R"({"action":"start","filename":"good..\\..\\escape.cap"})",   // embedded dot-dot
	};

	for (const auto& body : malicious)
	{
		auto req = MakePost(body);
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_MESSAGE(boost::beast::http::status::bad_request == resp.result(),
			"Expected 400 for malicious filename body: " << body);
	}

	// The controller must never have been asked to open any of these.
	BOOST_CHECK_EQUAL(controller->start_calls, 0);
	BOOST_CHECK(!controller->IsRecording());
}

//-----------------------------------------------------------------------------
// SECURITY REGRESSION: a filename without the required .cap extension is
// rejected, so the sink cannot be steered at an arbitrary name inside the
// capture directory.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_WrongExtension_Rejected)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	auto req = MakePost(R"({"action":"start","filename":"config.conf"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::bad_request, resp.result());
	BOOST_CHECK_EQUAL(controller->start_calls, 0);
}

//-----------------------------------------------------------------------------
// A legitimate bare basename passes the jail and reaches the controller; the
// path handed to the controller is confined to the capture directory (the
// route hands an absolute, jailed path to the controller, not the raw input).
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_BareName_IsJailedIntoCaptureDir)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	auto req = MakePost(R"({"action":"start","filename":"capture.cap"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK_EQUAL(controller->start_calls, 1);

	// The controller was handed a jailed path, not the raw input, and that path
	// is inside the fixed "captures" directory and keeps the basename.
	const std::filesystem::path handed{ controller->last_start_filename };
	BOOST_CHECK_EQUAL(handed.filename().string(), "capture.cap");
	const auto parent = handed.parent_path().filename().string();
	BOOST_CHECK_EQUAL(parent, "captures");
}

//-----------------------------------------------------------------------------
// Non GET/POST verbs are 405.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_MethodNotAllowed)
{
	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	HTTP::Request req;
	req.version(11);
	req.method(boost::beast::http::verb::delete_);
	req.target("/api/diagnostics/recording");
	req.set(boost::beast::http::field::host, "localhost.localdomain");

	auto resp = InvokeRoute(route, req);
	BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, resp.result());
}

//-----------------------------------------------------------------------------
// No controller registered (dev-mode/replay): GET reports recording=false and
// POST toggles are rejected with 503.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_NoController_GetFalse_PostUnavailable)
{
	Kernel::HubLocator hub_locator; // no IRecordingController registered

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ DEFAULT_CAPTURE_DIR });

	auto get_req = MakeGet();
	auto get_resp = InvokeRoute(route, get_req);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, get_resp.result());
	auto get_json = nlohmann::json::parse(get_resp.body());
	BOOST_CHECK_EQUAL(get_json["recording"].get<bool>(), false);

	auto post_req = MakePost(R"({"action":"start","filename":"x.cap"})");
	auto post_resp = InvokeRoute(route, post_req);
	BOOST_CHECK_EQUAL(boost::beast::http::status::service_unavailable, post_resp.result());
}

//-----------------------------------------------------------------------------
// The capture directory is CONFIGURABLE (--capture-directory): a start request
// must land inside whatever root the route was constructed with, not a
// hardcoded "captures" under the working directory.  This is what lets a
// packaged deployment (the Home Assistant add-on) put captures somewhere the
// user can reach.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_JailsIntoConfiguredDirectory)
{
	const auto capture_dir = MakeTempCaptureDir("configured");

	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ capture_dir });

	auto req = MakePost(R"({"action":"start","filename":"configured.cap"})");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_REQUIRE_EQUAL(controller->start_calls, 1);

	// The controller was handed a path inside the CONFIGURED root...
	const std::filesystem::path handed{ controller->last_start_filename };
	BOOST_CHECK_EQUAL(handed.filename().string(), "configured.cap");

	std::error_code ec;
	const auto canonical_root = std::filesystem::weakly_canonical(capture_dir, ec);
	BOOST_REQUIRE(!ec);
	BOOST_CHECK_EQUAL(handed.parent_path().string(), canonical_root.string());

	// ...and the root was created on demand (a start must not fail because the
	// operator never made the directory).
	BOOST_CHECK(std::filesystem::is_directory(capture_dir));

	// The wire response still reports only the basename, never the server path.
	auto json = nlohmann::json::parse(resp.body());
	BOOST_CHECK_EQUAL(json["file"].get<std::string>(), "configured.cap");

	std::filesystem::remove_all(capture_dir, ec);
}

//-----------------------------------------------------------------------------
// SECURITY REGRESSION: making the capture directory configurable must NOT have
// widened the jail.  Every traversal form is still rejected with 400 before the
// controller is asked to open anything - now measured against a configured
// (non-default) root, and additionally asserting nothing escaped onto disk.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_PathTraversal_RejectedAgainstConfiguredDirectory)
{
	const auto capture_dir = MakeTempCaptureDir("traversal");
	std::filesystem::create_directories(capture_dir);

	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ capture_dir });

	const std::string malicious[] =
	{
		R"({"action":"start","filename":"../escape.cap"})",             // parent traversal
		R"({"action":"start","filename":"../../etc/passwd.cap"})",      // deep POSIX traversal
		R"({"action":"start","filename":"sub/dir/file.cap"})",          // nested separator
		R"({"action":"start","filename":"/etc/cron.d/evil.cap"})",      // absolute POSIX
		R"({"action":"start","filename":"\windows\system32\x.cap"})", // backslash separators
		R"({"action":"start","filename":"C:\windows\evil.cap"})",     // drive letter
		R"({"action":"start","filename":"..\escape.cap"})",            // windows parent traversal
		R"({"action":"start","filename":"good..\..\escape.cap"})",    // embedded dot-dot
		R"({"action":"start","filename":"notes.txt"})",                 // wrong extension
	};

	for (const auto& body : malicious)
	{
		auto req = MakePost(body);
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_MESSAGE(boost::beast::http::status::bad_request == resp.result(),
			"Expected 400 for malicious filename body: " << body);
	}

	BOOST_CHECK_EQUAL(controller->start_calls, 0);
	BOOST_CHECK(!controller->IsRecording());

	// Nothing was created inside the jail either.
	BOOST_CHECK(std::filesystem::is_empty(capture_dir));

	std::error_code ec;
	std::filesystem::remove_all(capture_dir, ec);
}

//-----------------------------------------------------------------------------
// SECURITY REGRESSION: a capture name is echoed back in the download route's
// Content-Disposition header, so control characters (CR/LF => header injection)
// and quotes (=> quoted-string break-out) must never survive the jail.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Recording_PostStart_ControlCharactersAndQuotes_Rejected)
{
	const auto capture_dir = MakeTempCaptureDir("headerinject");

	Kernel::HubLocator hub_locator;
	auto controller = std::make_shared<FakeRecordingController>();
	hub_locator.Register<Interfaces::IRecordingController>(controller);

	HTTP::WebRoute_Diagnostics_Recording route(hub_locator, HTTP::CaptureDirectory{ capture_dir });

	const std::string malicious[] =
	{
		R"({"action":"start","filename":"a\r\nX-Injected: yes.cap"})", // CRLF header injection
		R"({"action":"start","filename":"a\nb.cap"})",                 // bare LF
		R"({"action":"start","filename":"a\tb.cap"})",                 // tab (control char)
		R"({"action":"start","filename":"a\"b.cap"})",                 // quoted-string break-out
	};

	for (const auto& body : malicious)
	{
		auto req = MakePost(body);
		auto resp = InvokeRoute(route, req);
		BOOST_CHECK_MESSAGE(boost::beast::http::status::bad_request == resp.result(),
			"Expected 400 for hostile filename body: " << body);
	}

	BOOST_CHECK_EQUAL(controller->start_calls, 0);

	std::error_code ec;
	std::filesystem::remove_all(capture_dir, ec);
}

BOOST_AUTO_TEST_SUITE_END()
