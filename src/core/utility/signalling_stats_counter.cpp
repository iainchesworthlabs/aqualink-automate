#include "utility/signalling_stats_counter.h"

namespace AqualinkAutomate::Utility
{

	StatsCounter::StatsCounter(StatsSignal& stat_signal) :
		m_StatsSignal(stat_signal)
	{
	}

	StatsCounter::StatsCounter(const StatsCounter& other) = default;

	StatsCounter::StatsCounter(StatsCounter&& other) noexcept :
		m_Count{ other.m_Count },
		m_StatsSignal(other.m_StatsSignal)
	{
	}

	StatsCounter& StatsCounter::operator=(const uint64_t count_to_assign)
	{
		m_Count = count_to_assign;
		m_StatsSignal(m_Count);
		return *this;
	}

	StatsCounter& StatsCounter::operator+=(const uint64_t count_to_add)
	{
		m_Count += count_to_add;
		m_StatsSignal(m_Count);
		return *this;
	}

	StatsCounter& StatsCounter::operator++()
	{
		m_Count++;
		m_StatsSignal(m_Count);
		return *this;
	}

	uint64_t StatsCounter::operator()() const
	{
		return Count();
	}

	uint64_t StatsCounter::Count() const
	{
		return m_Count;
	}

}
// namespace AqualinkAutomate::Utility
