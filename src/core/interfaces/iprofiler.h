#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "profiling/profiling_units/domain.h"
#include "profiling/profiling_units/frame.h"
#include "profiling/profiling_units/zone.h"

namespace AqualinkAutomate::Interfaces
{

	class IProfiler
	{
	public:
		virtual ~IProfiler() = default;

		// Process lifecycle: called once at startup / shutdown.
		virtual void StartProfiling() = 0;
		virtual void StopProfiling() = 0;

		// Runtime capture gating, distinct from the process lifecycle above.
		// Lets the runtime control surface (e.g. the diagnostics endpoint) pause
		// and resume sample/trace collection without tearing the profiler down.
		// Default no-op; backends that support it (VTune __itt_pause/resume, uProf
		// amdProfilePause/Resume) override. Tracy is client-driven so stays no-op.
		virtual void Resume();
		virtual void Pause();

		virtual Profiling::DomainPtr CreateDomain(const std::string& name) const;
		virtual Profiling::FramePtr CreateFrame(Profiling::DomainPtr domain, const std::string& name) const;
		virtual Profiling::ZonePtr CreateZone(Profiling::FramePtr frame, const std::string& name) const;

		virtual void Message(std::string_view text) const;
		virtual void Message(std::string_view text, uint32_t colour) const;
		virtual void PlotValue(const std::string& name, int64_t value);
		virtual void PlotValue(const std::string& name, double value);
		virtual void SetThreadName(const char* name) const;
		virtual void AppInfo(std::string_view text) const;
		virtual void EmitFrameMark(const char* name) const;
	};

}
// namespace AqualinkAutomate::Interfaces
