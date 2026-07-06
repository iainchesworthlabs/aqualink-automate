#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>

#include <boost/regex.hpp>

#include "logging/logging.h"
#include "auxillaries/jandy_auxillary_id.h"
#include "auxillaries/jandy_auxillary_reconciliation.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "devices/onetouch/onetouch_scraper.h"
#include "devices/onetouch/onetouch_schedule_parser.h"
#include "factories/jandy_auxillary_factory.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water.h"
#include "kernel/body_of_water_ids.h"
#include "utility/jandy_pool_configuration_decoder.h"
#include "utility/spa_switch_assignment.h"
#include "utility/string_manipulation.h"
#include "utility/string_conversion/auxillary_state_string_converter.h"
#include "utility/string_conversion/temperature_string_converter.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	void OneTouchScraper::PageProcessor_System(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_System", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_System page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 = Paddock Pools
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =  01/18/11 Tue
			Info:   OneTouch Menu Line 03 =    7:13 PM
			Info:   OneTouch Menu Line 04 =
			Info:   OneTouch Menu Line 05 = Filter Pump OFF
			Info:   OneTouch Menu Line 06 =     Air 22`C
			Info:   OneTouch Menu Line 07 =
			Info:   OneTouch Menu Line 08 =
			Info:   OneTouch Menu Line 09 = Equipment ON/OFF
			Info:   OneTouch Menu Line 10 = OneTouch  ON/OFF
			Info:   OneTouch Menu Line 11 =    Menu / Help
		*/

		m_DataHub->Mode = Kernel::EquipmentMode::Normal;

		// Parse the temperature readings on the home page and route each by its area label
		// ("Air"/"Pool"/"Spa") rather than a fixed line number. The layout is model-dependent:
		// a single-body / shared-equipment panel shows water temp on line 5 and air on line 6,
		// but a dual-equipment panel lists both Pool Pump and Spa Pump, which pushes the air
		// line down (air is on line 7 on an RS-2/10 Dual). Scanning by area handles every
		// layout. Water temps only appear here while the corresponding pump is running.
		for (std::size_t line = 0; line < page.Size(); ++line)
		{
			auto temp = Utility::TemperatureStringConverter(Utility::TrimWhitespace(page[line].Text));
			if (!temp().has_value())
			{
				continue;
			}

			auto area = temp.TemperatureArea();
			if (!area.has_value())
			{
				continue;
			}

			auto area_lower = area.value();
			std::transform(area_lower.begin(), area_lower.end(), area_lower.begin(), [](unsigned char c){ return std::tolower(c); });

			if (area_lower.contains("air"))
			{
				m_DataHub->AirTemp(temp().value());
			}
			else if (area_lower.contains("pool"))
			{
				m_DataHub->PoolTemp(temp().value());
			}
			else if (area_lower.contains("spa"))
			{
				m_DataHub->SpaTemp(temp().value());
			}
		}
	}

	void OneTouchScraper::PageProcessor_Service(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_Service", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_Service page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =
			Info:   OneTouch Menu Line 03 =  Service Mode
			Info:   OneTouch Menu Line 04 =
			Info:   OneTouch Menu Line 05 = No  operations
			Info:   OneTouch Menu Line 06 =  allowed here
			Info:   OneTouch Menu Line 07 =
			Info:   OneTouch Menu Line 08 =
			Info:   OneTouch Menu Line 09 =
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =
		*/

		m_DataHub->Mode = Kernel::EquipmentMode::Service;
	}

	void OneTouchScraper::PageProcessor_TimeOut(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_TimeOut", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_TimeOut page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =
			Info:   OneTouch Menu Line 03 =  Timeout Mode
			Info:   OneTouch Menu Line 04 =
			Info:   OneTouch Menu Line 05 = No  operations
			Info:   OneTouch Menu Line 06 =  allowed here
			Info:   OneTouch Menu Line 07 =
			Info:   OneTouch Menu Line 08 =
			Info:   OneTouch Menu Line 09 =
			Info:   OneTouch Menu Line 10 =    02:57:39
			Info:   OneTouch Menu Line 11 =
		*/

		m_DataHub->Mode = Kernel::EquipmentMode::TimeOut;
		m_DataHub->TimeoutRemaining = Utility::TimeoutDurationStringConverter(Utility::TrimWhitespace(page[10].Text));
	}

	void OneTouchScraper::PageProcessor_OneTouch(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_OneTouch", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_OneTouch page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =
			Info:   OneTouch Menu Line 01 = All Off
			Info:   OneTouch Menu Line 02 =
			Info:   OneTouch Menu Line 03 = 
			Info:   OneTouch Menu Line 04 = Spa Mode     OFF
			Info:   OneTouch Menu Line 05 = 
			Info:   OneTouch Menu Line 06 = 
			Info:   OneTouch Menu Line 07 = Clean Mode   OFF
			Info:   OneTouch Menu Line 08 =
			Info:   OneTouch Menu Line 09 =
			Info:   OneTouch Menu Line 10 =  More OneTouch
			Info:   OneTouch Menu Line 11 =      System
		*/
	}

	void OneTouchScraper::PageProcessor_EquipmentOnOff(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_EquipmentOnOff", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_EquipmentOnOff page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 = Filter Pump  ***
			Info:   OneTouch Menu Line 01 = Spa           ON
			Info:   OneTouch Menu Line 02 = Pool Heat    ENA
			Info:   OneTouch Menu Line 03 = Spa Heat     OFF
			Info:   OneTouch Menu Line 04 = Solar Heat   OFF
			Info:   OneTouch Menu Line 05 = Aux1         OFF
			Info:   OneTouch Menu Line 06 = Aux2         OFF
			Info:   OneTouch Menu Line 07 = Aux3         OFF
			Info:   OneTouch Menu Line 08 = Aux4         OFF
			Info:   OneTouch Menu Line 09 = Aux5         OFF
			Info:   OneTouch Menu Line 10 = Aux6         OFF
			Info:   OneTouch Menu Line 11 =    ^^ More vv
		*/

		for (std::size_t row_index = 0; row_index < (page.Size() - 1); row_index++)
		{
			auto new_aux_state = Utility::AuxillaryStateStringConverter(Utility::TrimWhitespace(page[row_index].Text));

			if (auto aux_ptr = Factory::JandyAuxillaryFactory::Instance().OneTouchDevice_CreateDevice(new_aux_state); !aux_ptr.has_value())
			{
				LogTrace(Channel::Devices, [this, &page, row_index]() { return std::format("OneTouch ({}): Failed to create a device for this specific devic's row text: {}", DeviceId(), Utility::TrimWhitespace(page[row_index].Text)); });
			}
			else
			{
				auto new_device = aux_ptr.value();

				// Aux devices carry a DETERMINISTIC stable id derived from the aux id, so a
				// device that already carries that id - a steady-state cache placeholder or one
				// discovered earlier this run - is found by id (independent of any custom label)
				// and updated in place. A LEGACY pre-stable-id cache placeholder (random id) is
				// reconciled away here, at this first live touch of the aux, so it never reaches
				// the MQTT/HA publishers as a duplicate (any cached custom label is preserved).
				if (new_device->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}))
				{
					const auto aux_id = *(new_device->AuxillaryTraits[Auxillaries::JandyAuxillaryId{}]);
					if (auto existing = m_DataHub->Devices.FindById(new_device->Id()); nullptr != existing)
					{
						// Grant the aux identity to a cache-restored placeholder (which lacks it).
						Auxillaries::EnsureAuxIdentity(existing, aux_id);
						if (auto status_opt = new_device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}); status_opt.has_value())
						{
							existing->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryStatusTrait{}, status_opt.value());
						}
						Auxillaries::RemoveOrphanAuxPlaceholders(m_DataHub->Devices, aux_id, existing);
						continue;
					}
				}

				m_DataHub->Devices.Add(new_device);

				// Collapse any legacy random-id placeholder for this aux onto the device just added.
				if (auto aux_id_opt = new_device->AuxillaryTraits.TryGet(Auxillaries::JandyAuxillaryId{}); aux_id_opt.has_value())
				{
					Auxillaries::RemoveOrphanAuxPlaceholders(m_DataHub->Devices, aux_id_opt.value(), new_device);
				}
			}
		}
	}

	void OneTouchScraper::PageProcessor_EquipmentStatus(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_EquipmentStatus", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_EquipmentStatus page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 = Equipment Status    Equipment Status	Equipment Status
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 = Intelliflo VS 3		Intelliflo VS 3		Intelliflo VF 2
			Info:   OneTouch Menu Line 03 =  *** Priming ***		   RPM: 2750		  RPM: 2250
			Info:   OneTouch Menu Line 04 =     Watts: 100           Watts: 600		    Watts: 55
			Info:   OneTouch Menu Line 05 =                            GPM: 55		      GPM: 80
			Info:   OneTouch Menu Line 06 =
			Info:   OneTouch Menu Line 07 =
			Info:   OneTouch Menu Line 08 =
			Info:   OneTouch Menu Line 09 =
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =
		*/

		// Member-function-pointer table (built once, static const) replaces the previous
		// per-call vector of nine std::bind closures (each a heap allocation per status
		// page).  Pointer-to-member is a trivial value type, so this incurs no per-message
		// allocation.
		using ScreenLineProcessor = void (OneTouchScraper::*)(const Utility::ScreenDataPage&, const uint8_t);

		static constexpr std::array<ScreenLineProcessor, 9> line_processors
		{
			&OneTouchScraper::StatusProcessor_FilterPump,
			&OneTouchScraper::StatusProcessor_SolarHeat,
			&OneTouchScraper::StatusProcessor_HeatPump,
			&OneTouchScraper::StatusProcessor_Chiller,
			&OneTouchScraper::StatusProcessor_PoolHeat,
			&OneTouchScraper::StatusProcessor_SpaHeat,
			&OneTouchScraper::StatusProcessor_AquaPurePercentage,
			&OneTouchScraper::StatusProcessor_SaltLevelPPM,
			&OneTouchScraper::StatusProcessor_CheckAquaPure
		};

		for (const auto matcher_processor : line_processors)
		{
			// Ignore line 0 on every page as it's the "Equipment Status" title line...

			for (std::size_t line_index = 1; line_index < page.Size(); ++line_index)
			{
				if (page[line_index].Text.empty())
				{
					// Ignore empty lines...
				}
				else
				{
					(this->*matcher_processor)(page, static_cast<uint8_t>(line_index));
				}
			}
		}
	}

	void OneTouchScraper::PageProcessor_SelectSpeed(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_SelectSpeed", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_SelectSpeed page.", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_MenuHelp(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_MenuHelp", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_MenuHelp page.", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_SetTemperature(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_SetTemperature", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_SetTemperature page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =     Set Temp
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 = Pool Heat   90`F
			Info:   OneTouch Menu Line 03 = Spa Heat   102`F
			Info:   OneTouch Menu Line 04 =
			Info:   OneTouch Menu Line 05 = Maintain     OFF
			Info:   OneTouch Menu Line 06 = Hours  12AM-12AM
			Info:   OneTouch Menu Line 07 =
			Info:   OneTouch Menu Line 08 = Highlight an
			Info:   OneTouch Menu Line 09 = item and press
			Info:   OneTouch Menu Line 10 = Select
			Info:   OneTouch Menu Line 11 =
		*/

		// Parse the heat setpoints by their area label ("Pool Heat" / "Spa Heat") rather than a
		// fixed line. The values live on lines 2/3 on the models seen so far, but scanning by
		// area is robust to layout shifts and matches the home-page approach. The labels are two
		// words and spa setpoints can be 100`F+, both of which the temperature converter now
		// handles (this is what made Combo setpoints decode as null previously -- see OBS-03).
		for (std::size_t line = 0; line < page.Size(); ++line)
		{
			auto temp = Utility::TemperatureStringConverter(Utility::TrimWhitespace(page[line].Text));
			if (!temp().has_value())
			{
				continue;
			}

			auto area = temp.TemperatureArea();
			if (!area.has_value())
			{
				continue;
			}

			auto area_lower = area.value();
			std::ranges::transform(area_lower, area_lower.begin(), [](unsigned char c){ return std::tolower(c); });

			// "Spa Heat" before "Pool Heat": neither label contains the other's keyword, but
			// guard order keeps it unambiguous if a future label ever does.
			if (area_lower.contains("spa"))
			{
				m_DataHub->SpaTempSetpoint(temp().value());
			}
			else if (area_lower.contains("pool"))
			{
				m_DataHub->PoolTempSetpoint(temp().value());
			}
		}
	}

	void OneTouchScraper::PageProcessor_SpaSwitch(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_SpaSwitch", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_SpaSwitch page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =    Spa Switch
			Info:   OneTouch Menu Line 03 = 1:1     Spa Jets
			Info:   OneTouch Menu Line 04 = 1:2   Pool Light
			Info:   OneTouch Menu Line 05 = 1:3   Air Blower
			Info:   OneTouch Menu Line 06 = 1:4     Spillway
			Info:   OneTouch Menu Line 07 = 2:1     Swim Jet
			...
		*/

		// Scan every line for a "<switch>:<button>  <function>" assignment row and store it into the
		// controller-agnostic DataHub map -- the SAME parser the iAQ path uses. Non-assignment lines
		// (the title, blanks, footer help text) are rejected by the parser. Gating to this page (the
		// detector matched "Spa Switch" at line 0) keeps the home-screen clock ("1:20 PM") out.
		for (std::size_t line = 0; line < page.Size(); ++line)
		{
			if (const auto assignment = Utility::ParseSpaSwitchAssignmentLine(page[line].Text))
			{
				m_DataHub->SetSpaSwitchAssignment(assignment->switch_number, assignment->button_number, assignment->function);
				LogDebug(Channel::Devices, [this, &assignment]() { return std::format("OneTouch ({}): spa-switch assignment {}:{} -> '{}'", DeviceId(), assignment->switch_number, assignment->button_number, assignment->function); });
			}
		}
	}

	void OneTouchScraper::PageProcessor_SetTime(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_SetTime", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_SetTime page.", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_SystemSetup(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_SystemSetup", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_SystemSetup page.", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_FreezeProtect(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_FreezeProtect", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_FreezeProtect page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =  Freeze Protect
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =
			Info:   OneTouch Menu Line 03 = Temp        38`F
			Info:   OneTouch Menu Line 04 =
			Info:   OneTouch Menu Line 05 =
			Info:   OneTouch Menu Line 06 = Use Arrow Keys
			Info:   OneTouch Menu Line 07 = to set value.
			Info:   OneTouch Menu Line 08 = Press SELECT
			Info:   OneTouch Menu Line 09 = to continue.
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =
		*/

		if (auto temperature = Utility::TemperatureStringConverter(Utility::TrimWhitespace(page[3].Text)); temperature().has_value())
		{
			m_DataHub->FreezeProtectPoint(temperature().value());
		}
	}

	void OneTouchScraper::PageProcessor_Boost(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_Boost", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_Boost page.", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_SetAquapure(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_SetAquapure", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_SetAquapure page.", DeviceId()); });

		/*
			Set AquaPure page (verified vs onetouch_chlorinator.cap):
			Info:   OneTouch Menu Line 00 = Set AQUAPURE
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =
			Info:   OneTouch Menu Line 03 = Set Pool to:  45%
			Info:   OneTouch Menu Line 04 = Set Spa to:   50%
			...
			Pool % = line 3, Spa % = line 4 - the same rows the value editor drives
			(see SETAQUAPURE_POOL_LINE / SETAQUAPURE_SPA_LINE and ValueEdit_ProcessStep).
		*/

		// Read the first contiguous digit run of a row as a 0-100 percentage, mirroring
		// DisplayedValue(); returns nullopt when the row carries no parseable value yet
		// (page not fully rendered / value blanked mid-edit), so we simply wait.
		const auto parse_percent = [](const std::string& text) -> std::optional<uint8_t>
		{
			int value{ 0 };
			bool found{ false };
			for (const char c : text)
			{
				if (c >= '0' && c <= '9') { value = (value * 10) + (c - '0'); found = true; }
				else if (found) { break; }
			}
			if (!found || value > 100) { return std::nullopt; }
			return static_cast<uint8_t>(value);
		};

		const auto pool_pct = (page.Size() > SETAQUAPURE_POOL_LINE) ? parse_percent(page[SETAQUAPURE_POOL_LINE].Text) : std::nullopt;
		const auto spa_pct = (page.Size() > SETAQUAPURE_SPA_LINE) ? parse_percent(page[SETAQUAPURE_SPA_LINE].Text) : std::nullopt;

		if (!pool_pct.has_value() && !spa_pct.has_value())
		{
			// Nothing parseable on this render - wait for the next page update.
			return;
		}

		if (!m_DataHub)
		{
			return;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		// The Set AquaPure page only exists on a panel with a salt chlorinator, so reaching
		// it proves an SWG is present.  Create the chlorinator auxillary if the AquaRite wire
		// path has not already (AQUARITE_Percent only creates it once the cell first generates),
		// mirroring AquariteDevice::EnsureChlorinatorDeviceExists so the configured setpoint can
		// be shown even while the cell is idle.
		auto chlorinators = m_DataHub->Chlorinators();
		if (chlorinators.empty())
		{
			LogInfo(Channel::Devices, [this]() { return std::format("OneTouch ({}): Creating chlorinator device from Set AquaPure menu scrape", DeviceId()); });

			auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
			ptr->AuxillaryTraits.Set(AuxillaryTypeTrait{}, AuxillaryTypes::Chlorinator);
			ptr->AuxillaryTraits.Set(LabelTrait{}, std::string{ "AquaPure" });
			ptr->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);
			ptr->AuxillaryTraits.Set(BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
			m_DataHub->Devices.Add(std::move(ptr));

			chlorinators = m_DataHub->Chlorinators();
		}

		if (chlorinators.empty())
		{
			return;
		}

		const auto& device = chlorinators.front();
		if (pool_pct.has_value())
		{
			device->AuxillaryTraits.Set(ChlorinatorPoolSetpointTrait{}, pool_pct.value());
			LogDebug(Channel::Devices, [this, &pool_pct]() { return std::format("OneTouch ({}): scraped configured Pool chlorinator setpoint -> {}%", DeviceId(), pool_pct.value()); });
		}
		if (spa_pct.has_value())
		{
			device->AuxillaryTraits.Set(ChlorinatorSpaSetpointTrait{}, spa_pct.value());
			LogDebug(Channel::Devices, [this, &spa_pct]() { return std::format("OneTouch ({}): scraped configured Spa chlorinator setpoint -> {}%", DeviceId(), spa_pct.value()); });
		}
	}

	void OneTouchScraper::PageProcessor_Version(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_Version", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_Version page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =
			Info:   OneTouch Menu Line 03 =
			Info:   OneTouch Menu Line 04 =     B0029221
			Info:   OneTouch Menu Line 05 =    RS-8 Combo
			Info:   OneTouch Menu Line 06 =
			Info:   OneTouch Menu Line 07 =    REV T.0.1
			Info:   OneTouch Menu Line 08 =
			Info:   OneTouch Menu Line 09 =
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =
		*/

		// The REV page carries the same model/type/revision + pool configuration as the cold-start
		// splash, so it shares PageProcessor_StartUp's decode. Previously this processor built the
		// bodies of water with its own inline switch that never marked the active body; routing the
		// build through DecodePanelConfiguration -> ApplyPoolConfiguration (as StartUp does) keeps
		// the two call sites consistent.
		DecodePanelConfiguration(page);
	}

	void OneTouchScraper::DecodePanelConfiguration(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::DecodePanelConfiguration", std::source_location::current());

		const auto model_number = Utility::TrimWhitespace(page[4].Text);
		const auto panel_type = Utility::TrimWhitespace(page[5].Text);
		const auto fw_revision = Utility::TrimWhitespace(page[7].Text);

		Utility::PoolConfigurationDecoder pool_config_decoder(panel_type);

		// Handle autodetect vs user-specified configuration.
		if (m_DataHub->PoolConfigurationSource == Kernel::ConfigurationSource::UserSpecified
			&& pool_config_decoder.Configuration() != m_DataHub->PoolConfiguration)
		{
			LogWarning(Channel::Equipment, [this, &pool_config_decoder]() { return std::format("Autodetected pool configuration '{}' disagrees with user-specified '{}'",
				magic_enum::enum_name(pool_config_decoder.Configuration()),
				magic_enum::enum_name(m_DataHub->PoolConfiguration)); });
			// User specification takes precedence; do not override.
		}
		else
		{
			m_DataHub->PoolConfiguration = pool_config_decoder.Configuration();
		}

		m_DataHub->SystemBoard = pool_config_decoder.SystemBoard();
		m_DataHub->ExpectedAuxillaryCount = pool_config_decoder.AuxillaryCount();
		m_DataHub->ExpectedPowerCenterCount = pool_config_decoder.PowerCenterCount();
		m_DataHub->EquipmentVersions.Set("Model", model_number);
		m_DataHub->EquipmentVersions.Set("Type", panel_type);
		m_DataHub->EquipmentVersions.Set("Revision", fw_revision);

		// Populate bodies if not already present (user config may have built them at startup).
		// Reaching here with no bodies means the user did NOT specify a configuration (otherwise
		// startup would have built them), so this is the auto-detected path: source is Auto and a
		// SingleBody is treated as pool-only (spa-only is user-signalled only). Body-building lives
		// in ApplyPoolConfiguration so every call site (startup user-config, this REV/splash decode)
		// stays consistent - including marking the active body.
		if (m_DataHub->Bodies().empty())
		{
			m_DataHub->ApplyPoolConfiguration(m_DataHub->PoolConfiguration, Kernel::ConfigurationSource::Auto);
		}

		LogInfo(Channel::Devices, [&model_number, &panel_type, &fw_revision]() { return std::format("Aqualink Power Center - Model: {}, Type: {}, Rev: {}", model_number, panel_type, fw_revision); });
	}

	void OneTouchScraper::PageProcessor_DiagnosticsSensors(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_DiagnosticsSensors", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_DiagnosticsSensors page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 = Model   B0029221
			Info:   OneTouch Menu Line 01 = Type  RS-8 Combo
			Info:   OneTouch Menu Line 02 = Firmware   T.0.1
			Info:   OneTouch Menu Line 03 =
			Info:   OneTouch Menu Line 04 =     
			Info:   OneTouch Menu Line 05 = 
			Info:   OneTouch Menu Line 06 =     Sensors
			Info:   OneTouch Menu Line 07 = Water         OK
			Info:   OneTouch Menu Line 08 = Air           OK
			Info:   OneTouch Menu Line 09 = Solar		  OK
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =       Next 
		*/
	}

	void OneTouchScraper::PageProcessor_DiagnosticsRemotes(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_DiagnosticsRemotes", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_DiagnosticsRemotes page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =     Remotes
			Info:   OneTouch Menu Line 01 = 
			Info:   OneTouch Menu Line 02 = OneTouch       4
			Info:   OneTouch Menu Line 03 = LX Htr         1
			Info:   OneTouch Menu Line 04 =                1
			Info:   OneTouch Menu Line 05 = (Not Compatible)
			Info:   OneTouch Menu Line 06 = Spa Sw Board   1
			Info:   OneTouch Menu Line 07 = 
			Info:   OneTouch Menu Line 08 = 
			Info:   OneTouch Menu Line 09 = 
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =       Next
		*/
	}

	void OneTouchScraper::PageProcessor_DiagnosticsErrors(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_DiagnosticsErrors", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_DiagnosticsErrors page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =      Errors              Errors
			Info:   OneTouch Menu Line 01 = 
			Info:   OneTouch Menu Line 02 = 
			Info:   OneTouch Menu Line 03 =                     Heater   Gas Sw
			Info:   OneTouch Menu Line 04 =
			Info:   OneTouch Menu Line 05 =    No Errors   
			Info:   OneTouch Menu Line 06 =     
			Info:   OneTouch Menu Line 07 = 
			Info:   OneTouch Menu Line 08 = 
			Info:   OneTouch Menu Line 09 = 
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =    Continue            Continue
		*/
	}

	void OneTouchScraper::PageProcessor_LabelAuxList(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_LabelAuxList", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_LabelAuxList page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =    Label Aux
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 = Aux1           >
			Info:   OneTouch Menu Line 03 = Aux2           >
			Info:   OneTouch Menu Line 04 = Aux3           >
			Info:   OneTouch Menu Line 05 = Aux4           >
			Info:   OneTouch Menu Line 06 = Aux5           >
			Info:   OneTouch Menu Line 07 = Aux6           >
			Info:   OneTouch Menu Line 08 = Aux7           >
			Info:   OneTouch Menu Line 09 = Aux B1         >
			Info:   OneTouch Menu Line 10 =    ^^ More vv
			Info:   OneTouch Menu Line 11 =           
		*/
	}

	void OneTouchScraper::PageProcessor_LabelAux(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_LabelAux", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_LabelAux page.", DeviceId()); });

		/*
			Info:   OneTouch Menu Line 00 =    Label Aux1
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =  Current Label 
			Info:   OneTouch Menu Line 03 =       Aux1      
			Info:   OneTouch Menu Line 04 =                
			Info:   OneTouch Menu Line 05 = General Labels >
			Info:   OneTouch Menu Line 06 = Light   Labels >
			Info:   OneTouch Menu Line 07 = Wtrfall Labels >
			Info:   OneTouch Menu Line 08 = Custom  Label  >
			Info:   OneTouch Menu Line 09 =
			Info:   OneTouch Menu Line 10 =
			Info:   OneTouch Menu Line 11 =           
		*/

		const auto aux_main_label = Utility::TrimWhitespace(page[0].Text);
		const auto aux_custom_label = Utility::TrimWhitespace(page[3].Text);

		static const boost::regex pattern("(Label) (Aux(\\s?[B-D][1-8]|\\s?[1-7]))");
		boost::smatch matches;

		if (aux_custom_label.empty())
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Custom auxillary label was not set; cannot continue", DeviceId()); });
		}
		else if (!boost::regex_search(aux_main_label, matches, pattern) || matches.size() < 3)
		{
			LogDebug(Channel::Devices, [this, &page]() { return std::format("OneTouch ({}): Failed to parse the row text looking for an auxillary id; text was {}", DeviceId(), Utility::TrimWhitespace(page[0].Text)); });
		}
		else if (auto aux_id = Auxillaries::ParseAuxId(matches[2].str()); !aux_id.has_value())
		{
			LogDebug(Channel::Devices, [this, &matches]() { return std::format("OneTouch ({}): Failed to generate the id for Auxillary Device given string {}", DeviceId(), matches[2].str()); });
		}
		else
		{
			std::shared_ptr<Kernel::AuxillaryDevice> aux_ptr(nullptr);
			bool newly_created = false;

			// Reconcile by the stable id derived from the aux id: matches a cache-restored
			// placeholder or a device already discovered via the Equipment On/Off page.
			if (auto existing = m_DataHub->Devices.FindById(Auxillaries::AuxStableId(aux_id.value())); nullptr != existing)
			{
				aux_ptr = existing;
			}
			else if (auto temp_ptr = Factory::JandyAuxillaryFactory::Instance().SerialAdapterDevice_CreateDevice(aux_id.value()); !temp_ptr.has_value())
			{
				LogDebug(Channel::Devices, [this, &aux_id]() { return std::format("OneTouch ({}): Failed to create a new Auxillary Device for aux id: {}", DeviceId(), magic_enum::enum_name(aux_id.value())); });
			}
			else
			{
				aux_ptr = temp_ptr.value();
				newly_created = true;
			}

			if (nullptr == aux_ptr)
			{
				LogDebug(Channel::Devices, [this, &aux_id]() { return std::format("OneTouch ({}): Failed to find the Auxillary Device for aux id: {}; cannot set custom label", DeviceId(), magic_enum::enum_name(aux_id.value())); });
			}
			else
			{
				// Grant the aux identity to a cache-restored placeholder (which lacks it), then
				// apply the custom label.
				Auxillaries::EnsureAuxIdentity(aux_ptr, aux_id.value());
				aux_ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, aux_custom_label);

				// If this device was newly created (not found in graph), add it now
				if (newly_created)
				{
					LogDebug(Channel::Devices, [this, &aux_custom_label, &aux_id]() { return std::format("OneTouch ({}): Adding newly created Auxillary Device with custom label '{}' for aux id: {}", DeviceId(), aux_custom_label, magic_enum::enum_name(aux_id.value())); });
					m_DataHub->Devices.Add(aux_ptr);
				}

				// Drop any legacy placeholder for this aux now superseded by this device
				// (one-time cleanup when upgrading from a pre-stable-id cache).
				Auxillaries::RemoveOrphanAuxPlaceholders(m_DataHub->Devices, aux_id.value(), aux_ptr);
			}
		}
	}

	void OneTouchScraper::PageProcessor_MoreOneTouch(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_MoreOneTouch invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_SetPoolHeat(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_SetPoolHeat invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_SetSpaHeat(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_SetSpaHeat invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_Program(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PageProcessor_Program", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is processing a PageProcessor_Program page.", DeviceId()); });

		/*
			Per-equipment Program DETAIL page (the page to parse):

			Info:   OneTouch Menu Line 00 =    Filter Pump      <- target (equipment name)
			Info:   OneTouch Menu Line 01 =
			Info:   OneTouch Menu Line 02 =     Pgm 1 of 1      <- program index / count
			Info:   OneTouch Menu Line 03 =  ON      11:00 AM
			Info:   OneTouch Menu Line 04 =  OFF      2:00 PM
			Info:   OneTouch Menu Line 05 =  All Days
			Info:   OneTouch Menu Line 06 =
			Info:   OneTouch Menu Line 07 =
			Info:   OneTouch Menu Line 08 =
			Info:   OneTouch Menu Line 09 =  Add      Program
			Info:   OneTouch Menu Line 10 =  Delete   Program
			Info:   OneTouch Menu Line 11 =  Change   Program

			The parser returns nullopt for an equipment with "No Programs" (Add/New Program
			only) or a malformed page, so those visits simply add nothing.
		*/

		PublishControllerSchedules(page);
	}

	void OneTouchScraper::PublishControllerSchedules(const Utility::ScreenDataPage& page)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::PublishControllerSchedules", std::source_location::current());

		if (nullptr == m_ControllerScheduleStore)
		{
			// No sink registered (e.g. a passive/test rig) - nothing to publish to.
			return;
		}

		int program_index = 1;
		int program_count = 1;
		auto parsed = OneTouch::ParseProgramDetailPage(page, &program_index, &program_count);
		if (!parsed.has_value())
		{
			// Not a program-detail page (e.g. "No Programs" placeholder) or a malformed
			// render. Leave the accumulated snapshot untouched.
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): Program page carried no parseable program; snapshot unchanged", DeviceId()); });
			return;
		}

		parsed->group = m_ControllerScheduleGroup;

		// The OneTouch shows one program per detail page, so accumulate across visits keyed by
		// (target, program-index); revisiting a page updates its entry in place. The map's
		// ordering gives a stable, sorted snapshot each time it is republished.
		const auto key = std::make_pair(parsed->target, program_index);
		m_ControllerSchedules[key] = std::move(parsed.value());

		std::vector<Scheduling::ControllerSchedule> schedules;
		schedules.reserve(m_ControllerSchedules.size());
		for (const auto& [entry_key, schedule] : m_ControllerSchedules)
		{
			auto snapshot = schedule;
			snapshot.group = m_ControllerScheduleGroup;
			// Stable per-slot id so the UI can key rows across reads (group + target + index).
			snapshot.id = std::format("onetouch-{}-{}-{}",
				m_ControllerScheduleGroup.empty() ? "?" : m_ControllerScheduleGroup,
				entry_key.first, entry_key.second);
			snapshot.name = snapshot.target;
			schedules.push_back(std::move(snapshot));
		}

		LogInfo(Channel::Devices, [this, &key, &program_index, &program_count, &schedules]() { return std::format("OneTouch ({}): parsed program '{}' (Pgm {} of {}); publishing {} controller schedule(s) for group '{}'",
			DeviceId(), key.first, program_index, program_count, schedules.size(), m_ControllerScheduleGroup); });

		m_ControllerScheduleStore->Replace(Scheduling::ControllerScheduleStatus::Available, std::move(schedules), m_ControllerScheduleGroup);
	}

	void OneTouchScraper::PageProcessor_DisplayLight(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_DisplayLight invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_Lockouts(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_Lockouts invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_PasswordSettings(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_PasswordSettings invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_ProgramGroup(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_ProgramGroup invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_GeneralLabels(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_GeneralLabels invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_LightLabels(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_LightLabels invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_WaterfallLabels(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_WaterfallLabels invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_CustomLabel(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_CustomLabel invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_EnterPassword(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_EnterPassword invoked", DeviceId()); });
	}

	void OneTouchScraper::PageProcessor_HelpKeys(const Utility::ScreenDataPage& page)
	{
		LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): PageProcessor_HelpKeys invoked", DeviceId()); });
	}

}
// namespace AqualinkAutomate::Devices
