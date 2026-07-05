#include "auxillaries/jandy_auxillary_id.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <utility>

#include "kernel/auxillary_devices/stable_id.h"

namespace AqualinkAutomate::Auxillaries
{
	namespace
	{
		// Trim leading/trailing whitespace and collapse interior runs to a single space.
		std::string NormaliseWhitespace(std::string_view raw)
		{
			std::string out;
			out.reserve(raw.size());
			bool pending_space = false;
			for (const char c : raw)
			{
				if (0 != std::isspace(static_cast<unsigned char>(c)))
				{
					pending_space = !out.empty();
				}
				else
				{
					if (pending_space) { out.push_back(' '); pending_space = false; }
					out.push_back(c);
				}
			}
			return out;
		}
	}
	// unnamed namespace

	std::optional<JandyAuxillaryIds> ParseAuxId(std::string_view label)
	{
		const std::string s = NormaliseWhitespace(label);
		if (s.empty()) { return std::nullopt; }

		// "Extra Aux" / "ExtraAux" (accept with or without the interior space).
		{
			std::string compact = s;
			std::erase(compact, ' ');
			if (compact == "ExtraAux") { return JandyAuxillaryIds::ExtraAux; }
		}

		// Everything else must be "Aux" + optional bank letter (B-D) + a single digit.
		if (!s.starts_with("Aux")) { return std::nullopt; }
		std::string_view rest{ s };
		rest.remove_prefix(3);
		if (!rest.empty() && ' ' == rest.front()) { rest.remove_prefix(1); }
		if (rest.empty()) { return std::nullopt; }

		char bank = 'A';
		if (const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(rest.front())));
			'B' == upper || 'C' == upper || 'D' == upper)
		{
			bank = upper;
			rest.remove_prefix(1);
			if (!rest.empty() && ' ' == rest.front()) { rest.remove_prefix(1); }
		}

		// What remains must be exactly one digit.
		if (1 != rest.size() || 0 == std::isdigit(static_cast<unsigned char>(rest.front()))) { return std::nullopt; }
		const int number = rest.front() - '0';

		int value = 0;
		switch (bank)
		{
		case 'A': if (number < 1 || number > 7) { return std::nullopt; } value = 0x01 + (number - 1); break;
		case 'B': if (number < 1 || number > 8) { return std::nullopt; } value = 0x08 + (number - 1); break;
		case 'C': if (number < 1 || number > 8) { return std::nullopt; } value = 0x10 + (number - 1); break;
		case 'D': if (number < 1 || number > 8) { return std::nullopt; } value = 0x18 + (number - 1); break;
		default: return std::nullopt;
		}

		return magic_enum::enum_cast<JandyAuxillaryIds>(value);
	}

	std::string AuxNativeKey(JandyAuxillaryIds id)
	{
		return std::format("jandy:aux:{}", std::to_underlying(id));
	}

	boost::uuids::uuid AuxStableId(JandyAuxillaryIds id)
	{
		return Kernel::DeriveStableUuid(AuxNativeKey(id));
	}

}
// namespace AqualinkAutomate::Auxillaries
