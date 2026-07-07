#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace AqualinkAutomate::Utility
{

	class ScreenDataPage
	{
	public:
		enum class HighlightStates
		{
			Normal,
			Highlighted,
			PartiallyHighlighted
		};

		struct HighlightRangeType
		{
			uint8_t Start;
			uint8_t Stop;
		};

		struct RowType
		{
			std::string Text;

			HighlightStates HighlightState;
			std::optional<HighlightRangeType> HighlightRange;
		};

	private:
		using RowCollection = std::vector<RowType>;

		inline static const RowType DEFAULT_ROW_DATA = { std::string(), HighlightStates::Normal, std::nullopt };
		inline static const uint8_t CLEAR_HIGHLIGHTS = 0xFF;

	public:
		explicit ScreenDataPage(std::size_t row_count);

		RowType& operator[](std::size_t index);
		const RowType& operator[](std::size_t index) const;

		enum class ShiftDirections
		{
			Up, Down
		};

		void Clear();
		void Highlight(uint8_t line_id);
		void HighlightChars(uint8_t line_id, uint8_t start_index, uint8_t stop_index);
		void ShiftLines(ShiftDirections direction, uint8_t start_id, uint8_t end_id, uint8_t lines_to_shift);
		std::size_t Size() const;

	private:
		RowCollection m_Rows;

	private:
		friend struct std::formatter<ScreenDataPage>;
	};

}
// namespace AqualinkAutomate::Utility
