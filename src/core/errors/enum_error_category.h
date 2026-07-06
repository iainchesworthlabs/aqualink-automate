#pragma once

#include <format>
#include <string>
#include <string_view>
#include <type_traits>

#include <boost/system/error_code.hpp>
#include <magic_enum/magic_enum.hpp>

namespace AqualinkAutomate::ErrorCodes
{

	//=========================================================================
	// EnumErrorCategory<DERIVED, ENUM>
	//
	// CRTP base that supplies the boost::system::error_category boilerplate
	// (Instance / name / message / make-helpers) for every project error
	// category exactly once.  A concrete category is reduced to a tiny shell
	// that provides two static hooks:
	//
	//   static constexpr std::string_view CategoryName;        // category name()
	//   static std::string_view Describe(ENUM e);              // human-readable text
	//
	// Each DERIVED keeps its own Meyers-singleton Instance(), so error_code
	// category equality (compared by address by the protocol registry) is
	// preserved per-category, and the numeric values of the enum are untouched.
	//=========================================================================
	template<typename DERIVED, typename ENUM>
		requires std::is_enum_v<ENUM>
	class EnumErrorCategory : public boost::system::error_category
	{
	public:
		const char* name() const noexcept override
		{
			return DERIVED::CategoryName.data();
		}

		// Re-expose the base's other message() overload (the buffer-filling
		// char const* message(int, char*, std::size_t) noexcept) which the
		// std::string override below would otherwise hide.
		using boost::system::error_category::message;

		std::string message(int ev) const override
		{
			if (const auto value = magic_enum::enum_cast<ENUM>(ev); value.has_value())
			{
				return std::string(DERIVED::Describe(*value));
			}

			return std::format("{} - unknown error code ({})", DERIVED::CategoryName, ev);
		}

		// Centralised factory helpers - each concrete category's free-function
		// make_error_code / make_error_condition overloads forward to these so
		// the construction logic lives in exactly one place.
		static boost::system::error_code MakeErrorCode(ENUM e)
		{
			return boost::system::error_code(static_cast<int>(e), DERIVED::Instance());
		}

		static boost::system::error_condition MakeErrorCondition(ENUM e)
		{
			return boost::system::error_condition(static_cast<int>(e), DERIVED::Instance());
		}

	protected:
		// Mirrors the base's protected non-virtual destructor: concrete categories
		// are Meyers singletons and are never destroyed through a base pointer.
		~EnumErrorCategory() = default;
	};

}
// namespace AqualinkAutomate::ErrorCodes
