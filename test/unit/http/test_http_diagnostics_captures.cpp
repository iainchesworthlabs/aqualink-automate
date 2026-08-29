#include <boost/test/unit_test.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <boost/asio/io_context.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "http/capture_directory.h"
#include "http/webroute_diagnostics_captures.h"

#include "mocks/mock_beast_basicstream_with_timeout.h"

using namespace AqualinkAutomate;

//=============================================================================
// Route coverage for the capture listing/download surface:
//
//   GET /api/diagnostics/recording/captures            -> { "captures": [...] }
//   GET /api/diagnostics/recording/captures/{filename} -> the capture file
//
// These exist so a finished capture can be retrieved WITHOUT a shell on the
// host (the Home Assistant add-on has no interactive container access).  The
// download route reuses the recording route's jail, so the security cases —
// traversal, absolute paths, drive letters, wrong extension — are asserted here
// against a real on-disk directory as well.
//=============================================================================

namespace
{
	// A unique, empty capture root per test.  Named per-test (not randomly) so a
	// crashed run leaves an obvious artefact behind.
	std::filesystem::path MakeTempCaptureDir(const std::string& tag)
	{
		auto dir = std::filesystem::temp_directory_path() / ("aqualink-capturesroute-" + tag);
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

	// Serialize a route's message_generator response and read it back as an
	// inspectable HTTP::Response (status, headers, body).
	template<typename ROUTE>
	HTTP::Response InvokeRoute(ROUTE& route, HTTP::Request& req)
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

	HTTP::Request MakeGet(const std::string& target)
	{
		HTTP::Request req;
		req.version(11);
		req.method(boost::beast::http::verb::get);
		req.target(target);
		req.set(boost::beast::http::field::host, "localhost.localdomain");
		return req;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_HttpDiagnosticsCaptures)

//-----------------------------------------------------------------------------
// An empty (or absent) capture directory lists nothing rather than erroring —
// the directory is only created when the first recording starts.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Captures_List_EmptyDirectory)
{
	const auto capture_dir = MakeTempCaptureDir("listempty");

	HTTP::WebRoute_Diagnostics_Captures route{ HTTP::CaptureDirectory{ capture_dir } };

	auto req = MakeGet("/api/diagnostics/recording/captures");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	auto json = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE(json.contains("captures"));
	BOOST_REQUIRE(json["captures"].is_array());
	BOOST_CHECK_EQUAL(json["captures"].size(), 0u);

	// A directory that does not exist at all behaves identically.
	std::error_code ec;
	std::filesystem::remove_all(capture_dir, ec);

	auto absent_resp = InvokeRoute(route, req);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, absent_resp.result());
	BOOST_CHECK_EQUAL(nlohmann::json::parse(absent_resp.body())["captures"].size(), 0u);
}

//-----------------------------------------------------------------------------
// The listing reports name/bytes/modified for each capture, and reports ONLY
// capture files — never other files that happen to share the directory, and
// never the server-side path.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Captures_List_ReportsOnlyCaptureFiles)
{
	const auto capture_dir = MakeTempCaptureDir("listfiles");

	WriteFile(capture_dir / "first.cap", "0x10|0x02\n");
	WriteFile(capture_dir / "second.cap", "0x10|0x02|0x50\n");
	WriteFile(capture_dir / "notes.txt", "not a capture");
	WriteFile(capture_dir / "secrets.conf", "password=hunter2");
	std::error_code ec;
	std::filesystem::create_directories(capture_dir / "subdir.cap", ec);

	HTTP::WebRoute_Diagnostics_Captures route{ HTTP::CaptureDirectory{ capture_dir } };

	auto req = MakeGet("/api/diagnostics/recording/captures");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());

	auto json = nlohmann::json::parse(resp.body());
	const auto& captures = json["captures"];
	BOOST_REQUIRE_EQUAL(captures.size(), 2u);

	bool saw_first = false;
	bool saw_second = false;
	for (const auto& entry : captures)
	{
		BOOST_REQUIRE(entry.contains("name"));
		BOOST_REQUIRE(entry.contains("bytes"));
		BOOST_REQUIRE(entry.contains("modified"));

		const auto name = entry["name"].get<std::string>();

		// Basename only: the capture directory is never disclosed.
		BOOST_CHECK(name.find('/') == std::string::npos);
		BOOST_CHECK(name.find('\\') == std::string::npos);

		if ("first.cap" == name) { saw_first = true; BOOST_CHECK_EQUAL(entry["bytes"].get<std::uintmax_t>(), 10u); }
		if ("second.cap" == name) { saw_second = true; BOOST_CHECK_EQUAL(entry["bytes"].get<std::uintmax_t>(), 15u); }
	}

