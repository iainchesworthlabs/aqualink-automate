#pragma once

#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

namespace AqualinkAutomate::Logging
{

	// The wire format an operational sink renders records in.
	enum class LogFormat
	{
		Text,  // human-readable "<line-id>: <Severity>\t(Channel) message"
		Json   // one JSON object per line (for container / SIEM pipelines)
	};

	// Human-readable text layout (the historical console format).
	void Formatter(boost::log::record_view const& rec, boost::log::formatting_ostream& strm);

	// JSON-lines layout: {"ts","severity","channel","message"[,"file","line"]} — one
	// compact object per record. file/line are included only for Trace/Debug records
	// (matching the text formatter). Built with nlohmann so escaping is correct.
	void JsonFormatter(boost::log::record_view const& rec, boost::log::formatting_ostream& strm);

}
// namespace AqualinkAutomate::Logging
