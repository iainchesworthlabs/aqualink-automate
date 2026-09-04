#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <span>
#include <variant>
#include <vector>

#include "interfaces/imessagesignal_recv.h"
#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_status.h"
#include "messages/serial_adapter/serial_adapter_message.h"
#include "kernel/temperature.h"
#include "types/units_electric_potential.h"

namespace AqualinkAutomate::Messages
{

	enum class SerialAdapter_CommandTypes : uint8_t
	{
		Query = 0x05,
		Toggle = 0xFF,	// Actually Unknown Value
		SetOff = 0x80,
		SetOn = 0x81,
		// CAPTURE-GATED (AqualinkD source/serialadapter.c): the data value of the
		// first of the two-step setpoint write -- readySP[] = {0x00,0x01,typeID,0x35}.
		// The second step (setSP) carries data value 0x00 with the raw temperature
		// supplied as the ack_type byte. Confirm against a live Brainboxes capture.
		ReadySetpoint = 0x35,
		Action = 0xFF,	// Actually Unknown Value
		Unknown = 0xFF
	};

	enum class SerialAdapter_ConfigControlCommands : uint8_t
	{
		ECHOCMD = 0xFF,	// Actually Unknown Value
		RSPFMT = 0xFF,	// Actually Unknown Value
		RST = 0xFF,		// Actually Unknown Value
		VERS = 0xFF,	// Actually Unknown Value
		DIAG = 0xFF,	// Actually Unknown Value
		S1 = 0xFF,		// Actually Unknown Value
		CMDCHR = 0xFF,	// Actually Unknown Value
		NRMCHR = 0xFF,	// Actually Unknown Value
		ERRCHR = 0xFF,	// Actually Unknown Value
		COSMSGS = 0xFF	// Actually Unknown Value
	};

	enum class SerialAdapter_SystemConfigurationStatuses : uint8_t
	{
		MODEL = 0x00,
		OPTIONS = 0x01,
		OPMODE = 0x0D,
		VBAT = 0x0E,
		LEDS = 0xFF		// Actually Unknown Value
	};

	enum class SerialAdapter_SCS_OpModes : uint8_t
	{
		Auto = 0x00,
		Service = 0x01,
		Timeout = 0x02,
		Unknown = 0xFF
	};

	// Decoded form of the OPTIONS byte -- the Power Center's S1 option DIP-switch bank,
	// one bit per switch, LSB first (bit 0 == S1 DIP #1 ... bit 7 == S1 DIP #8). Declaration
	// order IS the bit order; see the make_options lambda in DeserializeContents for the
	// per-bit provenance (AquaLink RS manual 6840 §4.1) and the capture-gated caveats.
	// Never build this by casting the raw byte -- that is a parenthesised aggregate
	// initialisation which lands the whole byte in HasCleaner and zeroes the rest.
	struct SerialAdapter_SCS_Options
	{
		bool HasCleaner : 1;
		bool TwoSpeedPump : 1;
		bool HasSpillover : 1;
		bool HeaterCoolDownDisabled : 1;
		bool ServiceCalibrationMode : 1;
		bool SpareAuxOnWithFilterPumpAndSpa : 1;
		bool CommonHeaterForSpaAndPool : 1;
		bool ExtraDelayForHeatPump : 1;
	};

	struct SerialAdapter_SCS_BatteryCondition
	{
		SerialAdapter_SCS_BatteryCondition() = default;
		SerialAdapter_SCS_BatteryCondition(SerialAdapter_SCS_BatteryCondition&& other) noexcept = default;
		SerialAdapter_SCS_BatteryCondition& operator=(SerialAdapter_SCS_BatteryCondition&& other) noexcept = default;
		SerialAdapter_SCS_BatteryCondition(const SerialAdapter_SCS_BatteryCondition&) = default;
		SerialAdapter_SCS_BatteryCondition& operator=(const SerialAdapter_SCS_BatteryCondition&) = default;

		bool IsLow{ true };
		AqualinkAutomate::Units::voltage Voltage{ 0 * AqualinkAutomate::Units::volts };
	};

	enum class SerialAdapter_SystemPumpCommands : uint8_t
	{
		PUMPLO = 0x0D,
		PUMP = 0x0C,
		CLEANR = 0x10,
		SPILLOVER = 0x0F,
		SPA = 0x0E
	};

	enum class SerialAdapter_SystemTemperatureCommands : uint8_t
	{
		UNITS = 0x0A,
		POOLHT = 0x11,
		POOLHT2 = 0x12,
		SPAHT = 0x13,
		SOLHT = 0x14,
		POOLSP = 0x05,
		POOLSP2 = 0x06,
		SPASP = 0x07,
		POOLTMP = 0x08,
		SPATMP = 0x0B,
		AIRTMP = 0x09,
		SOLTMP = 0x0C
	};

	enum class SerialAdapter_BAO_States : uint8_t
	{
		Off = 0x00,
		On = 0x01,
		Unknown = 0xFF
	};

	enum class SerialAdapter_ErrorCodes : uint8_t
	{
		AuxillaryUnavailable_OptionSwitchIsSet = 0x0E,
		AuxillaryUnavailable_NotInstalled = 0x11,
		Unknown = 0xFF
	};

	enum class SerialAdapter_UnknownCommands : uint8_t
	{
		ErrorOccurred = 0x02,
		Unknown = 0xFF
	};

