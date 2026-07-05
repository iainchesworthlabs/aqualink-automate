#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include "errors/options_errors.h"
#include "options/options_option_type.h"

namespace AqualinkAutomate::Options::Matter
{

	/// Matter (smart-home) bridge settings.
	///
	/// The Matter bridge runs as a separate Node.js sidecar (matter-bridge/) that the
	/// Docker entrypoint launches; the C++ app does not host the Matter stack itself.
	/// These settings exist so the feature is opt-OUT (default on), is visible in
	/// --help / the config file, and so the diagnostics route knows where to fetch the
	/// sidecar's status/QR. The opt-out flag is also read by the Docker entrypoint to
	/// decide whether to launch the sidecar at all.
	typedef struct tagMatterSettings
	{
		static const std::string& AreaName()
		{
			static const std::string AREA_NAME{ "Matter" };
			return AREA_NAME;
		}

		tagMatterSettings() = default;

		/// Enable the Matter bridge. Default ON; opt out with `--matter false` or
		/// `matter = false` in the config file.
		bool enabled{ true };

		/// Directory the sidecar persists fabric/commissioning state to (a Docker volume).
		std::string storage_path{ "/data/matter" };

		/// Port the sidecar's status/QR HTTP server listens on (localhost only). The
		/// diagnostics route fetches `http://127.0.0.1:<status_port>/matter/status`.
		uint16_t status_port{ 8099 };

		/// Commissioning passcode (8 digits). 0 => the sidecar generates and persists one.
		uint32_t passcode{ 0 };

		/// Commissioning discriminator (0-4095). 0 => the sidecar generates and persists one.
		uint16_t discriminator{ 0 };

	}
	MatterSettings;

	class OptionsProcessor
	{
	private:
		// Default-true value<bool> with an implicit value makes `--matter` opt-OUT:
		//   (absent)               -> true   (default)
		//   --matter               -> true   (implicit)
		//   --matter false / =false-> false
		//   matter = false (config)-> false
		AppOptionPtr OPTION_ENABLE{ make_appoption("matter", "Enable the Matter bridge sidecar (default: on; use --matter false to opt out)", boost::program_options::value<bool>()->default_value(true)->implicit_value(true)) };

		AppOptionPtr OPTION_STORAGE_PATH{ make_appoption("matter-storage-path", "Directory the Matter bridge persists fabric state to", boost::program_options::value<std::string>()->default_value("/data/matter")) };
		AppOptionPtr OPTION_STATUS_PORT{ make_appoption("matter-status-port", "Port the Matter bridge status/QR server listens on", boost::program_options::value<uint16_t>()->default_value(8099)) };
		AppOptionPtr OPTION_PASSCODE{ make_appoption("matter-passcode", "Matter commissioning passcode (8 digits; 0 = generate)", boost::program_options::value<uint32_t>()->default_value(0)) };
		AppOptionPtr OPTION_DISCRIMINATOR{ make_appoption("matter-discriminator", "Matter commissioning discriminator (0-4095; 0 = generate)", boost::program_options::value<uint16_t>()->default_value(0)) };

		const std::vector<AppOptionPtr> MatterOptionsCollection
		{
			OPTION_ENABLE,
			OPTION_STORAGE_PATH,
			OPTION_STATUS_PORT,
			OPTION_PASSCODE,
			OPTION_DISCRIMINATOR
		};

	public:
		using SettingsType = MatterSettings;

	public:
		std::string Name() const { return SettingsType::AreaName(); }
		boost::program_options::options_description Options() const;

		void Validate(const boost::program_options::variables_map& vm) const;
		std::expected<SettingsType, ErrorCodes::Options_ErrorCodes> Process(boost::program_options::variables_map& vm) const;
	};

}
// namespace AqualinkAutomate::Options::Matter
