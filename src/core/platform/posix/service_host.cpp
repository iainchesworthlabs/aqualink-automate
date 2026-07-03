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

}
// namespace AqualinkAutomate::Application
