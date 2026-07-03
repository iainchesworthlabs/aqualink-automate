#pragma once

#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::App
{

	typedef struct tagAppSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Application" };
			return AREA_NAME;
		}

		tagAppSettings()
		{
		}

		std::string config_file;
	}
	AppSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_CONFIG{ make_appoption("config", "c", "Path to configuration file", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_DEBUG{ make_appoption("debug", "d", "Enable debug logging") };
		AppOptionPtr OPTION_HELP{ make_appoption("help", "h", "Displays the help information") };
		AppOptionPtr OPTION_TRACE{ make_appoption("trace", "Enable trace logging") };
		AppOptionPtr OPTION_VERSION{ make_appoption("version", "v", "Displays the version information") };
		AppOptionPtr OPTION_VERSIONDETAILS{ make_appoption("version-detail", "Displays detailed version information (including git commit)") };
		AppOptionPtr OPTION_REGISTERLOGSOURCE{ make_appoption("register-log-source", "Register the Windows Event Log source (one-time, requires elevation), then exit. Windows only") };
		AppOptionPtr OPTION_UNREGISTERLOGSOURCE{ make_appoption("unregister-log-source", "Remove the Windows Event Log source registration, then exit. Windows only") };

		const std::vector<AppOptionPtr> AppOptionsCollection
		{
			OPTION_CONFIG,
			OPTION_DEBUG,
			OPTION_HELP,
			OPTION_TRACE,
			OPTION_VERSION,
			OPTION_VERSIONDETAILS,
			OPTION_REGISTERLOGSOURCE,
			OPTION_UNREGISTERLOGSOURCE
		};

	public:
		using SettingsType = AppSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

	public:
		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

	void HandleHelp(boost::program_options::variables_map& vm, boost::program_options::options_description& options);
	void HandleVersion(boost::program_options::variables_map& vm);
	void HandleHelpAndVersion(boost::program_options::variables_map& vm, boost::program_options::options_description& options);
}
// namespace AqualinkAutomate::Options::App
