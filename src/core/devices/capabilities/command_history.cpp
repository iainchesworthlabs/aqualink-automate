#include "devices/capabilities/command_history.h"

#include <utility>

#include <nlohmann/json.hpp>

namespace AqualinkAutomate::Devices::Capabilities
{

	void CommandHistory::RecordCommand(std::string description, std::string outcome)
	{
		m_Entries.emplace_back(std::chrono::system_clock::now(), std::move(description), std::move(outcome));

		while (m_Entries.size() > MAX_ENTRIES)
		{
			m_Entries.pop_front();
		}
	}

	nlohmann::json CommandHistory::DescribeRecentCommands() const
	{
		const auto now = std::chrono::system_clock::now();

		nlohmann::json commands = nlohmann::json::array();

		for (const auto& entry : m_Entries)
		{
			const auto ts_seconds = std::chrono::duration_cast<std::chrono::seconds>(entry.timestamp.time_since_epoch()).count();
			const auto age_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - entry.timestamp).count();

			commands.push_back(nlohmann::json{
				{ "ts", ts_seconds },
				{ "age_seconds", (age_seconds < 0) ? 0 : age_seconds },
				{ "description", entry.description },
				{ "outcome", entry.outcome },
			});
		}

		return commands;
	}

}
// namespace AqualinkAutomate::Devices::Capabilities
