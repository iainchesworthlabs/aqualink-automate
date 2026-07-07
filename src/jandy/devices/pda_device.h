#pragma once

#include <chrono>
#include <list>

#include "devices/jandy_controller.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/describable.h"
#include "devices/capabilities/emulated.h"
#include "devices/capabilities/restartable.h"
#include "devices/capabilities/scrapeable.h"
#include "devices/capabilities/screen.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_message_long.h"
#include "messages/jandy_message_status.h"
#include "messages/jandy_message_unknown.h"
#include "messages/pda/pda_message_clear.h"
#include "messages/pda/pda_message_highlight.h"
#include "messages/pda/pda_message_highlight_chars.h"
#include "messages/pda/pda_message_shiftlines.h"
#include "kernel/hub_locator.h"

namespace AqualinkAutomate::Devices
{

	class PDADevice : public JandyController, public Capabilities::Restartable, public Capabilities::Screen, public Capabilities::Scrapeable, public Capabilities::Emulated, public Capabilities::Describable
	{
		inline static constexpr uint8_t PDA_PAGE_LINES{ 10 };
		inline static constexpr Scrapeable::ScrapeId PDA_CONFIG_INIT_SCRAPER{ 1 };
		inline static constexpr std::chrono::seconds PDA_TIMEOUT_DURATION{ std::chrono::seconds(30) };

		enum class KeyCommands : uint8_t
		{
			NoKeyCommand = 0x00,
			HotKey1 = 0x01,
			Back = 0x02,
			HotKey2 = 0x03,
			Select = 0x04,
			Down = 0x05,
			Up = 0x06,
			Unknown = 0xFF
		};

	public:
		PDADevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated);
		~PDADevice() override = default;

		nlohmann::json DescribeDiagnostics() const override;

	private:
		void ProcessControllerUpdates() override;

		void WatchdogTimeoutOccurred() override;

		void Slot_PDA_Ack(const Messages::JandyMessage_Ack& msg);
		void Slot_PDA_Clear(const Messages::PDAMessage_Clear& msg);
		void Slot_PDA_Highlight(const Messages::PDAMessage_Highlight& msg);
		void Slot_PDA_HighlightChars(const Messages::PDAMessage_HighlightChars& msg);
		void Slot_PDA_MessageLong(const Messages::JandyMessage_MessageLong& msg);
		void Slot_PDA_Probe(const Messages::JandyMessage_Probe& msg);
		void Slot_PDA_Status(const Messages::JandyMessage_Status& msg);
		void Slot_PDA_ShiftLines(const Messages::PDAMessage_ShiftLines& msg);
		void Slot_PDA_Unknown_PDA_1B(const Messages::JandyMessage_Unknown& msg);

		void PageProcessor_System(const Utility::ScreenDataPage& page);
		void PageProcessor_SetTemperature(const Utility::ScreenDataPage& page);
		void PageProcessor_SetTime(const Utility::ScreenDataPage& page);
		void PageProcessor_PoolHeat(const Utility::ScreenDataPage& page);
		void PageProcessor_SpaHeat(const Utility::ScreenDataPage& page);
		void PageProcessor_AquaPure(const Utility::ScreenDataPage& page);
		void PageProcessor_FreezeProtect(const Utility::ScreenDataPage& page);
		void PageProcessor_EquipmentStatus(const Utility::ScreenDataPage& page);
		void PageProcessor_Boost(const Utility::ScreenDataPage& page);
		void PageProcessor_FirmwareVersion(const Utility::ScreenDataPage& page);
	};

}
// namespace AqualinkAutomate::Devices
