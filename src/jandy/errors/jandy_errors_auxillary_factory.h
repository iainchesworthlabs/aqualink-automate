#pragma once

#include <string_view>

#include <boost/system/error_code.hpp>
#include <magic_enum/magic_enum.hpp>

#include "errors/enum_error_category.h"

namespace AqualinkAutomate::ErrorCodes
{

	enum class Factory_ErrorCodes : int
	{
		Error_UnknownFactoryError = 5000,
		Error_UnknownDeviceLabel,
		Error_CannotCastToJandyAuxillaryId,
		Error_FailedToCreateAuxillaryPtr,
		Error_ReceivedInvalidAuxillaryStatus
	};

	class Factory_ErrorCategory : public EnumErrorCategory<Factory_ErrorCategory, Factory_ErrorCodes>
	{
	public:
		static const Factory_ErrorCategory& Instance();

	public:
		static constexpr std::string_view CategoryName{ "AqualinkAutomate::Factory Error Category" };
		static std::string_view Describe(Factory_ErrorCodes e);

	protected:
		// Meyers singleton; never destroyed through a base pointer (mirrors the
		// EnumErrorCategory base's protected non-virtual destructor).
		~Factory_ErrorCategory() = default;
	};

}
// namespace AqualinkAutomate::ErrorCodes

namespace boost::system
{
	template<>
	struct is_error_code_enum<AqualinkAutomate::ErrorCodes::Factory_ErrorCodes> : public std::true_type {};
}

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::Factory_ErrorCodes e);
boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::Factory_ErrorCodes e);

template <>
struct magic_enum::customize::enum_range<AqualinkAutomate::ErrorCodes::Factory_ErrorCodes>
{
	static constexpr int min = 5000;
	static constexpr int max = 5004;
	// (max - min) must be less than UINT16_MAX.
};
