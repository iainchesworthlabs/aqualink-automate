#include <format>

#include <sys/ioctl.h>
#include <unistd.h>

#include "logging/logging.h"
#include "utility/get_terminal_column_width.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Utility
{

	// The only divergence between the Unix variants is the log-line prefix
	// (Utility::PLATFORM_LOG_PREFIX), defined per-OS under platform/<os>/ and selected
	// by CMake; the probe body below is shared across every POSIX platform.
	uint32_t get_terminal_column_width()
	{
		uint32_t column_width = 0;

		if (struct winsize w {}; -1 == ioctl(STDOUT_FILENO, TIOCGWINSZ, &w))
		{
			LogDebug(Channel::Platform, std::format("{} Failed to get terminal column width, using default", PLATFORM_LOG_PREFIX));
			column_width = DEFAULT_TERMINAL_COLUMN_WIDTH;
		}
		else
		{
			LogTrace(Channel::Platform, std::format("{} Retrieve current terminal column width ({} columns)", PLATFORM_LOG_PREFIX, w.ws_col));
			column_width = w.ws_col;
		}

		return column_width;
	}

}
// namespace AqualinkAutomate::Utility
