#include <cstdint>

#include <sys/stat.h>
#include <unistd.h>

#include "logging/sinks/log_environment_probes.h"

namespace AqualinkAutomate::Logging::Sinks
{

	bool PlatformStderrIsTty() noexcept
	{
		return isatty(STDERR_FILENO) != 0;
	}

	std::optional<DevIno> PlatformStatStderr() noexcept
	{
		if (struct stat st{}; 0 == ::fstat(STDERR_FILENO, &st))
		{
			return DevIno{ static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino) };
		}

		return std::nullopt;
	}

}
// namespace AqualinkAutomate::Logging::Sinks
