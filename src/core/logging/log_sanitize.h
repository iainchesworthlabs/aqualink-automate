#pragma once

#include <string>
#include <string_view>

namespace AqualinkAutomate::Logging
{

	// Neutralise an UNTRUSTED string before it is interpolated into a log line.
	//
	// Values sourced from the network (an MQTT topic from the broker, a device-
	// supplied label, a request header) can carry control characters -- a newline
	// to forge an extra log record, or an ESC (0x1B) to start an ANSI escape
	// sequence that rewrites a terminal tailing the log.  Replace every control
	// byte (< 0x20) and DEL (0x7F) with '?'; printable bytes pass through.
	constexpr std::string SanitizeForLog(std::string_view value)
	{
		std::string out;
		out.reserve(value.size());
		for (const unsigned char c : value)
		{
			out.push_back((c < 0x20 || c == 0x7F) ? '?' : static_cast<char>(c));
		}
		return out;
	}

}
// namespace AqualinkAutomate::Logging
