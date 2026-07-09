#include "interfaces/iprofilingunit.h"

namespace AqualinkAutomate::Interfaces
{

	IProfilingUnit::IProfilingUnit(std::string_view name) :
		m_Name(name)
	{
	}

	std::string_view IProfilingUnit::Name() const
	{
		return m_Name;
	}

	void IProfilingUnit::Text(std::string_view) const
	{
		// No-op default: the null profiling unit discards text annotations.
	}

	void IProfilingUnit::Value(uint64_t) const
	{
		// No-op default: the null profiling unit discards value annotations.
	}

}
// namespace AqualinkAutomate::Interfaces