	BOOST_CHECK(saw_first);
	BOOST_CHECK(saw_second);

	// The whole response must not leak the absolute capture root.
	BOOST_CHECK(resp.body().find(capture_dir.filename().string()) == std::string::npos);

	std::filesystem::remove_all(capture_dir, ec);
}

//-----------------------------------------------------------------------------
// Downloading a capture returns its bytes verbatim, as an attachment.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Capture_Download_ServesFileAsAttachment)
{
	const auto capture_dir = MakeTempCaptureDir("download");

	const std::string content{ "# Serial recording\n[  1] R 0x10|0x02|0x50\n" };
	WriteFile(capture_dir / "session.cap", content);

	HTTP::WebRoute_Diagnostics_Capture route{ HTTP::CaptureDirectory{ capture_dir } };

	auto req = MakeGet("/api/diagnostics/recording/captures/session.cap");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, resp.result());
	BOOST_CHECK_EQUAL(resp.body(), content);

	const auto disposition = resp[boost::beast::http::field::content_disposition];
	BOOST_CHECK(disposition.find("attachment") != std::string::npos);
	BOOST_CHECK(disposition.find("session.cap") != std::string::npos);

	// Never let a browser sniff a client-named, verbatim-served body.
	BOOST_CHECK_EQUAL(std::string{ resp["X-Content-Type-Options"] }, "nosniff");

	std::error_code ec;
	std::filesystem::remove_all(capture_dir, ec);
}

//-----------------------------------------------------------------------------
// A capture that is not there is a 404 (not a 500, and not a directory probe).
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Capture_Download_MissingFile_NotFound)
{
	const auto capture_dir = MakeTempCaptureDir("missing");

	HTTP::WebRoute_Diagnostics_Capture route{ HTTP::CaptureDirectory{ capture_dir } };

	auto req = MakeGet("/api/diagnostics/recording/captures/absent.cap");
	auto resp = InvokeRoute(route, req);

	BOOST_CHECK_EQUAL(boost::beast::http::status::not_found, resp.result());

	auto json = nlohmann::json::parse(resp.body());
	BOOST_REQUIRE(json.contains("code"));
	BOOST_CHECK_EQUAL(json["code"].get<std::string>(), "capture_not_found");

	std::error_code ec;
	std::filesystem::remove_all(capture_dir, ec);
}

//-----------------------------------------------------------------------------
// SECURITY REGRESSION: the download's {filename} is client-controlled.  It goes
// through the SAME jail as the recording filename, so no traversal form can
// read a file outside the capture directory.  A real, readable file is planted
// one level above the jail: the correct outcome is a rejection, NEVER its
// contents.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Capture_Download_PathTraversal_Rejected)
{
	const auto capture_dir = MakeTempCaptureDir("traversal");

	const std::string secret{ "TOP-SECRET-CONTENTS" };
	WriteFile(capture_dir.parent_path() / "aqualink-capturesroute-outside.cap", secret);
	WriteFile(capture_dir / "legit.cap", "0x10\n");

	HTTP::WebRoute_Diagnostics_Capture route{ HTTP::CaptureDirectory{ capture_dir } };

	const std::string malicious[] =
	{
		"/api/diagnostics/recording/captures/..%2Faqualink-capturesroute-outside.cap", // encoded parent traversal
		"/api/diagnostics/recording/captures/..%5Caqualink-capturesroute-outside.cap", // encoded windows traversal
		"/api/diagnostics/recording/captures/%2Fetc%2Fpasswd.cap",                     // encoded absolute POSIX
		"/api/diagnostics/recording/captures/C%3A%5Cwindows%5Cevil.cap",               // encoded drive letter
		"/api/diagnostics/recording/captures/notes.txt",                               // wrong extension
		"/api/diagnostics/recording/captures/good..%5C..%5Cescape.cap",                // embedded dot-dot
	};

	for (const auto& target : malicious)
	{
		auto req = MakeGet(target);
		auto resp = InvokeRoute(route, req);

		BOOST_CHECK_MESSAGE(boost::beast::http::status::bad_request == resp.result(),
			"Expected 400 for malicious capture target: " << target);
		BOOST_CHECK_MESSAGE(resp.body().find(secret) == std::string::npos,
			"Capture download leaked out-of-jail content for target: " << target);
	}

	// The legitimate name in the same directory still works, so the jail is
	// rejecting the traversal rather than the route being broken outright.
	auto ok_req = MakeGet("/api/diagnostics/recording/captures/legit.cap");
	auto ok_resp = InvokeRoute(route, ok_req);
	BOOST_CHECK_EQUAL(boost::beast::http::status::ok, ok_resp.result());

	std::error_code ec;
	std::filesystem::remove_all(capture_dir, ec);
	std::filesystem::remove(capture_dir.parent_path() / "aqualink-capturesroute-outside.cap", ec);
}

