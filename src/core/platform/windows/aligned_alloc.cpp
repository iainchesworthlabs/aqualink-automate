// NOLINTBEGIN(cppcoreguidelines-no-malloc) — this file intentionally wraps the
// Win32 CRT aligned-allocation API to back the platform AlignedMalloc/AlignedFree
// seam (see docs/platform-isolation.md); RAII does not apply to a raw allocator.

#include <malloc.h>  // _aligned_malloc / _aligned_free

#include "platform/aligned_alloc.h"

namespace AqualinkAutomate::Platform
{

	void* AlignedMalloc(std::size_t size, std::size_t alignment) noexcept
	{
		return _aligned_malloc(size, alignment);
	}

	void AlignedFree(void* ptr) noexcept
	{
		_aligned_free(ptr);
	}

}
// namespace AqualinkAutomate::Platform

// NOLINTEND(cppcoreguidelines-no-malloc)
