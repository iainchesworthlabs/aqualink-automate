#include <cstdint>
#include <format>
#include <iomanip>
#include <string>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "logging/logging_attributes.h"
#include "logging/logging_formatter.h"
#include "logging/logging_severity_levels.h"

namespace AqualinkAutomate::Logging
{

	void Formatter(boost::log::record_view const& rec, boost::log::formatting_ostream& strm)
	{
		// Select the line layout from the record's OWN severity, not the channel's
		// configured filter level. The channel filter is a gate (does this record get
		// emitted at all); it must not decide whether THIS record renders the
		// file:line suffix. A Trace/Debug record always carries the source location.
		const auto record_severity_level = rec[severity].get<Severity>();

		auto debug_and_trace_formatter = [](auto& rec) -> std::string
		{
			return std::format(
				"{:08}: <{}>\t({}) [{}:{}] {}",
				rec[line_id].template get<uint32_t>(),
				magic_enum::enum_name(rec[severity].template get<Severity>()),
				magic_enum::enum_name(rec[channel].template get<Channel>()),
				rec[source_file].template get<std::string>(),
				rec[source_line].template get<uint32_t>(),
				rec[boost::log::expressions::smessage].template get<std::string>());
		};

		auto all_other_levels_formatter = [](auto& rec) -> std::string
		{
			return std::format(
				"{:08}: <{}>\t({}) {}",
				rec[line_id].template get<uint32_t>(),
				magic_enum::enum_name(rec[severity].template get<Severity>()),
				magic_enum::enum_name(rec[channel].template get<Channel>()),
				rec[boost::log::expressions::smessage].template get<std::string>());
		};

		switch (record_severity_level)
		{
		case Severity::Trace:
		case Severity::Debug:
			strm << debug_and_trace_formatter(rec);
			break;

		case Severity::Info:
		case Severity::Notify:
		case Severity::Warning:
		case Severity::Error:
		case Severity::Fatal:
		default:
			strm << all_other_levels_formatter(rec);
			break;
		}
	}

	void JsonFormatter(boost::log::record_view const& rec, boost::log::formatting_ostream& strm)
	{
		nlohmann::json entry;

		// TimeStamp comes from boost::log::add_common_attributes() (local clock).
		if (const auto ts = boost::log::extract<boost::posix_time::ptime>("TimeStamp", rec))
		{
			entry["ts"] = boost::posix_time::to_iso_extended_string(ts.get());
		}

		const auto record_severity_level = rec[severity].get<Severity>();
		entry["severity"] = std::string(magic_enum::enum_name(record_severity_level));

		// Operational records carry a channel; audit records (which use a
		// message-only formatter, never this one) do not.
		if (const auto record_channel = rec[channel])
		{
			entry["channel"] = std::string(magic_enum::enum_name(record_channel.get()));
		}

		entry["message"] = rec[boost::log::expressions::smessage].get<std::string>();

		// file:line only for Trace/Debug, mirroring the text formatter.
		if (Severity::Trace == record_severity_level || Severity::Debug == record_severity_level)
		{
			if (const auto file = rec[source_file])
			{
				entry["file"] = file.get();
			}
			if (const auto line = rec[source_line])
			{
				entry["line"] = line.get();
			}
		}

		// Compact single-line dump (JSON-lines); nlohmann handles escaping.
		strm << entry.dump();
	}

}
// namespace AqualinkAutomate::Logging
