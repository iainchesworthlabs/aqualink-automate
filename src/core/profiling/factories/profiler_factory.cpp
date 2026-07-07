#include "profiling/noop_profiler.h" 
#include "profiling/factories/profiler_factory.h"
#include "profiling/factories/profiling_unit_factory.h"

namespace AqualinkAutomate::Factory
{

	ProfilerFactory::ProfilerFactory()
	{
	}

	ProfilerFactory& ProfilerFactory::Instance()
	{
		static ProfilerFactory instance;
		return instance;
	}

	bool ProfilerFactory::Register(Types::ProfilerTypes type, Types::ProfilerTypePtr&& instance_ptr)
	{
		return m_Profilers.emplace(type, std::move(instance_ptr)).second;
	}

	void ProfilerFactory::SetDesired(Types::ProfilerTypes type)
	{
		m_DesiredProfiler = type;

		// Make sure that the profiling units are the same type.
		ProfilingUnitFactory::Instance().SetDesired(type);
	}

	std::vector<Types::ProfilerTypes> ProfilerFactory::RegisteredTypes() const
	{
		std::vector<Types::ProfilerTypes> types;
		types.reserve(m_Profilers.size());
		for (const auto& [type, instance] : m_Profilers)
		{
			types.push_back(type);
		}
		return types;
	}

	bool ProfilerFactory::IsRegistered(Types::ProfilerTypes type) const
	{
		return m_Profilers.contains(type);
	}

	std::optional<Types::ProfilerTypes> ProfilerFactory::Selected() const
	{
		return m_DesiredProfiler;
	}

	Types::ProfilerTypePtr ProfilerFactory::Get()
	{
		if (m_DesiredProfiler.has_value())
		{
			if (auto it = m_Profilers.find(m_DesiredProfiler.value()); m_Profilers.end() != it)
			{
				if (auto instance = it->second; nullptr != instance)
				{
					return instance;
				}
			}
		}

		// No selected profiler, not found, or could not be instantiated so it's NoOp...
		//
		// Get() is called on the per-frame hot path (PlotValue / EmitFrameMark), so the
		// NoOp fallback is shared from a single function-local static rather than being
		// heap-allocated (make_shared) on every call. The instance is stateless, so a
		// shared one is safe; the single-threaded cooperative model means no synchronisation
		// is required for the lazy init.
		static const Types::ProfilerTypePtr noop_profiler = std::make_shared<Profiling::NoOp_Profiler>();
		return noop_profiler;
	}

}
// namespace AqualinkAutomate::Factory
