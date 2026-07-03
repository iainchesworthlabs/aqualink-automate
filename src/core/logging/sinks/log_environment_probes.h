#pragma once

#include <optional>

#include "logging/sinks/log_environment.h"  // DevIno

namespace AqualinkAutomate::Logging::Sinks
{

	//
	// OS-native raw probes that sit behind the injectable EnvironmentProbes seam
	// (see log_environment.h). Implemented once per platform under
	// src/core/platform/<os>/ and selected by CMake, so the shared
	// logging/sinks/log_environment.cpp carries no OS preprocessor branch.
	// See docs/platform-isolation.md.
	//

	// True iff the real stderr is a terminal (isatty / _isatty).
	[[nodiscard]] bool PlatformStderrIsTty() noexcept;

	// Device/inode identity of the real stderr, or nullopt when it is unavailable
	// or not meaningful (e.g. Windows, where $JOURNAL_STREAM never applies).
	[[nodiscard]] std::optional<DevIno> PlatformStatStderr() noexcept;

}
// namespace AqualinkAutomate::Logging::Sinks
