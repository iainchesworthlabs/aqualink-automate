#include <boost/smart_ptr/shared_ptr.hpp>

#include "logging/sinks/sink_journald.h"

//
// journald is Linux-only. This macOS stub lets the shared code call
// IsJournaldAvailable()/MakeJournaldSink() without a platform #ifdef; the real
// implementation is platform/linux/journald_log_sink.cpp. (macOS's native path is
// os_log, wired via MakeNativeSink.)
//

namespace AqualinkAutomate::Logging::Sinks
{

	bool IsJournaldAvailable()
	{
		return false;
	}

	boost::shared_ptr<boost::log::sinks::sink> MakeJournaldSink(const JournaldSinkConfig& /* config */)
	{
		return {};
	}

}
// namespace AqualinkAutomate::Logging::Sinks
