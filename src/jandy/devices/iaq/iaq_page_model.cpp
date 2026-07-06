#include "devices/iaq/iaq_page_model.h"

#include "utility/string_manipulation.h"

namespace AqualinkAutomate::Devices::IAQ
{

	void PageModel::UpsertButton(uint8_t index, std::string name, Messages::ButtonStatuses status)
	{
		m_Buttons[index] = PageButtonInfo{ std::move(name), status };
	}

	std::optional<uint8_t> PageModel::FindButtonByLabel(const std::string& label) const
	{
		const std::string target{ Utility::TrimWhitespace(label) };
		if (target.empty())
		{
			return std::nullopt;
		}

		// Home-page button names carry a trailing status suffix (e.g. "Pool LightON"), so a
		// prefix match against the trimmed label resolves the live button index.
		for (const auto& [index, info] : m_Buttons)
		{
			if (Utility::TrimWhitespace(info.name).starts_with(target))
			{
				return index;
			}
		}

		return std::nullopt;
	}

	void PageModel::OnPageStart(uint8_t id)
	{
		m_PageId = id;

		// A fresh page invalidates any accumulated Schedule-list title/rows and the
		// device-picker rows the schedule writer reads.
		m_Title.clear();
		m_ScheduleRows.clear();
		m_DevicePickerRows.clear();
	}

}
// namespace AqualinkAutomate::Devices::IAQ
