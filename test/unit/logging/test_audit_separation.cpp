#include <boost/test/unit_test.hpp>

#include <sstream>
#include <string>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "auth/audit_log.h"
#include "logging/logging.h"
#include "logging/logging_channels.h"
#include "logging/logging_severity_filter.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/sink_filters.h"
#include "logging/sinks/sink_registry.h"

//
// The core separation guarantee (docs/logging-sinks-redesign.md §10.2): audit
// records reach ONLY the audit-filtered sink, and operational records reach ONLY
// the operational-filtered sink. Both directions are asserted end-to-end through
// the real Boost.Log emit path with two captured string sinks.
//

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Logging::Sinks;

namespace
{
	boost::shared_ptr<boost::log::sinks::sink> MakeCaptureSink(
		const boost::shared_ptr<std::ostringstream>& stream, boost::log::filter filter)
	{
		namespace expr = boost::log::expressions;
		using text_sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;

		auto sink = boost::make_shared<text_sink>();
		sink->locked_backend()->add_stream(stream);
		sink->locked_backend()->auto_flush(true);
		sink->set_filter(std::move(filter));
		sink->set_formatter(expr::stream << expr::smessage);
		return sink;
	}

	struct AuditSeparationFixture
	{
		AuditSeparationFixture()
		{
			boost::log::add_common_attributes();
			boost::log::core::get()->remove_all_sinks();
			SeverityFiltering::SetGlobalFilterLevel(Severity::Trace);

			SinkRegistry::Add(MakeCaptureSink(Operational, MakeOperationalFilter()));
			SinkRegistry::Add(MakeCaptureSink(Audit, MakeAuditFilter()));
		}

		~AuditSeparationFixture()
		{
			SinkRegistry::RemoveAll();
			boost::log::core::get()->remove_all_sinks();
			SeverityFiltering::SetGlobalFilterLevel(SeverityFiltering::DEFAULT_SEVERITY);
		}

		boost::shared_ptr<std::ostringstream> Operational = boost::make_shared<std::ostringstream>();
		boost::shared_ptr<std::ostringstream> Audit = boost::make_shared<std::ostringstream>();
	};
}

BOOST_AUTO_TEST_SUITE(TestSuite_AuditSeparation)

BOOST_FIXTURE_TEST_CASE(AuditRecord_ReachesAuditSink_NeverOperational, AuditSeparationFixture, *boost::unit_test::label("unit"))
{
	Auth::AuditLog audit({});  // empty JsonlFile => emit-only, no disk

	Auth::AuditEvent event;
	event.SubjectId = "user-1";
	event.Provider = "Local";
	event.Action = "auth.login";
	event.Decision = "success";
	event.PeerIp = "10.0.0.9";
	audit.Record(event);

	SinkRegistry::FlushAll();

	// Reaches the audit sink...
	BOOST_TEST(Audit->str().find("auth.login") != std::string::npos);
	// ...and never the operational sink.
	BOOST_TEST(Operational->str().find("auth.login") == std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(OperationalRecord_ReachesOperationalSink_NeverAudit, AuditSeparationFixture, *boost::unit_test::label("unit"))
{
	LogInfo(Channel::Main, "operational-line-xyz");

	SinkRegistry::FlushAll();

	BOOST_TEST(Operational->str().find("operational-line-xyz") != std::string::npos);
	BOOST_TEST(Audit->str().find("operational-line-xyz") == std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(AuditDeny_CarriesWarningSeverity, AuditSeparationFixture, *boost::unit_test::label("unit"))
{
	// A denial should carry a higher priority than a permit (§10.3). We assert the
	// audit sink receives the deny record; the severity-to-priority mapping itself
	// is covered exhaustively in test_severity_mappings.
	Auth::AuditLog audit({});

	Auth::AuditEvent event;
	event.Action = "equipment.control.aux";
	event.Decision = "deny";
	audit.Record(event);

	SinkRegistry::FlushAll();

	BOOST_TEST(Audit->str().find("deny") != std::string::npos);
	BOOST_TEST(Operational->str().find("deny") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
