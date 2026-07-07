#include "interfaces/iprofiler.h"

namespace AqualinkAutomate::Interfaces
{

	Profiling::DomainPtr IProfiler::CreateDomain(const std::string& name) const
	{
		auto domain = std::make_unique<Profiling::Domain>(name);
		return domain;
	}

	Profiling::FramePtr IProfiler::CreateFrame(Profiling::DomainPtr domain, const std::string& name) const
	{
		auto frame = std::make_unique<Profiling::Frame>(name);
		return frame;
	}

	Profiling::ZonePtr IProfiler::CreateZone(Profiling::FramePtr frame, const std::string& name) const
	{
		auto zone = std::make_unique<Profiling::Zone>(name);
		return zone;
	}

	void IProfiler::Resume()
	{
		// No-op default: the null profiler backend has nothing to resume.
	}

	void IProfiler::Pause()
	{
		// No-op default: the null profiler backend has nothing to pause.
	}

	void IProfiler::Message(std::string_view) const
	{
		// No-op default: the null profiler backend discards messages.
	}

	void IProfiler::Message(std::string_view, uint32_t) const
	{
		// No-op default: the null profiler backend discards messages.
	}

	void IProfiler::PlotValue(const std::string&, int64_t)
	{
		// No-op default: the null profiler backend discards plotted values.
	}

	void IProfiler::PlotValue(const std::string&, double)
	{
		// No-op default: the null profiler backend discards plotted values.
	}

	void IProfiler::SetThreadName(const char*) const
	{
		// No-op default: the null profiler backend does not track thread names.
	}

	void IProfiler::AppInfo(std::string_view) const
	{
		// No-op default: the null profiler backend discards application info.
	}

	void IProfiler::EmitFrameMark(const char*) const
	{
		// No-op default: the null profiler backend does not mark frames.
	}

}
// namespace AqualinkAutomate::Interfaces
