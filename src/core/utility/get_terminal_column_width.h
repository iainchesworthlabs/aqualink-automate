#pragma once

#include <cstdint>
#include <string_view>

namespace AqualinkAutomate::Utility
{
	static constexpr uint32_t DEFAULT_TERMINAL_COLUMN_WIDTH = 80;
	uint32_t get_terminal_column_width();

	// Per-OS log-line prefix for the shared POSIX terminal-width probe. Defined once
	// per Unix variant under src/core/platform/<os>/ and selected by CMake, so the
	// shared platform/posix/get_terminal_column_width.cpp carries no OS preprocessor
	// branch (the Windows probe is a separate platform/windows/ source).
	extern const std::string_view PLATFORM_LOG_PREFIX;

}
// namespace AqualinkAutomate::Utility
