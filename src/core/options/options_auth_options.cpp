#include <format>

#include <boost/program_options/value_semantic.hpp>

#include "exceptions/exception_optionparsingfailed.h"
#include "logging/logging.h"
#include "options/options_auth_options.h"
#include "utility/get_terminal_column_width.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Options::Auth
{

	boost::program_options::options_description OptionsProcessor::Options() const
	{
		boost::program_options::options_description options(SettingsType::AreaName(), Utility::get_terminal_column_width());

		LogDebug(Channel::Options, std::format("Adding {} options from the {} set", AuthOptionsCollection.size(), SettingsType::AreaName()));
		for (auto& option : AuthOptionsCollection)
		{
			options.add((*option)());
		}

		return options;
	}

	void OptionsProcessor::Validate(const boost::program_options::variables_map& /*vm*/) const
	{
		// Value-level checks need mutable variables_map access (AppOption::As)
		// and therefore live in Process() below; there is nothing to
		// cross-validate between these options.
	}

	std::expected<OptionsProcessor::SettingsType, ErrorCodes::Options_ErrorCodes> OptionsProcessor::Process(boost::program_options::variables_map& vm) const
	{
		SettingsType settings;

		if (OPTION_AUTH_MODE->IsPresent(vm))
		{
			const auto mode = OPTION_AUTH_MODE->As<std::string>(vm);

			if (("disabled" != mode) && ("enabled" != mode))
			{
				throw Exceptions::OptionParsingFailed(std::format("Invalid --auth-mode value '{}': expected 'disabled' or 'enabled'", mode));
			}

			settings.auth_mode_enabled = ("enabled" == mode);
		}

		if (OPTION_JWT_ACCESS_TTL->IsPresent(vm) && (0 == OPTION_JWT_ACCESS_TTL->As<std::uint32_t>(vm)))
		{
			throw Exceptions::OptionParsingFailed("Invalid --jwt-access-ttl value: must be at least 1 minute");
		}

		if (OPTION_AUTH_STATE_DIR->IsPresent(vm))
		{
			settings.auth_state_dir = OPTION_AUTH_STATE_DIR->As<std::string>(vm);
		}

		if (OPTION_JWT_ACCESS_TTL->IsPresent(vm))
		{
			settings.jwt_access_ttl_minutes = OPTION_JWT_ACCESS_TTL->As<std::uint32_t>(vm);
		}

		if (OPTION_BOOTSTRAP_ADMIN->IsPresent(vm))
		{
			settings.bootstrap_admin_username = OPTION_BOOTSTRAP_ADMIN->As<std::string>(vm);
		}

		if (OPTION_BOOTSTRAP_PASSWORD_FILE->IsPresent(vm))
		{
			settings.bootstrap_admin_password_file = OPTION_BOOTSTRAP_PASSWORD_FILE->As<std::string>(vm);
		}

		return settings;
	}

}
// namespace AqualinkAutomate::Options::Auth
