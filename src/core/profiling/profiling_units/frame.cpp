#include "profiling/profiling_units/frame.h"

namespace AqualinkAutomate::Profiling
{

	Frame::Frame(std::string_view name, [[maybe_unused]] const std::source_location& src_loc, [[maybe_unused]] UnitColours colour) :
		Interfaces::IProfilingUnit(name)
	{
	}

	void Frame::Start() const
	{
		// Intentionally empty: the base Frame unit is a no-op; concrete backends override this.
	}

	void Frame::End() const
	{
		// Intentionally empty: the base Frame unit is a no-op; concrete backends override this.
	}

	void Frame::Mark() const
	{
		// Intentionally empty: the base Frame unit is a no-op; concrete backends override this.
	}

}
// namespace AqualinkAutomate::Profiling
