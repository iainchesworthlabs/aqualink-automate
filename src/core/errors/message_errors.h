#pragma once

#include <string_view>

#include <boost/system/error_code.hpp>
#include <magic_enum/magic_enum.hpp>

#include "errors/enum_error_category.h"

namespace AqualinkAutomate::ErrorCodes
{

	enum class Message_ErrorCodes : int
	{
		Error_InvalidMessageData = 1000,
		Error_CannotFindGenerator,
		Error_UnknownMessageType,
		Error_GeneratorFailed,
		Error_FailedToSerialize,
		Error_FailedToDeserialize
	};

	class Message_ErrorCategory : public EnumErrorCategory<Message_ErrorCategory, Message_ErrorCodes>
	{
	public:
		static const Message_ErrorCategory& Instance();

	public:
		static constexpr std::string_view CategoryName{ "AqualinkAutomate::Message Error Category" };
		static std::string_view Describe(Message_ErrorCodes e);

	protected:
		// Meyers singleton; never destroyed through a base pointer (mirrors the
		// EnumErrorCategory base's protected non-virtual destructor).
		~Message_ErrorCategory() = default;
	};

}
// namespace AqualinkAutomate::ErrorCodes

namespace boost::system
{
	template<>
	struct is_error_code_enum<AqualinkAutomate::ErrorCodes::Message_ErrorCodes> : public std::true_type {};
}

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::Message_ErrorCodes e);
boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::Message_ErrorCodes e);

template <>
struct magic_enum::customize::enum_range<AqualinkAutomate::ErrorCodes::Message_ErrorCodes>
{
	static constexpr int min = 1000;
	static constexpr int max = 1005;
	// (max - min) must be less than UINT16_MAX.
};