//-----------------------------------------------------------------------------
// Non-GET verbs are 405 on both routes.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Captures_MethodNotAllowed)
{
	const auto capture_dir = MakeTempCaptureDir("verbs");

	HTTP::WebRoute_Diagnostics_Captures list_route{ HTTP::CaptureDirectory{ capture_dir } };
	HTTP::WebRoute_Diagnostics_Capture item_route{ HTTP::CaptureDirectory{ capture_dir } };

	{
		auto req = MakeGet("/api/diagnostics/recording/captures");
		req.method(boost::beast::http::verb::delete_);
		auto resp = InvokeRoute(list_route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, resp.result());
	}
	{
		auto req = MakeGet("/api/diagnostics/recording/captures/x.cap");
		req.method(boost::beast::http::verb::delete_);
		auto resp = InvokeRoute(item_route, req);
		BOOST_CHECK_EQUAL(boost::beast::http::status::method_not_allowed, resp.result());
	}

	std::error_code ec;
	std::filesystem::remove_all(capture_dir, ec);
}

//-----------------------------------------------------------------------------
// The jail's basename rules are shared by the recording and download routes, so
// pin them directly: what may be recorded is exactly what may be downloaded.
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_CaptureDirectory_BasenameRules)
{
	std::string reason;

	BOOST_CHECK(HTTP::CaptureDirectory::IsAcceptableBasename("capture.cap", reason));
	BOOST_CHECK(HTTP::CaptureDirectory::IsAcceptableBasename("2026-08-29_bus.cap", reason));

	// Traversal / path forms.
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("../escape.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("sub/dir.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("sub\\dir.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("/abs.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("C:\\evil.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("good..\\..\\escape.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("..", reason));

	// Extension.
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("capture", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("capture.conf", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("capture.cap.conf", reason));

	// Header-hostile characters (echoed in Content-Disposition on download).
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("a\r\nX: y.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("a\nb.cap", reason));
	BOOST_CHECK(!HTTP::CaptureDirectory::IsAcceptableBasename("a\"b.cap", reason));

	// Every rejection carries a reason for the log line.
	BOOST_CHECK(!reason.empty());
}

//-----------------------------------------------------------------------------
// A symlink in the capture directory is a way to point outside the jail: it
// must neither be listed nor served, even when it ends in .cap.  (Symlink
// creation needs privileges on some hosts; the test skips itself if so.)
//-----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Test_Captures_SymlinkEscape_NotListedOrServed)
{
	const auto capture_dir = MakeTempCaptureDir("symlink");

	const std::string secret{ "TOP-SECRET-CONTENTS" };
	const auto outside = capture_dir.parent_path() / "aqualink-capturesroute-symtarget.cap";
	WriteFile(outside, secret);

	std::error_code ec;
	std::filesystem::create_symlink(outside, capture_dir / "link.cap", ec);

	if (!ec)
	{
		HTTP::WebRoute_Diagnostics_Captures list_route{ HTTP::CaptureDirectory{ capture_dir } };
		auto list_req = MakeGet("/api/diagnostics/recording/captures");
		auto list_resp = InvokeRoute(list_route, list_req);
		BOOST_CHECK_EQUAL(nlohmann::json::parse(list_resp.body())["captures"].size(), 0u);

		HTTP::WebRoute_Diagnostics_Capture item_route{ HTTP::CaptureDirectory{ capture_dir } };
		auto item_req = MakeGet("/api/diagnostics/recording/captures/link.cap");
		auto item_resp = InvokeRoute(item_route, item_req);

		BOOST_CHECK(boost::beast::http::status::ok != item_resp.result());
		BOOST_CHECK(item_resp.body().find(secret) == std::string::npos);
	}

	std::filesystem::remove_all(capture_dir, ec);
	std::filesystem::remove(outside, ec);
}

BOOST_AUTO_TEST_SUITE_END()
