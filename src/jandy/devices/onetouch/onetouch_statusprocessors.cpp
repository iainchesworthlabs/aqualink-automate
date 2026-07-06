#include <array>
#include <cctype>

#include <boost/regex.hpp>

#include "devices/onetouch/onetouch_scraper.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/hub_events/data_hub_config_event_button_state_change.h"
#include "logging/logging.h"
#include "utility/string_manipulation.h"
#include "utility/to_number.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices
{

	bool OneTouchScraper::StatusProcessor_ShouldSkipLineProcessing(const HintArrayType& hint_array, const std::string_view line_to_process) const
	{
		// ASSUMPTIONS:
		//
		//   - The line of text referenced will always be in lowercase
		//   - The format of text on the line eg. SALT XXXX PPM is fixed
		//   - Hints are contiguous and the first two characters
		//
		// The hints are simply the (case-insensitive) first two characters that a line
		// must start with for it to be worth running the full regex.  The previous
		// implementation looped but unconditionally broke on the first iteration (so the
		// index increment was dead code); this is the equivalent direct two-character
		// compare.

		if (line_to_process.size() < 2)
		{
			// Too short to match a two-character hint -> skip processing.
			return true;
		}

		const bool first_matches = (hint_array[0] == std::tolower(static_cast<unsigned char>(line_to_process[0])));
		const bool second_matches = (hint_array[1] == std::tolower(static_cast<unsigned char>(line_to_process[1])));

		// Worth processing the line only when BOTH hint characters match.
		return !(first_matches && second_matches);
	}

	void OneTouchScraper::StatusProcessor_FilterPump(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_FilterPump", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_FilterPump status line.", DeviceId()); });

		static const boost::regex re("filter pump", boost::regex_constants::icase);
		static const HintArrayType hints{ 'f', 'i' };

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_FilterPump skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_FilterPump status line; failed to identify pump line", DeviceId()); });
		}
		else
		{
			if (0 == m_DataHub->FilterPumps().size())
			{
				// No primary filter pump...add it
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Pump);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, Kernel::AuxillaryTraitsTypes::LabelTrait::COMMON_LABEL_FILTER_PUMP);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpSpeedTrait{}, Kernel::PumpSpeeds::Unknown);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::PumpStatuses::Unknown);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpTypeTrait{}, Kernel::PumpTypes::FilterCirculation);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			for (auto& pump : m_DataHub->FilterPumps())
			{
				// AquaLink RS Equipment Status shows a single "Filter Pump" line;
				// individual pump identification requires Intelliflo-specific messages.
				LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_FilterPump setting filter pump status trait to '{}'", DeviceId(), magic_enum::enum_name(Kernel::PumpStatuses::Running)); });
				pump->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::PumpStatusTrait{}, Kernel::PumpStatuses::Running);

				// Signal that a button state change has occurred.
				auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(pump);
				std::string pump_lbl;
				if (auto label_opt = pump->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
				{
					pump_lbl = label_opt.value();
				}
				m_DataHub->EmitButtonStateChange(pump->Id(), status_string, pump_lbl);
			}
		}
	}

	void OneTouchScraper::StatusProcessor_PoolHeat(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_PoolHeat", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_PoolHeat status line.", DeviceId()); });

		static const boost::regex re("(pool heat)(?:\\s+(ena))?", boost::regex_constants::icase);
		static const HintArrayType hints{ 'p', 'o' };
		boost::smatch matches;

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_PoolHeat skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, matches, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_PoolHeat status line; failed to identify heat line", DeviceId()); });
		}
		else if (!matches[1].matched)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_PoolHeat status line; incorrect token count returned", DeviceId()); });
		}
		else
		{
			// Match for at least POOL HEAT
			using Kernel::AuxillaryTraitsTypes::HeaterStatusTrait;

			const std::string pool_heater_label{ "Pool Heat" };

			if (0 == m_DataHub->Devices.FindByLabel(pool_heater_label).size())
			{
				// Check for installed pool heating.  If it doesn't exist, add it.
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Heater);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, pool_heater_label);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{}, Kernel::HeaterStatuses::Off);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Pool);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			// The status is either going to be 'Heating' because only POOL HEAT was matched or 'Enabled' because POOL HEAT ENA was matched
			const auto heater_status = (!matches[2].matched) ? Kernel::HeaterStatuses::Heating : Kernel::HeaterStatuses::Enabled;

			LogTrace(Channel::Devices, [this, heater_status]() { return std::format("OneTouch ({}): StatusProcessor_PoolHeat setting Pool Heating status trait to '{}'", DeviceId(), magic_enum::enum_name(heater_status)); });
			auto heater = m_DataHub->Devices.FindByLabel(pool_heater_label).front();
			heater->AuxillaryTraits.Set(HeaterStatusTrait{}, heater_status);

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(heater);
			std::string heater_lbl;
			if (auto label_opt = heater->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				heater_lbl = label_opt.value();
			}
			m_DataHub->EmitButtonStateChange(heater->Id(), status_string, heater_lbl);
		}
	}

	void OneTouchScraper::StatusProcessor_SpaHeat(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_SpaHeat", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_SpaHeat status line.", DeviceId()); });

		static const boost::regex re("(spa heat)(?:\\s+(ena))?", boost::regex_constants::icase);
		static const HintArrayType hints{ 's', 'p' };
		boost::smatch matches;

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_SpaHeat skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, matches, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_SpaHeat status line; failed to identify heat line", DeviceId()); });
		}
		else if (!matches[1].matched)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_SpaHeat status line; incorrect token count returned", DeviceId()); });
		}
		else
		{
			// Match for at least SPA HEAT
			using Kernel::AuxillaryTraitsTypes::HeaterStatusTrait;

			const std::string spa_heater_label{ "Spa Heat" };

			if (0 == m_DataHub->Devices.FindByLabel(spa_heater_label).size())
			{
				// Check for installed spa heating.  If it doesn't exist, add it.
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Heater);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, spa_heater_label);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{}, Kernel::HeaterStatuses::Off);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Spa);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			// The status is either going to be 'Heating' because only SPA HEAT was matched or 'Enabled' because SPA HEAT ENA was matched
			const auto heater_status = (!matches[2].matched) ? Kernel::HeaterStatuses::Heating : Kernel::HeaterStatuses::Enabled;

			LogTrace(Channel::Devices, [this, heater_status]() { return std::format("OneTouch ({}): StatusProcessor_SpaHeat setting Spa Heating status trait to '{}'", DeviceId(), magic_enum::enum_name(heater_status)); });
			auto heater = m_DataHub->Devices.FindByLabel(spa_heater_label).front();
			heater->AuxillaryTraits.Set(HeaterStatusTrait{}, heater_status);

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(heater);
			std::string heater_lbl;
			if (auto label_opt = heater->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				heater_lbl = label_opt.value();
			}
			m_DataHub->EmitButtonStateChange(heater->Id(), status_string, heater_lbl);
		}
	}

	void OneTouchScraper::StatusProcessor_SolarHeat(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_SolarHeat", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_SolarHeat status line.", DeviceId()); });

		static const boost::regex re("(solar heat)(?:\\s+(ena))?", boost::regex_constants::icase);
		static const HintArrayType hints{ 's', 'o' };
		boost::smatch matches;

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_SolarHeat skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, matches, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_SolarHeat status line; failed to identify heat line", DeviceId()); });
		}
		else if (!matches[1].matched)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_SolarHeat status line; incorrect token count returned", DeviceId()); });
		}
		else 
		{
			// Match for at least SOLAR HEAT
			using Kernel::AuxillaryTraitsTypes::HeaterStatusTrait;

			const std::string solar_heater_label{ "Solar Heat" };

			if (0 == m_DataHub->Devices.FindByLabel(solar_heater_label).size())
			{
				// Check for installed solar heating.  If it doesn't exist, add it.
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Heater);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, solar_heater_label);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{}, Kernel::HeaterStatuses::Off);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			// The status is either going to be 'Heating' because only SOLAR HEAT was matched or 'Enabled' because SOLAR HEAT ENA was matched
			const auto heater_status = (!matches[2].matched) ? Kernel::HeaterStatuses::Heating : Kernel::HeaterStatuses::Enabled;

			LogTrace(Channel::Devices, [this, heater_status]() { return std::format("OneTouch ({}): StatusProcessor_SolarHeat setting Solar Heating status trait to '{}'", DeviceId(), magic_enum::enum_name(heater_status)); });
			auto heater = m_DataHub->Devices.FindByLabel(solar_heater_label).front();
			heater->AuxillaryTraits.Set(HeaterStatusTrait{}, heater_status);

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(heater);
			std::string heater_lbl;
			if (auto label_opt = heater->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				heater_lbl = label_opt.value();
			}
			m_DataHub->EmitButtonStateChange(heater->Id(), status_string, heater_lbl);
		}
	}

	void OneTouchScraper::StatusProcessor_HeatPump(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_HeatPump", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_HeatPump status line.", DeviceId()); });

		static const boost::regex re("(heat pump)(?:\\s+(ena))?", boost::regex_constants::icase);
		static const HintArrayType hints{ 'h', 'e' };
		boost::smatch matches;

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_HeatPump skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, matches, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_HeatPump status line; failed to identify heat line", DeviceId()); });
		}
		else if (!matches[1].matched)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_HeatPump status line; incorrect token count returned", DeviceId()); });
		}
		else
		{
			// Match for at least HEAT PUMP
			using Kernel::AuxillaryTraitsTypes::HeaterStatusTrait;

			const std::string heat_pump_label{ "Heat Pump" };

			if (0 == m_DataHub->Devices.FindByLabel(heat_pump_label).size())
			{
				// Check for installed heat pump heating.  If it doesn't exist, add it.
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Heater);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, heat_pump_label);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{}, Kernel::HeaterStatuses::Off);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			// The status is either going to be 'Heating' because only HEAT PUMP was matched or 'Enabled' because HEAT PUMP ENA was matched
			const auto heater_status = (!matches[2].matched) ? Kernel::HeaterStatuses::Heating : Kernel::HeaterStatuses::Enabled;

			LogTrace(Channel::Devices, [this, heater_status]() { return std::format("OneTouch ({}): StatusProcessor_HeatPump setting Heat Pump Heating status trait to '{}'", DeviceId(), magic_enum::enum_name(heater_status)); });
			auto heater = m_DataHub->Devices.FindByLabel(heat_pump_label).front();
			heater->AuxillaryTraits.Set(HeaterStatusTrait{}, heater_status);

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(heater);
			std::string heater_lbl;
			if (auto label_opt = heater->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				heater_lbl = label_opt.value();
			}
			m_DataHub->EmitButtonStateChange(heater->Id(), status_string, heater_lbl);
		}
	}

	void OneTouchScraper::StatusProcessor_Chiller(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_Chiller", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_Chiller status line.", DeviceId()); });

		static const boost::regex re("(chiller)(?:\\s+(ena))?", boost::regex_constants::icase);
		static const HintArrayType hints{ 'c', 'h' };
		boost::smatch matches;

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_Chiller skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, matches, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_Chiller status line; failed to identify chiller line", DeviceId()); });
		}
		else if (!matches[1].matched)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_Chiller status line; incorrect token count returned", DeviceId()); });
		}
		else
		{
			// Match for at least CHILLER
			using Kernel::AuxillaryTraitsTypes::HeaterStatusTrait;

			const std::string chiller_label{ "ChillerCooling" };

			if (0 == m_DataHub->Devices.FindByLabel(chiller_label).size())
			{
				// Check for installed chiller cooling.  If it doesn't exist, add it.
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Heater);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, chiller_label);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::HeaterStatusTrait{}, Kernel::HeaterStatuses::Off);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			// The status is either going to be 'Heating' because only CHILLER was matched or 'Enabled' because CHILLER ENA was matched
			const auto heater_status = (!matches[2].matched) ? Kernel::HeaterStatuses::Heating : Kernel::HeaterStatuses::Enabled;

			LogTrace(Channel::Devices, [this, heater_status]() { return std::format("OneTouch ({}): StatusProcessor_Chiller setting Chiller Cooling status trait to '{}'", DeviceId(), magic_enum::enum_name(heater_status)); });
			auto chiller = m_DataHub->Devices.FindByLabel(chiller_label).front();
			chiller->AuxillaryTraits.Set(HeaterStatusTrait{}, heater_status);

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(chiller);
			std::string chiller_lbl;
			if (auto label_opt = chiller->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				chiller_lbl = label_opt.value();
			}
			m_DataHub->EmitButtonStateChange(chiller->Id(), status_string, chiller_lbl);
		}
	}

	void OneTouchScraper::StatusProcessor_AquaPurePercentage(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_AquaPurePercentage", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_AquaPurePercentage status line.", DeviceId()); });

		// Real OneTouch Equipment Status lines are 16 columns wide with the value
		// right-justified, so the token gap is MULTIPLE spaces (e.g. "AquaPure
		// 50%"), not one.  Match \s+ rather than a single literal space — the
		// previous single-space pattern never fired against live screen content.
		static const boost::regex re("aquapure\\s+([0-9]{1,2}|100)%", boost::regex_constants::icase);
		static const HintArrayType hints{ 'a', 'q' };
		boost::smatch matches;

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_AquaPurePercentage skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, matches, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_AquaPurePercentage status line; failed to identify percentage", DeviceId()); });
		}
		else if (!matches[1].matched)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_AquaPurePercentage status line; incorrect token count returned", DeviceId()); });
		}
		else if (const auto percentage_dutycycle_string = matches[1].str(); percentage_dutycycle_string.empty())
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_AquaPurePercentage status line; sub-match is empty", DeviceId()); });
		}
		else if (const auto percentage_dutycycle = Utility::ToNumber<uint8_t>(percentage_dutycycle_string); !percentage_dutycycle.has_value())
		{
			LogDebug(Channel::Devices, [this, &percentage_dutycycle_string]() { return std::format("OneTouch ({}): Percentage string-to-number conversation failed: original value was '{}'", DeviceId(), percentage_dutycycle_string); });
		}
		else 
		{
			using Kernel::AuxillaryTraitsTypes::DutyCycleTrait;

			const std::string chlorinator_label{"AquaPure"};

			if (0 == m_DataHub->Devices.FindByLabel(chlorinator_label).size())
			{
				// Check for an installed chlorinator.  If one doesn't exist, add one.
				// Default to Off; actual status comes from AquaRite RS-485 messages.
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Chlorinator);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, chlorinator_label);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			if (auto devices = m_DataHub->Devices.FindByLabel(chlorinator_label); !devices.empty())
			{
				devices.front()->AuxillaryTraits.Set(DutyCycleTrait{}, percentage_dutycycle.value());
			}
		}
	}

	void OneTouchScraper::StatusProcessor_SaltLevelPPM(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_SaltLevelPPM", std::source_location::current());
		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_SaltLevelPPM status line.", DeviceId()); });

		// As with the AquaPure percentage line, the real screen row is 16 columns
		// with the value right-justified, so the gaps between "salt", the number
		// and "ppm" are MULTIPLE spaces.  Match \s+ rather than single spaces.
		static const boost::regex re("salt\\s+([0-9]{1,4})\\s+ppm", boost::regex_constants::icase);
		static const HintArrayType hints{ 's', 'a' };
		boost::smatch matches;

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_SaltLevelPPM skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, matches, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_SaltLevelPPM status line; failed to identify salt level", DeviceId()); });
		}
		else if (!matches[1].matched)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_SaltLevelPPM status line; incorrect token count returned", DeviceId()); });
		}
		else if (const auto salt_level_string = matches[1].str(); salt_level_string.empty())
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_SaltLevelPPM status line; sub-match is empty", DeviceId()); });
		}
		else  if (const auto salt_level = Utility::ToNumber<uint32_t>(salt_level_string); !salt_level.has_value())
		{
			LogDebug(Channel::Devices, [this, &salt_level_string]() { return std::format("OneTouch ({}): Salt level string-to-number conversation failed: original value was '{}'", DeviceId(), salt_level_string); });
		}
		else
		{
			m_DataHub->SaltLevel(salt_level.value() * ppm);
		}
	}

	void OneTouchScraper::StatusProcessor_CheckAquaPure(const Utility::ScreenDataPage& page, const uint8_t line_id)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchScraper::StatusProcessor_CheckAquaPure", std::source_location::current());

		LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): OneTouch device is checking for a StatusProcessor_CheckAquaPure status line.", DeviceId()); });

		static const boost::regex re("check aquapure", boost::regex_constants::icase);
		static const HintArrayType hints{ 'c', 'h' };

		if (const auto line_to_process = Utility::TrimWhitespace(page[line_id].Text); StatusProcessor_ShouldSkipLineProcessing(hints, line_to_process))
		{
			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_CheckAquaPure skipping line processing; hints were not matched", DeviceId()); });
		}
		else if (!boost::regex_match(line_to_process, re))
		{
			LogDebug(Channel::Devices, [this]() { return std::format("OneTouch ({}): Failed while processing StatusProcessor_CheckAquaPure status line; failed to identify check line", DeviceId()); });
		}
		else
		{
			const std::string chlorinator_label{"AquaPure"};

			if (0 == m_DataHub->Devices.FindByLabel(chlorinator_label).size())
			{
				// Check for an installed chlorinator.  If one doesn't exist, add one.
				// Default to Off; actual status comes from AquaRite RS-485 messages.
				auto ptr = std::make_shared<Kernel::AuxillaryDevice>();
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::AuxillaryTypeTrait{}, Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Chlorinator);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::LabelTrait{}, chlorinator_label);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorStatusTrait{}, Kernel::ChlorinatorStatuses::Off);
				ptr->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::BodyOfWaterTrait{}, Kernel::BodyOfWaterIds::Shared);
				m_DataHub->Devices.Add(std::move(ptr));
			}

			// "Check AquaPure" on the Equipment Status page is a binary alert;
			// specific error codes are decoded from AquaRite RS-485 messages.
			// Flag the chlorinator status so consumers know there is a problem.
			auto chlorinators = m_DataHub->Devices.FindByLabel(chlorinator_label);
			if (chlorinators.empty()) { return; }
			auto chlorinator = chlorinators.front();

			LogTrace(Channel::Devices, [this]() { return std::format("OneTouch ({}): StatusProcessor_CheckAquaPure setting chlorinator health to GeneralFault (check system alert)", DeviceId()); });
			chlorinator->AuxillaryTraits.Set(Kernel::AuxillaryTraitsTypes::ChlorinatorHealthTrait{}, Kernel::ChlorinatorHealth::GeneralFault);

			// Signal that a button state change has occurred.
			auto status_string = Kernel::AuxillaryTraitsTypes::ConvertStatusToString(chlorinator);
			std::string chlorinator_lbl;
			if (auto label_opt = chlorinator->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{}); label_opt.has_value())
			{
				chlorinator_lbl = label_opt.value();
			}
			m_DataHub->EmitButtonStateChange(chlorinator->Id(), status_string, chlorinator_lbl);
		}
	}

}
// namespace AqualinkAutomate::Devices
