#include "profiling/noop_profiler.h"

namespace AqualinkAutomate::Profiling
{

	void NoOp_Profiler::StartProfiling()
	{
		// Intentionally empty: the no-op profiler is the fallback when no backend
		// is selected, so starting profiling does nothing.
	}

	void NoOp_Profiler::StopProfiling()
	{
		// Intentionally empty: the no-op profiler is the fallback when no backend
		// is selected, so stopping profiling does nothing.
	}

}
// namespace AqualinkAutomate::Profiling
