#pragma once

#include <expected>
#include <format>
#include <ostream>
#include <string>

#include "formatters/orp_formatter.h"
#include "formatters/ph_formatter.h"
#include "utility/string_conversion/chemistry_string_converter.h"

namespace std
{

	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Utility::ChemistryStringConverter& obj);

}
// namespace std

template<>
struct std::formatter<AqualinkAutomate::Utility::ChemistryStringConverter>
{
	constexpr auto parse(std::format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Utility::ChemistryStringConverter& chemistry, FormatContext& ctx) const
	{
		try
		{
			auto orp = chemistry.ORP().value();
			auto ph = chemistry.PH().value();

			return std::format_to(ctx.out(), "ORP={} PH={}", orp, ph);
		}
		catch (const std::bad_expected_access<boost::system::error_code>&)
		{
			return std::format_to(ctx.out(), "CHEM=??");
		}
	}
};
