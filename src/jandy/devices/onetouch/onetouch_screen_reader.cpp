#include <cctype>
#include <cstddef>

#include "devices/onetouch/onetouch_screen_reader.h"

namespace AqualinkAutomate::Devices::OneTouch
{

	namespace
	{
		std::string ToLower(std::string_view s)
		{
			std::string out(s);
			for (char& c : out)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return out;
		}
	}
	// namespace

	std::string SanitiseFunctionText(std::string_view raw)
	{
		// Trim surrounding whitespace and any non-printable bytes. The row Text is already the
		// clean line content (the controller's inverse-video highlight arrives as separate
		// Highlight/HighlightChars messages -> the row's HighlightRange, never the Text), so a
		// plain trim yields the function/label exactly as displayed.
		auto is_trim = [](char c)
		{
			const auto u = static_cast<unsigned char>(c);
			return (u < 0x20) || (u == 0x7f) || (c == ' ');
		};
		std::size_t b = 0;
		std::size_t e = raw.size();
		while (b < e && is_trim(raw[b])) { ++b; }
		while (e > b && is_trim(raw[e - 1])) { --e; }
		return std::string(raw.substr(b, e - b));
	}

	std::optional<int> FirstNumber(std::string_view text)
	{
		// Read the first contiguous digit run, e.g. "Pool Heat   90`F" -> 90, "Set Pool to:
		// 45%" -> 45. Digits after the first non-digit that follows the run are ignored (the
		// value is the FIRST run only). nullopt when the text carries no digit at all.
		int value{ 0 };
		bool found{ false };
		for (const char c : text)
		{
			if (c >= '0' && c <= '9')
			{
				value = (value * 10) + (c - '0');
				found = true;
			}
			else if (found)
			{
				break;  // first contiguous digit run only (the value)
			}
		}

		return found ? std::optional<int>{ value } : std::nullopt;
	}

	std::optional<int> DisplayedValue(const Utility::ScreenDataPage& page, uint8_t line_id)
	{
		if (line_id >= page.Size())
		{
			return std::nullopt;
		}
		return FirstNumber(page[line_id].Text);
	}

	std::optional<std::string> DisplayedFunctionOnRow(const Utility::ScreenDataPage& page, uint8_t line_id)
	{
		if (line_id >= page.Size())
		{
			return std::nullopt;
		}

		auto text = SanitiseFunctionText(page[line_id].Text);
		if (text.empty())
		{
			return std::nullopt;
		}
		return text;
	}

	std::optional<uint8_t> FindLineStartingWith(const Utility::ScreenDataPage& page, std::string_view prefix)
	{
		const std::string needle = ToLower(prefix);

		for (std::size_t i = 0; i < page.Size(); ++i)
		{
			const std::string hay = ToLower(SanitiseFunctionText(page[i].Text));
			if ((hay.size() >= needle.size()) && (hay.compare(0, needle.size(), needle) == 0))
			{
				return static_cast<uint8_t>(i);
			}
		}
		return std::nullopt;
	}

	std::string LineText(const Utility::ScreenDataPage& page, std::size_t line_id)
	{
		return (line_id < page.Size()) ? SanitiseFunctionText(page[line_id].Text) : std::string{};
	}

	bool EqualsCaseInsensitive(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
		{
			return false;
		}
		for (std::size_t i = 0; i < a.size(); ++i)
		{
			if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			{
				return false;
			}
		}
		return true;
	}

}
// namespace AqualinkAutomate::Devices::OneTouch