	using SerialAdapter_StatusTypes = 
		std::variant<
			std::monostate,
			SerialAdapter_ConfigControlCommands, 
			SerialAdapter_SystemConfigurationStatuses, 
			SerialAdapter_SystemPumpCommands, 
			SerialAdapter_SystemTemperatureCommands, 
			Auxillaries::JandyAuxillaryIds, 
			SerialAdapter_UnknownCommands
		>;

	class SerialAdapterMessage_DevStatus : public SerialAdapterMessage, public Interfaces::IMessageSignalRecv<SerialAdapterMessage_DevStatus>
	{
		static const uint8_t Index_StatusType = 4;
		static const uint8_t Index_DeviceId = 7;
		static const uint8_t Index_ModelType_Part1 = 5;
		static const uint8_t Index_ModelType_Part2 = 6;
		static const uint8_t Index_OpMode = 6;
		static const uint8_t Index_Options = 6;
		static const uint8_t Index_BatteryVoltage_Part1 = 5;
		static const uint8_t Index_BatteryVoltage_Part2 = 6;
		static const uint8_t Index_TemperatureUnits = 6;
		static const uint8_t Index_PoolTemperature_SetPoint = 6;
		static const uint8_t Index_SpaTemperature_SetPoint = 6;
		static const uint8_t Index_AirTemperature = 6;
		static const uint8_t Index_PoolTemperature = 6;
		static const uint8_t Index_SolarTemperature = 6;
		static const uint8_t Index_SpaTemperature = 6;
		static const uint8_t Index_AuxState = 6;

	public:
		static const uint8_t SERIALADAPTER_AUX_ID_OFFSET = 0x14;

	public:
		SerialAdapterMessage_DevStatus() noexcept;
		explicit SerialAdapterMessage_DevStatus(const SerialAdapter_ConfigControlCommands  sa_ccc);
		explicit SerialAdapterMessage_DevStatus(const SerialAdapter_SystemConfigurationStatuses sa_scs);
		explicit SerialAdapterMessage_DevStatus(const SerialAdapter_SystemPumpCommands sa_spc);
		explicit SerialAdapterMessage_DevStatus(const SerialAdapter_SystemTemperatureCommands sa_stc);
		explicit SerialAdapterMessage_DevStatus(const Auxillaries::JandyAuxillaryIds sa_jai);
		~SerialAdapterMessage_DevStatus() override = default;

		std::optional<uint16_t> ModelType() const;
		std::optional<SerialAdapter_SCS_OpModes> OpMode() const;
		std::optional<SerialAdapter_SCS_Options> Options() const;
		std::optional<SerialAdapter_SCS_BatteryCondition> BatteryCondition() const;

		std::optional<Kernel::TemperatureUnits> TemperatureUnits() const;
		std::optional<uint8_t> Pool_SetPoint_One() const;
		std::optional<uint8_t> Pool_SetPoint_Two() const;
		std::optional<bool> Pool_Heater_Two_Enabled() const;
		std::optional<uint8_t> Spa_SetPoint() const;
		std::optional<uint8_t> AirTemperature() const;
		std::optional<uint8_t> PoolTemperature() const;
		std::optional<uint8_t> SolarTemperature() const;
		std::optional<uint8_t> SpaTemperature() const;

		std::optional<std::tuple<Auxillaries::JandyAuxillaryIds, std::optional<Auxillaries::JandyAuxillaryStatuses>>> AuxilliaryState() const;

		std::string ToString() const override;

		bool SerializeContents(std::vector<uint8_t>& message_bytes) const override;
		bool DeserializeContents(std::span<const uint8_t> message_bytes) override;

	private:
		SerialAdapter_StatusTypes m_StatusType;

		std::optional<uint16_t> m_ModelType{ std::nullopt };

		std::optional<SerialAdapter_SCS_OpModes> m_OpMode{ std::nullopt };
		std::optional<SerialAdapter_SCS_Options> m_Options{ std::nullopt };
		std::optional<SerialAdapter_SCS_BatteryCondition> m_BatteryCondition{ std::nullopt };

		std::optional<Kernel::TemperatureUnits> m_TemperatureUnits{ std::nullopt };
		std::optional<uint8_t> m_PoolTemperature_SetPoint_One{ std::nullopt };
		std::optional<uint8_t> m_PoolTemperature_SetPoint_Two{ std::nullopt };
		// POOLHT2 -- whether the panel's "TEMP2" maintenance heating is enabled. CAPTURE-GATED: the
		// serial-adapter host protocol documents POOLHT2 as a boolean enable (st: 1=enabled,
		// 0=disabled), and we decode byte[4] != 0 accordingly. This is robust even if the wire
		// actually carries the richer 3-state (any non-zero == "heating enabled"), but the exact
		// DevStatus byte layout is not yet validated against a live capture -- proven only by the
		// synthetic round-trip test here.
		std::optional<bool> m_PoolHeater_Two_Enabled{ std::nullopt };
		std::optional<uint8_t> m_SpaTemperature_SetPoint{ std::nullopt };
		std::optional<uint8_t> m_AirTemperature{ std::nullopt };
		std::optional<uint8_t> m_PoolTemperature{ std::nullopt };
		std::optional<uint8_t> m_SolarTemperature{ std::nullopt };
		std::optional<uint8_t> m_SpaTemperature{ std::nullopt };

		std::optional<std::tuple<Auxillaries::JandyAuxillaryIds, std::optional<Auxillaries::JandyAuxillaryStatuses>>> m_Aux_State{ std::nullopt };

		std::optional<Auxillaries::JandyAuxillaryStatuses> m_Cleaner_State;
		std::optional<Auxillaries::JandyAuxillaryStatuses> m_Spillover_State;
	};

}
// namespace AqualinkAutomate::Messages
