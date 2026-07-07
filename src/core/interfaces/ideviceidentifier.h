#pragma once

namespace AqualinkAutomate::Interfaces
{

	class IDeviceIdentifier
	{
	public:
		IDeviceIdentifier() = default;
		virtual ~IDeviceIdentifier() = default;

		bool operator==(const IDeviceIdentifier& other) const { return Equals(other); }
		// NOTE: operator!= is intentionally retained (not redundant per SonarCloud
		// cpp:S6186): derived types re-export it via `using ...::operator!=;`, which
		// requires the base to declare it explicitly rather than rely on the C++20
		// synthesised rewrite.
		bool operator!=(const IDeviceIdentifier& other) const { return !Equals(other); }

	protected:
		virtual bool Equals(const IDeviceIdentifier& other) const = 0;
	};

}
// namespace AqualinkAutomate::Interfaces
