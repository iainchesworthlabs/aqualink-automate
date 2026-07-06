#pragma once

#include <algorithm>
#include <string_view>

namespace AqualinkAutomate::Utility
{

	// Correctly-spelled, PascalCase case-insensitive single-character comparison.
	// This is the canonical name; prefer it for all new call sites.
	bool CaseInsensitiveComparison(const unsigned char lhs, const unsigned char rhs);

	// Whole-string case-insensitive equality. Canonical replacement for the many ad-hoc
	// per-call-site `eq_ci` lambdas across the device state machines (IAQ / OneTouch).
	// Short-circuits on unequal length, then compares character-by-character.
	constexpr bool EqualsCaseInsensitive(std::string_view lhs, std::string_view rhs)
	{
		// ASCII-only tolower: matches std::tolower for ASCII (the only inputs here are
		// protocol / device text). Inlined so this stays usable in a constant expression
		// (CaseInsensitiveComparison() is a non-constexpr .cpp definition backed by std::tolower).
		constexpr auto ascii_tolower = [](char c) -> char { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
		return std::ranges::equal(lhs, rhs,
			[ascii_tolower](char a, char b) { return ascii_tolower(a) == ascii_tolower(b); });
	}

	// Legacy mis-spelled alias retained so existing call sites keep compiling while
	// they migrate to CaseInsensitiveComparison(). Forwards to the canonical helper.
	// Do not use in new code. (No [[deprecated]] attribute: the build treats warnings
	// as errors, so flagging it would break the as-yet-unmigrated out-of-unit callers.)
	inline bool case_insensitive_comparision(const unsigned char lhs, const unsigned char rhs)
	{
		return CaseInsensitiveComparison(lhs, rhs);
	}

}
// namespace AqualinkAutomate::Utility
