#include "application/service_host.h"

//
// POSIX has no equivalent of the Windows Service Control Manager: under systemd the
// process runs as a plain foreground service (Type=simple) and shutdown arrives as
// SIGTERM, which the application already handles via its boost::asio::signal_set. So
// RunHosted is a straight passthrough -- run the body with empty hooks, exactly as a
// console process. OS-specific service integration (if any) belongs in the packaging
// unit file, not here.
//

namespace AqualinkAutomate::Application
{

	int RunHosted(int argc, char** argv, const AppEntry& entry)
	{
		return entry(argc, argv, AppHostHooks{});
	}

	// No OS service manager to install into on POSIX; report Unsupported so the CLI
	// handler prints the "only supported on Windows" notice without an OS #ifdef.
	// See docs/platform-isolation.md.

	ServiceActionResult InstallService()
	{
		return ServiceActionResult::Unsupported;
	}

	ServiceActionResult UninstallService()
	{
		return ServiceActionResult::Unsupported;
	}

}
// namespace AqualinkAutomate::Application
