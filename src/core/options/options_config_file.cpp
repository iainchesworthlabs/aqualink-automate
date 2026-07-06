#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/program_options/errors.hpp>
#include <boost/program_options/parsers.hpp>

#include "logging/logging.h"
#include "options/options_config_file.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Options
{

	namespace
	{
		/// Trim leading and trailing whitespace from a string.
		std::string Trim(const std::string& s)
		{
			auto start = s.find_first_not_of(" \t\r\n");
			if (start == std::string::npos) return {};
			auto end = s.find_last_not_of(" \t\r\n");
			return s.substr(start, end - start + 1);
		}

		/// Build the set of option long-names that are zero-token (bool_switch).
		std::unordered_set<std::string> BuildZeroTokenSet(
			const boost::program_options::options_description& desc)
		{
			std::unordered_set<std::string> zero_token_options;
			for (const auto& opt : desc.options())
			{
				if (opt->semantic()->max_tokens() == 0)
				{
					zero_token_options.insert(opt->long_name());
				}
			}
			return zero_token_options;
		}

		/// Check if a string represents a truthy value.
		bool IsTruthy(const std::string& value)
		{
			std::string lower = value;
			std::ranges::transform(lower, lower.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return lower == "true" || lower == "yes" || lower == "on" || lower == "1";
		}

		/// Parse an INI-style config file into key-value pairs.
		/// Returns the pairs and logs warnings for malformed lines.
		std::vector<std::pair<std::string, std::string>> ReadConfigFile(
			const std::filesystem::path& path)
		{
			std::vector<std::pair<std::string, std::string>> entries;
			std::ifstream file(path);
			std::string line;
			int line_number = 0;

			while (std::getline(file, line))
			{
				++line_number;
				auto trimmed = Trim(line);

				// Skip blank lines and comments
				if (trimmed.empty() || trimmed[0] == '#')
				{
					continue;
				}

				auto eq_pos = trimmed.find('=');
				if (eq_pos == std::string::npos)
				{
					LogWarning(Channel::Options, std::format(
						"Config file line {}: no '=' found, skipping: {}", line_number, trimmed));
					continue;
				}

				auto key = Trim(trimmed.substr(0, eq_pos));
				auto value = Trim(trimmed.substr(eq_pos + 1));

				if (key.empty())
				{
					LogWarning(Channel::Options, std::format(
						"Config file line {}: empty key, skipping", line_number));
					continue;
				}

				entries.emplace_back(std::move(key), std::move(value));
			}

			return entries;
		}

		/// Convert config file entries to a synthetic argv for boost parsing.
		/// Zero-token (bool_switch) options are handled specially:
		///   truthy value -> "--key" flag
		///   falsy value  -> omitted entirely
		/// Normal options -> "--key" "value"
		std::vector<std::string> BuildSyntheticArgv(
			const std::vector<std::pair<std::string, std::string>>& entries,
			const std::unordered_set<std::string>& zero_token_options)
		{
			std::vector<std::string> argv;
			argv.emplace_back("config-file"); // program name placeholder

			for (const auto& [key, value] : entries)
			{
				if (zero_token_options.contains(key))
				{
					if (IsTruthy(value))
					{
						argv.push_back("--" + key);
					}
					// Falsy values: omit entirely (flag not set)
				}
				else
				{
					argv.push_back("--" + key);
					argv.push_back(value);
				}
			}

			return argv;
		}
	}
	// anonymous namespace

	[[nodiscard]] Transform ParseConfigFile()
	{
		return [](State state) -> Result
			{
				auto& [desc, vm, settings, validators] = state;

				try
				{
					// If --config was not provided on the CLI, just notify and return.
					if (!vm.count("config"))
					{
						boost::program_options::notify(vm);
						return state;
					}

					auto config_path = std::filesystem::path(vm["config"].as<std::string>());

					if (!std::filesystem::exists(config_path))
					{
						LogError(Channel::Options, std::format(
							"Configuration file not found: {}", config_path.string()));
						return std::unexpected(Error::OptionsParsingFailed);
					}

					LogInfo(Channel::Options, std::format(
						"Loading configuration from: {}", config_path.string()));

					// Read INI file
					auto entries = ReadConfigFile(config_path);

					// Build zero-token set from registered options
					auto zero_token_set = BuildZeroTokenSet(*desc);

					// Convert to synthetic argv
					auto synthetic_argv = BuildSyntheticArgv(entries, zero_token_set);

					// Build raw argv pointers
					std::vector<const char*> raw_argv;
					raw_argv.reserve(synthetic_argv.size());
					for (const auto& arg : synthetic_argv)
					{
						raw_argv.push_back(arg.c_str());
					}

					// Parse with allow_unregistered to catch unknown keys
					auto parsed = boost::program_options::command_line_parser(
						static_cast<int>(raw_argv.size()),
						const_cast<char**>(raw_argv.data()))
						.options(*desc)
						.allow_unregistered()
						.run();

					// Log warnings for unrecognized options
					auto unrecognized = boost::program_options::collect_unrecognized(
						parsed.options, boost::program_options::include_positional);
					for (const auto& unknown : unrecognized)
					{
						LogWarning(Channel::Options, std::format(
							"Config file: unrecognized option '{}'", unknown));
					}

					// Store into vm (first-write-wins: CLI values already present take precedence)
					boost::program_options::store(parsed, vm);
					boost::program_options::notify(vm);

					return state;
				}
				catch (const boost::program_options::error& e)
				{
					// boost::program_options::error is the common base of every
					// parser/store/notify failure (unknown option, invalid value,
					// ambiguous option, multiple occurrences, ...).
					LogError(Channel::Options, std::format(
						"Failed to parse configuration file: {}", e.what()));
					return std::unexpected(Error::OptionsParsingFailed);
				}
				catch (const std::filesystem::filesystem_error& e)
				{
					// path construction / exists() query on the config path.
					LogError(Channel::Options, std::format(
						"Failed to parse configuration file: {}", e.what()));
					return std::unexpected(Error::OptionsParsingFailed);
				}
			};
	}

	[[nodiscard]] Transform Notify()
	{
		return [](State state) -> Result
			{
				auto& [desc, vm, settings, validators] = state;
				boost::program_options::notify(vm);
				return state;
			};
	}

}
// namespace AqualinkAutomate::Options
