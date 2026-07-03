// NOLINTBEGIN(cppcoreguidelines-no-malloc)
// This file intentionally uses malloc/free/aligned_alloc to implement global
// operator new/delete replacements for Tracy memory profiling.

// PREfast C28251: Our global operator new/delete replacements intentionally
// lack the SAL annotations present on the CRT declarations.  These are
// legitimate replacements for Tracy memory profiling; suppress the noise.
#if defined(_MSC_VER)
#pragma warning(disable: 28251)
#endif

#if defined(TRACY_ENABLE)

#include <atomic>   // std::atomic_flag
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

#include <tracy/Tracy.hpp>
#include <client/TracyProfiler.hpp>

#include "platform/aligned_alloc.h"

// ============================================================================
// Overview
//
// Global operator new/delete overrides for Tracy memory profiling.
//
// Challenge: This project links many DLLs (Boost, Tracy, etc.).  Our operator
// new/delete overrides only apply to code compiled into the executable.  DLL
// code uses the CRT's default operator new/delete.  When an object is
// allocated inside a DLL but freed in our code (or vice versa), we get a
// TracyFree for an address that was never TracyAlloc'd, which causes Tracy to
// terminate the profiling session.
//
// Solution: Maintain a set of pointers that WE allocated (i.e. went through
// our operator new).  On free, only emit TracySecureFree if the pointer is in
// our tracked set.  The set itself uses malloc/free directly to avoid infinite
// recursion, and a thread-local guard prevents re-entrancy.
//
// We gate on tracy::ProfilerAvailable() rather than a custom static flag to
// ensure our tracker stays perfectly in sync with Tracy's internal state.
// This prevents mismatches during startup (before Tracy's profiler singleton
// is initialised) and shutdown (after it has been destroyed).
//
// Ghost entries: When our code allocates a pointer and a DLL later frees it
// (bypassing our operator delete), the pointer remains in the tracker as a
// "ghost".  If malloc reuses that address, Insert() returns false.  We
// intentionally do NOT emit a new TracySecureAlloc in this case — the
// original TracySecureAlloc is still active from Tracy's perspective (no
// TracySecureFree was ever sent for it).  When the address is eventually
// freed through our operator delete, the Remove() succeeds and a single
// TracySecureFree correctly balances the original allocation event.
// ============================================================================

// ============================================================================
// Pointer tracking set — open-addressing hash table backed by malloc
//
// Requirements:
//  - Must never call operator new/delete (would recurse)
//  - Must be thread-safe
//  - Must survive past all static destructors (intentionally leaked)
// ============================================================================

class PointerTracker
{
public:
	static PointerTracker& Instance()
	{
		// Intentionally leaked: allocated with malloc, never freed.
		// This ensures the tracker outlives all static destructors.
		static PointerTracker* s_instance = []()
		{
			void* mem = std::malloc(sizeof(PointerTracker));
			return new (mem) PointerTracker();
		}();
		return *s_instance;
	}

	bool Insert(void* ptr)
	{
		SpinLockGuard guard(m_Lock);
		if ((m_Count + m_TombstoneCount) * 4 >= m_Capacity * 3) // 75% load factor (live + tombstones)
		{
			Grow();
		}
		return InsertInternal(m_Buckets, m_Capacity, ptr);
	}

	bool Remove(void* ptr)
	{
		SpinLockGuard guard(m_Lock);
		auto idx = Hash(ptr) & (m_Capacity - 1);
		for (;;)
		{
			auto entry = m_Buckets[idx];
			if (entry == nullptr) return false;  // not found
			if (entry == ptr)
			{
				// Mark as tombstone
				m_Buckets[idx] = Tombstone();
				--m_Count;
				++m_TombstoneCount;
				return true;
			}
			idx = (idx + 1) & (m_Capacity - 1);
		}
	}

private:
	static constexpr std::size_t INITIAL_CAPACITY = 1u << 16; // 64K entries

