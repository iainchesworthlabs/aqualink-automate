#pragma once

#include <cstdint>
#include <list>
#include <utility>
#include <vector>

#include "interfaces/ideviceidentifier.h"
#include "devices/jandy_device_id.h"

namespace AqualinkAutomate::Devices
{

	enum class DeviceClasses
	{
		AqualinkMaster,
		RS_Keypad,
		DualSpaSwitch,	// "2x4rem" Dual Spa Switch spa-side keypad (class 0x02, 0x10-0x13)
		SpaRemote,		// "8button" Spa Link spa-side remote      (class 0x04, 0x20-0x23)
		OneTouch,
		LX_Heater,
		AqualinkTouch,
		IAQ,
		SerialAdapter,
		JXi_Heater,
		HeatPump,
		SWG_Aquarite,
		PC_Dock,
		PDA,
		Pentair_Pump,
		Jandy_ePump,
		Chemlink,
		RemotePowerCenter,
		Chem_Analyzer,
		Jandy_ePump_Ext,
		Jandy_Light,
		Unknown
	};

	class JandyDeviceType : public Interfaces::IDeviceIdentifier
	{
		using DeviceId = JandyDeviceId;
		using DeviceClassAndIds = std::pair<DeviceClasses, std::vector<DeviceId>>;
		using DeviceClassAndIdsList = std::list<DeviceClassAndIds>;

		inline static const DeviceClasses UNKNOWN_DEVICE_CLASS{ DeviceClasses::Unknown };
		inline static const DeviceId UNKNOWN_DEVICE_RAWID{ 0xFF };

		inline static const DeviceClassAndIdsList m_KnownDeviceIdsList =
		{
			{DeviceClasses::AqualinkMaster, {0x00, 0x01, 0x02, 0x03}},
			{DeviceClasses::RS_Keypad,		{0x08, 0x09, 0x0A, 0x0B}},
			{DeviceClasses::DualSpaSwitch,	{0x10, 0x11, 0x12, 0x13}},
			{DeviceClasses::SpaRemote,		{0x20, 0x21, 0x22, 0x23}},
			{DeviceClasses::OneTouch,		{0x40, 0x41, 0x42, 0x43}},
			{DeviceClasses::LX_Heater,		{0x38, 0x39, 0x3A, 0x3B}},
			{DeviceClasses::AqualinkTouch,	{0x30, 0x31, 0x32, 0x33}},
			{DeviceClasses::IAQ,			{0xA0, 0xA1, 0xA2, 0xA3}},
			{DeviceClasses::SerialAdapter,	{0x48, 0x49}},
			{DeviceClasses::JXi_Heater,		{0x68, 0x69, 0x6A, 0x6B}},
			{DeviceClasses::HeatPump,		{0x70, 0x71, 0x72, 0x73}},
			{DeviceClasses::SWG_Aquarite,	{0x50, 0x51, 0x52, 0x53}},
			{DeviceClasses::PC_Dock,		{0x58, 0x59, 0x5A, 0x5B}},
			{DeviceClasses::PDA,			{0x60, 0x61, 0x62, 0x63}},
			{DeviceClasses::RemotePowerCenter, {0x28, 0x29, 0x2A, 0x2B}},
			{DeviceClasses::Jandy_ePump,	{0x78, 0x79, 0x7A, 0x7B}},
			{DeviceClasses::Chemlink,		{0x80, 0x81, 0x82, 0x83}},
			{DeviceClasses::Chem_Analyzer,	{0x84, 0x85, 0x86, 0x87}},
			{DeviceClasses::Jandy_ePump_Ext, {0xE0, 0xE1, 0xE2, 0xE3}},
			{DeviceClasses::Jandy_Light,	{0xF0, 0xF1, 0xF2, 0xF3, 0xF4}},
			{UNKNOWN_DEVICE_CLASS,			{UNKNOWN_DEVICE_RAWID}}
		};

	public:
		JandyDeviceType();
		JandyDeviceType(DeviceId device_id);

		JandyDeviceType(const JandyDeviceType& other) = default;
		JandyDeviceType& operator=(const JandyDeviceType& other) = default;
		JandyDeviceType(JandyDeviceType&& other) noexcept = default;
		JandyDeviceType& operator=(JandyDeviceType&& other) noexcept = default;

		bool operator==(const JandyDeviceType& other) const;
		bool operator!=(const JandyDeviceType& other) const;
		using Interfaces::IDeviceIdentifier::operator==;
		using Interfaces::IDeviceIdentifier::operator!=;

	protected:
		bool Equals(const Interfaces::IDeviceIdentifier& other) const override;

	public:
		DeviceId operator()() const;

		DeviceClasses Class() const;
		DeviceId Id() const;

		// The full set of bus addresses (instances) the master probes for a device class -- e.g.
		// OneTouch -> {0x40,0x41,0x42,0x43}, SerialAdapter -> {0x48,0x49}. Used to relocate an
		// emulated device to a FREE instance of its class on a bus collision, since multiple of a
		// class can co-exist at different instances. Empty for an unknown class.
		static std::vector<std::uint8_t> InstanceAddressesForClass(DeviceClasses device_class);

	private:
		DeviceClasses m_DeviceClass{ DeviceClasses::Unknown };
		DeviceId m_DeviceId;
	};

}
// namespace AqualinkAutomate::Devices
