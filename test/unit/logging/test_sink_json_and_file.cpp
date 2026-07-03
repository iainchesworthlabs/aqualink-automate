#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include <boost/log/core.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <nlohmann/json.hpp>

#include "logging/logging.h"
#include "logging/logging_channels.h"
#include "logging/logging_formatter.h"
#include "logging/logging_severity_filter.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/sink_console.h"
#include "logging/sinks/sink_file.h"
#include "logging/sinks/sink_registry.h"

//
// Slice 2 coverage: the JSON-lines formatter (via the console sink) and the
// rotating async file sink. Both exercise the real Boost.Log emit path.
//

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Logging::Sinks;

namespace
{
	namespace fs = std::filesystem;

	struct CoreFixture
	{
		CoreFixture()
		{
			boost::log::add_common_attributes();
			boost::log::core::get()->remove_all_sinks();
			SeverityFiltering::SetGlobalFilterLevel(Severity::Trace);
		}

		~CoreFixture()
		{
			SinkRegistry::RemoveAll();
			boost::log::core::get()->remove_all_sinks();
			SeverityFiltering::SetGlobalFilterLevel(SeverityFiltering::DEFAULT_SEVERITY);
		}
	};

	struct TempDirFixture : CoreFixture
	{
		TempDirFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-logfile-test-{}", counter++);
			fs::create_directories(Dir);
		}

		~TempDirFixture()
		{
			// Sinks are torn down by ~CoreFixture (base) BEFORE this runs, so the file
			// handle is released and the directory can be removed.
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_SinkJsonAndFile)

//-----------------------------------------------------------------------------
// JSON-lines format (rendered via the console sink)
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(Json_EveryLineParses_WithExpectedFields, CoreFixture, *boost::unit_test::label("unit"))
{
	auto capture = boost::make_shared<std::ostringstream>();
	ConsoleSinkConfig config;
	config.Stream = capture;
	config.Format = LogFormat::Json;
	SinkRegistry::Add(MakeConsoleSink(config));

	LogInfo(Channel::Web, "plain message");
	LogWarning(Channel::Mqtt, R"(has "quotes" and	a tab)");
	LogDebug(Channel::Main, "debug with file/line");
	SinkRegistry::FlushAll();

	std::istringstream lines(capture->str());
	std::string line;
	std::size_t count = 0;
	bool saw_debug_with_location = false;

	while (std::getline(lines, line))
	{
		if (line.empty())
		{
			continue;
		}

		// Every emitted line MUST be valid JSON — the guarantee pipelines rely on.
		const auto entry = nlohmann::json::parse(line);
		++count;

		BOOST_TEST(entry.contains("severity"));
		BOOST_TEST(entry.contains("channel"));
		BOOST_TEST(entry.contains("message"));

		if (entry.value("severity", "") == "Debug")
		{
			saw_debug_with_location = entry.contains("file") && entry.contains("line");
		}
	}

	BOOST_TEST(count == static_cast<std::size_t>(3));
	// file/line present only for Trace/Debug (mirrors the text formatter).
	BOOST_TEST(saw_debug_with_location);
}

BOOST_FIXTURE_TEST_CASE(Json_EscapesQuotesAndPreservesMessage, CoreFixture, *boost::unit_test::label("unit"))
{
	auto capture = boost::make_shared<std::ostringstream>();
	ConsoleSinkConfig config;
	config.Stream = capture;
	config.Format = LogFormat::Json;
	SinkRegistry::Add(MakeConsoleSink(config));

	LogInfo(Channel::Web, R"(embedded "quote" chars)");
	SinkRegistry::FlushAll();

	const auto entry = nlohmann::json::parse(capture->str());
	BOOST_TEST(entry.value("message", "") == std::string(R"(embedded "quote" chars)"));
	BOOST_TEST(entry.value("channel", "") == "Web");
	BOOST_TEST(entry.value("severity", "") == "Info");
}

//-----------------------------------------------------------------------------
// Rotating async file sink
//-----------------------------------------------------------------------------

BOOST_FIXTURE_TEST_CASE(File_WritesAndRotatesWithinBoundedCount, TempDirFixture, *boost::unit_test::label("unit"))
{
	const auto log_path = Dir / "app.log";

	FileSinkConfig config;
	config.Path = log_path;
	config.MaxFileBytes = 256;   // tiny, so a handful of records trip rotation
	config.MaxFiles = 2;         // keep at most 2 rotated files
	config.Format = LogFormat::Text;
	SinkRegistry::Add(MakeFileSink(config));

	for (int i = 0; i < 60; ++i)
	{
		LogInfo(Channel::Main, std::format("file sink record number {} with some padding text", i));
	}

	// Drain the async frontend so all writes + rotations have completed.
	SinkRegistry::FlushAll();

	// The active file exists...
	BOOST_TEST(fs::exists(log_path));

	// ...and rotation produced at least one rotated file, bounded by MaxFiles.
	std::size_t rotated = 0;
	for (const auto& entry : fs::directory_iterator(Dir))
	{
		const auto name = entry.path().filename().string();
		if (name.rfind("app_", 0) == 0)   // rotated files are "app_<NNNNN>.log"
		{
			++rotated;
		}
	}

	BOOST_TEST(rotated >= static_cast<std::size_t>(1));
	BOOST_TEST(rotated <= static_cast<std::size_t>(2));
}

BOOST_FIXTURE_TEST_CASE(File_JsonFormat_ProducesParseableLines, TempDirFixture, *boost::unit_test::label("unit"))
{
	const auto log_path = Dir / "app.json";

	FileSinkConfig config;
	config.Path = log_path;
	config.MaxFileBytes = 1024 * 1024;   // no rotation for this test
	config.Format = LogFormat::Json;
	SinkRegistry::Add(MakeFileSink(config));

	LogInfo(Channel::Web, "json to file");
	SinkRegistry::FlushAll();
	SinkRegistry::RemoveAll();   // closing triggers a final rotation of the active file

	// On close Boost moves the active file to a rotated name, so the record may live
	// in app.json OR app_00000.json — read whichever file holds content.
	std::string line;
	for (const auto& file : fs::directory_iterator(Dir))
	{
		std::ifstream stream(file.path());
		std::string first;
		std::getline(stream, first);
		if (!first.empty())
		{
			line = first;
			break;
		}
	}

	BOOST_REQUIRE(!line.empty());
	const auto entry = nlohmann::json::parse(line);
	BOOST_TEST(entry.value("message", "") == "json to file");
	BOOST_TEST(entry.value("channel", "") == "Web");
}

BOOST_AUTO_TEST_SUITE_END()