	void** m_Buckets{ nullptr };
	std::size_t m_Capacity{ 0 };
	std::size_t m_Count{ 0 };
	std::size_t m_TombstoneCount{ 0 };
	bool m_OOMOccurred{ false };
	std::atomic_flag m_Lock = ATOMIC_FLAG_INIT;

	PointerTracker()
	{
		m_Capacity = INITIAL_CAPACITY;
		m_Buckets = static_cast<void**>(std::calloc(m_Capacity, sizeof(void*)));
	}

	static void* Tombstone()
	{
		// Use address 1 as tombstone — never a valid heap pointer.
		return reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
	}

	static std::size_t Hash(void* ptr)
	{
		// Mix pointer bits; shift right to discard low alignment bits.
		auto v = reinterpret_cast<std::uintptr_t>(ptr);
		v >>= 4;
		v ^= v >> 16;
		v *= 0x45d9f3b;
		v ^= v >> 16;
		return static_cast<std::size_t>(v);
	}

	bool InsertInternal(void** buckets, std::size_t capacity, void* ptr)
	{
		auto idx = Hash(ptr) & (capacity - 1);
		std::size_t firstTombstone = SIZE_MAX;
		for (;;)
		{
			auto entry = buckets[idx];
			if (entry == nullptr)
			{
				// Key not found. Insert at first tombstone if available, otherwise here.
				auto insertIdx = (firstTombstone != SIZE_MAX) ? firstTombstone : idx;
				if (buckets[insertIdx] == Tombstone() && m_TombstoneCount > 0)
				{
					--m_TombstoneCount;
				}
				buckets[insertIdx] = ptr;
				++m_Count;
				return true;
			}
			if (entry == Tombstone())
			{
				if (firstTombstone == SIZE_MAX)
				{
					firstTombstone = idx;
				}
			}
			else if (entry == ptr)
			{
				return false;  // already present
			}
			idx = (idx + 1) & (capacity - 1);
		}
	}

	void Grow()
	{
		auto newCapacity = m_Capacity * 2;
		auto* newBuckets = static_cast<void**>(std::calloc(newCapacity, sizeof(void*)));
		if (!newBuckets)
		{
			m_OOMOccurred = true; // Debugger-visible; cannot log (would recurse)
			return;
		}

		// Re-insert all live entries (skip nulls and tombstones)
		auto oldCount = m_Count;
		m_Count = 0;
		m_TombstoneCount = 0; // Rehash strips all tombstones
		for (std::size_t i = 0; i < m_Capacity; ++i)
		{
			auto entry = m_Buckets[i];
			if (entry != nullptr && entry != Tombstone())
			{
				InsertInternal(newBuckets, newCapacity, entry);
			}
		}
		(void)oldCount; // m_Count should now equal oldCount

		std::free(m_Buckets);
		m_Buckets = newBuckets;
		m_Capacity = newCapacity;
	}

	struct SpinLockGuard
	{
		std::atomic_flag& flag;
		SpinLockGuard(std::atomic_flag& f) : flag(f)
		{
			while (flag.test_and_set(std::memory_order_acquire))
			{
#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)
				_mm_pause();
#endif
			}
		}
		~SpinLockGuard() { flag.clear(std::memory_order_release); }
	};
};

// ============================================================================
// Thread-local re-entrancy guard
//
// Prevents infinite recursion when the tracker's own operations (calloc,
// free during Grow) call our operator new/delete.
// ============================================================================

static thread_local bool s_InAllocator = false;

// ============================================================================
// Tracking helpers
// ============================================================================

static void TrackAlloc(void* ptr, std::size_t size)
{
	if (!ptr || s_InAllocator || !tracy::ProfilerAvailable())
		return;
#ifdef TRACY_ON_DEMAND
	if (!tracy::GetProfiler().IsConnected())
		return;
#endif
	s_InAllocator = true;
	if (PointerTracker::Instance().Insert(ptr))
	{
		TracySecureAlloc(ptr, size);
	}
	// If Insert returned false, the address is already tracked (ghost entry
	// from a DLL-freed pointer).  The original TracySecureAlloc is still
	// active in Tracy — skip to avoid sending a duplicate allocation event.
	s_InAllocator = false;
}

