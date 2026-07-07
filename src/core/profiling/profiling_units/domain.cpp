#include "profiling/profiling_units/domain.h"

namespace AqualinkAutomate::Profiling
{

	Domain::Domain(std::string_view name, [[maybe_unused]] const std::source_location& src_loc, [[maybe_unused]] UnitColours colour) :
		Interfaces::IProfilingUnit(name)
	{
	}

	void Domain::Start() const
	{
		// Intentionally empty: the base Domain unit is a no-op; concrete backends override this.
	}

	void Domain::End() const
	{
		// Intentionally empty: the base Domain unit is a no-op; concrete backends override this.
	}

	void Domain::Mark() const
	{
		// Intentionally empty: the base Domain unit is a no-op; concrete backends override this.
	}

}
// namespace AqualinkAutomate::Profiling
