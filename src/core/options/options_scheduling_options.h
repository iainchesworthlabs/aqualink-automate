#pragma once

#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Scheduling
{

	/// Scheduler settings (WS4).
	struct tagSchedulingSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Scheduling" };
			return AREA_NAME;
		}

		tagSchedulingSettings() = default;

		/// JSON file persisting the schedules. EMPTY (the default) disables the
		/// scheduler entirely; the CRUD routes then return 503.
		std::string schedules_file;
	};
	using SchedulingSettings = tagSchedulingSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_SCHEDULES_FILE{ make_appoption("schedules-file", "JSON file persisting schedules (empty disables the scheduler)", boost::program_options::value<std::string>()) };

		const std::vector<AppOptionPtr> SchedulingOptionsCollection
		{
			OPTION_SCHEDULES_FILE
		};

	public:
		using SettingsType = SchedulingSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Scheduling
