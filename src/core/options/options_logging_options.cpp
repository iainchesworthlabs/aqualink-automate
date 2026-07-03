#include <cctype>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "options/options_logging_options.h"
#include "options/helpers/build_options_description.h"

using namespace AqualinkAutomate;

namespace
{
	// Lower-cased, whitespace-trimmed copy of a token.
	std::string NormaliseToken(std::string_view token)
	{
		std::size_t begin = 0;
		std::size_t end = token.size();
		while (begin < end && std::isspace(static_cast<unsigned char>(token[begin]))) { ++begin; }
		while (end > begin && std::isspace(static_cast<unsigned char>(token[end - 1]))) { --end; }

		std::string out;
		out.reserve(end - begin);
		for (std::size_t i = begin; i < end; ++i)
		{
			out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(token[i]))));
		}
		return out;
	}
}

namespace AqualinkAutomate::Options::LogSinks
{

	boost::program_options::options_description OptionsProcessor::Options() const
	{
		return BuildOptionsDescription(SettingsType::AreaName(), LoggingOptionsCollection);
	}

	void OptionsProcessor::Validate(const boost::program_options::variables_map& /* vm */) const
	{
		// No cross-option conflicts in this area; --log-sinks token validity is
		// enforced in Process (an invalid token yields OptionsValidationFailed).
	}

	std::expected<OptionsProcessor::SettingsType, ErrorCodes::Options_ErrorCodes> OptionsProcessor::Process(boost::program_options::variables_map& vm) const
	{
		SettingsType settings;

		// Facility carries a default, so it is always present.
		if (OPTION_LOGFACILITY->IsPresent(vm))
		{
			settings.Facility = OPTION_LOGFACILITY->As<AqualinkAutomate::Logging::Sinks::SyslogFacility>(vm);
		}

		// Format carries a default (text). text | json.
		if (OPTION_LOGFORMAT->IsPresent(vm))
		{
			const auto format = NormaliseToken(OPTION_LOGFORMAT->As<std::string>(vm));
			if (format == "text")
			{
				settings.Format = AqualinkAutomate::Logging::LogFormat::Text;
			}
			else if (format == "json")
			{
				settings.Format = AqualinkAutomate::Logging::LogFormat::Json;
			}
			else
			{
				return std::unexpected(ErrorCodes::Options_ErrorCodes::OptionsValidationFailed);
			}
		}

		// File sink target + rotation bounds (max-size / max-files carry defaults).
		if (OPTION_LOGFILE->IsPresent(vm))
		{
			settings.LogFile = std::filesystem::path(OPTION_LOGFILE->As<std::string>(vm));
		}
		if (OPTION_LOGFILEMAXSIZE->IsPresent(vm))
		{
			settings.LogFileMaxBytes = OPTION_LOGFILEMAXSIZE->As<std::uintmax_t>(vm);
		}
		if (OPTION_LOGFILEMAXFILES->IsPresent(vm))
		{
			settings.LogFileMaxFiles = static_cast<std::size_t>(OPTION_LOGFILEMAXFILES->As<std::uint32_t>(vm));
		}

		if (OPTION_LOGSINKS->IsPresent(vm))
		{
			const auto spec = NormaliseToken(OPTION_LOGSINKS->As<std::string>(vm));

			if (spec == "auto")
			{
				settings.Sinks = SinkMode::Auto;
			}
			else
			{
				settings.Sinks = SinkMode::Explicit;

				std::size_t start = 0;
				while (start <= spec.size())
				{
					const auto comma = spec.find(',', start);
					const auto piece = NormaliseToken(std::string_view{ spec }.substr(start, (comma == std::string::npos ? spec.size() : comma) - start));

					if (piece == "console")
					{
						settings.Console = true;
					}
					else if (piece == "native")
					{
						settings.Native = true;
					}
					else if (piece == "file")
					{
						settings.File = true;
					}
					else
					{
						// Empty or unknown token — fail validation with a clear error.
						return std::unexpected(ErrorCodes::Options_ErrorCodes::OptionsValidationFailed);
					}

					if (comma == std::string::npos)
					{
						break;
					}
					start = comma + 1;
				}

				// An explicit set that selects nothing is not meaningful.
				if (!settings.Console && !settings.Native && !settings.File)
				{
					return std::unexpected(ErrorCodes::Options_ErrorCodes::OptionsValidationFailed);
				}
			}
		}

		// An explicitly-selected file sink needs a target path.
		if (SinkMode::Explicit == settings.Sinks && settings.File && !settings.LogFile.has_value())
		{
			return std::unexpected(ErrorCodes::Options_ErrorCodes::OptionsValidationFailed);
		}

		return settings;
	}

}
// namespace AqualinkAutomate::Options::LogSinks
