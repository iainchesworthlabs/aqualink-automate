#include <boost/test/unit_test.hpp>

#include <memory>

#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_helpers.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"

// Regression coverage for the distinct status-trait keys: ConvertStatusToString must resolve the
// correct category status for each device type, and HasStatus must report presence consistently
// (it replaced the previous Has(StatusTrait{}) "catch-all" probe that relied on the shared key).

BOOST_AUTO_TEST_SUITE(AuxillaryTraitsStatusTestSuite)

namespace
{
	using namespace AqualinkAutomate::Kernel;
	using namespace AqualinkAutomate::Kernel::AuxillaryTraitsTypes;

	std::shared_ptr<AuxillaryDevice> MakeTypedDevice(AuxillaryTypes type)
	{
		auto device = std::make_shared<AuxillaryDevice>("TestDevice");
		device->AuxillaryTraits.Set(AuxillaryTypeTrait{}, type);
		return device;
	}
}

BOOST_AUTO_TEST_CASE(Test_ConvertStatusToString_PerType)
{
	auto pump = MakeTypedDevice(AuxillaryTypes::Pump);
	pump->AuxillaryTraits.Set(PumpStatusTrait{}, PumpStatuses::Running);
	BOOST_CHECK(HasStatus(pump));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(pump)), std::string("Running"));

	auto heater = MakeTypedDevice(AuxillaryTypes::Heater);
	heater->AuxillaryTraits.Set(HeaterStatusTrait{}, HeaterStatuses::Heating);
	BOOST_CHECK(HasStatus(heater));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(heater)), std::string("Heating"));

	auto chlorinator = MakeTypedDevice(AuxillaryTypes::Chlorinator);
	chlorinator->AuxillaryTraits.Set(ChlorinatorStatusTrait{}, ChlorinatorStatuses::On);
	BOOST_CHECK(HasStatus(chlorinator));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(chlorinator)), std::string("On"));

	auto aux = MakeTypedDevice(AuxillaryTypes::Auxillary);
	aux->AuxillaryTraits.Set(AuxillaryStatusTrait{}, AuxillaryStatuses::On);
	BOOST_CHECK(HasStatus(aux));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(aux)), std::string("On"));
}

//-----------------------------------------------------------------------------
// Regression: a Light was previously lumped in with Unknown/default, so its
// AuxillaryStatusTrait -- written by LightsDevice at construction, on every wire
// status message, and again on a watchdog timeout -- never resolved. HasStatus()
// returned false and the equipment-button routes silently dropped the "status"
// member for every light. Lights share the aux family's status vocabulary and
// must resolve exactly like the rest of it.
//-----------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Test_ConvertStatusToString_LightResolvesAuxillaryStatus)
{
	auto light = MakeTypedDevice(AuxillaryTypes::Light);

	light->AuxillaryTraits.Set(AuxillaryStatusTrait{}, AuxillaryStatuses::On);
	BOOST_CHECK(HasStatus(light));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(light)), std::string("On"));

	light->AuxillaryTraits.Set(AuxillaryStatusTrait{}, AuxillaryStatuses::Off);
	BOOST_CHECK(HasStatus(light));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(light)), std::string("Off"));

	// The watchdog-timeout write: still a real, reportable status ("Unknown" the
	// value), not an absent one.
	light->AuxillaryTraits.Set(AuxillaryStatusTrait{}, AuxillaryStatuses::Unknown);
	BOOST_CHECK(HasStatus(light));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(light)), std::string("Unknown"));
}

BOOST_AUTO_TEST_CASE(Test_HasStatus_LightWithoutStatusTraitHasNoStatus)
{
	// A Light that has never been given the trait still reports no status, so the
	// fix widens the type arm without inventing a value out of nothing.
	auto light = MakeTypedDevice(AuxillaryTypes::Light);
	BOOST_CHECK(!HasStatus(light));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(light)), std::string("Unknown"));
}

BOOST_AUTO_TEST_CASE(Test_HasStatus_FalseWhenNoCategoryStatus)
{
	// Device with a known type but no status trait set -> no displayable status.
	auto pump = MakeTypedDevice(AuxillaryTypes::Pump);
	BOOST_CHECK(!HasStatus(pump));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(pump)), std::string("Unknown"));

	// Device with no type at all.
	auto bare = std::make_shared<AuxillaryDevice>("Bare");
	BOOST_CHECK(!HasStatus(bare));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(bare)), std::string("Unknown"));

	// Null device must not be probed.
	std::shared_ptr<AuxillaryDevice> null_device;
	BOOST_CHECK(!HasStatus(null_device));
	BOOST_CHECK_EQUAL(std::string(ConvertStatusToString(null_device)), std::string("Unknown"));
}

BOOST_AUTO_TEST_SUITE_END()
