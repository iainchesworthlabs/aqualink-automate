#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>

#include "kernel/auxillary_devices/auxillary_device.h"

namespace AqualinkAutomate::Kernel
{
	AuxillaryDevice::AuxillaryDevice() :
		m_Id(boost::uuids::random_generator()())
	{
	}

	AuxillaryDevice::AuxillaryDevice(std::string_view label) :
		AuxillaryDevice()
	{
		AuxillaryTraits.Set(AuxillaryTraitsTypes::LabelTrait{}, std::string{label});
	}

	AuxillaryDevice::AuxillaryDevice(boost::uuids::uuid id) :
		m_Id(id)
	{
	}

	boost::uuids::uuid AuxillaryDevice::Id() const
	{
		return m_Id;
	}

	bool AuxillaryDevice::operator==(const AuxillaryDevice& other) const
	{
		using AuxillaryTraitsTypes::AuxillaryTypeTrait;
		using AuxillaryTraitsTypes::LabelTrait;

		bool trait_check_performed = false;
		bool matches = true;

		// First...attempt to match traits like type and label...

		if (AuxillaryTraits.Has(AuxillaryTypeTrait{}) && other.AuxillaryTraits.Has(AuxillaryTypeTrait{}))
		{
			AuxillaryTypeTrait::TraitValue type1{ AuxillaryTraits[AuxillaryTypeTrait{}] };
			AuxillaryTypeTrait::TraitValue type2{ other.AuxillaryTraits[AuxillaryTypeTrait{}] };

			matches &= (type1 == type2);

			if (matches && AuxillaryTraits.Has(LabelTrait{}) && other.AuxillaryTraits.Has(LabelTrait{}))
			{
				LabelTrait::TraitValue label1{ AuxillaryTraits[LabelTrait{}] };
				LabelTrait::TraitValue label2{ other.AuxillaryTraits[LabelTrait{}] };

				matches &= (label1 == label2);
				trait_check_performed = true;
			}
		}

		if (!trait_check_performed)
		{
			// Couldn't match on traits...attempt an id match.  This is a poor choice given how the factory generates ids.
			matches &= (Id() == other.Id());
		}
		
		return matches;
	}

	bool AuxillaryDevice::operator==(const boost::uuids::uuid id) const
	{
		// This is a poor choice given how the factory generates ids.
		return (Id() == id);
	}

	bool AuxillaryDevice::operator==(const std::string& id) const
	{
		try
		{
			boost::uuids::string_generator gen;
			boost::uuids::uuid uuid = gen(id);

			return (Id() == uuid);
		}
		catch (const std::runtime_error& ex_re)
		{
			// The string is invalid did not decode into a UUID correctly.
			return false;
		}
	}

	bool AuxillaryDevice::operator!=(const AuxillaryDevice& other) const
	{
		return !operator==(other);
	}

	AuxillaryHealthStates AuxillaryDevice::Health() const
	{
		using AuxillaryTraitsTypes::ErrorCodesTrait;
		using enum AuxillaryHealthStates;

		if (AuxillaryTraits.Has(ErrorCodesTrait{}))
		{
			if (ErrorCodesTrait::TraitValue errors{AuxillaryTraits[ErrorCodesTrait{}]}; 0 != errors.size())
			{
				// Presense of errors means the device is unhealthy.
				return Unhealthy;
			}

			// No errors...assume device is healthy.
			return Healthy;
		}

		// No mechanism available to determine health.
		return Unknown;
	}

}
// namespace AqualinkAutomate::Kernel
