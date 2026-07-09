#pragma once

#include <cstdint>
#include <string>
#include <type_traits>

namespace AqualinkAutomate::Interfaces
{
	template<typename MESSAGE_ID>
	class IMessage
	{
	public:
		constexpr explicit IMessage(const MESSAGE_ID message_id) :
			m_Id(message_id)
		{
		}

		virtual ~IMessage() = default;

		constexpr MESSAGE_ID Id() const
		{
			return m_Id;
		}

		virtual uint8_t MaxPermittedPacketLength() const = 0;
		virtual uint8_t MinPermittedPacketLength() const = 0;
		virtual std::string ToString() const = 0;

		constexpr bool operator==(const IMessage& other) const
		{
			bool is_equal = true;

			is_equal &= (std::is_same<decltype(*this), decltype(other)>::value);
			is_equal &= (m_Id == other.m_Id);

			return is_equal;
		}

	protected:
		constexpr void SetId(MESSAGE_ID id) { m_Id = id; }

	private:
		MESSAGE_ID m_Id;
	};

}
// namespace AqualinkAutomate::Interfaces
