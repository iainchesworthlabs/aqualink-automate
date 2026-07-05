#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <boost/program_options/variables_map.hpp>

#include "devices/jandy_device_id.h"
#include "devices/jandy_device_types.h"
#include "devices/jandy_emulated_device_types.h"
#include "errors/options_errors.h"
#include "options/options_option_type.h"
#include "options/validators/jandy_device_id_validator.h"
#include "options/validators/jandy_emulated_device_type_validator.h"

using namespace AqualinkAutomate::Options;

namespace AqualinkAutomate::Jandy::Options
{

	using JandyEmulatedDevice = std::pair<Devices::JandyEmulatedDeviceTypes, Devices::JandyDeviceType>;
	using JandyEmulatedDeviceCollection = std::vector<JandyEmulatedDevice>;

	struct JandySettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Jandy" };
			return AREA_NAME;
		}

		JandySettings() :
			disable_emulation{ false },
			emulated_devices{}
		{
		}

		bool disable_emulation;
		bool disable_presence_gating{ false };     // disable RSSA presence-gating (auto-suppress emulation on a detected real adapter)
		bool auto_startup{ false };                // detect the controller from the bus and choose what to emulate
		JandyEmulatedDeviceCollection emulated_devices;
		std::string navigation_password{};  // 4-digit password for menu navigation
		std::uint32_t chlorinator_setpoint_refresh_interval{ 300 };  // seconds between Set-AquaPure menu re-scrapes of the configured chlorinator % (0 = disabled)
	};

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_DISABLEEMULATION{ make_appoption("jandy-disable-emulation", "Disable Jandy controller emulation", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_DISABLEPRESENCEGATING{ make_appoption("jandy-disable-presence-gating", "Disable RSSA presence-gating (do not auto-suppress an emulated Serial Adapter when a real one is suspected on the bus)", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_AUTOSTARTUP{ make_appoption("jandy-auto-startup", "Auto-detect the controller type/revision from the bus and choose what to emulate + how to source data (overrides --jandy-device-type)", boost::program_options::bool_switch()->default_value(false)) };
		AppOptionPtr OPTION_EMULATEDDEVICETYPE{ make_appoption("jandy-device-type", "Space-separated Jandy emulation types (e.g. iaq onetouch)", boost::program_options::value<std::vector<Devices::JandyEmulatedDeviceTypes>>()->multitoken()) };
		AppOptionPtr OPTION_EMULATEDDEVICEID{ make_appoption("jandy-device-id", "Space-separated hex device IDs (e.g. 0xa1 0x41)", boost::program_options::value<std::vector<Devices::JandyDeviceId>>()->multitoken()) };
		AppOptionPtr OPTION_NAVPASSWORD{ make_appoption("jandy-nav-password", "4-digit password for navigating Jandy password-protected menus", boost::program_options::value<std::string>()->default_value("")) };
		AppOptionPtr OPTION_CHLORINATORSETPOINTREFRESH{ make_appoption("chlorinator-setpoint-refresh-interval", "Seconds between Set-AquaPure menu re-scrapes of the configured chlorinator Pool/Spa % (0 disables; requires active OneTouch/iAQ emulation)", boost::program_options::value<std::uint32_t>()->default_value(300)) };

		const std::vector<AppOptionPtr> JandyOptionsCollection
		{
			OPTION_DISABLEEMULATION,
			OPTION_DISABLEPRESENCEGATING,
			OPTION_AUTOSTARTUP,
			OPTION_EMULATEDDEVICETYPE,
			OPTION_EMULATEDDEVICEID,
			OPTION_NAVPASSWORD,
			OPTION_CHLORINATORSETPOINTREFRESH
		};

	public:
		using SettingsType = JandySettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

	public:
		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Jandy::Options
