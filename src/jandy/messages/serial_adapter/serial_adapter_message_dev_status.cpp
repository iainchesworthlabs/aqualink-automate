#include <format>

#include <boost/units/io.hpp>
#include <magic_enum/magic_enum.hpp>

#include "formatters/temperature_formatter.h"
#include "formatters/units_electric_potential_formatter.h"
#include "messages/jandy_message_constants.h"
#include "messages/jandy_message_ids.h"
#include "messages/serial_adapter/serial_adapter_message_dev_status.h"
#include "utility/jandy_checksum.h"
#include "logging/logging.h"
#include "utility/overloaded_variant_visitor.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Messages
{

	SerialAdapterMessage_DevStatus::SerialAdapterMessage_DevStatus() noexcept :
		SerialAdapterMessage(JandyMessageIds::RSSA_DevStatus),
		m_StatusType(SerialAdapter_UnknownCommands::Unknown)
	{
	}

	SerialAdapterMessage_DevStatus::SerialAdapterMessage_DevStatus(const SerialAdapter_ConfigControlCommands sa_ccc) :
		SerialAdapterMessage(JandyMessageIds::RSSA_DevStatus),
		m_StatusType(sa_ccc)
	{
	}

	SerialAdapterMessage_DevStatus::SerialAdapterMessage_DevStatus(const SerialAdapter_SystemConfigurationStatuses sa_scs) :
		SerialAdapterMessage(JandyMessageIds::RSSA_DevStatus),
		m_StatusType(sa_scs)
	{
	}

	SerialAdapterMessage_DevStatus::SerialAdapterMessage_DevStatus(const SerialAdapter_SystemPumpCommands sa_spc) :
		SerialAdapterMessage(JandyMessageIds::RSSA_DevStatus),
		m_StatusType(sa_spc)
	{
	}

	SerialAdapterMessage_DevStatus::SerialAdapterMessage_DevStatus(const SerialAdapter_SystemTemperatureCommands sa_stc) :
		SerialAdapterMessage(JandyMessageIds::RSSA_DevStatus),
		m_StatusType(sa_stc)
	{
	}

	SerialAdapterMessage_DevStatus::SerialAdapterMessage_DevStatus(const Auxillaries::JandyAuxillaryIds sa_jai) :
		SerialAdapterMessage(JandyMessageIds::RSSA_DevStatus),
		m_StatusType(sa_jai)
	{
	}


	std::optional<uint16_t> SerialAdapterMessage_DevStatus::ModelType() const
	{
		return m_ModelType;
	}

	std::optional<SerialAdapter_SCS_OpModes> SerialAdapterMessage_DevStatus::OpMode() const
	{
		return m_OpMode;
	}

	std::optional<SerialAdapter_SCS_Options> SerialAdapterMessage_DevStatus::Options() const
	{
		return m_Options;
	}

	std::optional<SerialAdapter_SCS_BatteryCondition> SerialAdapterMessage_DevStatus::BatteryCondition() const
	{
		return m_BatteryCondition;
	}

	std::optional<uint8_t> SerialAdapterMessage_DevStatus::Pool_SetPoint_One() const
	{
		return m_PoolTemperature_SetPoint_One;
	}

	std::optional<uint8_t> SerialAdapterMessage_DevStatus::Pool_SetPoint_Two() const
	{
		return m_PoolTemperature_SetPoint_Two;
	}

	std::optional<bool> SerialAdapterMessage_DevStatus::Pool_Heater_Two_Enabled() const
	{
		return m_PoolHeater_Two_Enabled;
	}

	std::optional<uint8_t> SerialAdapterMessage_DevStatus::Spa_SetPoint() const
	{
		return m_SpaTemperature_SetPoint;
	}

	std::optional<uint8_t> SerialAdapterMessage_DevStatus::AirTemperature() const
	{
		return m_AirTemperature;
	}

	std::optional<uint8_t> SerialAdapterMessage_DevStatus::PoolTemperature() const
	{
		return m_PoolTemperature;
	}

	std::optional<uint8_t> SerialAdapterMessage_DevStatus::SolarTemperature() const
	{
		return m_SolarTemperature;
	}

	std::optional<uint8_t> SerialAdapterMessage_DevStatus::SpaTemperature() const
	{
		return m_SpaTemperature;
	}

	std::optional<std::tuple<Auxillaries::JandyAuxillaryIds, std::optional<Auxillaries::JandyAuxillaryStatuses>>> SerialAdapterMessage_DevStatus::AuxilliaryState() const
	{
		return m_Aux_State;
	}

	std::optional<Kernel::TemperatureUnits> SerialAdapterMessage_DevStatus::TemperatureUnits() const
	{
		return m_TemperatureUnits;
	}

	std::string SerialAdapterMessage_DevStatus::ToString() const
	{
		return std::format("Packet: {} || Payload: {}", SerialAdapterMessage::ToString(), 0);
	}

	bool SerialAdapterMessage_DevStatus::SerializeContents(std::vector<uint8_t>& message_bytes) const
	{
		message_bytes =
		{
			Messages::HEADER_BYTE_DLE,
			Messages::HEADER_BYTE_STX,
			0x00,
			magic_enum::enum_integer(JandyMessageIds::RSSA_DevStatus),
			0x00,
			0x00,
			0x00,
			0x00,
			0x00,
			Messages::HEADER_BYTE_DLE,
			Messages::HEADER_BYTE_ETX
		};

		std::visit(
			Utility::OverloadedVisitor
			{ 
				[](std::monostate)
				{
					// Intentionally empty: an unset status variant serialises nothing.
				},
				[&message_bytes](SerialAdapter_ConfigControlCommands sa_ccc)
				{
					message_bytes[4] = 0x05;
					message_bytes[5] = magic_enum::enum_integer(sa_ccc);
				},
				[&message_bytes](SerialAdapter_SystemConfigurationStatuses sa_scs)
				{
					message_bytes[4] = 0x05;
					message_bytes[5] = magic_enum::enum_integer(sa_scs);
				},
				[&message_bytes](SerialAdapter_SystemPumpCommands sa_spc)
				{
					message_bytes[4] = 0x05;
					message_bytes[5] = magic_enum::enum_integer(sa_spc);
				},
				[&message_bytes](SerialAdapter_SystemTemperatureCommands sa_stc)
				{
					message_bytes[4] = 0x05;
					message_bytes[5] = magic_enum::enum_integer(sa_stc);
				},
				[&message_bytes](Auxillaries::JandyAuxillaryIds sa_jai)
				{
					message_bytes[4] = 0x00;
					message_bytes[5] = magic_enum::enum_integer(sa_jai) + SERIALADAPTER_AUX_ID_OFFSET;
				},
				[&message_bytes](SerialAdapter_UnknownCommands sa_uc)
				{
					message_bytes[4] = 0x00;
					message_bytes[5] = magic_enum::enum_integer(sa_uc);
				}
			}, 
			m_StatusType
		);

		auto message_span_to_checksum = std::as_bytes(std::span<uint8_t>(message_bytes.begin(), 8));
		message_bytes[8] = Utility::JandyPacket_CalculateChecksum(message_span_to_checksum.begin(), message_span_to_checksum.end());

		return true;
	}

	bool SerialAdapterMessage_DevStatus::DeserializeContents(std::span<const uint8_t> message_bytes)
	{
		LogTrace(Channel::Messages, std::format("Deserialising {} bytes from span into SerialAdapterMessage_DevStatus type", message_bytes.size()));

		if (message_bytes.size() <= Index_DeviceId)
		{
			LogDebug(Channel::Messages, "SerialAdapterMessage_DevStatus is too short to deserialise StatusType");
			return false;
		}

		auto HandleResponseAboutDevice = [](const auto& message_bytes) -> SerialAdapter_StatusTypes
		{
			SerialAdapter_StatusTypes return_status_type;

			if (auto status_type = magic_enum::enum_cast<SerialAdapter_SystemPumpCommands>(static_cast<uint8_t>(message_bytes[Index_DeviceId])); status_type.has_value())
			{
				LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: StatusType -> SerialAdapter_SystemPumpCommands: {}", magic_enum::enum_name(status_type.value())));
				return status_type.value();
			}
			else if (auto aux_status_type = magic_enum::enum_cast<Auxillaries::JandyAuxillaryIds>(static_cast<uint8_t>(message_bytes[Index_DeviceId]) - SERIALADAPTER_AUX_ID_OFFSET); aux_status_type.has_value())
			{
				LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: StatusType -> Auxillaries::JandyAuxillaryIds: {}", magic_enum::enum_name(aux_status_type.value())));
				return aux_status_type.value();
			}

			return SerialAdapter_StatusTypes();
		};

		//
		// Determine the status type of the message.
		//

		switch (message_bytes[Index_StatusType])
		{
		case 0x02:
			m_StatusType = SerialAdapter_UnknownCommands::ErrorOccurred;
			break;

		case 0x03:
			m_StatusType = HandleResponseAboutDevice(message_bytes);
			break;

		default:
			if (auto status_type = magic_enum::enum_cast<SerialAdapter_ConfigControlCommands>(static_cast<uint8_t>(message_bytes[Index_StatusType])); status_type.has_value())
			{
				LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: StatusType -> SerialAdapter_ConfigControlCommands: {}", magic_enum::enum_name(status_type.value())));
				m_StatusType = status_type.value();
			}
			else if (auto config_status_type = magic_enum::enum_cast<SerialAdapter_SystemConfigurationStatuses>(static_cast<uint8_t>(message_bytes[Index_StatusType])); config_status_type.has_value())
			{
				LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: StatusType -> SerialAdapter_SystemConfigurationStatuses: {}", magic_enum::enum_name(config_status_type.value())));
				m_StatusType = config_status_type.value();
			}
			else if (auto temp_status_type = magic_enum::enum_cast<SerialAdapter_SystemTemperatureCommands>(static_cast<uint8_t>(message_bytes[Index_StatusType])); temp_status_type.has_value())
			{
				LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: StatusType -> SerialAdapter_SystemTemperatureCommands: {}", magic_enum::enum_name(temp_status_type.value())));
				m_StatusType = temp_status_type.value();
			}
			else
			{
				LogDebug(Channel::Messages, "SerialAdapterMessage_DevStatus: StatusType -> Unknown");
				m_StatusType = SerialAdapter_UnknownCommands::Unknown;
			}
			break;
		}

		//
		// Attempt to decode the message based on the status type.
		//

		std::visit(
			Utility::OverloadedVisitor
			{
				[](std::monostate)
				{
					LogWarning(Channel::Messages, "SerialAdapterMessage_DevStatus: Invalid Status Type");
				},
				[this, &message_bytes](SerialAdapter_ConfigControlCommands)
				{
					// Intentionally empty: config-control command responses carry no decodable payload here.
				},
				[this, &message_bytes](SerialAdapter_SystemConfigurationStatuses sa_scs)
				{
					auto make_battery_condition = [](const auto& message_bytes) -> SerialAdapter_SCS_BatteryCondition
					{
						SerialAdapter_SCS_BatteryCondition battery_condition;

						battery_condition.IsLow = static_cast<bool>(message_bytes[5] & 0x04);

						auto voltage_bits_part1 = static_cast<uint16_t>((message_bytes[Index_BatteryVoltage_Part1] & 0x03) << 8);
						auto voltage_bits_part2 = message_bytes[Index_BatteryVoltage_Part2];
						auto voltage_bits = voltage_bits_part1 + voltage_bits_part2;
						auto voltage = static_cast<double>(voltage_bits) / 100.0f;

						battery_condition.Voltage = voltage * AqualinkAutomate::Units::volts;

						return battery_condition;
					};

					auto make_options = [](uint8_t options_byte) -> SerialAdapter_SCS_Options
					{
						// The OPTIONS value is a BIT-MASK of the Power Center's S1 option DIP-switch
						// bank -- one bit per switch, LSB first (bit 0 == S1 DIP #1). The switch
						// functions are from the AquaLink RS installation manual (P/N 6840, "Section
						// 4.1 DIP Switch Functions" and the S1 tables in 4.2 / 4.3):
						//
						//   bit 0 (#1) AUX 1 controls the pool cleaner       -> HasCleaner
						//   bit 1 (#2) AUX 2 controls filter-pump low speed  -> TwoSpeedPump
						//   bit 2 (#3) AUX 3 controls spa spillover          -> HasSpillover
						//   bit 3 (#4) heater cool-down disabled             -> HeaterCoolDownDisabled
						//   bit 4 (#5) factory calibration ("factory use")   -> ServiceCalibrationMode
						//   bit 5 (#6) spare AUX with filter pump + spa      -> SpareAuxOnWithFilterPumpAndSpa
						//   bit 6 (#7) see caveat below                      -> CommonHeaterForSpaAndPool
						//   bit 7 (#8) heat pump instead of gas heater       -> ExtraDelayForHeatPump
						//
						// Bits 0-4 and 7 are a direct, confirmed match for the manual's switch list,
						// and bits 0 and 2 are the only ones any consumer currently reads (the AUX1 ->
						// CLEANR and AUX3 -> SPILLOVER poll rewrites in Slot_SerialAdapter_DevStatus).
						//
						// CAPTURE-GATED caveat on bits 5/6: manual 6840 documents S1 #6 as carrying
						// BOTH meanings depending on system type (combo: spare AUX activates with the
						// filter pump in spa mode; dual equipment: pool and spa share one heater) and
						// lists S1 #7 as "not used" / air-sensor-becomes-solar-sensor. So the struct's
						// CommonHeaterForSpaAndPool name at bit 6 is not pinned by that manual. Left
						// as a straight positional bit image rather than folding #6's second meaning
						// into bit 5, so the struct stays a faithful picture of the wire byte.
						//
						// CAPTURE-GATED alternative hypothesis: docs/alwin32/ctrlpnl-simio.md recovers
						// the Alwin32 simulator's INTERNAL S1 byte with a different bit assignment
						// (0x01=No Cool, 0x02=Calibrate, 0x04=CL JVA Asn, 0x08=Heat Delay, 0x10=Cleaner,
						// 0x20=Heat Pump, 0x80=Spillover). That byte is PowerCenter-internal shared
						// memory that the same doc states does not appear on the RS-485 wire, and its
						// own notes warn the simulator's layout does not mirror the physical switch
						// order -- so the manual's DIP numbering is used here. Confirm against a live
						// capture from a panel with a known S1 configuration.
						SerialAdapter_SCS_Options options{};

						options.HasCleaner                     = (0U != (options_byte & 0x01U));
						options.TwoSpeedPump                   = (0U != (options_byte & 0x02U));
						options.HasSpillover                   = (0U != (options_byte & 0x04U));
						options.HeaterCoolDownDisabled         = (0U != (options_byte & 0x08U));
						options.ServiceCalibrationMode         = (0U != (options_byte & 0x10U));
						options.SpareAuxOnWithFilterPumpAndSpa = (0U != (options_byte & 0x20U));
						options.CommonHeaterForSpaAndPool      = (0U != (options_byte & 0x40U));
						options.ExtraDelayForHeatPump          = (0U != (options_byte & 0x80U));

						return options;
					};

					switch (sa_scs)
					{
					case SerialAdapter_SystemConfigurationStatuses::MODEL:
						m_ModelType = static_cast<uint16_t>(message_bytes[Index_ModelType_Part1] << 8) + message_bytes[Index_ModelType_Part2];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: ModelType -> {}", m_ModelType.value()));
						break;

					case SerialAdapter_SystemConfigurationStatuses::OPMODE:
						m_OpMode = magic_enum::enum_cast<SerialAdapter_SCS_OpModes>(message_bytes[Index_OpMode]).value_or(SerialAdapter_SCS_OpModes::Unknown);
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: OpMode -> {}", magic_enum::enum_name(m_OpMode.value())));
						break;

					case SerialAdapter_SystemConfigurationStatuses::OPTIONS:
						m_Options = make_options(message_bytes[Index_Options]);
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: Options -> {:08B}", message_bytes[Index_Options]));
						break;

					case SerialAdapter_SystemConfigurationStatuses::VBAT:
						m_BatteryCondition = make_battery_condition(message_bytes);
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: BatteryCondition -> {} ({})", m_BatteryCondition.value().Voltage, m_BatteryCondition.value().IsLow ? "Low" : "Okay"));
						break;

					case SerialAdapter_SystemConfigurationStatuses::LEDS:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: LEDs -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;
					}
				},
				[this, &message_bytes](SerialAdapter_SystemPumpCommands sa_spc)
				{
					using enum SerialAdapter_SystemPumpCommands;

					switch (sa_spc)
					{
					case CLEANR:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: Cleaner -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;

					case SPILLOVER:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: Spillover -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;

					case PUMP:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: Pump -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;

					case PUMPLO:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: PumpLowSpeed -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;

					case SPA:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: Spa -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;
					}
				},
				[this, &message_bytes](SerialAdapter_SystemTemperatureCommands sa_stc)
				{
					// Temperature/setpoint values are raw integer degrees in the system's configured
					// units (Fahrenheit or Celsius). The UNITS command determines interpretation.
					// Unit-aware conversion to Temperature objects happens in the message processor
					// which has access to DataHub's SystemTemperatureUnits().

					using enum SerialAdapter_SystemTemperatureCommands;

					switch (sa_stc)
					{
					case UNITS:
						m_TemperatureUnits = (0x00 == message_bytes[Index_TemperatureUnits]) ? Kernel::TemperatureUnits::Fahrenheit : Kernel::TemperatureUnits::Celsius;
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: TemperatureUnits -> {}", magic_enum::enum_name(m_TemperatureUnits.value())));
						break;

					case POOLHT:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: PoolHeat -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break; 

					case POOLHT2:
						// CAPTURE-GATED: documented as a boolean enable (st); decode the STC value
						// byte (Index_PoolTemperature_SetPoint == 6, the shared value slot for every
						// STC command -- byte[4] is the command selector) as "TEMP2 maintenance
						// heating enabled". Robust to a 3-state wire encoding (any non-zero ==
						// enabled). Not yet validated on a live capture.
						m_PoolHeater_Two_Enabled = (0x00 != message_bytes[Index_PoolTemperature_SetPoint]);
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: PoolHeat2 -> {:02x} {:02x} {:02x} {:02x} (TEMP2 enabled={})", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7], m_PoolHeater_Two_Enabled.value()));
						break;

					case SPAHT:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: SpaHeat -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;

					case SOLHT:
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: SolarHeat -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
						break;

					case POOLSP:
						m_PoolTemperature_SetPoint_One = message_bytes[Index_PoolTemperature_SetPoint];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: PoolTemp SetPoint 1 -> {}", m_PoolTemperature_SetPoint_One.value()));
						break;

					case POOLSP2:
						m_PoolTemperature_SetPoint_Two = message_bytes[Index_PoolTemperature_SetPoint];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: PoolTemp SetPoint 2 -> {}", m_PoolTemperature_SetPoint_Two.value()));
						break;

					case SPASP:
						m_SpaTemperature_SetPoint = message_bytes[Index_SpaTemperature_SetPoint];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: SpaTemp SetPoint -> {}", m_SpaTemperature_SetPoint.value()));
						break;

					case POOLTMP:
						m_PoolTemperature = message_bytes[Index_PoolTemperature];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: PoolTemp -> {}", m_PoolTemperature.value()));
						break;

					case SPATMP:
						m_SpaTemperature = message_bytes[Index_SpaTemperature];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: SpaTemp -> {}", m_SpaTemperature.value()));
						break;

					case AIRTMP:
						m_AirTemperature = message_bytes[Index_AirTemperature];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: AirTemp -> {}", m_AirTemperature.value()));
						break;

					case SOLTMP:
						m_SolarTemperature = message_bytes[Index_SolarTemperature];
						LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: SolarTemp -> {}", m_SolarTemperature.value()));
						break;
					}
				},
				[this, &message_bytes](Auxillaries::JandyAuxillaryIds sa_jai)
				{
					auto status = magic_enum::enum_cast<Auxillaries::JandyAuxillaryStatuses>(message_bytes[Index_AuxState]).value_or(Auxillaries::JandyAuxillaryStatuses::Unknown);

					m_Aux_State = std::make_tuple(sa_jai, status);

					LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: {} Status -> {}", magic_enum::enum_name(sa_jai), magic_enum::enum_name(status)));
				},
				[this, &message_bytes](SerialAdapter_UnknownCommands)
				{
					LogDebug(Channel::Messages, "SerialAdapterMessage_DevStatus: Unknown, error, and/or unhandled status type");
					LogDebug(Channel::Messages, std::format("SerialAdapterMessage_DevStatus: Unknown/Error -> {:02x} {:02x} {:02x} {:02x}", message_bytes[4], message_bytes[5], message_bytes[6], message_bytes[7]));
				}
			},
			m_StatusType
		);

		return true;
	}

}
// namespace AqualinkAutomate::Messages
