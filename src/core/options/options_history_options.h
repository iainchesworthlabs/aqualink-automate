#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::History
{

	/// Time-series history / persistence settings (WS2).
	typedef struct tagHistorySettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "History" };
			return AREA_NAME;
		}

		tagHistorySettings() = default;

		/// SQLite database path. EMPTY (the default) disables history entirely:
		/// the service is not constructed and the read API returns 503.
		std::string db_path;

		/// Days of samples to retain; a daily purge deletes anything older.
		std::uint32_t retention_days{ 90 };

		/// Buffered-sample flush interval (seconds).
		std::uint32_t flush_seconds{ 10 };
	}
	HistorySettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_DB{ make_appoption("history-db", "SQLite database path for time-series history (empty disables history)", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_RETENTION_DAYS{ make_appoption("history-retention-days", "Days of history to retain before purging", boost::program_options::value<std::uint32_t>()->default_value(90)) };
		AppOptionPtr OPTION_FLUSH_SECONDS{ make_appoption("history-flush-seconds", "Interval (seconds) at which buffered samples are flushed to disk", boost::program_options::value<std::uint32_t>()->default_value(10)) };

		const std::vector<AppOptionPtr> HistoryOptionsCollection
		{
			OPTION_DB,
			OPTION_RETENTION_DAYS,
			OPTION_FLUSH_SECONDS
		};

	public:
		using SettingsType = HistorySettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::History
