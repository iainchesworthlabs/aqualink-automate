#include "platform/safe_ctime.h"

namespace AqualinkAutomate::Platform
{

	char* SafeCtime(const std::time_t* time_t_val, char* buf, std::size_t buf_size)
	{
		if (ctime_s(buf, buf_size, time_t_val) == 0)
		{
			return buf;
		}

		return nullptr;
	}

	bool SafeGmTime(std::tm& out, const std::time_t& t) noexcept
	{
		return gmtime_s(&out, &t) == 0;
	}

	bool SafeLocalTime(std::tm& out, const std::time_t& t) noexcept
	{
		return localtime_s(&out, &t) == 0;
	}

}
// namespace AqualinkAutomate::Platform
