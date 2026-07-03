#include <cstddef>
#include <vector>

#include <boost/log/core.hpp>

#include "logging/sinks/sink_registry.h"

namespace AqualinkAutomate::Logging::Sinks::SinkRegistry
{

	namespace
	{
		std::vector<boost::shared_ptr<boost::log::sinks::sink>>& Tracked()
		{
			static std::vector<boost::shared_ptr<boost::log::sinks::sink>> tracked;
			return tracked;
		}
	}
	// namespace (anonymous)

	void Add(const boost::shared_ptr<boost::log::sinks::sink>& sink)
	{
		if (!sink)
		{
			return;
		}

		boost::log::core::get()->add_sink(sink);
		Tracked().push_back(sink);
	}

	void FlushAll()
	{
		for (const auto& sink : Tracked())
		{
			if (sink)
			{
				sink->flush();
			}
		}
	}

	void RemoveAll()
	{
		const auto core = boost::log::core::get();

		for (const auto& sink : Tracked())
		{
			if (sink)
			{
				// Flush first so an asynchronous frontend delivers its queued records
				// before we detach it. Clearing the vector then releases the last
				// reference, whose destructor stops the sink's feeder thread.
				sink->flush();
				core->remove_sink(sink);
			}
		}

		Tracked().clear();
	}

	std::size_t Count()
	{
		return Tracked().size();
	}

}
// namespace AqualinkAutomate::Logging::Sinks::SinkRegistry
