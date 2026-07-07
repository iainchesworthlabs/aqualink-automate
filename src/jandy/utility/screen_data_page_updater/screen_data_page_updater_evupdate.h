#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <boost/statechart/event.hpp>

namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
{

	class evUpdate : public boost::statechart::event<evUpdate>
	{
	public:
		using ScreenDataPageLine = std::pair<uint8_t, std::string>;

	public:
		evUpdate(uint8_t line_id, const std::string& line_text);

		ScreenDataPageLine::first_type Id() const;
		const ScreenDataPageLine::second_type& Text() const;

	private:
		const ScreenDataPageLine m_Line;
	};

}
// namespace AqualinkAutomate::Utility::ScreenDataPageUpdaterImpl
