#include "utility/spa_switch_assignment.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "utility/to_number.h"

namespace AqualinkAutomate::Utility
{

	namespace
	{
		// Field separators. Spaces pad the OneTouch screen rows; the iAQ TableMessage uses a TAB
		// (0x09) between "switch:button" and the function -- but the message layer sanitises any
		// non-printable byte (tab, and the trailing NUL terminator) to '?', so we accept '?' as a
		// separator too. Function names are alphanumeric + spaces, so this never eats real text.
		constexpr bool IsSep(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '?';
		}

		std::string TrimSep(const std::string& s)
		{
			std::size_t b = 0;
			std::size_t e = s.size();
			while (b < e && IsSep(s[b])) { ++b; }
			while (e > b && IsSep(s[e - 1])) { --e; }
			return s.substr(b, e - b);
		}
	}

	std::optional<SpaSwitchAssignment> ParseSpaSwitchAssignmentLine(const std::string& line)
	{
		const std::string s = TrimSep(line);

		std::size_t i = 0;

		// switch digits
		const std::size_t sw_start = i;
		while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; }
		if (i == sw_start || i >= s.size() || s[i] != ':')
		{
			return std::nullopt;
		}
		const std::string sw_str = s.substr(sw_start, i - sw_start);
		++i;   // skip ':'

		// button digits
		const std::size_t btn_start = i;
		while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; }
		if (i == btn_start)
		{
			return std::nullopt;
		}
		const std::string btn_str = s.substr(btn_start, i - btn_start);

		// A separator must sit between the "switch:button" tag and the function name -- this is
		// what rejects "1:23" (button greedily ate the digits, nothing left) and bare tags.
		if (i >= s.size() || !IsSep(s[i]))
		{
			return std::nullopt;
		}

		const std::string function = TrimSep(s.substr(i));
		if (function.empty())
		{
			return std::nullopt;
		}

		// Small 1-based indices only (the controllers number switches/buttons from 1).
		// Parse with the non-throwing converter: the digit runs above are unbounded, so
		// an attacker-supplied run such as "99999999999" would overflow std::stoi and
		// throw std::out_of_range -- which, on the serial dispatch path, is a remote
		// crash.  ToNumber returns std::nullopt on overflow/invalid input instead, so a
		// malformed line is simply "not an assignment line".
		const auto sw = Utility::ToNumber<int>(sw_str);
		const auto btn = Utility::ToNumber<int>(btn_str);
		if (!sw || !btn || *sw < 1 || *sw > 255 || *btn < 1 || *btn > 255)
		{
			return std::nullopt;
		}

		SpaSwitchAssignment out;
		out.switch_number = static_cast<uint8_t>(*sw);
		out.button_number = static_cast<uint8_t>(*btn);
		out.function = function;
		return out;
	}

	const std::vector<std::string>& KnownSpaSwitchFunctions()
	{
		// The function picker the OneTouch cycles (0x06) and the iAQ scrolls (group 0x01), in the
		// order they appear on the controller (docs/alwin32/spaside-remotes.md, RE-verified against
		// captures/spaside_setup_nav.cap). "All OFF" clears the button; the rest map to a pool
		// function the controller can drive. Kept as a static so callers can return a const ref.
		static const std::vector<std::string> functions{
			"All OFF",
			"OneTouch 4h",
			"Clean Mode",
			"Spa Mode",
			"Spillway",
			"Air Blower",
			"Pool Light",
			"Swim Jet",
			"Spa Jets",
			"Filter Pump",
			"Spa",
			"Pool Heat",
			"Spa Heat",
			"Solar Heat",
		};
		return functions;
	}

}
// namespace AqualinkAutomate::Utility
