#include <array>
#include <cstddef>

#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_utility.hpp>

#include "logging/logging_attributes.h"
#include "logging/logging_severity_filter.h"

namespace AqualinkAutomate::Logging
{
	namespace SeverityFiltering
	{
		namespace
		{
			//
			// O(1), exhaustive per-channel minimum-severity table indexed by the Channel
			// enum value. Replaces the previous std::map<Channel, Severity>, which did a
			// red-black-tree lookup on every log call and could "fail open" (log everything)
			// for any channel that was missing from the map.
			//
			constexpr std::size_t CHANNEL_COUNT = magic_enum::enum_count<Channel>();

			std::array<Severity, CHANNEL_COUNT> MakeDefaultSeverityTable()
			{
				std::array<Severity, CHANNEL_COUNT> table{};
				table.fill(DEFAULT_SEVERITY);
				return table;
			}

			std::array<Severity, CHANNEL_COUNT> MinimumSeverityLevelPerChannel = MakeDefaultSeverityTable();

			[[nodiscard]] std::size_t ChannelIndex(Channel channel_id) noexcept
			{
				// magic_enum::enum_index returns std::nullopt only for a value outside the
				// declared enumerators; treat that as the default channel so an unknown
				// channel "fails closed" to DEFAULT_SEVERITY rather than logging everything.
				const auto index = magic_enum::enum_index(channel_id);
				return index.has_value() ? *index : magic_enum::enum_index(DEFAULT_CHANNEL).value_or(0U);
			}
		}
		// namespace (anonymous)

		void SetGlobalFilterLevel(Severity severity)
		{
			magic_enum::enum_for_each<Channel>([severity](auto const& channel)
				{
					SetChannelFilterLevel(channel.value, severity);
				}
			);
		}

		void SetChannelFilterLevel(Channel channel, Severity severity)
		{
			MinimumSeverityLevelPerChannel[ChannelIndex(channel)] = severity;
		}

		Severity GetChannelFilterLevel(Channel channel)
		{
			return MinimumSeverityLevelPerChannel[ChannelIndex(channel)];
		}

		bool ShouldLog(Channel channel_id, Severity severity_level)
		{
			return severity_level >= MinimumSeverityLevelPerChannel[ChannelIndex(channel_id)];
		}

		bool PerChannelTest(boost::log::value_ref<Channel, tag::channel> const& channel, boost::log::value_ref<Severity, tag::severity> const& severity)
		{
			if (!channel || !severity)
			{
				// Missing channel/severity attributes default to the configured channel filter.
				return (severity ? (*severity >= GetChannelFilterLevel(DEFAULT_CHANNEL)) : true);
			}

			return (*severity >= MinimumSeverityLevelPerChannel[ChannelIndex(*channel)]);
		}
	}
	// namespace SeverityFiltering
}
// namespace AqualinkAutomate::Logging
