#pragma once

#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Preferences
{

	/// User/admin preferences persistence settings.
	struct tagPreferencesSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Preferences" };
			return AREA_NAME;
		}

		tagPreferencesSettings() = default;

		/// JSON file persisting user/admin preferences. EMPTY (the default) keeps
		/// preferences in-memory only (still editable for the session, reset on
		/// restart); the API works either way.
		std::string preferences_file;
	};
	using PreferencesSettings = tagPreferencesSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_PREFERENCES_FILE{ make_appoption("preferences-file", "JSON file persisting user/admin preferences (empty keeps them in-memory only)", boost::program_options::value<std::string>()) };

		const std::vector<AppOptionPtr> PreferencesOptionsCollection
		{
			OPTION_PREFERENCES_FILE
		};

	public:
		using SettingsType = PreferencesSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Preferences
