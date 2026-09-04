#pragma once

#include <format>
#include <ostream>

#include <boost/test/unit_test.hpp>

#include "formatters/units_dimensionless_formatter.h"
#include "formatters/units_electric_potential_formatter.h"
#include "types/units_dimensionless.h"
#include "types/units_electric_potential.h"

// Boost.Test prints a mismatched value by streaming it from inside its own namespace. The
// project's operator<< for these quantities lives in namespace AqualinkAutomate, which
// argument-dependent lookup will not reach: a Boost.Units quantity's associated namespace is
// boost::units, not ours. The operators cannot be moved into boost::units (it is not our
// namespace to extend) and must not go back into std (adding overloads there is undefined
// behaviour, cpp:S3470).
//
// print_log_value is the library's documented extension point for exactly this case. It is a
// template specialisation rather than an injected overload, so it is found by name without
// depending on lookup reaching a namespace it cannot see. Each one defers to the std::formatter
// specialisation that already exists for the quantity, so test output matches log output.

namespace boost::test_tools::tt_detail
{

	template<>
	struct print_log_value<AqualinkAutomate::Units::millivolt_quantity>
	{
		void operator()(std::ostream& os, const AqualinkAutomate::Units::millivolt_quantity& value) const
		{
			os << std::format("{}", value);
		}
	};

	template<>
	struct print_log_value<AqualinkAutomate::Units::volt_quantity>
	{
		void operator()(std::ostream& os, const AqualinkAutomate::Units::volt_quantity& value) const
		{
			os << std::format("{}", value);
		}
	};

	template<>
	struct print_log_value<AqualinkAutomate::Units::ppm_quantity>
	{
		void operator()(std::ostream& os, const AqualinkAutomate::Units::ppm_quantity& value) const
		{
			os << std::format("{}", value);
		}
	};

}
// namespace boost::test_tools::tt_detail
