#include <format>
#include <functional>
#include <numeric>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_utility.hpp>

#include "equipment/equipment_status.h"
#include "devices/aquarite_device.h"
#include "devices/chemlink_device.h"
#include "devices/epump_device.h"
#include "devices/heater_device.h"
#include "devices/iaq_device.h"
#include "devices/keypad_device.h"
#include "devices/lights_device.h"
#include "devices/onetouch_device.h"
#include "devices/pda_device.h"
#include "devices/serial_adapter_device.h"
#include "devices/spaside_remote_device.h"
#include "equipment/jandy_equipment.h"
#include "equipment/master_traffic_snoop.h"
#include "formatters/jandy_device_formatters.h"
#include "formatters/stats_counter_formatter.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_message.h"
#include "messages/jandy_message_message_long.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_status.h"
#include "messages/jandy_message_unknown.h"
#include "messages/chemlink/chemlink_message_response.h"
#include "messages/epump/epump_message_status.h"
#include "messages/epump/epump_message_rpm.h"
#include "messages/epump/epump_message_watts.h"
#include "messages/heater/heater_message_request.h"
#include "messages/heater/heater_message_status.h"
#include "messages/light/light_message_status.h"
#include "messages/aquarite/aquarite_message_getid.h"
#include "messages/aquarite/aquarite_message_percent.h"
#include "messages/aquarite/aquarite_message_ppm.h"
#include "messages/iaq/iaq_message_aux_status.h"
#include "messages/iaq/iaq_message_command_ready.h"
#include "messages/iaq/iaq_message_control_ready.h"
#include "messages/iaq/iaq_message_heartbeat.h"
#include "messages/iaq/iaq_message_main_status.h"
#include "messages/iaq/iaq_message_message_long.h"
#include "messages/iaq/iaq_message_onetouch_status.h"
#include "messages/iaq/iaq_message_page_button.h"
#include "messages/iaq/iaq_message_page_continue.h"
#include "messages/iaq/iaq_message_page_end.h"
#include "messages/iaq/iaq_message_page_message.h"
#include "messages/iaq/iaq_message_page_start.h"
#include "messages/iaq/iaq_message_poll.h"
#include "messages/iaq/iaq_message_startup.h"
#include "messages/iaq/iaq_message_table_message.h"
#include "messages/iaq/iaq_message_title_message.h"
#include "messages/pda/pda_message_clear.h"
#include "messages/pda/pda_message_highlight.h"
#include "messages/pda/pda_message_highlight_chars.h"
#include "messages/pda/pda_message_shiftlines.h"
#include "messages/serial_adapter/serial_adapter_message_dev_ready.h"
#include "messages/serial_adapter/serial_adapter_message_dev_status.h"
#include "types/jandy_types.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Equipment
{
	JandyEquipment::JandyEquipment(Kernel::HubLocator& hub_locator, bool decode_to_master) :
		IEquipment(),
		IStatusPublisher(Equipment::EquipmentStatus_Unknown{}),
		m_MessageConnections(),
		m_HubLocator(hub_locator),
		m_DecodeToMaster(decode_to_master)
	{
		m_DataHub = m_HubLocator.Find<Kernel::DataHub>();
		m_EquipmentHub = m_HubLocator.Find<Kernel::EquipmentHub>().get();
		m_StatsHub = m_HubLocator.Find<Kernel::StatisticsHub>();

		magic_enum::enum_for_each<Messages::JandyMessageIds>([this](auto id)
			{
				constexpr auto value = decltype(id)::value;
				m_StatsHub->MessageCounts[value] = 0;
			}
		);

		ConnectIdentify<
			Messages::JandyMessage_Ack,
			Messages::JandyMessage_Message,
			Messages::JandyMessage_MessageLong,
			Messages::JandyMessage_Probe,
			Messages::JandyMessage_Status,
			Messages::JandyMessage_Unknown,
			Messages::ChemlinkMessage_Response,
			Messages::EPumpMessage_Status,
			Messages::EPumpMessage_RPM,
			Messages::EPumpMessage_Watts,
			Messages::HeaterMessage_Request,
			Messages::HeaterMessage_Status,
			Messages::LightMessage_Status,
			Messages::AquariteMessage_GetId,
			Messages::AquariteMessage_Percent,
			Messages::AquariteMessage_PPM,
			Messages::IAQMessage_AuxStatus,
			Messages::IAQMessage_CommandReady,
			Messages::IAQMessage_ControlReady,
			Messages::IAQMessage_Heartbeat,
			Messages::IAQMessage_MainStatus,
			Messages::IAQMessage_MessageLong,
			Messages::IAQMessage_OneTouchStatus,
			Messages::IAQMessage_PageButton,
			Messages::IAQMessage_PageContinue,
			Messages::IAQMessage_PageEnd,
			Messages::IAQMessage_PageMessage,
			Messages::IAQMessage_PageStart,
			Messages::IAQMessage_Poll,
			Messages::IAQMessage_StartUp,
			Messages::IAQMessage_TableMessage,
			Messages::IAQMessage_TitleMessage,
			Messages::PDAMessage_Clear,
			Messages::PDAMessage_Highlight,
			Messages::PDAMessage_HighlightChars,
			Messages::PDAMessage_ShiftLines,
			Messages::SerialAdapterMessage_DevReady,
			Messages::SerialAdapterMessage_DevStatus
		>();

		m_MessageConnections.push_back(Messages::JandyMessage_Unknown::GetSignal()->connect(std::bind(&JandyEquipment::DisplayUnknownMessages, this, std::placeholders::_1)));
	}

	JandyEquipment::~JandyEquipment()
	{
		for (auto& connection : m_MessageConnections)
		{
			connection.disconnect();
		}

		magic_enum::enum_for_each<Messages::JandyMessageIds>([this](Messages::JandyMessageIds id)
			{
				LogInfo(Channel::Devices, [this, id] { return std::format("Stats: processed {} messages of type {}", m_StatsHub->MessageCounts[id], magic_enum::enum_name(id)); });
			}
		);

		LogInfo(
			Channel::Equipment,
			[this]
			{
				return std::format("Stats: {} total messages received",
					std::accumulate(m_StatsHub->MessageCounts.cbegin(), m_StatsHub->MessageCounts.cend(), static_cast<uint64_t>(0), [](const uint64_t previous, const decltype(m_StatsHub->MessageCounts)::value_type& elem)
					{
						return previous + elem.second.Count();
					}));
			}
		);
	}

	void JandyEquipment::IdentifyAndAddDevice(const Messages::JandyMessage& message)
	{
		if (Messages::JandyMessageIds::Probe == message.Id() || m_EquipmentHub->DeviceExists(message.Destination()))
		{
			// A probe message is just the Aqualink master looking for a (potentially non-existant) device,
			// or we already have this device in the devices list...do nothing.
		}
		else
		{
			auto device_id = std::make_unique<Devices::JandyDeviceType>(message.Destination().Id());

			switch (message.Destination().Class())
			{
			case Devices::DeviceClasses::IAQ:
				LogInfo(Channel::Equipment, std::format("Adding new IAQ device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::IAQDevice>(std::move(device_id), m_HubLocator, false));
				break;

			case Devices::DeviceClasses::AqualinkTouch:
				// Passively snoop a real iAqualink2's UI half (AqualinkTouch, 0x30-0x33).
				// The rich status (MainStatus/AuxStatus/pages) is sent to this address,
				// whereas the 0xA0-0xA3 IAQ side only carries the heartbeat. Created
				// NON-emulated, so it never transmits (ACKs are gated by IsEmulated in
				// emulated.h) -- it only decodes the stream into the DataHub. This lets us
				// "use" a real iAqualink2 by watching what it does, without bus contention.
				LogInfo(Channel::Equipment, std::format("Adding new AqualinkTouch (iAqualink2 UI) device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::IAQDevice>(std::move(device_id), m_HubLocator, false));
				break;

			case Devices::DeviceClasses::OneTouch:
				LogInfo(Channel::Equipment, std::format("Adding new OneTouch device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::OneTouchDevice>(std::move(device_id), m_HubLocator, false));
				break;

			case Devices::DeviceClasses::PDA:
				LogInfo(Channel::Equipment, std::format("Adding new PDA device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::PDADevice>(std::move(device_id), m_HubLocator, false));
				break;

			case Devices::DeviceClasses::RS_Keypad:
				LogInfo(Channel::Equipment, std::format("Adding new RS Keypad device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::KeypadDevice>(std::move(device_id), m_HubLocator, false));
				break;

			case Devices::DeviceClasses::DualSpaSwitch:
			case Devices::DeviceClasses::SpaRemote:
				// Spaside remotes ("Dual Spa Switch" = 2x4rem at 0x10-0x13, "Spa Link" = 8button
				// at 0x20-0x23) are spa-side keypads. SpasideRemoteDevice decodes their button
				// presses by correlating the master's poll (a cmd-0x02 Status to our id) with the
				// generic Ack the remote sends back to the master (see spaside_remote_device.h).
				// Created non-emulated: passively observe a real remote on the bus.
				LogInfo(Channel::Equipment, [&message] { return std::format("Adding new {} (spaside remote) device with id: {}", magic_enum::enum_name(message.Destination().Class()), message.Destination().Id()); });
				m_EquipmentHub->AddDevice(std::make_unique<Devices::SpasideRemoteDevice>(std::move(device_id), m_HubLocator, false));
				break;

			case Devices::DeviceClasses::SerialAdapter:
				LogInfo(Channel::Equipment, std::format("Adding new Serial Adapter device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::SerialAdapterDevice>(std::move(device_id), m_HubLocator, false));
				break;

			case Devices::DeviceClasses::LX_Heater:
			case Devices::DeviceClasses::JXi_Heater:
			case Devices::DeviceClasses::HeatPump:
				// All three heater classes map to the same HeaterDevice; the wire
				// class is preserved in the device id so they remain distinguishable.
				LogInfo(Channel::Equipment, [&message] { return std::format("Adding new {} device with id: {}", magic_enum::enum_name(message.Destination().Class()), message.Destination().Id()); });
				m_EquipmentHub->AddDevice(std::make_unique<Devices::HeaterDevice>(std::move(device_id)));
				break;

			case Devices::DeviceClasses::SWG_Aquarite:
				LogInfo(Channel::Equipment, std::format("Adding new SWG device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::AquariteDevice>(std::move(device_id), m_HubLocator));
				break;

			case Devices::DeviceClasses::Jandy_ePump:
			case Devices::DeviceClasses::Jandy_ePump_Ext:
				LogInfo(Channel::Equipment, std::format("Adding new ePump device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::EPumpDevice>(std::move(device_id)));
				break;

			case Devices::DeviceClasses::Chemlink:
				LogInfo(Channel::Equipment, std::format("Adding new Chemlink device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::ChemlinkDevice>(std::move(device_id)));
				break;

			case Devices::DeviceClasses::Jandy_Light:
				LogInfo(Channel::Equipment, std::format("Adding new Jandy Light device with id: {}", message.Destination().Id()));
				m_EquipmentHub->AddDevice(std::make_unique<Devices::LightsDevice>(std::move(device_id), m_HubLocator));
				break;

			default:
				if (m_DecodeToMaster && (Devices::DeviceClasses::AqualinkMaster == message.Destination().Class()))
				{
					// --decode-to-master: observe-only decode of frames addressed TO the master (0x00).
					// Never transmits / emulates / replays -- it only surfaces the decode for analysis.
					LogInfo(Channel::Messages, [this, &message] { return FormatToMasterTraffic(message); });
				}
				else if (const auto device_class = message.Destination().Class(); m_ReportedUnsupportedClasses.insert(device_class).second)
				{
					// First time this class is seen as unsupported: surface it once at
					// Notify so an operator sees the gap, then stay silent for every
					// subsequent message addressed to the same class (rate limiting --
					// previously this logged a Debug line on every such message).
					LogNotify(Channel::Equipment, [&device_class, &message] { return std::format("Device class ({}, {}) not supported.", magic_enum::enum_name(device_class), message.Destination().Id()); });
				}
				break;
			}
		}

		// Capture statistics, given we are processing every message.  Resolve the
		// per-id counter once (a single map lookup) and reuse it for both the
		// increment and the trace, instead of indexing MessageCounts twice.
		const auto message_id = message.Id();
		auto& message_counter = m_StatsHub->MessageCounts[message_id];
		++message_counter;

		LogTrace(Channel::Equipment, [&message_counter, &message_id] { return std::format("Stats: {} messages of type {} received", message_counter, magic_enum::enum_name(message_id)); });
	}

	void JandyEquipment::DisplayUnknownMessages(const Messages::JandyMessage& message)
	{
		LogDebug(
			Channel::Equipment,
			std::format(
				"Unknown message received -> cannot process: destination {} ({}), message id {} (0x{:02x}), length {} bytes, checksum 0x{:02x}",
				magic_enum::enum_name(message.Destination().Class()),
				message.Destination().Id(),
				magic_enum::enum_name(message.Id()),
				message.RawId(),
				message.MessageLength(),
				message.ChecksumValue()
			)
		);
	}

}
// namespace AqualinkAutomate::Equipment
