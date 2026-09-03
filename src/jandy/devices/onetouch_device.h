#pragma once

#include <array>
#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "devices/jandy_controller.h"
#include "devices/jandy_device_types.h"
#include "devices/chlorinator_setpoint_refresh.h"
#include "devices/onetouch/onetouch_goals.h"
#include "devices/onetouch/onetouch_keypad.h"
#include "devices/onetouch/onetouch_message_router.h"
#include "devices/onetouch/onetouch_screen_reader.h"
#include "devices/onetouch/onetouch_startup_survey.h"
#include "devices/capabilities/chlorinator_controller.h"
#include "devices/capabilities/command_history.h"
#include "devices/capabilities/controller_schedule_writer.h"
#include "devices/capabilities/describable.h"
#include "devices/capabilities/device_actuator.h"
#include "devices/capabilities/emulated.h"
#include "devices/capabilities/restartable.h"
#include "devices/capabilities/screen.h"
#include "devices/capabilities/setpoint_controller.h"
#include "devices/capabilities/spa_switch_configurator.h"
#include "interfaces/iequipmentdiscoverycontroller.h"
#include "messages/jandy_message_ack.h"
#include "messages/jandy_message_ids.h"
#include "messages/jandy_message_probe.h"
#include "messages/jandy_message_message_long.h"
#include "messages/jandy_message_status.h"
#include "messages/jandy_message_display_update.h"
#include "messages/jandy_message_unknown.h"
#include "messages/pda/pda_message_clear.h"
#include "messages/pda/pda_message_highlight.h"
#include "messages/pda/pda_message_highlight_chars.h"
#include "messages/pda/pda_message_shiftlines.h"
#include "navigation/menu_model.h"
#include "navigation/navigator.h"
#include "navigation/spider_engine.h"
#include "kernel/hub_locator.h"
#include "profiling/profiling.h"
#include "scheduling/controller_schedule.h"

namespace AqualinkAutomate::Devices
{

	class OneTouchScraper;  // devices/onetouch/onetouch_scraper.h — owns the read path (page/status processors)

	class OneTouchDevice : public JandyController, public Capabilities::Restartable, public Capabilities::Screen, public Capabilities::Emulated, public Capabilities::Describable, public Capabilities::DeviceActuator, public Capabilities::SetpointController, public Capabilities::ChlorinatorController, public Capabilities::SpaSwitchConfigurator, public Capabilities::CommandHistory, public Capabilities::ControllerScheduleWriter, public Interfaces::IEquipmentDiscoveryController
	{
		inline static constexpr uint8_t ONETOUCH_PAGE_LINES = 12;
		inline static constexpr std::chrono::seconds ONETOUCH_TIMEOUT_DURATION{ std::chrono::seconds(30) };
		inline static constexpr uint32_t ONETOUCH_SCRAPING_STALL_LIMIT{ 10 };
		inline static constexpr uint32_t ONETOUCH_SETPOINT_REFRESH_STEP_LIMIT{ 500 };  // frame backstop for a read-only setpoint re-scrape crawl
		inline static constexpr uint32_t ONETOUCH_FAULT_RECOVERY_STATUS_FRAMES{ 3 };   // consecutive recognised-page Status frames required to trust a faulted controller again before recovering to NormalOperation

	protected:
		// Protected (not private) so the test-only Test::SeamedOneTouchDevice can force/read the
		// operating state in unit tests without the production class carrying test-only methods.
		enum class OperatingStates
		{
			ColdStart,          // Waiting for initial splash screen from controller
			StartUp,            // Spider engine initialisation
			Scraping,           // Navigator and tasks are running
			NormalOperation,
			ScrapingFaulted,    // Scraping failed unrecoverably, device state unknown
			FaultHasOccurred
		};

	public:
		enum class KeyCommands : uint8_t
		{
			NoKeyCommand = 0x00,
			PageDown_Or_Select1 = 0x01,
			Back_Or_Select2 = 0x02,
			PageUp_Or_Select3 = 0x03,
			Select = 0x04,
			LineDown = 0x05,
			LineUp = 0x06,
			Unknown = 0xFF
		};

	public:
		OneTouchDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated);
		~OneTouchDevice() override;

		nlohmann::json DescribeDiagnostics() const override;

		// Configure proactive re-acquisition of the configured chlorinator setpoint by
		// periodically (+ on comms-recovery) scraping the Set AquaPure menu (0 = disabled).
		// Applied post-construction (settings are not available to the constructor); only an
		// actively-emulating device ever navigates. See ChlorinatorSetpointRefresh.
		void EnableChlorinatorSetpointRefresh(std::chrono::seconds interval);

