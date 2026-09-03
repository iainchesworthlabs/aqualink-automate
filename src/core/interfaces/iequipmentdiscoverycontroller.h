#pragma once

#include <cstddef>

namespace AqualinkAutomate::Interfaces
{

	/// @brief Runtime control surface for on-demand auxillary re-discovery.
	///
	/// Implemented by the OneTouch controller (the one that owns the SpiderEngine crawl).
	/// Registered in the HubLocator so a diagnostics HTTP route can resolve it via TryFind<> and
	/// trigger a full "clear stale auxillaries, then rediscover" pass without restarting the
	/// application.
	class IEquipmentDiscoveryController
	{
	public:
		// Named DiscoveryStatusSnapshot, not Status: the implementer (OneTouchDevice) already
		// inherits IStatusPublisher, whose own `Status` member makes a same-named nested type here
		// an ambiguous-lookup compile error the moment both are in scope together.
		struct DiscoveryStatusSnapshot
		{
			bool in_progress{ false };          ///< True while a rediscovery crawl is running.
			std::size_t last_cleared_count{ 0 }; ///< Auxillaries removed by the most recent clear.
		};

	public:
		virtual ~IEquipmentDiscoveryController() = default;

		/// @brief Clear every auto-detected auxillary (sparing any forced Present by an operator
		///        override) and start a fresh full-discovery crawl.
		/// @returns true if the rediscovery was started; false if one is already in progress or
		///          the controller is not currently active.
		virtual bool RequestFullRediscovery() = 0;

		/// @brief Current controller status (in-progress flag, last clear count).
		virtual DiscoveryStatusSnapshot DiscoveryStatus() const = 0;
	};

}
// namespace AqualinkAutomate::Interfaces
