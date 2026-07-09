#include "profiling/profiling.h"

#if defined(TRACY_ENABLE)
#include "profiling/tracy_profiler.h"
#include "profiling/profiling_units/tracy_frame.h"
#include "profiling/profiling_units/tracy_zone.h"
#endif

#if defined(UProf_SUPPORT_ENABLED)
#include "profiling/uprof_profiler.h"
#if defined(UProf_ACTIVITY_LOGGER_ENABLED)
#include "profiling/profiling_units/uprof_frame.h"
#include "profiling/profiling_units/uprof_zone.h"
#endif
#endif

#if defined(VTUNE_SUPPORT_ENABLED)
#include "profiling/vtune_profiler.h"
#include "profiling/profiling_units/vtune_frame.h"
#include "profiling/profiling_units/vtune_zone.h"
#endif

namespace AqualinkAutomate::Profiling
{

	void RegisterAvailableProfilers([[maybe_unused]] Factory::ProfilerFactory& profiler, [[maybe_unused]] Factory::ProfilingUnitFactory& profiler_units)
	{
#if defined(TRACY_ENABLE)
		profiler.Register(Types::ProfilerTypes::Tracy, std::make_shared<Tracy_Profiler>());

		profiler_units.Register(Types::ProfilerTypes::Tracy, std::make_tuple(
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
				return std::make_unique<Domain>(name, src_loc, colour);
			},
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
				return std::make_unique<TracyFrame>(name, src_loc, colour);
			},
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
				return std::make_unique<TracyZone>(name, src_loc, colour);
			}
		));
#endif

#if defined(UProf_SUPPORT_ENABLED)
		profiler.Register(Types::ProfilerTypes::UProf, std::make_shared<UProf_Profiler>());

		profiler_units.Register(Types::ProfilerTypes::UProf, std::make_tuple(
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
				return std::make_unique<Domain>(name, src_loc, colour);
			},
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
#if defined(UProf_ACTIVITY_LOGGER_ENABLED)
				return std::make_unique<UProfFrame>(name, src_loc, colour);
#else
				return std::make_unique<Frame>(name, src_loc, colour);
#endif
			},
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
#if defined(UProf_ACTIVITY_LOGGER_ENABLED)
				return std::make_unique<UProfZone>(name, src_loc, colour);
#else
				return std::make_unique<Zone>(name, src_loc, colour);
#endif
			}
		));
#endif

#if defined(VTUNE_SUPPORT_ENABLED)
		profiler.Register(Types::ProfilerTypes::VTune, std::make_shared<VTune_Profiler>());

		profiler_units.Register(Types::ProfilerTypes::VTune, std::make_tuple(
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
				return std::make_unique<Domain>(name, src_loc, colour);
			},
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
				return std::make_unique<VTuneFrame>(name, src_loc, colour);
			},
			[](std::string_view name, const std::source_location& src_loc, UnitColours colour) -> Types::ProfilingUnitTypePtr
			{
				return std::make_unique<VTuneZone>(name, src_loc, colour);
			}
		));
#endif
	}

}
// namespace AqualinkAutomate::Profiling