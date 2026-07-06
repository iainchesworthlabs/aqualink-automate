#include <algorithm>
#include <cstddef>

#include "auth/entitlement.h"

namespace AqualinkAutomate::Auth
{

	namespace
	{
		constexpr bool IsValidActionChar(char ch)
		{
			return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ('_' == ch) || ('-' == ch);
		}

		// Validate a dotted action path: at least two non-empty segments of
		// [a-z0-9_-] separated by single dots.
		bool IsValidActionPath(std::string_view action)
		{
			if (action.empty())
			{
				return false;
			}

			std::size_t segment_count = 0;
			std::size_t segment_len = 0;

			for (const auto ch : action)
			{
				if ('.' == ch)
				{
					if (0 == segment_len)
					{
						return false; // Empty segment (leading dot or "..").
					}

					++segment_count;
					segment_len = 0;
				}
				else if (IsValidActionChar(ch))
				{
					++segment_len;
				}
				else
				{
					return false;
				}
			}

			if (0 == segment_len)
			{
				return false; // Trailing dot.
			}

			++segment_count;

			return segment_count >= 2;
		}
	}
	// anonymous namespace

	Entitlement::Entitlement(std::string action, std::optional<std::string> selector) :
		m_Action(std::move(action)),
		m_Selector(std::move(selector))
	{
	}

	std::optional<Entitlement> Entitlement::Parse(std::string_view text)
	{
		const auto colon_pos = text.find(':');

		const auto action = text.substr(0, colon_pos);

		if (!IsValidActionPath(action))
		{
			return std::nullopt;
		}

		if (std::string_view::npos == colon_pos)
		{
			return Entitlement{ std::string{ action } };
		}

		const auto selector = text.substr(colon_pos + 1);

		if (selector.empty())
		{
			return std::nullopt; // "action:" with nothing after the colon.
		}

		return Entitlement{ std::string{ action }, std::string{ selector } };
	}

	bool Entitlement::Matches(std::string_view action, std::string_view resource_id) const
	{
		if (m_Action != action)
		{
			return false;
		}

		if (!m_Selector.has_value())
		{
			// Selector-less entitlements only satisfy resource-less requests.
			return resource_id.empty();
		}

		if ("*" == *m_Selector)
		{
			return true;
		}

		return *m_Selector == resource_id;
	}

	std::string Entitlement::ToString() const
	{
		if (m_Selector.has_value())
		{
			return m_Action + ":" + *m_Selector;
		}

		return m_Action;
	}

	EntitlementSet EntitlementSet::Parse(const std::vector<std::string>& texts, std::vector<std::string>* rejected)
	{
		EntitlementSet set;

		for (const auto& text : texts)
		{
			if (auto entitlement = Entitlement::Parse(text); entitlement.has_value())
			{
				set.Add(std::move(*entitlement));
			}
			else if (nullptr != rejected)
			{
				rejected->push_back(text);
			}
		}

		return set;
	}

	void EntitlementSet::Add(Entitlement entitlement)
	{
		if (!Contains(entitlement))
		{
			m_Entitlements.push_back(std::move(entitlement));
		}
	}

	void EntitlementSet::Merge(const EntitlementSet& other)
	{
		for (const auto& entitlement : other.m_Entitlements)
		{
			Add(entitlement);
		}
	}

	bool EntitlementSet::Contains(const Entitlement& entitlement) const
	{
		return std::ranges::find(m_Entitlements, entitlement) != m_Entitlements.end();
	}

	bool EntitlementSet::Permits(std::string_view action, std::string_view resource_id) const
	{
		return std::ranges::any_of(m_Entitlements, [&](const auto& entitlement)
			{
				return entitlement.Matches(action, resource_id);
			});
	}

	std::vector<std::string> EntitlementSet::ToStrings() const
	{
		std::vector<std::string> strings;
		strings.reserve(m_Entitlements.size());

		for (const auto& entitlement : m_Entitlements)
		{
			strings.push_back(entitlement.ToString());
		}

		std::ranges::sort(strings);

		return strings;
	}

}
// namespace AqualinkAutomate::Auth
