// NOLINTBEGIN(cppcoreguidelines-no-malloc) — this file intentionally wraps the
// POSIX C11 aligned-allocation CRT API to back the platform AlignedMalloc/AlignedFree
// seam (see docs/platform-isolation.md); RAII does not apply to a raw allocator.

#include <cstdlib>  // aligned_alloc / std::free

#include "platform/aligned_alloc.h"

namespace AqualinkAutomate::Platform
{

	void* AlignedMalloc(std::size_t size, std::size_t alignment) noexcept
	{
		// C11 aligned_alloc requires size to be a multiple of alignment.
		const std::size_t aligned_size = (size + alignment - 1) & ~(alignment - 1);
		return aligned_alloc(alignment, aligned_size);
	}

	void AlignedFree(void* ptr) noexcept
	{
		std::free(ptr);
	}

}
// namespace AqualinkAutomate::Platform

// NOLINTEND(cppcoreguidelines-no-malloc)
