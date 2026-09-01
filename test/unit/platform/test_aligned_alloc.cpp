#include <cstddef>
#include <cstdint>

#include <boost/test/unit_test.hpp>

#include "platform/aligned_alloc.h"

using namespace AqualinkAutomate;

//=============================================================================
// Platform::AlignedMalloc / AlignedFree — the OS-neutral seam over the Win32
// CRT (_aligned_malloc/_aligned_free) and POSIX C11 (aligned_alloc/free)
// over-aligned allocators (see docs/platform-isolation.md). Exercised through
// the shared header only, so whichever backend CMake selected for the running
// platform is what these tests drive.
//=============================================================================

BOOST_AUTO_TEST_SUITE(TestSuite_Platform_AlignedAlloc)

BOOST_AUTO_TEST_CASE(AlignedMalloc_ReturnsNonNull_ForOrdinarySizeAndAlignment)
{
	void* ptr = Platform::AlignedMalloc(64, 16);
	BOOST_REQUIRE(nullptr != ptr);
	Platform::AlignedFree(ptr);
}

BOOST_AUTO_TEST_CASE(AlignedMalloc_ReturnsPointerAlignedToEachRequestedBoundary)
{
	for (const std::size_t alignment : { std::size_t{ 8 }, std::size_t{ 16 }, std::size_t{ 32 }, std::size_t{ 64 }, std::size_t{ 128 } })
	{
		void* ptr = Platform::AlignedMalloc(256, alignment);
		BOOST_REQUIRE(nullptr != ptr);
		BOOST_TEST((reinterpret_cast<std::uintptr_t>(ptr) % alignment) == 0u);
		Platform::AlignedFree(ptr);
	}
}

BOOST_AUTO_TEST_CASE(AlignedMalloc_MemoryIsWritableAcrossFullRequestedSize)
{
	constexpr std::size_t size = 128;
	auto* bytes = static_cast<unsigned char*>(Platform::AlignedMalloc(size, 16));
	BOOST_REQUIRE(nullptr != bytes);

	// Write a distinct pattern across the FULL requested size and read it back,
	// proving the block is real, usable memory of at least `size` bytes - not
	// just an aligned-but-truncated allocation.
	for (std::size_t i = 0; i < size; ++i)
	{
		bytes[i] = static_cast<unsigned char>(i & 0xFF);
	}
	for (std::size_t i = 0; i < size; ++i)
	{
		BOOST_TEST(bytes[i] == static_cast<unsigned char>(i & 0xFF));
	}

	Platform::AlignedFree(bytes);
}

BOOST_AUTO_TEST_CASE(AlignedMalloc_HandlesSizeNotAMultipleOfAlignment)
{
	// size need not already be a multiple of alignment - the backend must round up
	// internally (the POSIX backend in particular: C11 aligned_alloc requires size
	// to already be a multiple of alignment, so it has real rounding logic to get
	// this right).
	constexpr std::size_t size = 30;
	constexpr std::size_t alignment = 16;

	auto* bytes = static_cast<unsigned char*>(Platform::AlignedMalloc(size, alignment));
	BOOST_REQUIRE(nullptr != bytes);
	BOOST_TEST((reinterpret_cast<std::uintptr_t>(bytes) % alignment) == 0u);

	for (std::size_t i = 0; i < size; ++i)
	{
		bytes[i] = static_cast<unsigned char>(i);
	}
	for (std::size_t i = 0; i < size; ++i)
	{
		BOOST_TEST(bytes[i] == static_cast<unsigned char>(i));
	}

	Platform::AlignedFree(bytes);
}

BOOST_AUTO_TEST_CASE(AlignedFree_AcceptsNullptr)
{
	// Both backends alias a plain free() (POSIX) / _aligned_free() (Windows) on
	// nullptr, which is a documented, safe no-op on either CRT.
	BOOST_CHECK_NO_THROW(Platform::AlignedFree(nullptr));
}

BOOST_AUTO_TEST_SUITE_END()
