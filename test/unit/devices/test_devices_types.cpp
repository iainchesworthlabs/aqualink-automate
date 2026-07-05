#include <cstdint>
#include <vector>

#include <boost/test/unit_test.hpp>

#include "jandy/devices/jandy_device_types.h"
#include "jandy/formatters/jandy_device_formatters.h"

#include "support/unit_test_ostream_support.h"

BOOST_AUTO_TEST_SUITE(JandyDeviceTypeTestSuite)

BOOST_AUTO_TEST_CASE(ConstructorTest)
{
    // Test the default constructor
    AqualinkAutomate::Devices::JandyDeviceType device1;
    BOOST_CHECK_EQUAL(device1.Class(), AqualinkAutomate::Devices::DeviceClasses::Unknown);
    BOOST_CHECK_EQUAL(device1.Id(), 0xFF);

    // Test the constructor that takes a DeviceId
    AqualinkAutomate::Devices::JandyDeviceType device2(0x09);
    BOOST_CHECK_EQUAL(device2.Class(), AqualinkAutomate::Devices::DeviceClasses::RS_Keypad);
    BOOST_CHECK_EQUAL(device2.Id(), 0x09);
}

BOOST_AUTO_TEST_CASE(InstanceAddressesForClass_ReturnsEveryInstanceOfTheClass)
{
    using namespace AqualinkAutomate::Devices;

    // The instance set the master probes for a class -- used to relocate an emulated device to a
    // free instance on a bus collision.
    BOOST_CHECK((JandyDeviceType::InstanceAddressesForClass(DeviceClasses::OneTouch)
        == std::vector<std::uint8_t>{ 0x40, 0x41, 0x42, 0x43 }));
    BOOST_CHECK((JandyDeviceType::InstanceAddressesForClass(DeviceClasses::AqualinkTouch)
        == std::vector<std::uint8_t>{ 0x30, 0x31, 0x32, 0x33 }));
    BOOST_CHECK((JandyDeviceType::InstanceAddressesForClass(DeviceClasses::SerialAdapter)
        == std::vector<std::uint8_t>{ 0x48, 0x49 }));

    // The two spaside remotes are distinct device families and must not be conflated (see
    // docs/alwin32/spaside-remotes.md): the "Dual Spa Switch" (2x4rem) is class 0x02 at 0x10-0x13,
    // whereas the "Spa Link" (8button) is class 0x04 at 0x20-0x23.
    BOOST_CHECK((JandyDeviceType::InstanceAddressesForClass(DeviceClasses::DualSpaSwitch)
        == std::vector<std::uint8_t>{ 0x10, 0x11, 0x12, 0x13 }));
    BOOST_CHECK((JandyDeviceType::InstanceAddressesForClass(DeviceClasses::SpaRemote)
        == std::vector<std::uint8_t>{ 0x20, 0x21, 0x22, 0x23 }));
}

BOOST_AUTO_TEST_CASE(InstanceAddressesForClass_UnknownClass_ReturnsEmpty)
{
    using namespace AqualinkAutomate::Devices;

    // A device class with no entry in the known-ids table (Pentair_Pump is a DeviceClasses
    // value but is not present in m_KnownDeviceIdsList) drives the no-match fall-through and
    // must return an empty instance set rather than a partial or garbage list.
    BOOST_CHECK(JandyDeviceType::InstanceAddressesForClass(DeviceClasses::Pentair_Pump).empty());
}

BOOST_AUTO_TEST_CASE(SpaRemoteClasses_AreDistinctAndCorrectlyAddressed)
{
    using namespace AqualinkAutomate::Devices;

    // Regression: the address map previously had ONLY a single SpaRemote entry at 0x20-0x23 and
    // no class for 0x10-0x13, conflating the two physical spaside remotes recovered from the
    // official Jandy Alwin32 simulator suite (docs/alwin32/spaside-remotes.md, RVA-cited):
    //   * 2x4rem.exe  -> class 0x02, base 0x10  ("Dual Spa Switch")
    //   * 8button.exe -> class 0x04, base 0x20  ("Spa Link" 8-button remote)

    // Every 0x10-0x13 instance resolves to the Dual Spa Switch class...
    for (int id : { 0x10, 0x11, 0x12, 0x13 })
    {
        BOOST_CHECK_EQUAL(JandyDeviceType(id).Class(), DeviceClasses::DualSpaSwitch);
    }

    // ...and every 0x20-0x23 instance resolves to the Spa Link (SpaRemote) class.
    for (int id : { 0x20, 0x21, 0x22, 0x23 })
    {
        BOOST_CHECK_EQUAL(JandyDeviceType(id).Class(), DeviceClasses::SpaRemote);
    }

    // The two classes are genuinely different (the bug was them being one and the same).
    BOOST_CHECK(JandyDeviceType(0x10).Class() != JandyDeviceType(0x20).Class());
}

BOOST_AUTO_TEST_CASE(CopyConstructorTest)
{
    // Initialize a device
    AqualinkAutomate::Devices::JandyDeviceType device1(0x03);

    // Copy construct a device
    AqualinkAutomate::Devices::JandyDeviceType device2(device1);

    BOOST_CHECK_EQUAL(device2.Class(), AqualinkAutomate::Devices::DeviceClasses::AqualinkMaster);
    BOOST_CHECK_EQUAL(device2.Id(), 0x03);
}

BOOST_AUTO_TEST_CASE(MoveConstructorTest)
{
    // Initialize a device
    AqualinkAutomate::Devices::JandyDeviceType device1(0x09);

    // Move construct a device
    AqualinkAutomate::Devices::JandyDeviceType device2(std::move(device1));

    BOOST_CHECK_EQUAL(device2.Class(), AqualinkAutomate::Devices::DeviceClasses::RS_Keypad);
    BOOST_CHECK_EQUAL(device2.Id(), 0x09);
}

BOOST_AUTO_TEST_CASE(AssignmentOperatorTest)
{
    // Initialize two devices
    AqualinkAutomate::Devices::JandyDeviceType device1(0x03);
    AqualinkAutomate::Devices::JandyDeviceType device2(0x09);

    // Test the assignment operator
    device2 = device1;

    BOOST_CHECK_EQUAL(device2.Class(), AqualinkAutomate::Devices::DeviceClasses::AqualinkMaster);
    BOOST_CHECK_EQUAL(device2.Id(), 0x03);
}

BOOST_AUTO_TEST_CASE(MoveAssignmentOperatorTest)
{
    // Initialize two devices
    AqualinkAutomate::Devices::JandyDeviceType device1(0x03);
    AqualinkAutomate::Devices::JandyDeviceType device2(0x09);

    // Test the move assignment operator
    device2 = std::move(device1);

    BOOST_CHECK_EQUAL(device2.Class(), AqualinkAutomate::Devices::DeviceClasses::AqualinkMaster);
    BOOST_CHECK_EQUAL(device2.Id(), 0x03);
}

BOOST_AUTO_TEST_CASE(EqualityOperatorTest)
{
    // Initialize two identical devices
    AqualinkAutomate::Devices::JandyDeviceType device1(0x09);
    AqualinkAutomate::Devices::JandyDeviceType device2(0x09);

    // Test the equality operator
    BOOST_CHECK(device1 == device2);
}

BOOST_AUTO_TEST_CASE(InequalityOperatorTest)
{
    // Initialize two different devices
    AqualinkAutomate::Devices::JandyDeviceType device1(0x03);
    AqualinkAutomate::Devices::JandyDeviceType device2(0x09);

    // Test the inequality operator
    BOOST_CHECK(device1 != device2);
}

BOOST_AUTO_TEST_SUITE_END()
