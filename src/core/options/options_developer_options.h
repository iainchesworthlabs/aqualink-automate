#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "logging/logging_channels.h"
#include "logging/logging_severity_levels.h"
#include "options/options_option_type.h"
#include "options/validators/profiler_type_validator.h"
#include "options/validators/severity_level_validator.h"
#include "profiling/types/profiling_types.h"

namespace AqualinkAutomate::Options::Developer
{

	typedef struct tagDeveloperSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Developer" };
			return AREA_NAME;
		}

		tagDeveloperSettings() = default;

		bool dev_mode_enabled{ false };
		bool decode_to_master_enabled{ false };
		std::string replay_file;
		std::string recording_file;

		// Capture-replay pacing (developer mode only; see --replay-filename).
		// replay_frame_period_ms is the wall-clock period between successive
		// read/parse cycles when replaying a capture, so frames are delivered at
		// roughly the bus's natural inter-frame rate instead of as fast as the
		// parser will accept them (0 = unpaced / as fast as possible).
		// replay_speed scales that period (>1 faster, <1 slower).
		std::uint32_t replay_frame_period_ms{ 15 };
		double replay_speed{ 1.0 };
	}
	DeveloperSettings;

	class OptionsProcessor
	{
	public:
		OptionsProcessor();

	private:
		AppOptionPtr OPTION_DEVMODE{ make_appoption("dev-mode", "Enable developer mode", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_DEVREPLAYFILE{ make_appoption("replay-filename", "Developer replay file from which to source test data", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_DEVRECORDFILE{ make_appoption("record-serial", "Record serial port data to file for later replay", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_DECODE_TO_MASTER{ make_appoption("decode-to-master", "DEV: decode and log RS-485 frames addressed TO the master (0x00); observe-only, no emulation/replay", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_REPLAY_FRAME_PERIOD{ make_appoption("replay-frame-period", "DEV: capture-replay inter-frame period in milliseconds (paces --replay-filename to the bus's natural rate; 0 = unpaced / as fast as possible)", boost::program_options::value<std::uint32_t>()->default_value(15)) };
		AppOptionPtr OPTION_REPLAY_SPEED{ make_appoption("replay-speed", "DEV: capture-replay speed factor scaling --replay-frame-period (>1 faster, <1 slower)", boost::program_options::value<double>()->default_value(1.0)) };
		AppOptionPtr OPTION_PROFILER{ make_appoption("profiler", "DEV: select the profiling backend to activate at startup: tracy, uprof, or vtune (case-insensitive). Requires a profiling-enabled build (ENABLE_PROFILING=ON) with that backend compiled in; otherwise it is reported as unavailable and profiling stays disabled", boost::program_options::value<AqualinkAutomate::Types::ProfilerTypes>()->multitoken()) };

		// One `--loglevel-<channel>` option per Logging::Channel value, built in
		// the constructor by enumerating the Channel enum (no manual per-channel
		// repetition). Keyed by channel so Process()/Validate() can iterate.
		std::map<Logging::Channel, AppOptionPtr> OPTION_LOGLEVELS;

		// Aggregate of every option this processor contributes (the fixed options
		// plus the per-channel log-level options), assembled in the constructor.
		std::vector<AppOptionPtr> DeveloperOptionsCollection;

	public:
		using SettingsType = DeveloperSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

	public:
		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Developer