		// DeviceActuator: actuate (toggle/on/off) a pool device by driving the emulated
		// keypad to the Equipment ON/OFF page and Selecting the row whose label matches
		// the device. Ranks Low so a Serial Adapter (direct command) is preferred when
		// both controllers are present.
		Capabilities::ActuationResult ActuateDevice(const std::shared_ptr<Kernel::AuxillaryDevice>& device, Capabilities::ActuationAction action) override;
		Capabilities::ActuationPriority ControllerPriority() const override { return Capabilities::ActuationPriority::Low; }

		// SetpointController: change the pool/spa heater setpoint by driving the emulated
		// keypad to the Set Temperature page, Select-ing the Pool Heat / Spa Heat row to
		// enter the in-place editor, arrow-stepping the value, then Select-ing again to
		// commit. The value arrives already in the system's configured units (the setpoints
		// route converts before dispatch; the dispatcher validates the range), so it matches
		// the on-screen value 1:1. (ControllerPriority() above satisfies the DeviceActuator,
		// SetpointController AND ChlorinatorController mixins - identical signature.)
		Capabilities::ActuationResult SetPoolSetpoint(uint8_t temperature) override;
		Capabilities::ActuationResult SetSpaSetpoint(uint8_t temperature) override;

		// ChlorinatorController: set the chlorinator output % via the Set AquaPure page (same
		// value-editor as the setpoint, but 5% steps) and start/stop the 100% boost via the
		// Boost Pool page. The % drives the POOL chlorination row to match the IAQ's single-%
		// behaviour; targets are rounded to a multiple of 5 (the OneTouch's step). Ranks Low,
		// so the IAQ's direct value-submit chlorinator (Medium) is preferred on a combined rig.
		Capabilities::ActuationResult SetChlorinatorPercentage(uint8_t percentage, Kernel::BodyOfWaterIds body) override;
		Capabilities::ActuationResult SetChlorinatorBoost(bool enable) override;

