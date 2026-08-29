#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/signals2.hpp>

#include "interfaces/ihub.h"
#include "kernel/body_of_water.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/circulation.h"
#include "kernel/equipment_validation.h"
#include "kernel/equipment_versions.h"
#include "kernel/hub_events/data_hub_config_event.h"
#include "utility/timeout_duration_string_converter.h"
#include "kernel/device_graph/device_graph.h"
#include "kernel/orp.h"
#include "kernel/ph.h"
#include "kernel/pool_configurations.h"
#include "kernel/powercenter.h"
#include "kernel/temperature.h"
#include "kernel/system_boards.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_devices/chlorinator_status.h"
#include "logging/logging.h"
#include "types/units_dimensionless.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Kernel
{

	// Forward declarations for the concrete config-update event types populated
	// by the temperature/chemistry/circulation emit helpers. The full definitions
	// are only required in data_hub.cpp where the helpers are defined.
	class DataHub_ConfigEvent_Temperature;
	class DataHub_ConfigEvent_Chemistry;
	class DataHub_ConfigEvent_Circulation;
	class DataHub_ConfigEvent_ButtonStateChange;

	enum class EquipmentMode
	{
		Normal,
		Service,
		TimeOut
	};

	enum class ConfigurationSource
	{
		Auto,
		UserSpecified
	};

	class DataHub : public Interfaces::IHub
	{
	public:
		DataHub();

	//---------------------------------------------------------------------
	// UPDATES / NOTIFICATIONS / EVENTS
	//---------------------------------------------------------------------

	public:
		mutable boost::signals2::signal<void(std::shared_ptr<Kernel::DataHub_ConfigEvent>)> ConfigUpdateSignal;

	public:
		// Emits a button-state-change event, de-duplicated per button. Status processors re-scrape
		// and re-publish on every poll; this only fans out when the (status, label) for a button
		// actually differs from the last published value, avoiding redundant WebSocket/MQTT churn.
		void EmitButtonStateChange(const boost::uuids::uuid& button_id, std::string_view status, std::string_view label);

	//---------------------------------------------------------------------
	// EQUIPMENT CONFIGURATION
	//---------------------------------------------------------------------

	public:
		Kernel::EquipmentMode Mode{ Kernel::EquipmentMode::Normal };
		Kernel::PoolConfigurations PoolConfiguration{ Kernel::PoolConfigurations::Unknown };
		Kernel::ConfigurationSource PoolConfigurationSource{ Kernel::ConfigurationSource::Auto };
		Kernel::SystemBoards SystemBoard{ Kernel::SystemBoards::Unknown };
		bool EmulationDisabled{ false };
		// When true, the RSSA presence-gating (auto-suppress an emulated Serial Adapter
		// on detecting a real one) is disabled. A safety opt-out: the detection can
		// false-positive on the controller's own status push to our emulated adapter.
		bool PresenceGatingDisabled{ false };

		// Expected equipment layout for the detected model (from PoolConfigurationDecoder);
		// 0 until the version page is scraped. Used to validate the discovered equipment set.
		uint8_t ExpectedAuxillaryCount{ 0 };
		uint8_t ExpectedPowerCenterCount{ 0 };

		// Outcome of cross-checking discovered equipment against the expected layout; set once
		// the startup scrape completes (std::nullopt until then).
		std::optional<Kernel::EquipmentValidation> EquipmentValidationResult;

	//---------------------------------------------------------------------
	// EQUIPMENT STATUS
	//---------------------------------------------------------------------

		Utility::TimeoutDurationStringConverter TimeoutRemaining;
		std::chrono::year_month_day Date{ std::chrono::year{2000}, std::chrono::month{1}, std::chrono::day{1} };
		std::chrono::hh_mm_ss<std::chrono::milliseconds> Time{ std::chrono::milliseconds(0) };

	//---------------------------------------------------------------------
	// EQUIPMENT VERSIONS
	//---------------------------------------------------------------------

		Kernel::EquipmentVersions EquipmentVersions;

	//---------------------------------------------------------------------
	// SPA-SIDE SWITCH BUTTON ASSIGNMENTS
	//---------------------------------------------------------------------
	// The controller's spa-side switch button->function map, decoded from its config UI:
	// the iAQ "Spa Remotes" page or the OneTouch "Spa Switch" menu. Stored CONTROLLER-AGNOSTICALLY
	// (keyed by 1-based (switch, button)) so whichever controller a given system has populates the
	// same map -- the read path works for iAQ and OneTouch installs alike.

	public:
		void SetSpaSwitchAssignment(uint8_t switch_number, uint8_t button_number, std::string_view function);
		std::optional<std::string> SpaSwitchAssignment(uint8_t switch_number, uint8_t button_number) const;
		const std::map<std::pair<uint8_t, uint8_t>, std::string>& SpaSwitchAssignments() const;

	private:
		std::map<std::pair<uint8_t, uint8_t>, std::string> m_SpaSwitchAssignments;

	//---------------------------------------------------------------------
	// CIRCULATION MODES
	//---------------------------------------------------------------------

	public:
		CirculationModes CirculationMode{ CirculationModes::Pool };

		// Update the decoded circulation mode and, for a dual-body system, the active
		// body that follows it. Fans out a Circulation config event to WS/MQTT consumers
		// only when the resolved state actually changes (callers may invoke it every
		// status poll). This is the single authority for live circulation-state changes.
		void SetCirculationMode(CirculationModes mode);

		bool SpaMode() const
		{
			using enum CirculationModes;
			return Spa == CirculationMode
				|| SpaFill == CirculationMode
				|| SpaDrain == CirculationMode;
		}

		bool InCleanMode{ false };

	//---------------------------------------------------------------------
	// BODIES OF WATER
	//---------------------------------------------------------------------

		// single_body_kind selects which body a SingleBody configuration creates: Pool (default)
		// or Spa. Pool-only and spa-only installs are indistinguishable on the wire (single body,
		// single equipment - set by the Power Center DIP switches), so a spa-only install can only
		// be signalled by the user via --pool-configuration spa-only. Auto-detect and cache-restore
		// cannot tell them apart and therefore always default to Pool. Ignored for DualBody configs.
		void ApplyPoolConfiguration(PoolConfigurations config, ConfigurationSource source = ConfigurationSource::UserSpecified, BodyOfWaterIds single_body_kind = BodyOfWaterIds::Pool);
		void AddBody(BodyOfWater body);
		std::optional<std::reference_wrapper<BodyOfWater>> GetBody(BodyOfWaterIds id);
		std::optional<std::reference_wrapper<const BodyOfWater>> GetBody(BodyOfWaterIds id) const;
		std::optional<std::reference_wrapper<BodyOfWater>> ActiveBody();
		const std::vector<BodyOfWater>& Bodies() const;

	private:
		std::vector<BodyOfWater> m_Bodies;

	//---------------------------------------------------------------------
	// TEMPERATURES
	//---------------------------------------------------------------------

	public:
		std::optional<Kernel::Temperature> AirTemp() const;
		std::optional<Kernel::Temperature> PoolTemp() const;
		std::optional<Kernel::Temperature> SpaTemp() const;
		std::optional<Kernel::Temperature> PoolTempSetpoint() const;
		std::optional<Kernel::Temperature> PoolTempSetpoint2() const;
		std::optional<bool> PoolHeater2Enabled() const;
		std::optional<Kernel::Temperature> SpaTempSetpoint() const;
		std::optional<Kernel::Temperature> FreezeProtectPoint() const;
		Kernel::TemperatureUnits SystemTemperatureUnits() const;

		// Wall-clock time each temperature channel was last written from the wire (nullopt until
		// first set). The controller only reports temperatures while the filter pump runs, and for
		// a combo system only for the active body, so a cached value can persist long after it
		// stopped being refreshed - the timestamp lets consumers detect that.
		std::optional<std::chrono::system_clock::time_point> AirTempUpdatedAt() const;
		std::optional<std::chrono::system_clock::time_point> PoolTempUpdatedAt() const;
		std::optional<std::chrono::system_clock::time_point> SpaTempUpdatedAt() const;
		std::optional<std::chrono::system_clock::time_point> PoolTempSetpointUpdatedAt() const;
		std::optional<std::chrono::system_clock::time_point> SpaTempSetpointUpdatedAt() const;
		std::optional<std::chrono::system_clock::time_point> FreezeProtectPointUpdatedAt() const;

		// True when a live temperature is older than TemperatureStalenessThreshold (pure age; a
		// never-set/null temperature is not "stale").
		bool AirTempIsStale() const;
		bool PoolTempIsStale() const;
		bool SpaTempIsStale() const;

		// The current temperature to REPORT for a body, accounting for the combo-system reality
		// that the controller only meaningfully reports the ACTIVE body's temperature: a dual-body
		// system keeps broadcasting a junk value (e.g. ~1C) for the inactive body, so we surface an
		// inactive body's current temp as unavailable (nullopt) rather than a misleading number.
		// For a body that doesn't exist, or the (always-active) single body, returns its temp as-is.
		std::optional<Kernel::Temperature> CurrentTempForReporting(Kernel::BodyOfWaterIds body_id) const;

		// Age beyond which a temperature is considered stale (see *IsStale()). Set at startup from
		// --temperature-staleness-threshold; default 10 minutes.
		std::chrono::seconds TemperatureStalenessThreshold{ std::chrono::seconds(600) };

		void AirTemp(const Kernel::Temperature& air_temp);
		void PoolTemp(const Kernel::Temperature& pool_temp);
		void SpaTemp(const Kernel::Temperature& spa_temp);
		void PoolTempSetpoint(const Kernel::Temperature& pool_temp_setpoint);
		void PoolTempSetpoint2(const Kernel::Temperature& pool_temp_setpoint_2);
		void PoolHeater2Enabled(bool pool_heater_2_enabled);
		void SpaTempSetpoint(const Kernel::Temperature& spa_temp_setpoint);
		void FreezeProtectPoint(const Kernel::Temperature& freeze_protect_point);
		void SystemTemperatureUnits(Kernel::TemperatureUnits units);

	private:
		std::optional<Kernel::Temperature> m_AirTemp;
		std::optional<Kernel::Temperature> m_PoolTemp;
		std::optional<Kernel::Temperature> m_SpaTemp;
		std::optional<Kernel::Temperature> m_PoolTempSetpoint;
		// Second pool setpoint -- POOLSP2, the panel's "TEMP2" maintenance (low) temperature. Only
		// present on single-body (Pool-Only/Spa-Only) systems, where TEMP1 (m_PoolTempSetpoint) is
		// the priority temperature and TEMP2 is a lower maintenance setpoint for the same body.
		std::optional<Kernel::Temperature> m_PoolTempSetpoint2;
		// Whether the panel's "TEMP2" maintenance heating (POOLHT2) is enabled. CAPTURE-GATED: the
		// underlying wire byte encoding is documented (boolean enable) but not yet live-validated.
		std::optional<bool> m_PoolHeater2Enabled;
		std::optional<Kernel::Temperature> m_SpaTempSetpoint;
		std::optional<Kernel::Temperature> m_FreezeProtectPoint;
		Kernel::TemperatureUnits m_SystemTemperatureUnits{ Kernel::TemperatureUnits::Fahrenheit };

		std::optional<std::chrono::system_clock::time_point> m_AirTempUpdatedAt;
		std::optional<std::chrono::system_clock::time_point> m_PoolTempUpdatedAt;
		std::optional<std::chrono::system_clock::time_point> m_SpaTempUpdatedAt;
		std::optional<std::chrono::system_clock::time_point> m_PoolTempSetpointUpdatedAt;
		std::optional<std::chrono::system_clock::time_point> m_SpaTempSetpointUpdatedAt;
		std::optional<std::chrono::system_clock::time_point> m_FreezeProtectPointUpdatedAt;

		// Shared staleness test: a value is stale only if it has been set and exceeds the threshold.
		bool IsStale(const std::optional<std::chrono::system_clock::time_point>& updated_at) const;

	//---------------------------------------------------------------------
	// CHEMISTRY
	//---------------------------------------------------------------------
		 
	public:
		Kernel::ORP ORP() const;
		Kernel::pH pH() const;
		ppm_quantity SaltLevel() const;

		void ORP(const Kernel::ORP& orp);
		void pH(const Kernel::pH& pH);
		void SaltLevel(const ppm_quantity& salt_level_in_ppm);

	private:
		Kernel::ORP m_ORP{ 0.0f };
		Kernel::pH m_pH{ 0.0f };
		ppm_quantity m_SaltLevel{ 0 };

	//---------------------------------------------------------------------
	// DEVICES GRAPH
	//---------------------------------------------------------------------

	public:
		DevicesGraph Devices{};

	public:
		std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> DevicesOfType(AuxillaryTraitsTypes::AuxillaryTypes type) const;
		std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> Auxillaries() const;
		std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> Chlorinators() const;
		std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> Heaters() const;
		std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> Lights() const;
		std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> Pumps() const;
		std::vector<std::shared_ptr<Kernel::AuxillaryDevice>> FilterPumps() const;

		// Count / existence predicates that avoid materialising a vector for the
		// common size() / empty() checks on the status hot path.
		uint32_t CountOfType(AuxillaryTraitsTypes::AuxillaryTypes type) const;
		bool HasAnyOfType(AuxillaryTraitsTypes::AuxillaryTypes type) const;
		uint32_t CountFilterPumps() const;
		bool HasAnyFilterPumps() const;

		[[deprecated("Use FilterPumps() instead; that returns a collection of pumps as there might be more than one")]]
		std::optional<std::shared_ptr<Kernel::AuxillaryDevice>> FilterPump();

		// Is any discovered filter pump running? nullopt when none has been discovered yet, so a
		// caller can tell "no pump running" from "we do not know about a pump".
		std::optional<bool> AnyFilterPumpRunning() const;

		// ---- Chlorinator derived values -------------------------------------------------
		// Shared by every consumer (web, MQTT, Home Assistant) so they cannot disagree about
		// what the same cell is doing.

		// The configured output target for whichever body is currently ACTIVE, falling back to
		// the other body's value and finally to the last-known generating %. Distinct from the
		// instantaneous output, which is 0 whenever the cell is idle.
		std::optional<uint8_t> ChlorinatorResolvedSetpoint(const AuxillaryDevice& chlorinator) const;

		// Why the cell is (or is not) producing right now -- "0%" alone cannot distinguish a
		// chlorinator that is switched off from one that is simply waiting for the filter pump.
		ChlorinatorGeneratingReason ResolveChlorinatorGeneratingReason(const AuxillaryDevice& chlorinator) const;

	//---------------------------------------------------------------------
	// CONFIG-UPDATE EVENT EMITTERS
	//---------------------------------------------------------------------

	private:
		void EmitTemperatureEvent(const std::function<void(DataHub_ConfigEvent_Temperature&)>& populate) const;
		void EmitChemistryEvent(const std::function<void(DataHub_ConfigEvent_Chemistry&)>& populate) const;
		void EmitCirculationEvent(const std::function<void(DataHub_ConfigEvent_Circulation&)>& populate) const;

	private:
		// Last published (status, label) per button id, used to de-duplicate button-state changes.
		std::map<boost::uuids::uuid, std::pair<std::string, std::string>> m_LastButtonState;

	};

}
// namespace AqualinkAutomate::Equipment
