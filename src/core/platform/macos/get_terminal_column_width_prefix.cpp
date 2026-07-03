#include "utility/get_terminal_column_width.h"

namespace AqualinkAutomate::Utility
{

	// macOS log-line prefix for the shared POSIX terminal-width probe. Selected by the
	// if(APPLE) block in src/core/CMakeLists.txt (see docs/platform-isolation.md).
	const std::string_view PLATFORM_LOG_PREFIX{ "macOS:" };

}
// namespace AqualinkAutomate::Utility
