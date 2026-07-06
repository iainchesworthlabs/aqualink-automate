#pragma once

#include <cstdint>

namespace AqualinkAutomate::Devices::IAQ
{

	// The poll-ACK command channel as seen by the write state machines and actuators. They push at
	// most one command per poll; the device drains it into the IAQ_Poll ACK. Extracted from
	// IAQDevice (SonarCloud S1820) so the writers can be serviced against a fake sink in isolation.
	// See docs/iaq_device_decomposition.md.
	class ICommandSink
	{
	public:
		virtual ~ICommandSink() = default;

		// Set the single next poll-ACK command (0x00 == send nothing / dwell this poll).
		virtual void IssueCommand(uint8_t command) = 0;

		// True when the command channel is busy: a multi-step command queue is draining or a
		// control-data handshake is in flight. A new write goal must not be armed on top of it.
		virtual bool IsBusy() const = 0;
	};

}
// namespace AqualinkAutomate::Devices::IAQ
