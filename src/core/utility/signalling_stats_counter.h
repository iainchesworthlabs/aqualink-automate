#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>

#include <boost/signals2.hpp>
#include <magic_enum/magic_enum.hpp>

#include "exceptions/exception_signallingstatscounter_badaccess.h"
#include "logging/logging.h"

// NOTE: This header historically pulled `using namespace AqualinkAutomate::Logging;`
// into file scope; many translation units that include this header (transitively via
// statistics_hub.h) rely on that leak to resolve unqualified `Channel`/`Log*` names.
// Removing it is out of scope for this work unit (no header-hygiene finding is assigned
// here) and would risk breaking unrelated TUs, so it is retained. Names used inside this
// header are nonetheless fully qualified so the header does not itself depend on the leak.
using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Utility
{

	using StatsSignalFunc = void(uint64_t);
	using StatsSignal = boost::signals2::signal<StatsSignalFunc>;

	//---------------------------------------------------------------------
	//
	//---------------------------------------------------------------------

	class StatsCounter
	{
	public:
		explicit StatsCounter(StatsSignal& stat_signal);
		StatsCounter(const StatsCounter& other);
		StatsCounter(StatsCounter&& other) noexcept;
		StatsCounter& operator=(const StatsCounter& other) = delete;
		StatsCounter& operator=(StatsCounter&& other) noexcept = delete;

		StatsCounter& operator=(const uint64_t count_to_assign);
		StatsCounter& operator+=(const uint64_t count_to_add);
		StatsCounter& operator++();

		uint64_t operator()() const;
		uint64_t Count() const;

	private:
		std::uint64_t m_Count{ 0 };
		StatsSignal& m_StatsSignal;
	};

	//---------------------------------------------------------------------
	//
	//---------------------------------------------------------------------

	class SignallingStatsCounter
	{
	private:
		//
		// Lightweight, heap-free key used for *lookups*. It carries only the
		// two hashes that operator==/AnyEnumHash actually use, so the hot path
		// (an already-registered stat) builds no std::string at all.
		//
		struct AnyEnumKey
		{
			template<typename STAT_TYPE>
				requires std::is_enum_v<STAT_TYPE>
			explicit AnyEnumKey(STAT_TYPE value) noexcept :
				type_hash(typeid(STAT_TYPE).hash_code()),
				value_hash(std::hash<std::underlying_type_t<STAT_TYPE>>{}(std::to_underlying(value)))
			{
			}

			std::size_t type_hash;
			std::size_t value_hash;
		};

		//
		// The *stored* key. It is constructed exactly once (only on insertion of a
		// not-yet-seen stat) and additionally retains a display name for iteration
		// consumers (e.g. the equipment-stats JSON serialiser reads `key.name`).
		// The previous design built this std::string on *every* operator[] call;
		// it is now built only on the cold insertion path.
		//
		struct AnyEnum
		{
			template<typename STAT_TYPE>
				requires std::is_enum_v<STAT_TYPE>
			explicit AnyEnum(STAT_TYPE value) :
				type_hash(typeid(STAT_TYPE).hash_code()),
				value_hash(std::hash<std::underlying_type_t<STAT_TYPE>>{}(std::to_underlying(value))),
				name(magic_enum::enum_name(value))
			{
			}

			std::size_t type_hash;
			std::size_t value_hash;
			std::string name;

			bool operator==(const AnyEnum& other) const noexcept
			{
				return type_hash == other.type_hash && value_hash == other.value_hash;
			}

			bool operator==(const AnyEnumKey& other) const noexcept
			{
				return type_hash == other.type_hash && value_hash == other.value_hash;
			}
		};

		//
		// Transparent hasher: hashes AnyEnum and AnyEnumKey identically so that
		// heterogeneous lookup with the heap-free AnyEnumKey resolves to the
		// stored AnyEnum entry without constructing a std::string.
		//
		struct AnyEnumHash
		{
			using is_transparent = void;

			static constexpr std::size_t Mix(std::size_t type_hash, std::size_t value_hash) noexcept
			{
				return type_hash ^ (value_hash + 0x9e3779b9 + (type_hash << 6) + (type_hash >> 2));
			}

			std::size_t operator()(const AnyEnum& key) const noexcept
			{
				return Mix(key.type_hash, key.value_hash);
			}

			std::size_t operator()(const AnyEnumKey& key) const noexcept
			{
				return Mix(key.type_hash, key.value_hash);
			}
		};

		//
		// Transparent equality so that find(AnyEnumKey) compares against the
		// stored AnyEnum entries.
		//
		struct AnyEnumEqual
		{
			using is_transparent = void;

			bool operator()(const AnyEnum& lhs, const AnyEnum& rhs) const noexcept
			{
				return lhs == rhs;
			}

			bool operator()(const AnyEnum& lhs, const AnyEnumKey& rhs) const noexcept
			{
				return lhs == rhs;
			}

			bool operator()(const AnyEnumKey& lhs, const AnyEnum& rhs) const noexcept
			{
				return rhs == lhs;
			}
		};

	public:
		using StatsTypesMap = std::unordered_map<AnyEnum, StatsCounter, AnyEnumHash, AnyEnumEqual>;

		using key_type = typename StatsTypesMap::key_type;
		using mapped_type = typename StatsTypesMap::mapped_type;
		using value_type = typename StatsTypesMap::value_type;

	public:
		SignallingStatsCounter()
		{
		}

		template<typename STAT_TYPE>
			requires std::is_enum_v<STAT_TYPE>
		StatsCounter& operator[](const STAT_TYPE& stat_type)
		{
			// Hot path: heterogeneous lookup with a heap-free key (no std::string built).
			if (auto it = m_StatsMap.find(AnyEnumKey{ stat_type }); it != m_StatsMap.end())
			{
				return it->second;
			}

			// Cold path (first sighting of this stat): single insert lookup.
			auto [it, was_inserted] = m_StatsMap.try_emplace(AnyEnum{ stat_type }, m_StatsSignal);
			if (was_inserted)
			{
				LogTrace(Logging::Channel::Equipment, [] { return std::format("Inserted new stats counter for element (type: {})", typeid(STAT_TYPE).name()); });
			}

			return it->second;
		}

		template<typename STAT_TYPE>
			requires std::is_enum_v<STAT_TYPE>
		void AddStatsToCount(STAT_TYPE stat_type)
		{
			// Reuse operator[] so insertion stays a single hash lookup.
			(void)(*this)[stat_type];
		}

		auto begin() const noexcept { return m_StatsMap.begin(); }
		auto end() const noexcept { return m_StatsMap.end(); }
		auto cbegin() const noexcept { return m_StatsMap.cbegin(); }
		auto cend() const noexcept { return m_StatsMap.cend(); }

		StatsSignal& Signal() const
		{
			return m_StatsSignal;
		}

	private:
		StatsTypesMap m_StatsMap{};
		mutable StatsSignal m_StatsSignal;
	};

}
// namespace AqualinkAutomate::Utility
