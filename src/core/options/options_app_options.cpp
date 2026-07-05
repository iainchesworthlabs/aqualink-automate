#include <format>
#include <iostream>
#include <string>

#include "application/log_source_registration.h"
#include "application/service_host.h"
#include "exceptions/exception_optionshelporversion.h"
#include "logging/logging.h"
#include "logging/logging_severity_filter.h"
#include "options/options_app_options.h"
#include "options/options_option_type.h"
#include "options/helpers/build_options_description.h"
#include "options/helpers/conflicting_options_helper.h"
#include "version/version.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Options::App
{

	boost::program_options::options_description OptionsProcessor::Options() const
	{
		return BuildOptionsDescription(SettingsType::AreaName(), AppOptionsCollection);
	}

	void OptionsProcessor::Validate(const boost::program_options::variables_map& vm) const
	{
		Helper_CheckForConflictingOptions(vm, OPTION_DEBUG, OPTION_TRACE);
	}

	std::expected<OptionsProcessor::SettingsType, ErrorCodes::Options_ErrorCodes> OptionsProcessor::Process(boost::program_options::variables_map& vm) const
	{
		SettingsType settings;

		if (OPTION_CONFIG->IsPresent(vm))
		{
			settings.config_file = OPTION_CONFIG->As<std::string>(vm);
		}

		if (OPTION_DEBUG->IsPresent(vm))
		{
			LogTrace(Channel::Options, "Setting global logging severity level filter to Debug");
			Logging::SeverityFiltering::SetGlobalFilterLevel(Severity::Debug);
		}
		else if (OPTION_TRACE->IsPresent(vm))
		{
			LogTrace(Channel::Options, "Setting global logging severity level filter to Trace");
			Logging::SeverityFiltering::SetGlobalFilterLevel(Severity::Trace);
		}
		else
		{
			// Do nothing...
		}

		return settings;
	}

	void HandleHelp(boost::program_options::variables_map& vm, boost::program_options::options_description& options)
	{
		// Query the variables_map directly by the declared option long name
		// rather than reconstructing a throwaway AppOption from string literals.
		if (0 < vm.count("help"))
		{
			// Display the help information to the user.
			std::cout << options << '\n';

			// Terminate the application...
			throw Exceptions::OptionsHelpOrVersion();
		}
		else
		{
			LogTrace(Channel::Options, "Help option not provided; doing nothing...");
		}
	}

	void HandleVersion(boost::program_options::variables_map& vm)
	{
		if (0 < vm.count("version-detail"))
		{
			// Display the version information to the user.
			std::cout << Version::VersionDetails() << '\n' << Version::GitCommitDetails() << '\n';

			// Terminate the application...
			throw Exceptions::OptionsHelpOrVersion();
		}
		else if (0 < vm.count("version"))
		{
			const auto version_info = std::format
			(
				"{} v{}\n{}",
				Version::VersionInfo::ProjectName(),
				Version::VersionInfo::ProjectVersionFull(),
				Version::VersionInfo::ProjectDescription()
			);

			// Display the version information to the user.
			std::cout << version_info << '\n';

			// Terminate the application...
			throw Exceptions::OptionsHelpOrVersion();
		}
		else
		{
			LogTrace(Channel::Options, "Version option not provided; doing nothing...");
		}
	}

	void HandleLogSourceRegistration(boost::program_options::variables_map& vm)
	{
		const bool do_register = (0 < vm.count("register-log-source"));

		if (const bool do_unregister = (0 < vm.count("unregister-log-source")); !do_register && !do_unregister)
		{
			return;
		}

		// The runtime Event Log sink and this registration must name the same source.
		static const std::string source_name{ "Aqualink-Automate" };

		// The action is dispatched to the platform layer (Windows writes/removes the
		// Event Log source key; every other platform returns Unsupported). Branching on
		// the result keeps this shared handler free of an OS #ifdef — see
		// docs/platform-isolation.md.
		switch (const auto result = do_register
			? Application::RegisterLogSource(source_name)
			: Application::UnregisterLogSource(source_name))
		{
			using enum Application::LogSourceRegistrationResult;

		case Succeeded:
			std::cout << (do_register ? "Registered" : "Unregistered") << " Windows Event Log source '" << source_name << "'.\n";
			break;

		case Failed:
			std::cout << "Failed to " << (do_register ? "register" : "unregister")
				<< " Windows Event Log source '" << source_name << "' (administrator privileges are required).\n";
			break;

		case Unsupported:
			std::cout << "--register-log-source / --unregister-log-source are only supported on Windows.\n";
			break;
		}

		// One-shot action: exit cleanly (same mechanism as --help/--version).
		throw Exceptions::OptionsHelpOrVersion();
	}

	void HandleServiceInstallation(boost::program_options::variables_map& vm)
	{
		const bool do_install = (0 < vm.count("install-service"));
		const bool do_uninstall = (0 < vm.count("uninstall-service"));

		if (!do_install && !do_uninstall)
		{
			return;
		}

		if (do_install && do_uninstall)
		{
			std::cout << "Specify only one of --install-service / --uninstall-service.\n";
			throw Exceptions::OptionsHelpOrVersion();
		}

		// Dispatched to the platform layer (Windows talks to the SCM; every other
		// platform returns Unsupported), so this shared handler needs no OS #ifdef
		// — see docs/platform-isolation.md.
		switch (const auto result = do_install
			? Application::InstallService()
			: Application::UninstallService())
		{
			using enum Application::ServiceActionResult;

		case Succeeded:
		case Failed:
			// The Install/Uninstall actions print their own detailed result (binary path,
			// account, Event Log source, error reason) to stdout.
			break;

		case Unsupported:
			std::cout << "--install-service / --uninstall-service are only supported on Windows.\n";
			break;
		}

		// One-shot action: exit cleanly (same mechanism as --help/--version).
		throw Exceptions::OptionsHelpOrVersion();
	}

	void HandleHelpAndVersion(boost::program_options::variables_map& vm, boost::program_options::options_description& options)
	{
		HandleHelp(vm, options);
		HandleVersion(vm);
		HandleLogSourceRegistration(vm);
		HandleServiceInstallation(vm);
	}

}
// namespace AqualinkAutomate::Options::App
