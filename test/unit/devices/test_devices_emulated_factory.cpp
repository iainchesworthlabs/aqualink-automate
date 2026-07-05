#include <cstdint>
#include <memory>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/jandy_emulated_device_factory.h"
#include "jandy/devices/jandy_emulated_device_types.h"
#include "jandy/devices/jandy_device_id.h"

#include "support/unit_test_hublocatorinjector.h"

using namespace AqualinkAutomate;
using namespace AqualinkAutomate::Devices;

//=============================================================================
// MakeEmulatedDevice factory tests.
//
// MakeEmulatedDevice() is the single mapping from a JandyEmulatedDeviceTypes tag
// + bus id to a concrete emulated device.  These tests drive every switch arm:
// each supported type builds a non-null IDevice, and the Unknown type takes the
// default arm and returns nullptr.  A HubLocatorInjector supplies the hubs the
// concrete device constructors resolve.
//=============================================================================

namespace
{
	struct EmulatedFactoryFixture : public AqualinkAutomate::Test::HubLocatorInjector
	{
	};
}

BOOST_FIXTURE_TEST_SUITE(EmulatedDeviceFactory_TestSuite, EmulatedFactoryFixture)

BOOST_AUTO_TEST_CASE(Make_OneTouch_ReturnsDevice)
{
	auto device = MakeEmulatedDevice(JandyEmulatedDeviceTypes::OneTouch, JandyDeviceId(0x40), *this);
	BOOST_CHECK(nullptr != device);
}

BOOST_AUTO_TEST_CASE(Make_RsKeypad_ReturnsDevice)
{
	auto device = MakeEmulatedDevice(JandyEmulatedDeviceTypes::RS_Keypad, JandyDeviceId(0x08), *this);
	BOOST_CHECK(nullptr != device);
}

BOOST_AUTO_TEST_CASE(Make_Iaq_ReturnsDevice)
{
	auto device = MakeEmulatedDevice(JandyEmulatedDeviceTypes::IAQ, JandyDeviceId(0xA0), *this);
	BOOST_CHECK(nullptr != device);
}

BOOST_AUTO_TEST_CASE(Make_Pda_ReturnsDevice)
{
	auto device = MakeEmulatedDevice(JandyEmulatedDeviceTypes::PDA, JandyDeviceId(0x60), *this);
	BOOST_CHECK(nullptr != device);
}

BOOST_AUTO_TEST_CASE(Make_SerialAdapter_ReturnsDevice)
{
	auto device = MakeEmulatedDevice(JandyEmulatedDeviceTypes::SerialAdapter, JandyDeviceId(0x48), *this);
	BOOST_CHECK(nullptr != device);
}

BOOST_AUTO_TEST_CASE(Make_SpasideRemote_ReturnsDevice)
{
	auto device = MakeEmulatedDevice(JandyEmulatedDeviceTypes::SpasideRemote, JandyDeviceId(0x10), *this);
	BOOST_CHECK(nullptr != device);
}

BOOST_AUTO_TEST_CASE(Make_Unknown_ReturnsNullptr)
{
	auto device = MakeEmulatedDevice(JandyEmulatedDeviceTypes::Unknown, JandyDeviceId(0xFF), *this);
	BOOST_CHECK(nullptr == device);
}

BOOST_AUTO_TEST_SUITE_END()
