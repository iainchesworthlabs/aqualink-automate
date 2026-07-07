#pragma once

#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

#include "concepts/is_option_processor.h"
#include "errors/options_errors.h"
#include "exceptions/exception_optionparsingfailed.h"
#include "logging/logging.h"
#include "options/options_app_options.h"
#include "options/options_settings.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Options
{
	using Error = ErrorCodes::Options_ErrorCodes;
	using Validator = std::function<void(const boost::program_options::variables_map&)>;

	using State = std::tuple<
		std::shared_ptr<boost::program_options::options_description>,  // Options
		boost::program_options::variables_map,        // Parsed values
		Settings,									  // Results
		std::vector<Validator>						  // Validators
	>;

	using Result = std::expected<State, Error>;
	using Transform = std::function<Result(State)>;

	[[nodiscard]] inline auto Initialise() -> Result
	{
		return State
		{
			std::make_shared<boost::program_options::options_description>(),
			boost::program_options::variables_map{},
			Settings{},
			std::vector<Validator>{}
		};
	}

	template<Concepts::IsOptionProcessor PROCESSOR>
	[[nodiscard]] auto Add(PROCESSOR processor) -> Transform
	{
		return [p = std::move(processor)](State state) -> Result
			{
				auto& [desc, vm, settings, validators] = state;

				desc->add(p.Options());

				validators.push_back([p](const boost::program_options::variables_map& vm)
					{
						p.Validate(vm);
					});

				return state;
			};
	}

	[[nodiscard]] inline auto Parse(int argc, char* argv[]) -> Transform
	{
		return [argc, argv](State state) -> Result
			{
				auto& [desc, vm, settings, validators] = state;

				try
				{
					auto parsed = boost::program_options::command_line_parser(argc, argv)
						.options(*desc)
						.run();

					if (auto unrecognized = boost::program_options::collect_unrecognized(parsed.options, boost::program_options::include_positional); !unrecognized.empty())
					{
						std::string unknown_args;
						for (const auto& arg : unrecognized)
						{
							if (!unknown_args.empty()) unknown_args += ", ";
							unknown_args += std::format("'{}'", arg);
						}
						LogError(Channel::Options, std::format("Unrecognized arguments on command line: {}", unknown_args));
						return std::unexpected(Error::OptionsParsingFailed);
					}

					boost::program_options::store(parsed, vm);
					return state;
				}
				catch (const std::exception& e)
				{
					LogError(Channel::Options, std::format("Failed to parse command line options: {}", e.what()));
					return std::unexpected(Error::OptionsParsingFailed);
				}
				catch (...)
				{
					LogError(Channel::Options, "Failed to parse command line options: unknown error");
					return std::unexpected(Error::OptionsParsingFailed);
				}
			};
	}

	[[nodiscard]] inline auto Validate() -> Transform
	{
		return [](State state) -> Result
			{
				auto& [desc, vm, settings, validators] = state;

				try
				{
					for (const auto& validator : validators)
					{
						validator(vm);
					}

					return state;
				}
				catch (const std::exception& e)
				{
					using namespace AqualinkAutomate::Logging;
					LogError(Channel::Options, std::format("Validation failed: {}", e.what()));
					return std::unexpected(Error::OptionsValidationFailed);
				}
				catch (...)
				{
					using namespace AqualinkAutomate::Logging;
					LogError(Channel::Options, "Validation failed with unknown exception");
					return std::unexpected(Error::OptionsValidationFailed);
				}
			};
	}

	[[nodiscard]] inline auto CheckHelpAndVersion() -> Transform
	{
		return [](State state) -> Result
			{
				auto& [desc, vm, settings, validators] = state;

				// Throws OptionsHelpOrVersion if --help or --version was provided.
				App::HandleHelpAndVersion(vm, *desc);

				return state;
			};
	}

	// Runs a single processor's Process() against the parsed values, storing the
	// resulting settings area on success. Records the first error into
	// 'first_error' and skips any subsequent processor once an error is set, so
	// the fold short-circuits with the first failure.
	template<typename PROCESSOR>
	inline void Process_RunOne(
		PROCESSOR& p,
		boost::program_options::variables_map& vm,
		Settings& settings,
		std::optional<Error>& first_error)
	{
		if (first_error.has_value())
		{
			return;
		}

		auto result = p.Process(vm);
		if (result)
		{
			settings.Set(p.Name(), std::any{ std::move(*result) });
		}
		else
		{
			LogError(Channel::Options, std::format("Processing options for the '{}' area failed", p.Name()));
			first_error = result.error();
		}
	}

	template<Concepts::IsOptionProcessor... Processors>
	[[nodiscard]] auto Process(Processors... processors) -> Transform
	{
		return [... ps = std::move(processors)](State state) -> Result
			{
				auto& [desc, vm, settings, validators] = state;

				// Short-circuit the fold: the first processor whose Process()
				// reports an error aborts the whole pipeline with that error
				// (previously the error was silently swallowed and the missing
				// settings area only surfaced as a downstream Get() failure).
				std::optional<Error> first_error;

				(..., Process_RunOne(ps, vm, settings, first_error));

				if (first_error.has_value())
				{
					return std::unexpected(*first_error);
				}

				return state;
			};
	}

	[[nodiscard]] inline auto Finalise() -> std::function<std::expected<Settings, Error>(State)>
	{
		return [](State state) -> std::expected<Settings, Error>
			{
				return std::get<2>(state);
			};
	}

	template<typename T, typename E>
	auto operator|(std::expected<T, E>&& exp, Transform func)
	{
		return exp.and_then(func);
	}

	template<typename T, typename E, typename F>
	auto operator|(std::expected<T, E>&& exp, F&& func)
	{
		return exp.and_then(std::forward<F>(func));
	}

}
// namespace AqualinkAutomate::Options
