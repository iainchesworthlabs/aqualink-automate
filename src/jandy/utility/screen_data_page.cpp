#include "utility/screen_data_page.h"
#include "logging/logging.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Utility
{

	ScreenDataPage::ScreenDataPage(std::size_t row_count)
	{
		m_Rows.reserve(row_count);

		for (std::size_t i = 0; i < row_count; i++)
		{
			m_Rows.push_back(DEFAULT_ROW_DATA);
		}
	}

	ScreenDataPage::RowType& ScreenDataPage::operator[](std::size_t index)
	{
		return m_Rows.at(index);
	}

	const ScreenDataPage::RowType& ScreenDataPage::operator[](std::size_t index) const
	{
		return m_Rows.at(index);
	}

	void ScreenDataPage::Clear()
	{
		for (auto& row : m_Rows)
		{
			row = DEFAULT_ROW_DATA;
		}
	}

	void ScreenDataPage::Highlight(uint8_t line_index)
	{
		if (CLEAR_HIGHLIGHTS == line_index)
		{
			LogTrace(Channel::Devices, "ScreenDataPage: Clearing all previously set highlighted lines");

			for (auto& row : m_Rows)
			{
				row.HighlightState = HighlightStates::Normal;
				row.HighlightRange = std::nullopt;
			}
		}
		else if (m_Rows.size() <= line_index)
		{
			LogDebug(Channel::Devices, std::format("ScreenDataPage: Cannot toggle highlight, line id is out of range; requested line id -> {}, max line id -> {}", line_index, m_Rows.size()));
		}
		else
		{
			// Clear all existing highlights first - there can only be one highlighted line at a time
			// (the cursor position). Without this, highlights accumulate as new ones are set.
			for (auto& row : m_Rows)
			{
				row.HighlightState = HighlightStates::Normal;
				row.HighlightRange = std::nullopt;
			}

			m_Rows[line_index].HighlightState = HighlightStates::Highlighted;
			m_Rows[line_index].HighlightRange = std::nullopt;
		}
	}

	void ScreenDataPage::HighlightChars(uint8_t line_index, uint8_t start_index, uint8_t stop_index)
	{
		if (m_Rows.size() <= line_index)
		{
			LogDebug(Channel::Devices, std::format("ScreenDataPage: Cannot toggle highlight, line id is out of range; requested line id -> {}, max line id -> {}", line_index, m_Rows.size()));
		}
		else
		{
			m_Rows[line_index].HighlightState = HighlightStates::PartiallyHighlighted;
			m_Rows[line_index].HighlightRange = { start_index, stop_index };
		}
	}

	void ScreenDataPage::ShiftLines(ShiftDirections direction, uint8_t start_id, uint8_t end_id, uint8_t lines_to_shift)
	{
		if (m_Rows.size() < 2 || start_id > (m_Rows.size() - 2))
		{
			// Out of range - no suitable lines or not enough to rotate
			LogDebug(Channel::Devices, std::format("ScreenDataPage: cannot shift lines, start index out of range; start index -> {} (0-based); total lines -> {}", start_id, m_Rows.size()));
		}
		else if (end_id > (m_Rows.size() - 1))
		{
			// Out of range - no suitable lines or not enough to rotate
			LogDebug(Channel::Devices, std::format("ScreenDataPage: cannot shift lines, end index out of range; end index -> {} (0-based); total lines -> {}", end_id, m_Rows.size()));
		}
		else if (start_id >= end_id)
		{
			// Span is not the correct range; cannot rotate.
			LogDebug(Channel::Devices, std::format("ScreenDataPage: cannot shift lines, start index must preceed end index by at least 1; start index -> {}, end index -> {}", start_id, end_id));
		}
		else if (const auto span_size = static_cast<std::size_t>(end_id - start_id) + 1; (lines_to_shift == 0) || (lines_to_shift > span_size))
		{
			// A shift of 0 lines is a no-op; a shift larger than the span would push the
			// rotate pivot (start + offset) or the clear range outside [start, end), causing
			// std::rotate / std::for_each to read/write out of bounds. Reject both.
			LogDebug(Channel::Devices, std::format("ScreenDataPage: cannot shift lines, number of shifts out of range; shifts -> {}, span size -> {} (start -> {}, end -> {})", lines_to_shift, span_size, start_id, end_id));
		}
		else
		{
			// Get iterators to the start and end of the range
			auto start = m_Rows.begin() + start_id;
			auto end = m_Rows.begin() + end_id + 1;

			// Calculate the offset for the rotation
			const auto offset = (direction == ShiftDirections::Up) ? static_cast<RowCollection::difference_type>(lines_to_shift) : (end - start) - lines_to_shift;

			// Rotate the range left or right by the specified number of elements
			std::rotate(start, start + offset, end);

			// Erase the elements that were rotated to the end of the range
			if (direction == ShiftDirections::Up)
			{
				std::for_each(end - lines_to_shift, end, [](auto& elem) { elem = DEFAULT_ROW_DATA; });
			}
			else if (direction == ShiftDirections::Down)
			{
				std::for_each(start, start + lines_to_shift, [](auto& elem) { elem = DEFAULT_ROW_DATA; });
			}
			else
			{
				LogDebug(Channel::Devices, "ScreenDataPage: Got a weird shift direction (not up or down)...doing nothing");
			}
		}
	}

	std::size_t ScreenDataPage::Size() const
	{
		return m_Rows.size();
	}

}
// namespace AqualinkAutomate::Utility
