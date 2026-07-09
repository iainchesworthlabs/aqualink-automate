#include <format>

#include "logging/logging.h"
#include "options/options_pentair.h"
#include "utility/get_terminal_column_width.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Pentair::Options
{

	boost::program_options::options_description OptionsProcessor::Options() const
	{
		boost::program_options::options_description options(SettingsType::AreaName(), Utility::get_terminal_column_width());

		LogDebug(Channel::Options, std::format("Adding {} options from the {} set", PentairOptionsCollection.size(), SettingsType::AreaName()));
		for (auto& option : PentairOptionsCollection)
		{
			options.add((*option)());
		}

		return options;
	}

	void OptionsProcessor::Validate(const boost::program_options::variables_map& /*vm*/) const
	{
		// Intentionally empty: Pentair options carry no cross-option constraints to validate.
	}

	std::expected<OptionsProcessor::SettingsType, ErrorCodes::Options_ErrorCodes> OptionsProcessor::Process(boost::program_options::variables_map& /*vm*/) const
	{
		SettingsType settings;

		return settings;
	}

}
// namespace AqualinkAutomate::Pentair::Options
