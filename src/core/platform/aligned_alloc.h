#pragma once

#include <cstddef>

namespace AqualinkAutomate::Platform
{

	//
	// Over-aligned allocation, split per-OS because the C runtimes disagree: the
	// Win32 CRT pairs _aligned_malloc with _aligned_free (a block from one must be
	// freed with the other), while POSIX uses C11 aligned_alloc paired with
	// std::free. Implemented once per platform under src/core/platform/<os>/ and
	// selected by CMake, so callers (e.g. the Tracy operator new/delete hooks in
	// profiling/memory/tracy_memory.cpp) carry no OS preprocessor branch.
	// See docs/platform-isolation.md.
	//

	// Allocate @p size bytes aligned to @p alignment, or nullptr on failure.
	// Must be released with AlignedFree (never plain std::free on Windows).
	[[nodiscard]] void* AlignedMalloc(std::size_t size, std::size_t alignment) noexcept;

	// Release a block obtained from AlignedMalloc.
	void AlignedFree(void* ptr) noexcept;

}
// namespace AqualinkAutomate::Platform
