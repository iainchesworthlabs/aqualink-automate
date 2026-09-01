#pragma once

#include <set>
#include <string>

#include <boost/system/error_code.hpp>

#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_devices/chlorinator_boost_mode.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "kernel/auxillary_devices/heater_status.h"
#include "kernel/auxillary_devices/pump_speed.h"
#include "kernel/auxillary_devices/pump_status.h"
#include "kernel/auxillary_devices/pump_type.h"
#include "kernel/auxillary_traits/auxillary_traits_base.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/flow_rate.h"
#include "kernel/temperature.h"

namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes
{

	//---------------------------------------------------------------------
	// COMMMON TRAITS
	//---------------------------------------------------------------------

	enum class AuxillaryTypes
	{
		Auxillary,
		Chlorinator,
		Cleaner,
		Heater,
		Light,
		Pump,
		Spillover,
		Sprinkler,
		Unknown
	};

	class AuxillaryTypeTrait : public ImmutableTraitType<const AuxillaryTypes>
	{
	public:
		TraitKey Name() const final { return std::string{"AuxillaryTypesTrait"}; }
	};

	class DutyCycleTrait : public MutableTraitType<uint8_t>
	{
	public:
		TraitKey Name() const final { return std::string{"DutyCycleTrait"}; }
	};

	class ErrorCodesTrait : public ImmutableTraitType<std::set<boost::system::error_code>>
	{
	public:
		TraitKey Name() const final { return std::string{"ErrorCodesTrait"}; }
	};

	class LabelTrait : public MutableTraitType<const std::string>
	{
	public:
		static const std::string COMMON_LABEL_FILTER_PUMP;

	public:
		TraitKey Name() const final { return std::string{"LabelTrait"}; }
	};

	// The protocol-native short identity label for a device (e.g. "Aux5"), as opposed to
	// the user-facing LabelTrait (which may be a friendly name like "Pool Light"). Immutable
	// because it reflects the hardware address, not a name the user can change. Carries the
	// aux id to layers (core cache, web display) that cannot see protocol-specific traits.
	class HardwareLabelTrait : public ImmutableTraitType<const std::string>
	{
	public:
		TraitKey Name() const final { return std::string{"HardwareLabelTrait"}; }
	};

	class StatusTrait : public ImmutableTraitType<const std::string>
	{
	public:
		TraitKey Name() const final { return std::string{"StatusTrait"}; }
	};

	//---------------------------------------------------------------------
	// AUXILLARY TRAITS
	//---------------------------------------------------------------------

	class AuxillaryStatusTrait : public MutableTraitType<AuxillaryStatuses>
	{
	public:
		TraitKey Name() const final { return std::string{"AuxillaryStatusTrait"}; }
	};

	//---------------------------------------------------------------------
	// CHLORINATOR TRAITS
	//---------------------------------------------------------------------

	class ChlorinatorStatusTrait : public MutableTraitType<ChlorinatorStatuses>
	{
	public:
		TraitKey Name() const final { return std::string{"ChlorinatorStatusTrait"}; }
	};

	class GeneratingPercentageTrait : public MutableTraitType<uint8_t>
	{
	public:
		TraitKey Name() const final { return std::string{"GeneratingPercentageTrait"}; }
	};

	class BoostModeTrait : public MutableTraitType<ChlorinatorBoostModes>
	{
	public:
		TraitKey Name() const final { return std::string{"BoostModeTrait"}; }
	};

	class ChlorinatorHealthTrait : public MutableTraitType<ChlorinatorHealth>
	{
	public:
		TraitKey Name() const final { return std::string{"ChlorinatorHealthTrait"}; }
	};

	// Every ChlorinatorHealth flag the cell is currently reporting simultaneously (the wire
	// status byte is a true bitfield - see docs/alwin32_simulator_protocol.md - so more than
	// one warning/fault can be active at once). ChlorinatorHealthTrait remains the single
	// worst-of-the-set value for backward-compatible consumers; this trait carries the full
	// set for anyone who needs it. Iteration order is enum declaration order (std::set), NOT
	// severity order. Always written together with ChlorinatorHealthTrait so the two can
	// never disagree - see AquariteDevice::PushPPMToDataHub.
	class ChlorinatorHealthFlagsTrait : public MutableTraitType<std::set<ChlorinatorHealth>>
	{
	public:
		TraitKey Name() const final { return std::string{"ChlorinatorHealthFlagsTrait"}; }
	};

	// Configured POOL output setpoint % (the value the user has dialled in on the
	// "Set AquaPure" menu), as distinct from GeneratingPercentageTrait which is the
	// instantaneous output (0 while the cell is idle). Populated by the OneTouch/iAQ
	// menu scrape; persists across idle periods so the dashboard can show a target.
	class ChlorinatorPoolSetpointTrait : public MutableTraitType<uint8_t>
	{
	public:
		TraitKey Name() const final { return std::string{"ChlorinatorPoolSetpointTrait"}; }
	};

	// Configured SPA output setpoint % (see ChlorinatorPoolSetpointTrait).
	class ChlorinatorSpaSetpointTrait : public MutableTraitType<uint8_t>
	{
	public:
		TraitKey Name() const final { return std::string{"ChlorinatorSpaSetpointTrait"}; }
	};

	// Passive fallback for the configured setpoint when no menu scrape is available
	// (e.g. pure passive-sniff mode): the last REAL, non-zero generating % seen on the
	// AQUARITE_Percent (0x11) wire stream, excluding the 0-idle / 101-boost / 255-service
	// sentinels. Used to seed the dashboard target until/unless an authoritative scrape lands.
	class ChlorinatorLastGeneratingTrait : public MutableTraitType<uint8_t>
	{
	public:
		TraitKey Name() const final { return std::string{"ChlorinatorLastGeneratingTrait"}; }
	};

	//---------------------------------------------------------------------
	// HEATER TRAITS
	//---------------------------------------------------------------------

	class HeaterStatusTrait : public MutableTraitType<HeaterStatuses>
	{
	public:
		TraitKey Name() const final { return std::string{"HeaterStatusTrait"}; }
	};

	class TargetTemperatureTrait : public MutableTraitType<Temperature>
	{
	public:
		TraitKey Name() const final { return std::string{"TargetTemperatureTrait"}; }
	};

	//---------------------------------------------------------------------
	// LIGHT TRAITS
	//---------------------------------------------------------------------

	class BrightnessLevelTrait : public MutableTraitType<uint8_t>
	{
	public:
		TraitKey Name() const final { return std::string{"BrightnessLevelTrait"}; }
	};

	class ColourTrait : public MutableTraitType<uint8_t>
	{
	public:
		TraitKey Name() const final { return std::string{"ColourTrait"}; }
	};

	//---------------------------------------------------------------------
	// PUMP TRAITS
	//---------------------------------------------------------------------

	class PumpSpeedTrait : public MutableTraitType<PumpSpeeds>
	{
	public:
		TraitKey Name() const final { return std::string{ "PumpSpeedTrait" }; }
	};

	class PumpStatusTrait : public MutableTraitType<PumpStatuses>
	{
	public:
		TraitKey Name() const final { return std::string{ "PumpStatusTrait" }; }
	};

	class PumpTypeTrait : public MutableTraitType<PumpTypes>
	{
	public:
		TraitKey Name() const final { return std::string{ "PumpTypeTrait" }; }
	};

	class FlowRateTrait : public MutableTraitType<Kernel::FlowRate>
	{
	public:
		TraitKey Name() const final { return std::string{"FlowRateTrait"}; }
	};

	//---------------------------------------------------------------------
	// BODY-OF-WATER TRAITS
	//---------------------------------------------------------------------

	class BodyOfWaterTrait : public MutableTraitType<BodyOfWaterIds>
	{
	public:
		TraitKey Name() const final { return std::string{"BodyOfWaterTrait"}; }
	};

	//---------------------------------------------------------------------
	// .... TRAITS
	//---------------------------------------------------------------------



}
// namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes
