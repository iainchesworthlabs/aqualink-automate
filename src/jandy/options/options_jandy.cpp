#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <vector>

#include <boost/program_options.hpp>
#include <magic_enum/magic_enum.hpp>

#include "devices/jandy_device_id.h"
#include "formatters/jandy_device_formatters.h"
#include "logging/logging.h"
#include "options/options_jandy.h"
#include "options/helpers/conflicting_options_helper.h"
#include "options/helpers/option_dependency_helper.h"
#include "utility/get_terminal_column_width.h"

using namespace AqualinkAutomate::Devices;
using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Jandy::Options
{

	namespace
	{
		// Returns a default device ID for each emulated device type.
		// Uses secondary IDs to avoid conflicting with real (primary) devices on the RS-485 bus.
		JandyDeviceId DefaultDeviceId(JandyEmulatedDeviceTypes device_type)
		{
			using enum JandyEmulatedDeviceTypes;
			switch (device_type)
			{
			case OneTouch:			return JandyDeviceId{ 0x41 };
			case IAQ:				return JandyDeviceId{ 0xA1 };
			case RS_Keypad:			return JandyDeviceId{ 0x09 };
			case PDA:				return JandyDeviceId{ 0x61 };
			case SerialAdapter:		return JandyDeviceId{ 0x48 };
			default:				return JandyDeviceId{ 0xFF };
			}
		}

		// Table 1 ("Diagnostics Table", AquaLink RS Service guide) addressing: an emulated type may
		// only occupy the RS-485 instance addresses of its device class(es). IAQ can stand up as an
		// AqualinkTouch page device (0x30-0x33) or a legacy iAQ (0xA0-0xA3).
		std::vector<DeviceClasses> AllowedClassesForEmulatedType(JandyEmulatedDeviceTypes type)
		{
			switch (type)
			{
			case JandyEmulatedDeviceTypes::RS_Keypad:		return { DeviceClasses::RS_Keypad };
			case JandyEmulatedDeviceTypes::OneTouch:		return { DeviceClasses::OneTouch };
			case JandyEmulatedDeviceTypes::IAQ:				return { DeviceClasses::AqualinkTouch, DeviceClasses::IAQ };
			case JandyEmulatedDeviceTypes::PDA:				return { DeviceClasses::PDA };
			case JandyEmulatedDeviceTypes::SerialAdapter:	return { DeviceClasses::SerialAdapter };
			case JandyEmulatedDeviceTypes::SpasideRemote:	return { DeviceClasses::SpaRemote };
			default:										return {};
			}
		}

		// The union of valid bus addresses (instances) an emulated type may use. Its size is the
		// per-type instance limit (Table 1 "Possible Unit Numbers": e.g. 2 Serial Adapters, 4
		// OneTouch, 1 of a single-instance type).
		std::set<std::uint8_t> ValidIdsForEmulatedType(JandyEmulatedDeviceTypes type)
		{
			std::set<std::uint8_t> ids;
			for (const auto device_class : AllowedClassesForEmulatedType(type))
			{
				const auto addresses = JandyDeviceType::InstanceAddressesForClass(device_class);
				ids.insert(addresses.cbegin(), addresses.cend());
			}
			return ids;
		}

		std::string FormatIdSet(const std::set<std::uint8_t>& ids)
		{
			std::string out;
			for (const auto id : ids)
			{
				if (!out.empty()) { out += ", "; }
				out += std::format("0x{:02x}", id);
			}
			return out;
		}

		// Enforces the Table 1 addressing rules on the resolved emulated-device set:
		//   * each device id must be a valid instance address for its type,
		//   * no two emulated devices may share a bus address,
		//   * the count of a given type may not exceed the addresses available to it.
		// Returns a human-readable message on the first violation, or nullopt when valid.
		std::optional<std::string> ValidateEmulatedDevicePlacement(const JandyEmulatedDeviceCollection& devices)
		{
			// R2: a type may not be requested more times than it has instance addresses (Table 1
			// "Possible Unit Numbers"). Checked first so this gives the clearest message.
			std::map<JandyEmulatedDeviceTypes, std::size_t> type_counts;
			for (const auto& [type, device] : devices)
			{
				if (const auto count = ++type_counts[type]; count > ValidIdsForEmulatedType(type).size())
				{
					return std::format("too many {} devices requested ({}): at most {} can exist on the bus",
						magic_enum::enum_name(type), count, ValidIdsForEmulatedType(type).size());
				}
			}

			// R1/R3: each id must be a valid instance address for its type, and ids must be unique.
			std::set<std::uint8_t> assigned_ids;
			for (const auto& [type, device] : devices)
			{
				const std::uint8_t id = device.Id()();

				if (const auto valid_ids = ValidIdsForEmulatedType(type); !valid_ids.contains(id))
				{
					return std::format("device id 0x{:02x} is not valid for emulated type {} (valid ids: {})",
						id, magic_enum::enum_name(type), FormatIdSet(valid_ids));
				}

				if (!assigned_ids.insert(id).second)
				{
					return std::format("duplicate emulated device id 0x{:02x}: two emulated devices cannot share a bus address", id);
				}
			}

			return std::nullopt;
		}
	}
	// anonymous namespace

	boost::program_options::options_description OptionsProcessor::Options() const
	{
		boost::program_options::options_description options(SettingsType::AreaName(), Utility::get_terminal_column_width());

		LogDebug(Channel::Options, std::format("Adding {} options from the {} set", JandyOptionsCollection.size(), SettingsType::AreaName()));
		for (auto& option : JandyOptionsCollection)
		{
			options.add((*option)());
		}

		return options;
	}

	void OptionsProcessor::Validate(const boost::program_options::variables_map& vm) const
	{
		// Device IDs require device types, but device types can stand alone (defaults will be assigned).
		Helper_ValidateOptionDependencies(vm, OPTION_EMULATEDDEVICEID, OPTION_EMULATEDDEVICETYPE);

		Helper_CheckForConflictingOptions(vm, OPTION_DISABLEEMULATION, OPTION_EMULATEDDEVICETYPE);
		Helper_CheckForConflictingOptions(vm, OPTION_DISABLEEMULATION, OPTION_EMULATEDDEVICEID);
	}

	std::expected<OptionsProcessor::SettingsType, ErrorCodes::Options_ErrorCodes> OptionsProcessor::Process(boost::program_options::variables_map& vm) const
	{
		SettingsType settings;

		if (OPTION_DISABLEEMULATION->IsPresent(vm))
		{
			settings.disable_emulation = OPTION_DISABLEEMULATION->As<bool>(vm);
		}

		if (OPTION_DISABLEPRESENCEGATING->IsPresent(vm))
		{
			settings.disable_presence_gating = OPTION_DISABLEPRESENCEGATING->As<bool>(vm);
		}

		if (OPTION_AUTOSTARTUP->IsPresent(vm))
		{
			settings.auto_startup = OPTION_AUTOSTARTUP->As<bool>(vm);
		}

		if (OPTION_NAVPASSWORD->IsPresent(vm))
		{
			settings.navigation_password = OPTION_NAVPASSWORD->As<std::string>(vm);
			if (!settings.navigation_password.empty())
			{
				LogInfo(Channel::Options, "Navigation password configured for menu access");
			}
		}

		if (OPTION_CHLORINATORSETPOINTREFRESH->IsPresent(vm))
		{
			settings.chlorinator_setpoint_refresh_interval = OPTION_CHLORINATORSETPOINTREFRESH->As<std::uint32_t>(vm);
		}

		if (!OPTION_EMULATEDDEVICETYPE->IsPresent(vm))
		{
			// No explicit device types specified — use default set.
			// OneTouch for menu scraping, IAQ for status data, SerialAdapter for commands.
			using enum JandyEmulatedDeviceTypes;
			static const std::vector<JandyEmulatedDeviceTypes> DEFAULT_DEVICE_TYPES
			{
				OneTouch,
				IAQ,
				SerialAdapter
			};

			for (auto type : DEFAULT_DEVICE_TYPES)
			{
				settings.emulated_devices.emplace_back(type, DefaultDeviceId(type));
			}

			LogInfo(Channel::Options, "No emulated device types specified; using defaults: OneTouch, IAQ, SerialAdapter");
		}
		else
		{
			// Device types and IDs are parsed and validated by boost::program_options via the
			// type-specific validators (multitoken, space-separated values on the command line).
			auto device_types = OPTION_EMULATEDDEVICETYPE->As<std::vector<JandyEmulatedDeviceTypes>>(vm);
			std::vector<Devices::JandyDeviceId> device_ids;

			if (OPTION_EMULATEDDEVICEID->IsPresent(vm))
			{
				device_ids = OPTION_EMULATEDDEVICEID->As<std::vector<Devices::JandyDeviceId>>(vm);
			}

			// Assign default device IDs for any types that don't have an explicit ID.
			// Defaults use a secondary ID to avoid conflicting with real devices on the bus.
			while (device_ids.size() < device_types.size())
			{
				auto index = device_ids.size();
				auto default_id = DefaultDeviceId(device_types[index]);

				LogDebug(Channel::Options, std::format("Assigning default device id 0x{:02x} for emulated device type: {}",
					default_id(), magic_enum::enum_name(device_types[index])));

				device_ids.push_back(default_id);
			}

			std::ranges::transform(
				std::views::zip(device_types, device_ids),
				std::back_inserter(settings.emulated_devices),
				[](auto pair)
				{
					auto& [type, id] = pair;
					LogDebug(Channel::Options, std::format("Emplacing emulated device: {} (id: {})", magic_enum::enum_name(type), id));
					return JandyEmulatedDevice(type, id);
				}
			);
		}

		// Enforce Table 1 addressing rules: each emulated device id must be a valid instance
		// address for its type, ids must be unique, and a type may not exceed its instance count.
		if (const auto error = ValidateEmulatedDevicePlacement(settings.emulated_devices); error.has_value())
		{
			LogError(Channel::Options, std::format("Invalid Jandy emulation configuration: {}", *error));
			return std::unexpected(ErrorCodes::Options_ErrorCodes::OptionsValidationFailed);
		}

		return settings;
	}

}
// namespace AqualinkAutomate::Jandy::Options
