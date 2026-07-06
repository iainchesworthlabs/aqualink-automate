#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "messages/iaq/iaq_message_page_button.h"

namespace AqualinkAutomate::Devices::IAQ
{

	// The decoded "what is on screen right now" state for the AqualinkTouch (0x33) page UI,
	// rebuilt from the master's PageStart / PageButton / TableMessage / TitleMessage frames.
	// Written by the IAQ message slots and read by the actuators and the write state machines
	// (spa-switch + controller-schedule). Extracted from IAQDevice (SonarCloud S1448/S1820) so
	// those collaborators can be exercised against a hand-built page model without the bus.
	//
	// Behaviour note: this is a faithful move of the former IAQDevice members -- a fresh page
	// (OnPageStart) invalidates the title and the schedule / device-picker row accumulators, but
	// NOT the button table (buttons refresh/erase per PageButton frame) and NOT the spa-switch
	// picker rows (those clear on their own group-0x01 header row). Preserve that exactly.
	class PageModel
	{
	public:
		// The live on-screen PageButton table for the CURRENT page (index -> name + status),
		// rebuilt from the master's IAQMessage_PageButton frames. DeviceActuator looks an aux up
		// here by name to get its (dynamic) button index. Button indices shift as the page's
		// device list changes, so always resolve by name rather than caching an index.
		struct PageButtonInfo
		{
			std::string name;
			Messages::ButtonStatuses status{ Messages::ButtonStatuses::Unknown };
		};

		// The page identifier of the page the master is currently pushing (IAQ_PageStart's first
		// payload byte: 0x01 home, 0x0f menu, 0x14 Setup, 0x3a Spa Remotes, 0x3b the 4-Function
		// detail). Used to page-GATE the writers so they never issue a row-select/commit off page.
		uint8_t PageId() const { return m_PageId; }

		// The page title (from TitleMessage). The Schedule list page's title carries the program
		// group ("Schedule Group A"/"B" or a custom label).
		const std::string& Title() const { return m_Title; }
		void SetTitle(std::string title) { m_Title = std::move(title); }

		// On-screen button table for the CURRENT page.
		const std::map<uint8_t, PageButtonInfo>& Buttons() const { return m_Buttons; }

		// A named button refreshes its entry; a blank name clears that slot so a stale name can't
		// be matched after the page changes.
		void UpsertButton(uint8_t index, std::string name, Messages::ButtonStatuses status);
		void EraseButton(uint8_t index) { m_Buttons.erase(index); }

		// Find the index of the on-screen PageButton whose name matches `label` (prefix match,
		// since home-page button names carry a trailing status suffix e.g. "Pool LightON").
		std::optional<uint8_t> FindButtonByLabel(const std::string& label) const;

		// Schedule-list rows accumulated on IAQ_SCHEDULE_PAGE_ID (0x28): attribute (1-based entry
		// ordinal) -> the row's ASCII text.
		const std::map<uint8_t, std::string>& ScheduleRows() const { return m_ScheduleRows; }
		void SetScheduleRow(uint8_t attribute, std::string text) { m_ScheduleRows[attribute] = std::move(text); }

		// Device-picker rows on the schedule editor (0x38): attribute (visible row) -> device label.
		const std::map<uint8_t, std::string>& DevicePickerRows() const { return m_DevicePickerRows; }
		void SetDevicePickerRow(uint8_t attribute, std::string label) { m_DevicePickerRows[attribute] = std::move(label); }

		// The 4-Function detail's device/function PICKER (group-0x01 rows): slot -> function.
		const std::map<uint8_t, std::string>& SpaSwitchPickerRows() const { return m_SpaSwitchPickerRows; }
		void SetSpaSwitchPickerRow(uint8_t slot, std::string function) { m_SpaSwitchPickerRows[slot] = std::move(function); }
		void ClearSpaSwitchPickerRows() { m_SpaSwitchPickerRows.clear(); }

		// A fresh PageStart: latch the new page id and invalidate the title + schedule / device
		// picker accumulators. Buttons and the spa-switch picker are intentionally NOT cleared here.
		void OnPageStart(uint8_t id);

	private:
		uint8_t m_PageId{ 0x00 };
		std::string m_Title;
		std::map<uint8_t, PageButtonInfo> m_Buttons;
		std::map<uint8_t, std::string> m_ScheduleRows;
		std::map<uint8_t, std::string> m_DevicePickerRows;
		std::map<uint8_t, std::string> m_SpaSwitchPickerRows;
	};

}
// namespace AqualinkAutomate::Devices::IAQ
