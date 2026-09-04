#pragma once

#include <concepts>
#include <format>
#include <ostream>
#include <string>
#include <type_traits>

#include <magic_enum/magic_enum.hpp>

#include "messages/jandy_message.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_message.h"
#include "messages/jandy_message_message_long.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_status.h"
#include "messages/jandy_message_unknown.h"
#include "messages/aquarite/aquarite_message_getid.h"
#include "messages/aquarite/aquarite_message_percent.h"
#include "messages/aquarite/aquarite_message_ppm.h"

// The std::formatter specialisations MUST be defined before the operator<<
// template below: that operator streams via std::format("{}", <JandyMessage&>),
// a non-dependent argument whose consteval format-string check is performed at
// template-definition time and therefore needs formatter<JandyMessage> to be a
// complete, declared specialisation at that point (otherwise the deleted primary
// template is implicitly instantiated and the later explicit specialisation is
// rejected as "specialisation after instantiation").

template<>
struct std::formatter<AqualinkAutomate::Messages::JandyMessageIds>
{
	constexpr auto parse(std::format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Messages::JandyMessageIds& msg_id, FormatContext& ctx) const
	{
		const auto v{ magic_enum::enum_name(msg_id) };
		return std::format_to(ctx.out(), "{}", v);
	}
};

template<>
struct std::formatter<AqualinkAutomate::Messages::JandyMessage>
{
	constexpr auto parse(std::format_parse_context& ctx)
	{
		return ctx.begin();
	}

	template<typename FormatContext>
	auto format(const AqualinkAutomate::Messages::JandyMessage& msg, FormatContext& ctx) const
	{
		const auto v{ msg.ToString() };
		return std::format_to(ctx.out(), "{}", v);
	}
};

// Every concrete JandyMessage-derived type formats via the base formatter; its
// virtual ToString() dispatches to the derived payload, so one constrained
// partial specialisation replaces the per-type formatter skeletons.  The
// explicit std::formatter<JandyMessage> above is more specialised and continues
// to win for the base type itself.
template <std::derived_from<AqualinkAutomate::Messages::JandyMessage> T>
struct std::formatter<T> : std::formatter<AqualinkAutomate::Messages::JandyMessage>
{
};

namespace AqualinkAutomate::Messages
{
	std::ostream& operator<<(std::ostream& os, const AqualinkAutomate::Messages::JandyMessageIds& obj);

	// A single constrained overload streams JandyMessage and every derived
	// message type (their virtual ToString() supplies the concrete payload),
	// replacing the previously hand-written per-type operator<< overloads.
	template <std::derived_from<AqualinkAutomate::Messages::JandyMessage> T>
	std::ostream& operator<<(std::ostream& os, const T& obj)
	{
		os << std::format("{}", static_cast<const AqualinkAutomate::Messages::JandyMessage&>(obj));
		return os;
	}

}
// namespace AqualinkAutomate::Messages
