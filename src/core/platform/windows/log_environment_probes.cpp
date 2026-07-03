#include <cstdio>
#include <io.h>

#include "logging/sinks/log_environment_probes.h"

namespace AqualinkAutomate::Logging::Sinks
{

	bool PlatformStderrIsTty() noexcept
	{
		return _isatty(_fileno(stderr)) != 0;
	}

	std::optional<DevIno> PlatformStatStderr() noexcept
	{
		// Windows has no journald and no meaningful device/inode identity for the
		// stderr handle in this sense; JOURNAL_STREAM never applies, so report
		// "unknown" and let JournalStreamMatches() fail closed.
		return std::nullopt;
	}

}
// namespace AqualinkAutomate::Logging::Sinks
