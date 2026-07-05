#pragma once

#include <string_view>

#include <boost/system/error_code.hpp>
#include <magic_enum/magic_enum.hpp>

#include "errors/enum_error_category.h"

namespace AqualinkAutomate::ErrorCodes
{

	enum class Scrapeable_ErrorCodes : int
	{
		WaitingForPage = 4000,
		WaitingForMessage,
		NoStepPossible,
		NoGraphBeingScraped,
		UnknownScrapeError,
		PreCommandValidationFailed,
		PostCommandValidationFailed,
		RecoveryInProgress,
		RecoveryComplete,
		MaxRecoveryAttemptsExceeded
	};

	class Scrapeable_ErrorCategory : public EnumErrorCategory<Scrapeable_ErrorCategory, Scrapeable_ErrorCodes>
	{
	public:
		static const Scrapeable_ErrorCategory& Instance();

	public:
		static constexpr std::string_view CategoryName{ "AqualinkAutomate::Scrapeable Error Category" };
		static std::string_view Describe(Scrapeable_ErrorCodes e);

	protected:
		// Meyers singleton; never destroyed through a base pointer (mirrors the
		// EnumErrorCategory base's protected non-virtual destructor).
		~Scrapeable_ErrorCategory() = default;
	};

}
// namespace AqualinkAutomate::ErrorCodes

namespace boost::system
{
	template<>
	struct is_error_code_enum<AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCodes> : public std::true_type {};
}

boost::system::error_code make_error_code(AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCodes e);
boost::system::error_condition make_error_condition(AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCodes e);

template <>
struct magic_enum::customize::enum_range<AqualinkAutomate::ErrorCodes::Scrapeable_ErrorCodes>
{
	static constexpr int min = 4000;
	static constexpr int max = 4009;
	// (max - min) must be less than UINT16_MAX.
};
