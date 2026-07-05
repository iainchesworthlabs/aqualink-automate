#include <cctype>
#include <string>

#include <magic_enum/magic_enum.hpp>

#include "profiling/profiling_controller.h"
#include "profiling/factories/profiler_factory.h"
#include "profiling/types/profiling_types.h"

namespace AqualinkAutomate::Profiling
{

	namespace
	{
		// Backend names are exposed lowercase to match the --profiler CLI option
		// convention (tracy / uprof / vtune).
		std::string ToLowerName(Types::ProfilerTypes type)
		{
			std::string name{ magic_enum::enum_name(type) };
			for (auto& c : name)
			{
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			return name;
		}

		// True when a backend has been selected AND it was compiled into this
		// build (so Get() returns a real profiler rather than the NoOp fallback).
		bool HasActiveBackend(const Factory::ProfilerFactory& factory)
		{
			const auto selected = factory.Selected();
			return selected.has_value() && factory.IsRegistered(*selected);
		}
	}

	ProfilingController::ProfilingController()
	{
		// Capture is resumed at startup (main calls StartProfiling() after options
		// are processed), so reflect "running" whenever a real backend is active.
		m_Running = HasActiveBackend(Factory::ProfilerFactory::Instance());
	}

	Interfaces::IProfilingController::Status ProfilingController::ProfilingStatus() const
	{
		auto& factory = Factory::ProfilerFactory::Instance();

		Status status;

		const auto registered = factory.RegisteredTypes();
		status.enabled = !registered.empty();
		status.available_backends.reserve(registered.size());
		for (const auto type : registered)
		{
			status.available_backends.push_back(ToLowerName(type));
		}

		if (HasActiveBackend(factory))
		{
			status.active_backend = ToLowerName(*factory.Selected());
		}

		status.running = m_Running && HasActiveBackend(factory);

		return status;
	}

	bool ProfilingController::Start()
	{
		auto& factory = Factory::ProfilerFactory::Instance();
		if (!HasActiveBackend(factory))
		{
			return false;
		}

		factory.Get()->Resume();
		m_Running = true;
		return true;
	}

	bool ProfilingController::Stop()
	{
		auto& factory = Factory::ProfilerFactory::Instance();
		if (!HasActiveBackend(factory))
		{
			return false;
		}

		factory.Get()->Pause();
		m_Running = false;
		return true;
	}

	bool ProfilingController::SelectBackend(std::string_view backend)
	{
		const auto type = magic_enum::enum_cast<Types::ProfilerTypes>(backend, magic_enum::case_insensitive);
		if (!type.has_value())
		{
			return false;  // not a recognised backend name
		}

		auto& factory = Factory::ProfilerFactory::Instance();
		if (!factory.IsRegistered(*type))
		{
			return false;  // recognised, but not compiled into this build
		}

		factory.SetDesired(*type);
		return true;
	}

}
// namespace AqualinkAutomate::Profiling
