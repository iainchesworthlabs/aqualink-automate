#pragma once

#include <functional>

namespace AqualinkAutomate::Application
{

	//
	// OS-neutral hooks the application body (RunApplication) uses to talk to whatever
	// service manager is hosting it. Every field is optional: in console mode (and on
	// POSIX) they are all empty, so each call site is guarded and behaviour is identical
	// to running as a plain process. Only the Windows service host populates them.
	//
	struct AppHostHooks
	{
		// True when the process was launched by an OS service manager (the Windows SCM).
		// Flows into the logging environment so the `auto` sink policy selects the
		// Event Log sink (docs/logging-sinks-redesign.md §6.2).
		bool RunningAsManagedService = false;

		// Report that startup is complete and the app is serving (SCM RUNNING).
		std::function<void()> OnRunning;

		// Report that an ordered shutdown has begun (SCM STOP_PENDING).
		std::function<void()> OnStopPending;

		// Publish a thread-safe "request stop" callable to the host. The host's control
		// handler (which runs on a separate thread) invokes it to trigger the SAME
		// ordered shutdown as a console Ctrl-C. Passing nullptr clears it (the app MUST
		// clear it before the captured io_context is destroyed).
		std::function<void(std::function<void()> request_stop)> PublishStopRequester;
	};

	//
	// The application body: the console/service-agnostic startup -> run -> shutdown
	// sequence. Defined in the executable translation unit (aqualink-automate.cpp) and
	// handed to RunHosted so the OS service host can drive it.
	//
	using AppEntry = std::function<int(int, char**, const AppHostHooks&)>;

	//
	// Run `entry`, wrapping it in the platform's service host where one exists. On
	// Windows this attempts Service Control Manager dispatch and, when the process was
	// NOT launched by the SCM, falls through to a direct console run. On POSIX it simply
	// calls `entry` with empty hooks. Returns the application exit code. Implemented
	// per-OS (platform/windows/windows_service.cpp, platform/posix/service_host.cpp).
	//
	int RunHosted(int argc, char** argv, const AppEntry& entry);

	//
	// Outcome of an install/uninstall action. `Unsupported` is returned on every
	// platform without an OS service manager to install into (i.e. everything but
	// Windows), so the CLI handler branches on this result instead of an OS #ifdef
	// (see docs/platform-isolation.md).
	//
	enum class ServiceActionResult
	{
		Succeeded,    // the service was installed / removed
		Failed,       // supported here, but the action failed (e.g. not elevated)
		Unsupported,  // this platform has no OS service manager to install into
	};

	//
	// One-shot, elevated CLI actions that register/remove the Windows service (and its
	// Event Log source). The Windows implementation lives in
	// platform/windows/windows_service.cpp; every other platform gets the Unsupported
	// stub in platform/posix/service_host.cpp (CMake selects which compiles), so callers
	// need no OS #ifdef. InstallService derives the service binary path from the current
	// process command line (so --config and any other flags supplied alongside
	// --install-service are baked in).
	//
	ServiceActionResult InstallService();
	ServiceActionResult UninstallService();

}
// namespace AqualinkAutomate::Application
