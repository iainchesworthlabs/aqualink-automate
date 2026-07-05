#pragma once

#include <string_view>

#include <boost/system/error_code.hpp>
#include <magic_enum/magic_enum.hpp>

#include "errors/enum_error_category.h"

namespace AqualinkAutomate::ErrorCodes
{

	enum class Protocol_ErrorCodes : int
	{
		DataAvailableToProcess = 2000,
		WaitingForMoreData,
		InvalidPacketFormat,
		UnknownFailure,
		ChecksumFailure,
		OverlappingPackets
	};

	class Protocol_ErrorCategory : public EnumErrorCategory<Protocol_ErrorCategory, Protocol_ErrorCodes>
	{
	public:
		static const Protocol_ErrorCategory& Instance();

	public:
		static constexpr std::string_view CategoryName{ "AqualinkAutomate::Protocol Error Category" };
		static std::string_view Describe(Protocol_ErrorCodes e);

	protected:
		// Meyers singleton; never destroyed through a base pointer (mirrors the
		// EnumErrorCategory base's protected non-virtual destructor).
		~Protocol_ErrorCategory() = default;
	};

}
// namespace AqualinkAutomate::ErrorCodes

namespace boost::system
{
	template<>
	struct is_error_code_enum<AqualinkAutomate::ErrorCodes::Protocol_ErrorCodes> : public std::true_type {};
}

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::Protocol_ErrorCodes e);
boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::Protocol_ErrorCodes e);

template <>
struct magic_enum::customize::enum_range<AqualinkAutomate::ErrorCodes::Protocol_ErrorCodes>
{
	static constexpr int min = 2000;
	static constexpr int max = 2005;
	// (max - min) must be less than UINT16_MAX.
};
