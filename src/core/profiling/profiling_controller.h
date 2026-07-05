#pragma once

#include <string_view>

#include "interfaces/iprofilingcontroller.h"

namespace AqualinkAutomate::Profiling
{

	/// @brief Runtime profiler control, implemented over the ProfilerFactory.
	///
	/// Reports the compiled-in / selected backends and gates capture at runtime
	/// (Start = Resume, Stop = Pause on the active backend). A single owning
	/// instance is registered in the HubLocator by main(); it is always present
	/// (even when ENABLE_PROFILING is OFF), reporting enabled=false in that case.
	class ProfilingController : public Interfaces::IProfilingController
	{
	public:
		ProfilingController();
		~ProfilingController() override = default;

		Status ProfilingStatus() const override;
		bool Start() override;
		bool Stop() override;
		bool SelectBackend(std::string_view backend) override;

	private:
		// Tracks resumed-vs-paused. Capture is resumed at startup, so this is
		// initialised from the factory's selected/registered state in the ctor.
		bool m_Running{ false };
	};

}
// namespace AqualinkAutomate::Profiling
