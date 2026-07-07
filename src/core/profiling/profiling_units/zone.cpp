#include "profiling/profiling_units/zone.h"

namespace AqualinkAutomate::Profiling
{

	Zone::Zone(std::string_view name, [[maybe_unused]] const std::source_location& src_loc, [[maybe_unused]] UnitColours colour) :
		Interfaces::IProfilingUnit(name)
	{
	}

	void Zone::Start() const
	{
		// Intentionally empty: the base Zone unit is a no-op; concrete backends override this.
	}

	void Zone::Mark() const
	{
		// Intentionally empty: the base Zone unit is a no-op; concrete backends override this.
	}

	void Zone::End() const
	{
		// Intentionally empty: the base Zone unit is a no-op; concrete backends override this.
	}

}
// namespace AqualinkAutomate::Profiling
