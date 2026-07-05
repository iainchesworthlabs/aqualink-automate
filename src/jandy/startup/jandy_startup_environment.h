#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include <boost/signals2/connection.hpp>

#include "devices/jandy_emulated_device_types.h"
#include "startup/jandy_startup_coordinator.h"

namespace AqualinkAutomate::Kernel { class HubLocator; class EquipmentHub; class DataHub; }

namespace AqualinkAutomate::Jandy::Startup
{

	// Production IStartupEnvironment backed by the live hubs + passive bus observation.
	//
	//  - EmulateDevice  stands the device up in the EquipmentHub (shared factory).
	//  - ObservedProbes records the addresses the master addresses AS A CONTROLLER: the cold-boot
	//    discovery probe (cmd 0x00), and the active controller-protocol polls a configured panel
	//    sends to an already-discovered controller (IAQ_Poll 0x30 -> AqualinkTouch; Status 0x02 ->
	//    OneTouch). The latter makes classification robust on a capture taken AFTER discovery,
	//    where the lone cold-boot probe may not be present.
	//  - OccupiedAddresses reports addresses where a REAL device answered, so the controller can
	//    be relocated off it. Presence is inferred from a device->master ACK that follows the
	//    master's probe of that address (excluding our own emulations). On a point-to-point link
	//    with no real devices this never fires, so OccupiedAddresses is empty -- exactly right,
	//    nothing to relocate around. (The per-device Emulated::SuppressEmulation remains the
	//    runtime collision backstop if two transmitters ever share an address.)
	//  - PanelModel / PanelRevision read DataHub::EquipmentVersions.
	class JandyStartupEnvironment : public IStartupEnvironment
	{
	public:
		explicit JandyStartupEnvironment(Kernel::HubLocator& hub_locator, std::chrono::seconds chlorinator_setpoint_refresh_interval = std::chrono::seconds{ 300 });

		void EmulateDevice(Devices::JandyEmulatedDeviceTypes type, std::uint8_t id, std::string_view role) override;
		std::set<std::uint8_t> ObservedProbes() const override;
		std::set<std::uint8_t> OccupiedAddresses() const override;
		std::string PanelModel() const override;
		std::string PanelRevision() const override;

		// Relocate an emulated device to a free instance of its class after a bus collision at
		// `colliding_id` (a real device answered there). Stands up a fresh emulation at the first
		// free instance -> true, or gives up -> false (the colliding instance stays passive). This
		// is the collision handler set on every emulated device, and is public for tests.
		bool RelocateEmulation(Devices::JandyEmulatedDeviceTypes type, std::uint8_t colliding_id);

	private:
		Kernel::HubLocator& m_HubLocator;
		std::shared_ptr<Kernel::EquipmentHub> m_EquipmentHub;
		std::shared_ptr<Kernel::DataHub> m_DataHub;
		std::chrono::seconds m_ChlorinatorSetpointRefreshInterval{ 300 };  // applied to an emulated OneTouch in EmulateDevice

		std::set<std::uint8_t> m_ProbedIds;     // addresses the master probed (cmd 0x00)
		std::set<std::uint8_t> m_RespondedIds;  // addresses a device answered from
		std::set<std::uint8_t> m_EmulatedIds;   // our own emulations (excluded from occupied)
		std::uint8_t m_LastProbedId{ 0x00 };

		boost::signals2::scoped_connection m_ProbeConnection;
		boost::signals2::scoped_connection m_IAQPollConnection;     // AqualinkTouch active poll (0x30)
		boost::signals2::scoped_connection m_StatusConnection;      // OneTouch active addressing (0x02)
		boost::signals2::scoped_connection m_AckConnection;
	};

}
// namespace AqualinkAutomate::Jandy::Startup
