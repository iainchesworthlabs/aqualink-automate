#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace AqualinkAutomate::Interfaces
{

	/// @brief Runtime control surface for the active performance profiler.
	///
	/// Implemented by Profiling::ProfilingController (a thin wrapper over the
	/// ProfilerFactory) and registered in the HubLocator so a diagnostics HTTP
	/// route can report which backends are compiled in / selected and pause &
	/// resume capture at runtime — without restarting the application.
	///
	/// "Running" is distinct from "enabled": @ref Status::enabled reflects whether
	/// ANY backend was compiled in (ENABLE_PROFILING), while @ref Status::running
	/// reflects whether capture is currently resumed (Start) vs paused (Stop).
	///
	/// Pause/Resume are no-ops on backends that do not support runtime gating
	/// (Tracy is client-driven); they map to __itt_pause/resume on VTune and
	/// amdProfilePause/Resume on uProf.
	class IProfilingController
	{
	public:
		/// @brief Snapshot of the profiler's current state.
		struct Status
		{
			bool enabled{ false };                        ///< True if any backend was compiled in (ENABLE_PROFILING).
			bool running{ false };                        ///< True while capture is resumed (vs paused).
			std::string active_backend;                   ///< Selected backend name ("tracy"/"uprof"/"vtune"), empty if none/NoOp.
			std::vector<std::string> available_backends;  ///< Compiled-in backend names (lowercase).
		};

	public:
		virtual ~IProfilingController() = default;

		/// @brief Current profiler status (enabled, running, active + available backends).
		virtual Status ProfilingStatus() const = 0;

		/// @brief Resume capture on the active backend.
		/// @returns true if a backend is active and capture was resumed; false if
		///          profiling is unavailable in this build.
		virtual bool Start() = 0;

		/// @brief Pause capture on the active backend.
		/// @returns true if a backend is active and capture was paused; false if
		///          profiling is unavailable in this build.
		virtual bool Stop() = 0;

		/// @brief Select the active backend by name (case-insensitive).
		/// @returns true if @p backend names a compiled-in backend that is now
		///          selected; false if the name is unknown or that backend was not
		///          compiled into this build.
		virtual bool SelectBackend(std::string_view backend) = 0;
	};

}
// namespace AqualinkAutomate::Interfaces
