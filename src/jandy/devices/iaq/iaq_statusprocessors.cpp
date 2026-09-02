#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "logging/logging.h"
#include "devices/capabilities/screen.h"
#include "devices/iaq_device.h"
#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_presence_override.h"
#include "auxillaries/jandy_auxillary_reconciliation.h"
#include "auxillaries/jandy_auxillary_span.h"
#include "auxillaries/jandy_auxillary_status.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "factories/jandy_auxillary_factory.h"
#include "kernel/auxillary_devices/auxillary_status.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/hub_events/data_hub_config_event_button_state_change.h"
#include "utility/screen_data_page_processor.h"
#include "utility/screen_data_page_updater.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Devices
{

	void IAQDevice::ProcessMainStatus(const Messages::IAQMessage_MainStatus& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::ProcessMainStatus", std::source_location::current());

		// This id carries rich status (it is the AqualinkTouch 0x33 side, not the
		// heartbeat-only 0xA3 cloud interface).  Latch it so a later heartbeat keeps
		// the System Status page rather than flipping the screen to Cloud Link.
		m_HasReceivedMainStatus = true;

		// The home page is now established; an emulated panel with a survey armed walks its
		// data pages from here (runs once).
		MaybeStartPageSurvey();

		LogDebug(Channel::Devices, [this, &msg]() {
			auto temp_str = [](const std::optional<Kernel::Temperature>& temp)
			{
				return temp.has_value() ? std::format("{:.0f}F", temp->InFahrenheit().value()) : "n/a";
			};

			return std::format("IAQ ({}): Processing MainStatus: pool={}, spa={}, air={}, pump={}, pool_heat={}, spa_heat={}, solar={}",
				DeviceId(),
				temp_str(msg.PoolTemperature()),
				temp_str(msg.SpaTemperature()),
				temp_str(msg.AirTemperature()),
				msg.PumpOn(),
				magic_enum::enum_name(msg.PoolHeaterStatus()),
				magic_enum::enum_name(msg.SpaHeaterStatus()),
				magic_enum::enum_name(msg.SolarHeaterStatus())); });

		// Heuristic: if we see spa mode and have no bodies yet (IAQ-only setup),
		// infer DualBody_SharedEquipment and create both bodies. This runs BEFORE the
		// circulation update below so SetCirculationMode has both bodies to resolve the
		// active body against.
		if (msg.SpaMode() && m_DataHub->Bodies().empty()
			&& m_DataHub->PoolConfiguration == Kernel::PoolConfigurations::Unknown
			&& m_DataHub->PoolConfigurationSource == Kernel::ConfigurationSource::Auto)
		{
			// Body-building lives in ApplyPoolConfiguration so all three call sites stay consistent.
			m_DataHub->ApplyPoolConfiguration(Kernel::PoolConfigurations::DualBody_SharedEquipment, Kernel::ConfigurationSource::Auto);
			LogInfo(Channel::Devices, "IAQ: Auto-detected DualBody_SharedEquipment from MainStatus SpaMode");
		}

		// Update circulation mode + active body from the MainStatus spa-mode flag. This is
		// the single authority for decoded circulation state and fans out a CirculationUpdate
		// event to WS/MQTT consumers only when the resolved state actually changes.
		m_DataHub->SetCirculationMode(msg.SpaMode()
			? Kernel::CirculationModes::Spa
			: Kernel::CirculationModes::Pool);

		// Update temperatures in the DataHub. Absent values mean "no reading in this
		// message" (e.g. water temp while the pump is off) and must NOT be written:
		// writing would refresh the value's timestamp and defeat the staleness
		// tracking that lets consumers flag the last real reading as stale.
		if (auto temp = msg.PoolTemperature(); temp.has_value())
		{
			m_DataHub->PoolTemp(temp.value());
		}

		if (auto temp = msg.SpaTemperature(); temp.has_value())
		{
			m_DataHub->SpaTemp(temp.value());
		}

		if (auto temp = msg.AirTemperature(); temp.has_value())
		{
			m_DataHub->AirTemp(temp.value());
		}

		// Update heat setpoints. The current wire format reports both targets on every
		// message; the legacy format only carries the active body's target (HeaterSetpoint).
		if (msg.PoolSetpoint().has_value() || msg.SpaSetpoint().has_value())
		{
			if (auto setpoint = msg.PoolSetpoint(); setpoint.has_value())
			{
				m_DataHub->PoolTempSetpoint(setpoint.value());
			}

			if (auto setpoint = msg.SpaSetpoint(); setpoint.has_value())
			{
				m_DataHub->SpaTempSetpoint(setpoint.value());
			}
		}
		else if (auto setpoint = msg.HeaterSetpoint(); setpoint.has_value())
		{
			if (msg.SpaMode())
			{
				m_DataHub->SpaTempSetpoint(setpoint.value());
			}
			else
			{
				m_DataHub->PoolTempSetpoint(setpoint.value());
			}
		}

		// Update filter pump status.
		{
			// Query the filtered pump view once; only re-query after creating a new pump
			// (the Add invalidates the first snapshot).  Previously FilterPumps() ran a
			// full device-graph scan twice per MainStatus even when a pump already existed.
			auto filter_pumps = m_DataHub->FilterPumps();
			if (filter_pumps.empty())
			{
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Pump);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, Kernel::AuxillaryTraitsTypes::LabelTrait::COMMON_LABEL_FILTER_PUMP);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpSpeedTrait{}, Kernel::PumpSpeeds::Unknown);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::PumpStatuses::Unknown);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpTypeTrait{}, Kernel::PumpTypes::FilterCirculation);

				// Filter pump is Shared in combo/dual systems, Pool in single-body.
				auto body_id = (m_DataHub->PoolConfiguration == Kernel::PoolConfigurations::DualBody_SharedEquipment
					|| m_DataHub->PoolConfiguration == Kernel::PoolConfigurations::DualBody_DualEquipment)
					? Kernel::BodyOfWaterIds::Shared
					: Kernel::BodyOfWaterIds::Pool;
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, body_id);

				m_DataHub->Devices.Add(std::move(ptr));
				filter_pumps = m_DataHub->FilterPumps();
			}

			const auto pump_status = msg.PumpOn() ? Kernel::PumpStatuses::Running : Kernel::PumpStatuses::Off;
			for (const auto& pump : filter_pumps)
			{
				pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, pump_status);

				auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(pump);
				std::string label;
				if (auto label_opt = pump->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
				{
					label = label_opt.value();
				}
				m_DataHub->EmitButtonStateChange(pump->Id(), status_string, label);
			}
		}

		// Create/update the three heater devices in the DataHub.  Extracted into a
		// member function so this hot decode path stays readable.
		UpdateHeaterDevice("Pool Heat", msg.PoolHeaterStatus(), Kernel::BodyOfWaterIds::Pool);
		UpdateHeaterDevice("Spa Heat", msg.SpaHeaterStatus(), Kernel::BodyOfWaterIds::Spa);
		UpdateHeaterDevice("Solar Heat", msg.SolarHeaterStatus(), Kernel::BodyOfWaterIds::Shared);

		// Now that the DataHub reflects the freshly-decoded MainStatus, render that
		// live state into the device's Screen so the diagnostics "Actual Devices"
		// card shows real data rather than a Page_Unknown blank.
		RenderStatusScreen(msg);
	}

	void IAQDevice::UpdateHeaterDevice(const std::string& label, Kernel::HeaterStatuses status, Kernel::BodyOfWaterIds body_id)
	{
		// Query the label view once; only re-query after creating a new heater (the
		// Add invalidates the first snapshot).  Previously FindByLabel() ran a full
		// label scan up to three times per heater per MainStatus.
		auto heaters = m_DataHub->Devices.FindByLabel(label);
		if (heaters.empty())
		{
			auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
			ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Heater);
			ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, label);
			ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{}, Kernel::HeaterStatuses::Off);
			ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, body_id);
			m_DataHub->Devices.Add(std::move(ptr));
			heaters = m_DataHub->Devices.FindByLabel(label);
		}

		if (heaters.empty())
		{
			return;
		}

		auto heater = heaters.front();
		heater->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{}, status);

		auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(heater);
		std::string heater_label;
		if (auto label_opt = heater->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
		{
			heater_label = label_opt.value();
		}
		m_DataHub->EmitButtonStateChange(heater->Id(), status_string, heater_label);
	}

	void IAQDevice::RenderStatusScreen(const Messages::IAQMessage_MainStatus& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::RenderStatusScreen", std::source_location::current());

		// The IAQ (iAqualink2 cloud interface) has no navigable physical screen; its
		// "screen" is a rendered reflection of the system status it just decoded.
		// Rebuild the whole page from scratch on every MainStatus so it tracks live
		// state and never accumulates stale lines.
		ScreenMode(Capabilities::ScreenModes::Updating);
		ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evClear());

		// Collect the human-readable summary lines first; only the lines that fit on
		// the fixed-size page (IAQ_STATUS_PAGE_LINES) are pushed to the updater.
		std::vector<std::string> lines;
		lines.reserve(IAQ_STATUS_PAGE_LINES);

		// A missing reading renders as "--" (matches the panel's own display for a
		// sensor with no flow) rather than a fabricated number.
		auto temp_line = [](const std::optional<Kernel::Temperature>& temp)
		{
			return temp.has_value() ? std::format("{:.0f}F", temp->InFahrenheit().value()) : "--";
		};

		lines.emplace_back("System Status");
		lines.emplace_back(std::format("Mode: {}", msg.SpaMode() ? "Spa" : "Pool"));
		lines.emplace_back(std::format("Pool Temp: {}", temp_line(msg.PoolTemperature())));
		lines.emplace_back(std::format("Spa Temp:  {}", temp_line(msg.SpaTemperature())));
		lines.emplace_back(std::format("Air Temp:  {}", temp_line(msg.AirTemperature())));

		if (auto setpoint = msg.HeaterSetpoint(); setpoint.has_value())
		{
			lines.emplace_back(std::format("{} Setpoint: {:.0f}F",
				msg.SpaMode() ? "Spa" : "Pool",
				setpoint->InFahrenheit().value()));
		}

		lines.emplace_back(std::format("Pump: {}", msg.PumpOn() ? "On" : "Off"));
		lines.emplace_back(std::format("Pool Heat: {}", magic_enum::enum_name(msg.PoolHeaterStatus())));
		lines.emplace_back(std::format("Spa Heat:  {}", magic_enum::enum_name(msg.SpaHeaterStatus())));
		lines.emplace_back(std::format("Solar:     {}", magic_enum::enum_name(msg.SolarHeaterStatus())));

		// Aux on/off summary, read from the DataHub (AuxStatus keeps these fresh).
		auto auxillaries = m_DataHub->Auxillaries();
		for (const auto& aux : auxillaries)
		{
			if (nullptr == aux)
			{
				continue;
			}

			auto label_opt = aux->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
			auto status_opt = aux->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{});
			if (!label_opt.has_value() || !status_opt.has_value())
			{
				continue;
			}

			const bool is_on = (status_opt.value() == Kernel::AuxillaryStatuses::On);
			lines.emplace_back(std::format("{}: {}", label_opt.value(), is_on ? "On" : "Off"));
		}

		const std::size_t line_count = std::min(static_cast<std::size_t>(IAQ_STATUS_PAGE_LINES), lines.size());
		for (std::size_t line_index = 0; line_index < line_count; ++line_index)
		{
			ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evUpdate(static_cast<uint8_t>(line_index), lines[line_index]));
		}

		// Mark the page as a KNOWN, fixed status view.  There are no page processors
		// for the IAQ, so do NOT call ProcessScreenUpdates() (it would reset the type
		// back to Page_Unknown); set the type directly instead.
		DisplayedPageType(Utility::ScreenDataPageTypes::Page_SystemStatus);
		ScreenMode(Capabilities::ScreenModes::Normal);

		LogTrace(Channel::Devices, [this]() { return std::format("IAQ ({}): Rendered System Status screen ({} lines)", DeviceId(), DisplayedPage().Size()); });
	}

	void IAQDevice::RenderCloudLinkScreen()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::RenderCloudLinkScreen", std::source_location::current());

		// A heartbeat-only IAQ (the iAqualink2 cloud interface on 0xA3) receives ONLY
		// the heartbeat (0x53) -- no MainStatus/AuxStatus and no navigable page -- so
		// its "screen" is a rendered reflection of the heartbeat liveness rather than
		// of decoded system status.  Rebuild from scratch on every heartbeat so it
		// tracks the live state and never accumulates stale lines.
		ScreenMode(Capabilities::ScreenModes::Updating);
		ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evClear());

		// The watchdog (Restartable) is Kick()ed on each heartbeat; while it is still
		// running the link is "active", otherwise the beacon has gone stale.
		const bool heartbeat_active = IsRunning();

		std::vector<std::string> lines;
		lines.reserve(IAQ_STATUS_PAGE_LINES);

		lines.emplace_back("iAqualink2 Cloud Link");
		lines.emplace_back(std::format("Heartbeat: {}", heartbeat_active ? "active" : "stale"));
		lines.emplace_back(std::format("Timeout: {}s", GetTimeout().count()));
		// The heartbeat ACK is a constant presence beacon (Type=0x1f, Command=0x00);
		// there is no MainStatus/AuxStatus on this id, so report the idle beacon state.
		lines.emplace_back("ACK: 0x1f/0x00 (idle)");

		const std::size_t line_count = std::min(static_cast<std::size_t>(IAQ_STATUS_PAGE_LINES), lines.size());
		for (std::size_t line_index = 0; line_index < line_count; ++line_index)
		{
			ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evUpdate(static_cast<uint8_t>(line_index), lines[line_index]));
		}

		// Mark the page as a KNOWN, fixed Cloud Link view.  As with RenderStatusScreen
		// there are no page processors for the IAQ, so set the type directly rather
		// than calling ProcessScreenUpdates() (which would reset it to Page_Unknown).
		DisplayedPageType(Utility::ScreenDataPageTypes::Page_CloudLink);
		ScreenMode(Capabilities::ScreenModes::Normal);

		LogTrace(Channel::Devices, [this]() { return std::format("IAQ ({}): Rendered Cloud Link screen ({} lines)", DeviceId(), DisplayedPage().Size()); });
	}

	void IAQDevice::ProcessAuxStatus(const Messages::IAQMessage_AuxStatus& msg)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::ProcessAuxStatus", std::source_location::current());

		LogDebug(Channel::Devices, [this, &msg]() { return std::format("IAQ ({}): Processing AuxStatus: {} devices", DeviceId(), msg.DeviceCount()); });

		for (const auto& info : msg.Devices())
		{
			auto aux_id = magic_enum::enum_cast<Auxillaries::JandyAuxillaryIds>(info.device_index);
			if (!aux_id.has_value())
			{
				LogDebug(Channel::Devices, [this, &info]() { return std::format("IAQ ({}): Unknown device_index {} in AuxStatus, skipping", DeviceId(), info.device_index); });
				continue;
			}

			const auto status = info.is_on ? Auxillaries::JandyAuxillaryStatuses::On : Auxillaries::JandyAuxillaryStatuses::Off;

			// A reply about an aux the detected panel model cannot have is not evidence that the
			// relay exists (see Auxillaries::AuxillaryModelSpan) - mirrors the same gate the RSSA
			// and OneTouch Label-Aux paths apply, which this path was previously missing.
			if (const auto span = Auxillaries::AuxillaryModelSpan::FromDataHub(*m_DataHub); !span.Contains(aux_id.value()))
			{
				LogDebug(Channel::Devices, [this, &aux_id]() { return std::format("IAQ ({}): Ignoring auxillary status for {}; the detected panel model has no such relay", DeviceId(), magic_enum::enum_name(aux_id.value())); });
				continue;
			}

			// An operator-forced Absent override must survive the next wire event too, otherwise
			// it flip-flops back the next time the panel reports this aux.
			if ((nullptr != m_PreferencesHub) && Auxillaries::IsForcedAbsent(aux_id.value(), m_PreferencesHub->AuxPresenceOverrides))
			{
				LogDebug(Channel::Devices, [this, &aux_id]() { return std::format("IAQ ({}): Ignoring auxillary status for {}; forced absent by operator override", DeviceId(), magic_enum::enum_name(aux_id.value())); });

				if (auto existing = m_DataHub->Devices.FindById(Auxillaries::AuxStableId(aux_id.value())); nullptr != existing)
				{
					m_DataHub->Devices.Remove(existing);
				}
				continue;
			}

			// Find or create the auxillary device, reconciling by the stable id derived from the
			// aux id - this matches a cache-restored placeholder regardless of its label.
			std::shared_ptr<Kernel::AuxillaryDevice> aux_ptr(nullptr);

			if (auto existing = m_DataHub->Devices.FindById(Auxillaries::AuxStableId(aux_id.value())); nullptr != existing)
			{
				aux_ptr = existing;
				// Grant the aux identity to a cache-restored placeholder (which lacks it).
				Auxillaries::EnsureAuxIdentity(aux_ptr, aux_id.value());
			}
			else if (auto new_aux_ptr = Factory::JandyAuxillaryFactory::Instance().SerialAdapterDevice_CreateDevice(aux_id.value(), status); new_aux_ptr.has_value())
			{
				m_DataHub->Devices.Add(new_aux_ptr.value());
				aux_ptr = new_aux_ptr.value();
			}
			else
			{
				LogDebug(Channel::Devices, [this, &aux_id, &new_aux_ptr]() { return std::format("IAQ ({}): Failed to create auxillary device for {}: {}", DeviceId(), magic_enum::enum_name(aux_id.value()), new_aux_ptr.error().message()); });
				continue;
			}

			// Update the device status.
			aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{},
				info.is_on ? Kernel::AuxillaryStatuses::On : Kernel::AuxillaryStatuses::Off);

			// This is real wire evidence -- clear any "synthesized by a forced-present operator
			// override, not yet independently confirmed" marker so the slot now reads as detected.
			aux_ptr->AuxillaryTraits.Set(Auxillaries::SynthesizedTrait{}, false);

			// Collapse any legacy random-id cache placeholder for this aux onto the live device at
			// the first touch - before the custom label is known - so it never publishes as a
			// duplicate (any cached custom label/body is transferred across).
			Auxillaries::RemoveOrphanAuxPlaceholders(m_DataHub->Devices, aux_id.value(), aux_ptr);

			// Update the label from the IAQ-provided name if non-empty.
			if (!info.name.empty())
			{
				aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, info.name);

				// Set BodyOfWaterTrait based on label heuristic if not already set.
				if (!aux_ptr->AuxillaryTraits.Has(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}))
				{
					auto body_id = Kernel::BodyOfWaterIds::Unknown;
					if (info.name.find("Spa") != std::string::npos)
					{
						body_id = Kernel::BodyOfWaterIds::Spa;
					}
					else if (info.name.find("Pool") != std::string::npos)
					{
						body_id = Kernel::BodyOfWaterIds::Pool;
					}
					aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, body_id);
				}
			}

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(aux_ptr);
			std::string aux_label;
			if (auto label_opt = aux_ptr->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				aux_label = label_opt.value();
			}
			m_DataHub->EmitButtonStateChange(aux_ptr->Id(), status_string, aux_label);

			LogTrace(Channel::Devices, [this, &aux_id, &info]() { return std::format("IAQ ({}): AuxStatus device {}: name='{}', status={}",
				DeviceId(), magic_enum::enum_name(aux_id.value()), info.name, info.is_on ? "On" : "Off"); });
		}
	}

}
// namespace AqualinkAutomate::Devices
