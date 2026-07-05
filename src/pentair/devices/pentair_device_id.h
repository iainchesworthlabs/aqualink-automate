#pragma once

#include <cstdint>

#include "interfaces/ideviceidentifier.h"

namespace AqualinkAutomate::Pentair::Devices
{

	// A Pentair device address (the FROM/DEST byte on the wire), wrapped as an
	// IDeviceIdentifier so Pentair devices satisfy the core IDevice contract.
	class PentairDeviceId : public Interfaces::IDeviceIdentifier
	{
	public:
		PentairDeviceId() = default;
		explicit PentairDeviceId(uint8_t address);
		~PentairDeviceId() override = default;

		uint8_t Address() const;
		uint8_t operator()() const;

	protected:
		bool Equals(const Interfaces::IDeviceIdentifier& other) const override;

	private:
		uint8_t m_Address{ 0 };
	};

}
// namespace AqualinkAutomate::Pentair::Devices
