#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/expressions/filter.hpp>

#include "logging/logging_attributes.h"
#include "logging/logging_channels.h"
#include "logging/logging_severity_filter.h"
#include "logging/logging_severity_levels.h"
#include "logging/sinks/sink_filters.h"

namespace AqualinkAutomate::Logging::Sinks
{

	boost::log::filter MakeOperationalFilter()
	{
		// Written as one lambda over the record's attributes (rather than composed
		// Boost.Log expression templates) so the audit exclusion is unmistakable and
		// cannot be accidentally reordered away.
		return [](boost::log::attribute_value_set const& attrs)
		{
			// Audit records never reach an operational sink.
			if (const auto audit = attrs[is_audit]; audit && audit.get())
			{
				return false;
			}

			// Per-channel severity gate; PerChannelTest handles absent attributes.
			return SeverityFiltering::PerChannelTest(attrs[channel], attrs[severity]);
		};
	}

	boost::log::filter MakeAuditFilter()
	{
		return [](boost::log::attribute_value_set const& attrs)
		{
			const auto audit = attrs[is_audit];
			return audit && audit.get();
		};
	}

}
// namespace AqualinkAutomate::Logging::Sinks
