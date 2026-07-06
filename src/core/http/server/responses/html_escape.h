#pragma once

#include <string>
#include <string_view>

namespace AqualinkAutomate::HTTP::Responses
{

	// Escape a string for safe interpolation into an HTML text/attribute context.
	//
	// Wire-/client-controlled values (e.g. the request target) are reflected into
	// HTML error bodies; escaping the five HTML-significant characters prevents a
	// crafted request target from breaking out of the body and injecting markup
	// (reflected XSS).
	constexpr std::string HtmlEscape(std::string_view input)
	{
		std::string escaped;
		escaped.reserve(input.size());

		for (const char c : input)
		{
			switch (c)
			{
			case '&':
				escaped += "&amp;";
				break;
			case '<':
				escaped += "&lt;";
				break;
			case '>':
				escaped += "&gt;";
				break;
			case '"':
				escaped += "&quot;";
				break;
			case '\'':
				escaped += "&#39;";
				break;
			default:
				escaped += c;
				break;
			}
		}

		return escaped;
	}

}
// namespace AqualinkAutomate::HTTP::Responses
