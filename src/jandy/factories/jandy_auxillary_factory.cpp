#include <boost/algorithm/string.hpp>

#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "errors/jandy_errors_auxillary_factory.h"
#include "factories/jandy_auxillary_factory.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_devices/heater_status.h"
#include "kernel/auxillary_devices/pump_status.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "logging/logging.h"
#include "utility/overloaded_variant_visitor.h"
#include "utility/string_conversion/auxillary_state_string_converter.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Factory
{

	JandyAuxillaryFactory::JandyAuxillaryFactory() = default;

	JandyAuxillaryFactory& JandyAuxillaryFactory::Instance()
	{
		static JandyAuxillaryFactory instance;
		return instance;
	}

	std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> JandyAuxillaryFactory::SerialAdapterDevice_CreateDevice(const Auxillaries::JandyAuxillaryIds aux_id)
	{
		DeviceData data{ AuxillaryDevice_Data{ std::nullopt, aux_id, std::nullopt } };
		return CreateDevice_Impl(data);
	}

	std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> JandyAuxillaryFactory::SerialAdapterDevice_CreateDevice(const Auxillaries::JandyAuxillaryIds aux_id, const Auxillaries::JandyAuxillaryStatuses status)
	{
		Kernel::AuxillaryStatuses aux_status = Kernel::AuxillaryStatuses::Unknown;

		switch (status)
		{
		case Auxillaries::JandyAuxillaryStatuses::Off:
			aux_status = Kernel::AuxillaryStatuses::Off;
			break;

		case Auxillaries::JandyAuxillaryStatuses::On:
			aux_status = Kernel::AuxillaryStatuses::On;
			break;

		case Auxillaries::JandyAuxillaryStatuses::Unknown:
			[[fallthrough]];
		default:
			aux_status = Kernel::AuxillaryStatuses::Unknown;
			break;
		}

		DeviceData data{ AuxillaryDevice_Data{ std::nullopt, aux_id, aux_status } };
		return CreateDevice_Impl(data);
	}

	std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> JandyAuxillaryFactory::OneTouchDevice_CreateDevice(const Utility::AuxillaryStateStringConverter& aux_state)
	{
		auto ec = make_error_code(ErrorCodes::Factory_ErrorCodes::Error_UnknownFactoryError);

		if (!aux_state.Label().has_value())
		{
			LogDebug(Channel::Equipment, "Received an invalid auxillary status; factory cannot create a new device using auxillary status");
			ec = make_error_code(ErrorCodes::Factory_ErrorCodes::Error_ReceivedInvalidAuxillaryStatus);
		}
		else if (auto aux_id = Auxillaries::ParseAuxId(aux_state.Label().value()); aux_id.has_value())
		{
			DeviceData data{ AuxillaryDevice_Data{ std::nullopt, aux_id.value(), aux_state.State().value_or(Kernel::AuxillaryStatuses::Unknown) }};
			return CreateDevice_Impl(data);
		}
		else if (IsChlorinatorDevice(aux_state.Label().value()))
		{
			DeviceData data{ ChlorinatorDevice_Data{ aux_state.Label().value(), aux_state.State().value_or(Kernel::AuxillaryStatuses::Unknown) }};
			return CreateDevice_Impl(data);
		}
		else if (IsCleanerDevice(aux_state.Label().value()))
		{
			DeviceData data{ CleanerDevice_Data{ aux_state.Label().value() } };
			return CreateDevice_Impl(data);
		}
		else if (IsHeaterDevice(aux_state.Label().value()))
		{
			// Pool Heat, Spa Heat, Heat Pump
			//
			// Note that ordering means that "Heat Pump" is caught here!

			DeviceData data{ HeaterDevice_Data{ aux_state.Label().value(), aux_state.State().value_or(Kernel::AuxillaryStatuses::Unknown) } };
			return CreateDevice_Impl(data);
		}
		else if (IsPumpDevice(aux_state.Label().value()))
		{
			// Filter Pump, Pool Pump, Spa Pump
			//
			// Note that ordering means that "Heat Pump" is caught above!

			DeviceData data{ PumpDevice_Data{ aux_state.Label().value(), aux_state.State().value_or(Kernel::AuxillaryStatuses::Unknown) } };
			return CreateDevice_Impl(data);
		}
		else if (IsSpilloverDevice(aux_state.Label().value()))
		{
			DeviceData data{ SpilloverDevice_Data{ aux_state.Label().value() } };
			return CreateDevice_Impl(data);
		}
		else if (IsSprinklerDevice(aux_state.Label().value()))
		{
			DeviceData data{ SprinklerDevice_Data{ aux_state.Label().value() } };
			return CreateDevice_Impl(data);
		}
		else
		{
			// Unknown device type...ignore it.
			ec = make_error_code(ErrorCodes::Factory_ErrorCodes::Error_UnknownDeviceLabel);
		}

		return std::unexpected(ec);
	}

	bool JandyAuxillaryFactory::IsAuxillaryDevice(const std::string& label) const
	{
		// Defer to the single normalising parser so "Aux5", "Aux 5", "AuxB1", "Aux B1" and
		// "Extra Aux" are all recognised consistently (the old length-gate rejected the
		// space/no-space variants).
		return Auxillaries::ParseAuxId(label).has_value();
	}

	bool JandyAuxillaryFactory::IsChlorinatorDevice(const std::string& label) const
	{
		return ((CHLORINATOR == label) || (AQUAPURE == label));
	}

	bool JandyAuxillaryFactory::IsCleanerDevice(const std::string& label) const
	{
		return (CLEANER == label);
	}

	bool JandyAuxillaryFactory::IsHeaterDevice(const std::string& label) const
	{
		return boost::algorithm::contains(label, HEAT);
	}

	bool JandyAuxillaryFactory::IsPumpDevice(const std::string& label) const
	{
		return boost::algorithm::contains(label, PUMP);
	}

	bool JandyAuxillaryFactory::IsSpilloverDevice(const std::string& label) const
	{
		return (SPILLOVER == label);
	}

	bool JandyAuxillaryFactory::IsSprinklerDevice(const std::string& /*label*/) const
	{
		return false;
	}

	std::expected<std::shared_ptr<Kernel::AuxillaryDevice>, boost::system::error_code> JandyAuxillaryFactory::CreateDevice_Impl(DeviceData& device_data)
	{
		if (auto aux_ptr = std::make_shared<Kernel::AuxillaryDevice>(); nullptr == aux_ptr)
		{
			return std::unexpected(make_error_code(ErrorCodes::Factory_ErrorCodes::Error_FailedToCreateAuxillaryPtr));
		}
		else
		{
			std::visit(
				Utility::OverloadedVisitor
				{
					[&aux_ptr](const AuxillaryDevice_Data& data)
					{
						// Construct with a DETERMINISTIC id derived from the aux id so a
						// cache-restored device and a live-discovered device for the SAME aux
						// reconcile to one identity (the cache, being protocol-agnostic, cannot
						// persist the JandyAuxillaryId trait but does persist this stable id).
						aux_ptr = std::make_shared<Kernel::AuxillaryDevice>(Auxillaries::AuxStableId(data.Id));
						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Auxillary);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(std::string{ magic_enum::enum_name(data.Id) }));
						aux_ptr->AuxillaryTraits.Set(Auxillaries::JandyAuxillaryId{}, data.Id);
						// Immutable hardware id ("Aux5") - set ONCE here; carries the aux id to
						// the cache + web display layers that cannot read JandyAuxillaryId.
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HardwareLabelTrait{}, std::string{ magic_enum::enum_name(data.Id) });
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, data.Status.value_or(Kernel::AuxillaryStatuses::Unknown));
					},
					[&aux_ptr](const ChlorinatorDevice_Data& data)
					{
						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Chlorinator);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(CHLORINATOR));
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);

						if (data.Status.has_value())
						{
							aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::ChlorinatorStatusTrait{}, Kernel::ConvertToChlorinatorStatus(data.Status.value()));
						}
					},
					[&aux_ptr](const CleanerDevice_Data& data)
					{
						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Cleaner);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(CLEANER));
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Pool);
					},
					[&aux_ptr](const HeaterDevice_Data& data)
					{
						using enum Kernel::BodyOfWaterIds;

						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Heater);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(HEATER));

						if (data.Status.has_value())
						{
							aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::HeaterStatusTrait{}, Kernel::ConvertToHeaterStatus(data.Status.value()));
						}

						auto label = data.Label.value_or(HEATER);
						auto body_id = Unknown;
						if (label.find("Pool") != std::string::npos)
							body_id = Pool;
						else if (label.find("Spa") != std::string::npos)
							body_id = Spa;
						else if (label.find("Solar") != std::string::npos)
							body_id = Shared;
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, body_id);
					},
					[&aux_ptr](const PumpDevice_Data& data)
					{
						using enum Kernel::BodyOfWaterIds;

						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Pump);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(PUMP));

						if (data.Status.has_value())
						{
							aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::ConvertToPumpStatus(data.Status.value()));
						}

						auto label = data.Label.value_or(PUMP);
						auto body_id = Unknown;
						if (label.find("Filter") != std::string::npos)
							body_id = Shared;
						else if (label.find("Pool") != std::string::npos)
							body_id = Pool;
						else if (label.find("Spa") != std::string::npos)
							body_id = Spa;
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, body_id);
					},
					[&aux_ptr](const SpilloverDevice_Data& data)
					{
						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Spillover);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(SPILLOVER));
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
					},
					[&aux_ptr](const SprinklerDevice_Data& data)
					{
						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Sprinkler);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(SPRINKLER));
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, Kernel::AuxillaryStatuses::Off);
					},
					[&aux_ptr](const UnknownDevice_Data& data)
					{
						aux_ptr->AuxillaryTraits.Set(AuxillaryTraitsTypes::AuxillaryTypeTrait{}, AuxillaryTraitsTypes::AuxillaryTypes::Unknown);
						aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, data.Label.value_or(UNKNOWN));
					}
				},
				device_data
			);

			return aux_ptr;
		}
	}

}
// namespace AqualinkAutomate::Factory
