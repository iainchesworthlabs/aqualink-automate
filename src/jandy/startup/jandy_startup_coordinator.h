#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include "devices/jandy_emulated_device_types.h"
#include "startup/jandy_startup_types.h"

namespace AqualinkAutomate::Jandy::Startup
{

	// The seam between the coordinator's decision logic and the live app. The production
	// implementation is backed by the EquipmentHub/DataHub + bus observation; unit tests
	// supply a mock so the whole start-up flow is exercised without hardware.
	class IStartupEnvironment
	{
	public:
		virtual ~IStartupEnvironment() = default;

		// Stand up an emulated device at a specific bus id.
		virtual void EmulateDevice(Devices::JandyEmulatedDeviceTypes type, std::uint8_t id, std::string_view role) = 0;

		// Discovery probes (master cmd 0x00 destinations) observed so far -- the capability
		// signal used to classify the controller type.
		virtual std::set<std::uint8_t> ObservedProbes() const = 0;

		// Bus addresses where a REAL (non-emulated) device is answering, so an emulated
		// controller can be relocated to a free slot instead of colliding.
		virtual std::set<std::uint8_t> OccupiedAddresses() const = 0;

		// Panel identity sourced so far (SerialAdapter model command / a scrape); "" if unknown.
		virtual std::string PanelModel() const = 0;
		virtual std::string PanelRevision() const = 0;
	};

	// Phased emulation start-up. Stand up a SerialAdapter (always safe, sources identity),
	// observe the master's probe set to learn the controller type, then choose a data-
	// gathering method + controller emulation and place it on a FREE bus address (relocating
	// off any real device). Drive it by calling Advance() periodically until Running.
	//
	//   Idle --Begin()--> Detecting --(controller classified | window elapsed)--> Engaging --> Running
	//
	class StartupCoordinator
	{
	public:
		enum class Phase
		{
			Idle,        // not yet begun
			Detecting,   // SerialAdapter up; observing probes / sourcing identity
			Engaging,    // plan computed + addresses resolved; standing up the controller
			Running,     // done
		};

	public:
		explicit StartupCoordinator(IStartupEnvironment& env);

		// Stand up the detector (SerialAdapter) and enter Detecting.
		void Begin();

		// Advance the state machine. `detection_window_elapsed` lets the driver end the
		// detection window (enough probes seen, or a timeout): when set, the coordinator
		// plans from whatever it has (possibly ObserveOnly). Returns the current phase.
		Phase Advance(bool detection_window_elapsed);

		Phase CurrentPhase() const { return m_Phase; }
		const StartupPlan& Plan() const { return m_Plan; }

	private:
		// Have we observed enough to classify the controller type?
		bool HaveControllerSignal() const;

	private:
		IStartupEnvironment& m_Env;
		Phase m_Phase{ Phase::Idle };
		StartupPlan m_Plan;
	};

}
// namespace AqualinkAutomate::Jandy::Startup
