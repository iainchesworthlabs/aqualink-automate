#pragma once

#include <string_view>

#include <boost/system/error_code.hpp>
#include <magic_enum/magic_enum.hpp>

#include "errors/enum_error_category.h"

namespace AqualinkAutomate::ErrorCodes
{
	enum class StringConversion_ErrorCodes : int
	{
		MalformedInput = 3000,
		UnknownStringConversionFailure
	};

	class StringConversion_ErrorCategory : public EnumErrorCategory<StringConversion_ErrorCategory, StringConversion_ErrorCodes>
	{
	public:
		static const StringConversion_ErrorCategory& Instance();

	public:
		static constexpr std::string_view CategoryName{ "AqualinkAutomate::String Conversion Error Category" };
		static std::string_view Describe(StringConversion_ErrorCodes e);

	protected:
		// Meyers singleton; never destroyed through a base pointer (mirrors the
		// EnumErrorCategory base's protected non-virtual destructor).
		~StringConversion_ErrorCategory() = default;
	};

}
// namespace AqualinkAutomate::ErrorCodes

namespace boost::system
{
	template<>
	struct is_error_code_enum<AqualinkAutomate::ErrorCodes::StringConversion_ErrorCodes> : public std::true_type {};
}

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::StringConversion_ErrorCodes e);
boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::StringConversion_ErrorCodes e);

template <>
struct magic_enum::customize::enum_range<AqualinkAutomate::ErrorCodes::StringConversion_ErrorCodes>
{
	static constexpr int min = 3000;
	static constexpr int max = 3001;
	// (max - min) must be less than UINT16_MAX.
};
