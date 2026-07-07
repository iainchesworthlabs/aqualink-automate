#include "profiling/noop_profiler.h" 
#include "profiling/factories/profiling_unit_factory.h"
#include "profiling/profiling_units/frame.h"
#include "profiling/profiling_units/zone.h"

namespace AqualinkAutomate::Factory
{

	ProfilingUnitFactory::ProfilingUnitFactory()
	{
	}

	ProfilingUnitFactory& ProfilingUnitFactory::Instance()
	{
		static ProfilingUnitFactory instance;
		return instance;
	}

	bool ProfilingUnitFactory::Register(const Types::ProfilerTypes type, ProfilingUnitGenerators&& generators)
	{
		return m_Generators.emplace(type, std::move(generators)).second;
	}

	void ProfilingUnitFactory::SetDesired(Types::ProfilerTypes type)
	{
		m_DesiredProfiler = type;
	}

	Types::ProfilingUnitTypePtr ProfilingUnitFactory::CreateDomain(std::string_view name, const std::source_location& src_loc, Profiling::UnitColours colour)
	{
		const auto& generators = Get();
		const auto& domain_generator = std::get<0>(generators);
		return domain_generator(name, src_loc, colour);
	}

	Types::ProfilingUnitTypePtr ProfilingUnitFactory::CreateFrame(std::string_view name, const std::source_location& src_loc, Profiling::UnitColours colour)
	{
		const auto& generators = Get();
		const auto& frame_generator = std::get<1>(generators);
		return frame_generator(name, src_loc, colour);
	}

	Types::ProfilingUnitTypePtr ProfilingUnitFactory::CreateZone(std::string_view name, const std::source_location& src_loc, Profiling::UnitColours colour)
	{
		// CreateZone runs on the per-message hot path. Bind the generator by const reference
		// to avoid copying the std::function (which can heap-allocate / touch a refcount) on
		// every zone creation.
		const auto& generators = Get();
		const auto& zone_generator = std::get<2>(generators);
		return zone_generator(name, src_loc, colour);
	}

	const ProfilingUnitGenerators& ProfilingUnitFactory::Get()
	{
		if (m_DesiredProfiler.has_value())
		{
			if (auto it = m_Generators.find(m_DesiredProfiler.value()); m_Generators.end() != it)
			{
				return it->second;
			}
		}
		// No selected profiler or not found so it's NoOp...

		static ProfilingUnitGenerators noop_generators = std::make_tuple
		(
			[](std::string_view name, const std::source_location& src_loc, Profiling::UnitColours colour) -> Types::ProfilingUnitTypePtr { return std::make_unique<Profiling::Domain>(name, src_loc, colour); },
			[](std::string_view name, const std::source_location& src_loc, Profiling::UnitColours colour) -> Types::ProfilingUnitTypePtr { return std::make_unique<Profiling::Frame>(name, src_loc, colour); },
			[](std::string_view name, const std::source_location& src_loc, Profiling::UnitColours colour) -> Types::ProfilingUnitTypePtr { return std::make_unique<Profiling::Zone>(name, src_loc, colour); }
		);

		return noop_generators;
	}

}
// namespace AqualinkAutomate::Factory
