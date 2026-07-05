#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Alerting
{

	/// Fault-detection / alerting settings (WS3).
	typedef struct tagAlertingSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Alerting" };
			return AREA_NAME;
		}

		tagAlertingSettings() = default;

		/// Salt PPM below which the salt_low condition raises.  0 disables the
		/// salt check entirely.  Clears with +100 ppm hysteresis.
		std::uint32_t salt_low_ppm{ 2600 };

		/// Optional webhook URL (http/https).  Empty => no webhook is sent; only
		/// the MQTT problem entities and UI toasts are produced.
		std::string webhook_url;

		/// Seconds with no decoded protocol message before serial_comms_loss
		/// raises.  Must be > 0.
		std::uint32_t comms_timeout_seconds{ 60 };
	}
	AlertingSettings;

	class OptionsProcessor
	{
	private:
		AppOptionPtr OPTION_SALT_LOW_PPM{ make_appoption("alert-salt-low-ppm", "Salt PPM threshold for the salt-low alert (0 disables)", boost::program_options::value<std::uint32_t>()->default_value(2600)) };
		AppOptionPtr OPTION_WEBHOOK_URL{ make_appoption("alert-webhook-url", "Webhook URL (http/https) POSTed on every alert transition", boost::program_options::value<std::string>()) };
		AppOptionPtr OPTION_COMMS_TIMEOUT{ make_appoption("alert-comms-timeout-seconds", "Seconds without a decoded message before the serial-comms-loss alert raises", boost::program_options::value<std::uint32_t>()->default_value(60)) };

		const std::vector<AppOptionPtr> AlertingOptionsCollection
		{
			OPTION_SALT_LOW_PPM,
			OPTION_WEBHOOK_URL,
			OPTION_COMMS_TIMEOUT
		};

	public:
		using SettingsType = AlertingSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Alerting
