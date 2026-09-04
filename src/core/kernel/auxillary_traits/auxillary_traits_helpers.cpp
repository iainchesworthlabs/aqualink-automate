#include <optional>

#include <magic_enum/magic_enum.hpp>

#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes
{

	namespace
	{

		// Maps a device's AuxillaryTypeTrait to the category-specific status trait and resolves it
		// to a human-readable string. Returns std::nullopt when no status is available for the type.
		std::optional<std::string_view> ResolveStatusString(const AuxillaryDevice& device, AuxillaryTypes type)
		{
			using enum AuxillaryTypes;

			switch (type)
			{
			case Auxillary:
			case Cleaner:
			case Light:
			case Spillover:
			case Sprinkler:
				// Lights are plain on/off relays on the wire (LightsDevice writes the same
				// AuxillaryStatusTrait the aux family uses, and clears it to Unknown on a
				// watchdog timeout), so they resolve through the very same arm.
				if (auto status = device.AuxillaryTraits.TryGet(AuxillaryStatusTrait{}); status.has_value())
				{
					return magic_enum::enum_name(status.value());
				}
				break;

			case Chlorinator:
				if (auto status = device.AuxillaryTraits.TryGet(ChlorinatorStatusTrait{}); status.has_value())
				{
					return magic_enum::enum_name(status.value());
				}
				break;

			case Heater:
				if (auto status = device.AuxillaryTraits.TryGet(HeaterStatusTrait{}); status.has_value())
				{
					return magic_enum::enum_name(status.value());
				}
				break;

			case Pump:
				if (auto status = device.AuxillaryTraits.TryGet(PumpStatusTrait{}); status.has_value())
				{
					return magic_enum::enum_name(status.value());
				}
				break;

			case Unknown:
			default:
				// No status mapping for this device type.
				break;
			}

			return std::nullopt;
		}

	}
	// anonymous namespace

	std::string_view ConvertStatusToString(const AuxillaryDevice& device)
	{
		static const std::string UNKNOWN_STATUS{"Unknown"};

		// Single trait lookup for the device type; ResolveStatusString does the second (category) lookup.
		if (auto type = device.AuxillaryTraits.TryGet(AuxillaryTypeTrait{}); type.has_value())
		{
			if (auto status = ResolveStatusString(device, type.value()); status.has_value())
			{
				return status.value();
			}
		}

		return UNKNOWN_STATUS;
	}

	std::string_view ConvertStatusToString(const std::shared_ptr<AuxillaryDevice>& device)
	{
		static const std::string UNKNOWN_STATUS{"Unknown"};
		if (nullptr == device)
		{
			return UNKNOWN_STATUS;
		}

		return ConvertStatusToString(*device);
	}

	bool HasStatus(const AuxillaryDevice& device)
	{
		// A device has a displayable status when its type maps to a populated category status trait.
		if (auto type = device.AuxillaryTraits.TryGet(AuxillaryTypeTrait{}); type.has_value())
		{
			return ResolveStatusString(device, type.value()).has_value();
		}

		return false;
	}

	bool HasStatus(const std::shared_ptr<AuxillaryDevice>& device)
	{
		return (nullptr != device) && HasStatus(*device);
	}

}
// namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes
