#pragma once

#include <cstddef>

#include <boost/log/sinks/sink.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

//
// Owns the set of sinks installed on the Boost.Log core so they can be flushed
// and removed as a group (docs/logging-sinks-redesign.md §5.1). Boost async sink
// frontends must be stopped/flushed before destruction; the add-and-forget
// pattern the audit sink used had no such owner. Namespace-with-static-state,
// mirroring SeverityFiltering — the application is single-threaded cooperative,
// and these are touched only at init/reconfigure/shutdown on the main thread.
//
namespace AqualinkAutomate::Logging::Sinks::SinkRegistry
{
	// Add a sink to the Boost.Log core AND track it. A null sink is ignored
	// (e.g. a native sink that failed to construct returns null).
	void Add(const boost::shared_ptr<boost::log::sinks::sink>& sink);

	// Flush every tracked sink. Wired into the ordered shutdown so an async
	// frontend delivers its queued records before the process exits.
	void FlushAll();

	// Remove every tracked sink from the core and clear tracking. The app calls
	// this on shutdown; tests call it for a hermetic teardown.
	void RemoveAll();

	// Number of currently-tracked sinks (test / diagnostic aid).
	[[nodiscard]] std::size_t Count();
}
// namespace AqualinkAutomate::Logging::Sinks::SinkRegistry
