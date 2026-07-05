#include <cctype>
#include <format>
#include <string>
#include <string_view>

#include <boost/program_options/value_semantic.hpp>
#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_utility.hpp>

#include "logging/logging.h"
#include "logging/logging_severity_filter.h"
#include "logging/formatters/logging_formatters.h"
#include "options/options_developer_options.h"
#include "options/helpers/build_options_description.h"
#include "options/helpers/conflicting_options_helper.h"
#include "options/helpers/option_dependency_helper.h"
#include "options/validators/profiler_type_validator.h"
#include "options/validators/severity_level_validator.h"
#include "profiling/profiling.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Options::Developer
{

	namespace
	{
		std::string ToLowerAscii(std::string_view text)
		{
			std::string lowered;
			lowered.reserve(text.size());
			for (const char c : text)
			{
				lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
			}
			return lowered;
		}
	}

	OptionsProcessor::OptionsProcessor()
	{
		// Data-drive the per-channel log-level options by enumerating the
		// Logging::Channel enum instead of hand-writing one member per channel.
		// Each contributes a `--loglevel-<channel>` option (e.g. Channel::Main
		// -> `--loglevel-main`) taking a Severity value.
		magic_enum::enum_for_each<Logging::Channel>([this](Logging::Channel log_channel)
			{
				const std::string channel_name{ magic_enum::enum_name(log_channel) };
				auto option = make_appoption(
					std::format("loglevel-{}", ToLowerAscii(channel_name)),
					std::format("Set the logging level for Channel::{}", channel_name),
					boost::program_options::value<AqualinkAutomate::Logging::Severity>()->multitoken());

				OPTION_LOGLEVELS.emplace(log_channel, option);
			});

		DeveloperOptionsCollection =
		{
			OPTION_DEVMODE,
			OPTION_DEVREPLAYFILE,
			OPTION_DEVRECORDFILE,
			OPTION_DECODE_TO_MASTER,
			OPTION_REPLAY_FRAME_PERIOD,
			OPTION_REPLAY_SPEED,
			OPTION_PROFILER
		};

		DeveloperOptionsCollection.reserve(DeveloperOptionsCollection.size() + OPTION_LOGLEVELS.size());
		for (const auto& entry : OPTION_LOGLEVELS)
		{
			DeveloperOptionsCollection.push_back(entry.second);
		}
	}

	boost::program_options::options_description OptionsProcessor::Options() const
	{
		return BuildOptionsDescription(SettingsType::AreaName(), DeveloperOptionsCollection);
	}

	void OptionsProcessor::Validate(const boost::program_options::variables_map& vm) const
	{
		Helper_CheckForConflictingOptions(vm, OPTION_DEVREPLAYFILE, OPTION_DEVRECORDFILE);
		Helper_ValidateOptionDependencies(vm, OPTION_DEVREPLAYFILE, OPTION_DEVMODE);
		Helper_ValidateOptionDependencies(vm, OPTION_DEVREPLAYFILE, OPTION_PROFILER);

		// Every per-channel log-level option is a developer-mode dependency of
		// the replay file (mirrors the historical hand-written dependency list).
		for (const auto& entry : OPTION_LOGLEVELS)
		{
			Helper_ValidateOptionDependencies(vm, OPTION_DEVREPLAYFILE, entry.second);
		}
	}

	std::expected<OptionsProcessor::SettingsType, ErrorCodes::Options_ErrorCodes> OptionsProcessor::Process(boost::program_options::variables_map& vm) const
	{
		SettingsType settings;

		if (OPTION_DEVMODE->IsPresent(vm)) { settings.dev_mode_enabled = OPTION_DEVMODE->As<bool>(vm); }
		if (OPTION_DEVREPLAYFILE->IsPresent(vm)) { settings.replay_file = OPTION_DEVREPLAYFILE->As<std::string>(vm); }
		if (OPTION_DEVRECORDFILE->IsPresent(vm)) { settings.recording_file = OPTION_DEVRECORDFILE->As<std::string>(vm); }
		if (OPTION_DECODE_TO_MASTER->IsPresent(vm)) { settings.decode_to_master_enabled = OPTION_DECODE_TO_MASTER->As<bool>(vm); }
		if (OPTION_REPLAY_FRAME_PERIOD->IsPresent(vm)) { settings.replay_frame_period_ms = OPTION_REPLAY_FRAME_PERIOD->As<std::uint32_t>(vm); }
		if (OPTION_REPLAY_SPEED->IsPresent(vm)) { settings.replay_speed = OPTION_REPLAY_SPEED->As<double>(vm); }

		for (const auto& [log_channel, option] : OPTION_LOGLEVELS)
		{
			if (option->IsPresent(vm))
			{
				SeverityFiltering::SetChannelFilterLevel(log_channel, option->As<Severity>(vm));
			}
		}

		if (OPTION_PROFILER->IsPresent(vm)) { Factory::ProfilerFactory::Instance().SetDesired(OPTION_PROFILER->As<Types::ProfilerTypes>(vm)); }

		return settings;
	}

}
// namespace AqualinkAutomate::Options::Developer
