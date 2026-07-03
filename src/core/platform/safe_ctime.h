#pragma once

#include <cstddef>
#include <ctime>

namespace AqualinkAutomate::Platform
{

	/// Thread-safe conversion of time_t to string.
	/// @param time_t_val Pointer to time_t value to convert.
	/// @param buf Output buffer (must be at least 26 chars).
	/// @param buf_size Size of the output buffer.
	/// @return Pointer to buf on success, or nullptr on failure.
	char* SafeCtime(const std::time_t* time_t_val, char* buf, std::size_t buf_size);

	/// Thread-safe UTC decomposition of time_t (gmtime_s on Windows, gmtime_r on POSIX).
	/// @param out Broken-down time, filled on success.
	/// @param t   Seconds since the epoch to convert.
	/// @return true on success; on failure @p out is left unspecified.
	bool SafeGmTime(std::tm& out, const std::time_t& t) noexcept;

	/// Thread-safe local-time decomposition of time_t (localtime_s on Windows, localtime_r on POSIX).
	/// @param out Broken-down time, filled on success.
	/// @param t   Seconds since the epoch to convert.
	/// @return true on success; on failure @p out is left unspecified.
	bool SafeLocalTime(std::tm& out, const std::time_t& t) noexcept;

}
// namespace AqualinkAutomate::Platform
