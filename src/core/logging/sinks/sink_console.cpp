#include <iostream>
#include <ostream>

#include <boost/core/null_deleter.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/logging_attributes.h"
#include "logging/logging_formatter.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/severity_mappings.h"
#include "logging/sinks/sink_console.h"
#include "logging/sinks/sink_filters.h"

namespace AqualinkAutomate::Logging::Sinks
{

	boost::shared_ptr<boost::log::sinks::sink> MakeConsoleSink(const ConsoleSinkConfig& config)
	{
		using text_sink = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;

		auto sink = boost::make_shared<text_sink>();

		// Caller-provided stream (tests) or std::clog via a null_deleter — an unowned,
		// process-lifetime handle, matching the historical console target.
		boost::shared_ptr<std::ostream> stream = config.Stream
			? config.Stream
			: boost::shared_ptr<std::ostream>(&std::clog, boost::null_deleter());
		sink->locked_backend()->add_stream(stream);

		// Formatter: optionally emit the sd-daemon "<N>" priority prefix first, then
		// the selected body (text or JSON). The prefix precedes the whole record, so
		// only its first line carries the priority (§5.2 multi-line rule).
		const bool journald = config.JournaldPrefixes;
		const bool as_json = (config.Format == LogFormat::Json);
		sink->set_formatter([journald, as_json](boost::log::record_view const& rec, boost::log::formatting_ostream& strm)
			{
				if (journald)
				{
					strm << JournaldPrefix(rec[severity].get<Severity>());
				}

				if (as_json)
				{
					JsonFormatter(rec, strm);
				}
				else
				{
					Formatter(rec, strm);
				}
			});

		// The shared operational filter: the same per-channel severity gate the
		// console has always applied, now also excluding audit records (§10.2).
		sink->set_filter(MakeOperationalFilter());

		// Flush after every record. std::clog is fully buffered; under a container
		// (stderr is a pipe, not a TTY) that buffering makes `docker logs` appear to
		// stall and loses the buffered tail on a crash. auto_flush trades a little
		// throughput for real-time, crash-safe delivery.
		sink->locked_backend()->auto_flush(true);

		return sink;
	}

}
// namespace AqualinkAutomate::Logging::Sinks
