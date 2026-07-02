#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "auth/audit_log.h"

using namespace AqualinkAutomate;

namespace
{
	namespace fs = std::filesystem;

	struct TempDirFixture
	{
		TempDirFixture()
		{
			static std::uint32_t counter{ 0 };
			Dir = fs::temp_directory_path() / std::format("aa-audit-test-{}-{}", boost::unit_test::framework::current_test_case().p_name.get(), counter++);
			fs::create_directories(Dir);
		}

		~TempDirFixture()
		{
			std::error_code ec;
			fs::remove_all(Dir, ec);
		}

		fs::path Dir;
	};

	std::vector<std::string> ReadLines(const fs::path& file)
	{
		std::vector<std::string> lines;
		std::ifstream stream(file);

		for (std::string line; std::getline(stream, line);)
		{
			lines.push_back(line);
		}

		return lines;
	}

	Auth::AuditEvent MakeEvent()
	{
		Auth::AuditEvent event;
		event.SubjectId = "user-42";
		event.Provider = "Local";
		event.Action = "equipment.control.aux";
		event.ResourceKind = "aux";
		event.ResourceId = "AUX3";
		event.Decision = "permit";
		event.PeerIp = "192.168.1.50";
		event.Detail = "toggle on";
		return event;
	}
}

BOOST_AUTO_TEST_SUITE(TestSuite_AuditLog)

BOOST_FIXTURE_TEST_CASE(Test_AuditLog_RecordWritesStructuredJsonl, TempDirFixture)
{
	const auto audit_file = Dir / "audit.jsonl";

	Auth::AuditLog audit({ .JsonlFile = audit_file });

	audit.Record(MakeEvent());

	const auto lines = ReadLines(audit_file);
	BOOST_REQUIRE_EQUAL(lines.size(), 1u);

	const auto entry = nlohmann::json::parse(lines[0]);

	BOOST_CHECK_EQUAL(entry.value("subject", ""), "user-42");
	BOOST_CHECK_EQUAL(entry.value("provider", ""), "Local");
	BOOST_CHECK_EQUAL(entry.value("action", ""), "equipment.control.aux");
	BOOST_CHECK_EQUAL(entry.value("resource_kind", ""), "aux");
	BOOST_CHECK_EQUAL(entry.value("resource_id", ""), "AUX3");
	BOOST_CHECK_EQUAL(entry.value("decision", ""), "permit");
	BOOST_CHECK_EQUAL(entry.value("peer_ip", ""), "192.168.1.50");
	BOOST_CHECK_EQUAL(entry.value("detail", ""), "toggle on");
	BOOST_CHECK(!entry.value("ts", "").empty());
}

BOOST_FIXTURE_TEST_CASE(Test_AuditLog_AppendsAcrossRecords, TempDirFixture)
{
	const auto audit_file = Dir / "audit.jsonl";

	Auth::AuditLog audit({ .JsonlFile = audit_file });

	audit.Record(MakeEvent());
	audit.Record(MakeEvent());
	audit.Record(MakeEvent());

	BOOST_CHECK_EQUAL(ReadLines(audit_file).size(), 3u);
}

BOOST_FIXTURE_TEST_CASE(Test_AuditLog_RotatesWhenFileWouldExceedBudget, TempDirFixture)
{
	const auto audit_file = Dir / "audit.jsonl";

	// A budget small enough that every record trips rotation.
	Auth::AuditLog audit({ .JsonlFile = audit_file, .MaxFileBytes = 64 });

	audit.Record(MakeEvent());
	audit.Record(MakeEvent());

	BOOST_CHECK(fs::exists(audit_file));
	BOOST_CHECK(fs::exists(fs::path{ audit_file } += ".1"));

	// Current file holds only the newest record.
	BOOST_CHECK_EQUAL(ReadLines(audit_file).size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(Test_AuditLog_EmptyPathDisablesJsonlSink, TempDirFixture)
{
	Auth::AuditLog audit({});

	// Channel-only mode: must not throw, and must write nothing to disk.
	BOOST_CHECK_NO_THROW(audit.Record(MakeEvent()));
	BOOST_CHECK(fs::is_empty(Dir));
}

BOOST_AUTO_TEST_CASE(Test_AuditLog_OsSinkRegistrationDoesNotThrow)
{
	// Registration may legitimately fail (no privileges / no syslog); the
	// contract is a clean boolean, never an exception.
	BOOST_CHECK_NO_THROW(Auth::RegisterAuditOsSink());
}

BOOST_AUTO_TEST_SUITE_END()
