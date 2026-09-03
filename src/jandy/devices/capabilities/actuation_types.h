#pragma once

namespace AqualinkAutomate::Devices::Capabilities
{

	// The action a caller wants applied to a logical device. Mirrors
	// Interfaces::ICommandDispatcher::DeviceAction so the dispatcher can translate
	// without the capability layer depending on the core command interface.
	enum class ActuationAction
	{
		On,
		Off,
		Toggle
	};

	// The outcome of asking a controller to actuate something. The dispatcher maps
	// this back onto Interfaces::ICommandDispatcher::CommandResult.
	//
	//   Accepted      - the controller queued the command for emission on the wire.
	//   NotSupported  - this controller cannot perform the requested action.
	//   InvalidValue  - the request was well-formed but the value was out of range.
	//   MappingFailed - the controller is capable in general but could not map THIS
	//                   particular device/request onto a wire command.
	//   Busy          - the controller CAN perform this action but is currently mid-way
	//                   through an earlier goal on the same shared command channel (e.g. a
	//                   OneTouch menu walk or an IAQ page write already in flight). This is
	//                   transient -- retrying once that goal finishes is expected to succeed,
	//                   unlike NotSupported/MappingFailed which will not resolve on their own
	//                   -- so the dispatcher can report a "still applying, try again shortly"
	//                   result instead of the "no capable controller" failure it uses when
	//                   nothing could ever have served the request.
	enum class ActuationResult
	{
		Accepted,
		NotSupported,
		InvalidValue,
		MappingFailed,
		Busy
	};

	// When more than one connected controller advertises the same actuation
	// capability (e.g. a Serial Adapter, an AqualinkTouch AND a OneTouch can all
	// toggle equipment), the dispatcher prefers the numerically-lowest priority.
	// Ranking follows how directly each controller can effect the action:
	//   * Serial Adapter (RSSA)      - direct, stateless command bytes  -> High
	//   * AqualinkTouch / iAqualink2 - direct page-button + value-submit -> Medium
	//   * OneTouch                   - menu navigation / screen scraping -> Low
	//   * PDA                        - menu navigation (last resort)     -> Lowest
	// The AqualinkTouch outranks the OneTouch because its page-button/value-submit
	// commands take effect immediately, whereas the OneTouch must crawl menus.
	enum class ActuationPriority
	{
		High = 0,    // direct, stateless command channel (e.g. Serial Adapter / RSSA)
		Medium = 1,  // direct page-button / value-submit channel (e.g. AqualinkTouch / IAQ)
		Low = 2,     // menu-navigation / scraper channel (e.g. OneTouch)
		Lowest = 3   // last-resort menu-navigation channel (e.g. PDA)
	};

}
// namespace AqualinkAutomate::Devices::Capabilities
