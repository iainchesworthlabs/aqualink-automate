#pragma once

#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/pool_configurations.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Equipment
{

	typedef struct tagEquipmentSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Equipment" };
			return AREA_NAME;
		}

		tagEquipmentSettings() :
			pool_configuration{ Kernel::PoolConfigurations::Unknown }
		{
		}

		Kernel::PoolConfigurations pool_configuration;
		bool pool_configuration_is_user_specified{ false };

		/// For a SingleBody (pool-only / spa-only) install, which body the single body is.
		/// Pool-only and spa-only are indistinguishable on the wire, so spa-only is signalled
		/// only by --pool-configuration spa-only. Ignored for DualBody configurations.
		Kernel::BodyOfWaterIds single_body_kind{ Kernel::BodyOfWaterIds::Pool };

		/// Age (seconds) beyond which a reported temperature is flagged stale. The controller only
		/// reports temperatures while the filter pump runs (and only for the active body on a combo
		/// system), so a value can persist after it stops being refreshed. Default 600s (10 min).
		unsigned int temperature_staleness_threshold_seconds{ 600 };

		/// JSON cache of discovered equipment config (board, pool config, device
		/// list with stable UUIDs). Loaded at boot for an instant dashboard, then
		/// corrected by live discovery. EMPTY (default) disables caching.
		std::string equipment_cache_file;
	}
	EquipmentSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_POOL_CONFIGURATION{ make_appoption("pool-configuration", "Pool installation type: pool-only, spa-only, combo, dual (default: auto)", boost::program_options::value<std::string>()->default_value("auto")) };
		AppOptionPtr OPTION_EQUIPMENT_CACHE_FILE{ make_appoption("equipment-cache-file", "JSON cache of discovered equipment config for an instant dashboard on restart (empty disables)", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_TEMPERATURE_STALENESS_THRESHOLD{ make_appoption("temperature-staleness-threshold", "Age in seconds after which a reported temperature is flagged stale (default: 600)", boost::program_options::value<unsigned int>()->default_value(600)) };

		const std::vector<AppOptionPtr> EquipmentOptionsCollection
		{
			OPTION_POOL_CONFIGURATION,
			OPTION_EQUIPMENT_CACHE_FILE,
			OPTION_TEMPERATURE_STALENESS_THRESHOLD
		};

	public:
		using SettingsType = EquipmentSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

	public:
		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Equipment