		// SpaSwitchConfigurator: program a spa-side switch button's function by driving the
		// emulated keypad through the Spa Switch config menu (System Setup -> Spa Switch -> the
		// number-of-switches page -> Button Setup list -> the "S:B" row -> the function picker),
		// then cycling the picker until it shows the target function and Select-ing to commit.
		// Screen-driven (not menu-model pathfinding) because the number-of-switches page must be
		// passed with a bare Select so the cursor never moves and the switch count is preserved.
		Capabilities::ActuationResult SetSpaSwitchAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function) override;

		// SpaSwitchConfigurator: the function set the OneTouch picker cycles (the canonical list).
		std::vector<std::string> AvailableFunctions() const override;

		// ControllerScheduleWriter: create / delete / edit one of the controller's own internal
		// Program timers by driving the emulated keypad through the Program menu (Menu/Help ->
		// Program -> the equipment list -> that equipment's Program detail page -> Add/Change/Delete).
		// The editor has no field cursor highlight, so the active field is tracked by counting Selects
		// since editor entry (0=ON-hour .. 4=days) and each field is stepped CLOSED-LOOP against the
		// echoed on-screen value. Screen-driven (not menu-model pathfinding) for the sub-pages, exactly
		// like the spa-switch writer. RE'd from captures/onetouch_program.cap; see
		// docs/onetouch_schedule_protocol.md (write path). Only an emulated panel transmits, so a
		// passive OneTouch reports NotSupported and the dispatcher falls back. Ranks Low so the IAQ's
		// direct value-submit writer (Medium) is preferred on a combined rig.
		Capabilities::ActuationResult CreateControllerProgram(const Scheduling::ControllerSchedule& program) override;
		Capabilities::ActuationResult DeleteControllerProgram(const Scheduling::ControllerSchedule& program) override;
		Capabilities::ActuationResult EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired) override;

		// IEquipmentDiscoveryController: the diagnostics "clear & rediscover" action. Clears every
		// auto-detected auxillary (sparing any forced Present by an operator override), resets the
		// decoded panel-model facts back to Unknown, and starts a fresh FullDiscoveryVisitPolicy
		// crawl -- the same one Scraping_ProcessStep_StartUp runs at boot. Refuses (returns false)
		// while a crawl is already in progress or the device is not actively emulating.
		bool RequestFullRediscovery() override;
		Interfaces::IEquipmentDiscoveryController::DiscoveryStatusSnapshot DiscoveryStatus() const override;

	protected:
		// The controller-update tick. ProcessControllerUpdates(bool) is protected (not private) so
		// the test-only Test::SeamedOneTouchDevice can drive one real update; the base declares
		// ProcessControllerUpdates() protected-virtual. The former "*ForTest" seams that used to sit
		// here now live on that subclass (which is why m_OpState / m_HighlightedLine / the
		// OperatingStates enum below are protected).
		void ProcessControllerUpdates() override;
		void ProcessControllerUpdates(bool is_status_message);

	private:
		void WatchdogTimeoutOccurred() override;

		// Recovery from the otherwise-dead-end fault states (ScrapingFaulted / FaultHasOccurred):
		// when a Status frame carrying a RECOGNISED page arrives for our device id while faulted,
		// the controller has resumed coherent comms. After ONETOUCH_FAULT_RECOVERY_STATUS_FRAMES
		// consecutive such frames (hysteresis, so a noisy/garbled bus never thrashes us out of the
		// fault), degrade straight to NormalOperation -- mirroring the watchdog Scraping->Normal
		// path -- so the device becomes actuatable again instead of staying stuck until a process
		// restart. Called only from the faulted switch arms in ProcessControllerUpdates(bool).
		void AttemptFaultRecovery(bool is_status_message);

	private:
		// The RS-485 message-ingest slot handlers live in the OneTouchMessageRouter collaborator
		// (devices/onetouch/onetouch_message_router.h), which drives this device's Screen capability,
		// controller tick and watchdog - hence the friendship below.
		friend class OneTouchMessageRouter;

		// The page processors (screen -> DataHub), the Equipment-Status line processors, the
		// panel-config decode and the controller-schedule accumulation have moved to the
		// OneTouchScraper collaborator (devices/onetouch/onetouch_scraper.h). This device owns one
		// scraper (m_Scraper) and registers its processors into the Screen capability.

	private:
		// Navigation-based scraping op-state drivers. The crawl analysis (survey / equipment
		// validation / seeded-label check) lives as free functions in
		// devices/onetouch/onetouch_startup_survey.h.
		void Scraping_ProcessStep_StartUp();
		void Scraping_ProcessStep();

		// Service the in-flight on-demand keypad goal (toggle / value-edit / ... - whatever the
		// GoalRunner holds) one Status cycle: build the KeypadContext view of the shared keypad,
		// drive the active goal, and translate the key it emits into m_KeyCommand_ToSend.
		void ServiceActiveGoal();

		// Read a device's current on/off state so an explicit On/Off only acts when the state
		// actually differs (DeviceActuator).
		std::optional<bool> CurrentOnState(const std::shared_ptr<Kernel::AuxillaryDevice>& device) const;

		// Proactive chlorinator-setpoint re-acquisition (the GET): periodically (and on a
		// chlorinator offline->online edge) drive a READ-ONLY menu visit to the Set AquaPure
		// page so PageProcessor_SetAquapure re-scrapes the configured Pool/Spa %. Runs 5th in
		// NormalOperation; deferred while any user/SET goal is in flight; gated on active
		// emulation. The visit reuses the SpiderEngine with a single-page TargetedVisitPolicy
		// and submits NO value (read-only). m_RefreshInProgress makes it count as a goal so a
		// user command cannot interleave on the single shared Navigator.
		void SetpointRefresh_ProcessStep();

		// Row-scraping primitives (displayed value, function-on-row, find-line-by-prefix) now
		// live as pure functions in devices/onetouch/onetouch_screen_reader.h and are called
		// directly with DisplayedPage(); they are no longer device methods.

		// True when any on-demand goal (toggle / value-edit / boost / spa-switch / setpoint
		// refresh) is mid-flight; the single shared Navigator/keypad means goals must never
		// interleave. Including the read-only setpoint refresh here makes a user command arriving
		// mid-refresh be rejected rather than corrupt the in-flight navigation.
		bool GoalInProgress() const;

		// Convert Navigator key command to device KeyCommand
		static KeyCommands ConvertNavKeyCommand(Navigation::NavKeyCommand nav_cmd);

		// Shared precondition guard for accepting a keypad actuation goal: the device must be
		// actively emulating and NOT in a dead-end fault state (the per-frame service steps run
		// only in NormalOperation, so a goal queued while faulted would be stranded). Returns the
		// ActuationResult to hand back to the caller when the device cannot act, or nullopt when it
		// is clear to queue. 'what' is a short description used only for the log line. (The
		// one-at-a-time GoalInProgress() gate is applied separately by each caller.)
		std::optional<Capabilities::ActuationResult> ReasonCannotActuate(std::string_view what) const;

	private:
		// Navigation system
		Navigation::MenuModel m_MenuModel;
		std::unique_ptr<Navigation::Navigator> m_Navigator;
		std::unique_ptr<Navigation::SpiderEngine> m_SpiderEngine;

	protected:
		// m_OpState and m_HighlightedLine are protected so the test-only Test::SeamedOneTouchDevice
		// can force the fault states / position the cursor in unit tests (see onetouch_test_device.h).
		OperatingStates m_OpState{ OperatingStates::ColdStart };
		uint32_t m_ScrapingStallCounter{ 0 };
		uint32_t m_FaultRecoveryStatusCount{ 0 };  // consecutive recognised-page Status frames seen during the current faulted dwell (drives AttemptFaultRecovery's hysteresis)
		uint8_t m_HighlightedLine{ 0 };
		std::optional<OneTouch::MenuSurveyResult> m_MenuSurveyResult;  // populated when the startup crawl completes

	private:
		// The on-demand keypad goals (toggle / value-edit / boost / spa-switch / schedule-write)
		// serialise through one GoalRunner: at most one is in flight, serviced each Status cycle by
		// ServiceActiveGoal. Replaces the per-goal m_PendingX / m_XInProgress / m_XStepCount fields.
		OneTouch::OneTouchGoalRunner m_Runner;

		// Cleared-device count from the most recent RequestFullRediscovery(), reported by
		// DiscoveryStatus() for the diagnostics UI.
		std::size_t m_LastRediscoveryClearedCount{ 0 };

		// Value rows (see PageProcessor_SetTemperature / the Set AquaPure page): on Set
		// Temperature, Pool Heat is line 2 / Spa Heat line 3; on Set AquaPure, "Set Pool to:"
		// is line 3 (verified vs onetouch_chlorinator.cap).
		inline static constexpr uint8_t SETTEMP_POOL_HEAT_LINE{ 2 };
		inline static constexpr uint8_t SETTEMP_SPA_HEAT_LINE{ 3 };
		inline static constexpr uint8_t ONETOUCH_CHLORINATOR_STEP{ 5 };  // the OneTouch edits AquaPure % in 5% increments
		// The Set AquaPure Pool-% row the value editor targets is OneTouchScraper::SETAQUAPURE_POOL_LINE.

		// Shared body of the value-edit capability methods (SetPoolSetpoint / SetSpaSetpoint /
		// SetChlorinatorPercentage): validate the device can actuate, reject if another goal is
		// mid-flight, then start a ValueEditGoal on the runner.
		Capabilities::ActuationResult QueueValueEdit(Navigation::PageId page, uint8_t line, std::string label, int target, std::string desc);

		// Shared body of the three ControllerScheduleWriter capability methods (validate the device
		// can actuate, reject if another goal is mid-flight, then start a ScheduleWriteGoal on the
		// runner). The feasibility / emulation checks that precede it live in the capability methods.
		Capabilities::ActuationResult QueueScheduleWrite(OneTouch::ScheduleWriteOp op, const Scheduling::ControllerSchedule& program, std::string desc);

		// Proactive chlorinator-setpoint refresh (read-only Set AquaPure re-scrape). m_RefreshState
		// holds the "when to scrape" policy; m_RefreshInProgress tracks an in-flight visit (counts
		// as a goal, so it blocks user commands on the shared Navigator) and is driven step-by-step
		// with m_RefreshStepCount as a frame backstop.
		ChlorinatorSetpointRefresh m_RefreshState;
		bool m_RefreshInProgress{ false };
		uint32_t m_RefreshStepCount{ 0 };

		// AqualinkD always uses 0x80 (V2_Normal) for ACKs. The controller may ignore
	// 0x00 responses and fail to register the device, so start with V2_Normal.
	Messages::AckTypes m_AckType_ToSend{ Messages::AckTypes::V2_Normal };
		KeyCommands m_KeyCommand_ToSend{ KeyCommands::NoKeyCommand };

		// The read path: page + status processors, panel-config decode and controller-schedule
		// accumulation. Created in the constructor with the shared DataHub + schedule store; its
		// processors are registered into the Screen capability. Destroyed after the Screen base's
		// processor closures are torn down (bases destruct last), which never invoke them.
		std::unique_ptr<OneTouchScraper> m_Scraper;

		// Wire-message ingest: the boost::signals2 slot handlers, registered (bound to this router)
		// in the constructor. Holds a reference back to this device (see friend declaration above).
		OneTouchMessageRouter m_Router{ *this };

		Types::ProfilingUnitTypePtr m_ProfilingDomain;
	};

}
// namespace AqualinkAutomate::Devices