static void TrackFree(void* ptr)
{
	if (!ptr || s_InAllocator || !tracy::ProfilerAvailable())
		return;
	s_InAllocator = true;
	if (PointerTracker::Instance().Remove(ptr))
	{
#ifdef TRACY_ON_DEMAND
		if (tracy::GetProfiler().IsConnected())
#endif
		{
			TracySecureFree(ptr);
		}
	}
	s_InAllocator = false;
}

// ============================================================================
// Platform helpers for aligned allocation
// ============================================================================

static void* AlignedMalloc(std::size_t size, std::size_t alignment)
{
	return AqualinkAutomate::Platform::AlignedMalloc(size, alignment);
}

static void AlignedFree(void* ptr)
{
	AqualinkAutomate::Platform::AlignedFree(ptr);
}

// ============================================================================
// Regular (non-aligned) operator new / delete
// ============================================================================

void* operator new(std::size_t size)
{
	auto ptr = std::malloc(size);
	if (!ptr)
	{
		throw std::bad_alloc();
	}
	TrackAlloc(ptr, size);
	return ptr;
}

void* operator new[](std::size_t size)
{
	auto ptr = std::malloc(size);
	if (!ptr)
	{
		throw std::bad_alloc();
	}
	TrackAlloc(ptr, size);
	return ptr;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	auto ptr = std::malloc(size);
	TrackAlloc(ptr, size);
	return ptr;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
	auto ptr = std::malloc(size);
	TrackAlloc(ptr, size);
	return ptr;
}

void operator delete(void* ptr) noexcept
{
	TrackFree(ptr);
	std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
	TrackFree(ptr);
	std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
	TrackFree(ptr);
	std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
	TrackFree(ptr);
	std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
	TrackFree(ptr);
	std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
	TrackFree(ptr);
	std::free(ptr);
}

// ============================================================================
// Aligned operator new / delete  (C++17)
// ============================================================================

void* operator new(std::size_t size, std::align_val_t al)
{
	auto ptr = AlignedMalloc(size, static_cast<std::size_t>(al));
	if (!ptr)
	{
		throw std::bad_alloc();
	}
	TrackAlloc(ptr, size);
	return ptr;
}

void* operator new[](std::size_t size, std::align_val_t al)
{
	auto ptr = AlignedMalloc(size, static_cast<std::size_t>(al));
	if (!ptr)
	{
		throw std::bad_alloc();
	}
	TrackAlloc(ptr, size);
	return ptr;
}

void* operator new(std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
	auto ptr = AlignedMalloc(size, static_cast<std::size_t>(al));
	TrackAlloc(ptr, size);
	return ptr;
}

void* operator new[](std::size_t size, std::align_val_t al, const std::nothrow_t&) noexcept
{
	auto ptr = AlignedMalloc(size, static_cast<std::size_t>(al));
	TrackAlloc(ptr, size);
	return ptr;
}

void operator delete(void* ptr, std::align_val_t) noexcept
{
	TrackFree(ptr);
	AlignedFree(ptr);
}

void operator delete[](void* ptr, std::align_val_t) noexcept
{
	TrackFree(ptr);
	AlignedFree(ptr);
}

void operator delete(void* ptr, std::align_val_t al, const std::nothrow_t&) noexcept
{
	TrackFree(ptr);
	AlignedFree(ptr);
}

void operator delete[](void* ptr, std::align_val_t al, const std::nothrow_t&) noexcept
{
	TrackFree(ptr);
	AlignedFree(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept
{
	TrackFree(ptr);
	AlignedFree(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept
{
	TrackFree(ptr);
	AlignedFree(ptr);
}

#endif // defined(TRACY_ENABLE)

// NOLINTEND(cppcoreguidelines-no-malloc)
