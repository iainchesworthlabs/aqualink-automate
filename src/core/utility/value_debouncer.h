#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>

namespace AqualinkAutomate::Utility
{

	template<typename DEBOUNCED_VALUE_TYPE, typename VALUE_COMPARATOR = std::equal_to<DEBOUNCED_VALUE_TYPE>>
	class ValueDebouncer
	{
		static_assert(std::is_default_constructible_v<DEBOUNCED_VALUE_TYPE>, "DEBOUNCED_VALUE_TYPE must be default constructible");
		static_assert(std::is_copy_assignable_v<DEBOUNCED_VALUE_TYPE>, "DEBOUNCED_VALUE_TYPE must be copy-assignable");
		static_assert(std::is_move_assignable_v<DEBOUNCED_VALUE_TYPE>, "DEBOUNCED_VALUE_TYPE must be move-assignable");

	public:
		static const uint32_t DEFAULT_DEBOUNCE_THRESHOLD = 10;

	public:
		explicit ValueDebouncer(uint32_t threshold = DEFAULT_DEBOUNCE_THRESHOLD) :
			m_Threshold(threshold)
		{
		}

	public:
		void operator()(const DEBOUNCED_VALUE_TYPE& future_value)
		{
			Update(future_value);
		}

		void operator()(DEBOUNCED_VALUE_TYPE&& future_value)
		{
			Update(std::move(future_value));
		}

		const DEBOUNCED_VALUE_TYPE& operator()() const
		{
			return m_CurrentValue;
		}

		ValueDebouncer& operator=(const DEBOUNCED_VALUE_TYPE& future_value)
		{
			Update(future_value);
			return *this;
		}

		ValueDebouncer& operator=(DEBOUNCED_VALUE_TYPE&& future_value)
		{
			Update(std::move(future_value));
			return *this;
		}

	private:
		// Single forwarding implementation for both the lvalue and rvalue overloads.
		// FUTURE_VALUE_TYPE is deduced to const&/&& and forwarded only at the one site
		// that consumes future_value (the reset branch). On commit we move out of our
		// own m_FutureValue member: it is internal state that is always overwritten
		// before reuse (a matching value short-circuits through the current-value
		// branch), so the move is observably identical to the previous copy.
		template<typename FUTURE_VALUE_TYPE>
		requires std::convertible_to<FUTURE_VALUE_TYPE&&, DEBOUNCED_VALUE_TYPE>
		void Update(FUTURE_VALUE_TYPE&& future_value)
		{
			if (m_ValueComparator(future_value, m_CurrentValue))
			{
				// The future value is the same as the current value so let's ignore it.
			}
			else if (m_ValueComparator(future_value, m_FutureValue))
			{
				m_UpdateCount++;

				if (m_Threshold > m_UpdateCount)
				{
					// The future value is different and is stable however there have not enough updates yet so don't change current value.
				}
				else
				{
					m_CurrentValue = std::move(m_FutureValue);
					m_UpdateCount = 0;
				}
			}
			else
			{
				/// The future value is different to both current and future values so let's reset and track this value for the debounce count.
				m_FutureValue = std::forward<FUTURE_VALUE_TYPE>(future_value);
				m_UpdateCount = 0;
			}
		}

	private:
		const uint32_t m_Threshold;
		uint32_t m_UpdateCount{ 0 };

		DEBOUNCED_VALUE_TYPE m_CurrentValue{};
		DEBOUNCED_VALUE_TYPE m_FutureValue{};
		VALUE_COMPARATOR m_ValueComparator{};
	};

}
// namespace AqualinkAutomate::Utility
