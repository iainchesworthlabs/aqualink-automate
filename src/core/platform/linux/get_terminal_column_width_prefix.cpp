#include "utility/get_terminal_column_width.h"

namespace AqualinkAutomate::Utility
{

	// Linux log-line prefix for the shared POSIX terminal-width probe. Selected by the
	// if(LINUX) block in src/core/CMakeLists.txt (see [[platform-isolation]]).
	const std::string_view PLATFORM_LOG_PREFIX{ "LINUX:" };

}
// namespace AqualinkAutomate::Utility
