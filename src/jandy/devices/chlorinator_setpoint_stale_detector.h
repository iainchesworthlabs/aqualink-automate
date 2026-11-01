#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace AqualinkAutomate::Devices
{

	// Decides when the configured chlorinator (AquaPure) setpoint shown to users has gone STALE
	// and is worth an immediate Set AquaPure menu re-scrape, instead of waiting out the periodic
	// refresh interval.
	//
	// Why this exists: the configured Pool/Spa % can only be read by visiting the Set AquaPure
	// menu page (the RS-485 protocol has no "get setpoint" query), so a change made at the panel
	// or in iAqualink is invisible until the next scheduled visit. The live AQUARITE_Percent
	// (0x11) frame DOES carry what the panel is currently commanding, every ~0.4 s, but only for
	// whichever body the panel is serving - and this app cannot always tell which body that is
	// (some panels never report one as active). So the wire value is deliberately NOT written
	// into the setpoint; it is only used here as a cheap "something changed, go and look" signal.
	// The scrape stays the single source of truth for the configured value.
	//
	// All the policy lives here, with no device or bus dependency, so it is unit-testable in
	// isolation (mirroring ChlorinatorSetpointRefresh, which consumes the request).
	class ChlorinatorSetpointStaleDetector
	{
	public:
		using Clock = std::chrono::steady_clock;
		using TimePoint = Clock::time_point;

		// Consecutive disagreeing 0x11 frames required before asking (about 2 s at the observed
		// ~2.4 frames/s): ignores a single corrupt or in-flight frame.
		static constexpr unsigned REQUIRED_FRAMES{ 5 };

		// Minimum gap between two requests, however the situation evolves, so a persistently
		// confusing panel can never turn into a stream of menu walks.
		static constexpr std::chrono::seconds COOLDOWN{ 60 };

		struct Observation
		{
			// What the panel is commanding right now (a real 1..100 %); nullopt for the Boost and
			// Service-mode sentinels. 0 is also ignored: it is both "configured off" and "idle",
			// so it says nothing reliable about the configured setpoint.
			std::optional<std::uint8_t> WirePercent;

			// The configured target the app currently shows (the active body's configured value,
			// WITHOUT the passive last-generating fallback, which is derived from this very wire
			// value and would always "agree" with it).
			std::optional<std::uint8_t> ConfiguredSetpoint;

			// Both bodies' configured values, used only to recognise "I already asked about exactly
			// this situation and the scrape did not change it" (for example the panel is in spa
			// mode while the app believes pool): no point asking again.
			std::optional<std::uint8_t> PoolSetpoint;
			std::optional<std::uint8_t> SpaSetpoint;

			// This app queued a set moments ago and the panel has not finished applying it (a
			// menu walk takes 10-20 s). The difference is expected, not staleness.
			bool CommandRecent{ false };
		};

		// Feed one 0x11 frame's observation. Returns true exactly when a refresh should be
		// requested now.
		bool Observe(const Observation& observation, TimePoint now)
		{
			if (!observation.WirePercent.has_value() || (0 == observation.WirePercent.value()) || observation.CommandRecent)
			{
				m_DisagreeingFrames = 0;
				return false;
			}

			const auto wire = observation.WirePercent.value();

			// The confidence count is for ONE commanded value: a wire value that moves around (the
			// panel stepping, or flipping between bodies) is not yet a settled disagreement.
			if (!m_LastWire.has_value() || (m_LastWire.value() != wire))
			{
				m_DisagreeingFrames = 0;
			}
			m_LastWire = wire;

			if (observation.ConfiguredSetpoint.has_value() && (observation.ConfiguredSetpoint.value() == wire))
			{
				// In step again: a later disagreement is a new event, not a repeat of an old one.
				m_DisagreeingFrames = 0;
				m_Asked.reset();
				return false;
			}

			if (m_DisagreeingFrames < REQUIRED_FRAMES)
			{
				++m_DisagreeingFrames;
			}

			if (m_DisagreeingFrames < REQUIRED_FRAMES)
			{
				return false;
			}

			const Situation situation{ wire, observation.PoolSetpoint, observation.SpaSetpoint };

			if (m_Asked.has_value() && (m_Asked.value() == situation))
			{
				return false;   // already asked about exactly this; the scrape did not change it
			}

			if (m_HasRequested && ((now - m_LastRequest) < COOLDOWN))
			{
				return false;
			}

			m_Asked = situation;
			m_LastRequest = now;
			m_HasRequested = true;
			return true;
		}

	private:
		struct Situation
		{
			std::uint8_t Wire{ 0 };
			std::optional<std::uint8_t> Pool;
			std::optional<std::uint8_t> Spa;

			bool operator==(const Situation&) const = default;
		};

		unsigned m_DisagreeingFrames{ 0 };
		std::optional<std::uint8_t> m_LastWire;
		std::optional<Situation> m_Asked;
		bool m_HasRequested{ false };
		TimePoint m_LastRequest{};
	};

}
// namespace AqualinkAutomate::Devices
