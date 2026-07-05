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
#include "devices/capabilities/chlorinator_controller.h"
#include "devices/capabilities/command_history.h"
#include "devices/capabilities/describable.h"
#include "devices/capabilities/device_actuator.h"
#include "devices/capabilities/emulated.h"
#include "devices/capabilities/restartable.h"
#include "devices/capabilities/screen.h"
#include "devices/capabilities/setpoint_controller.h"
#include "devices/capabilities/spa_switch_configurator.h"
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

	class OneTouchDevice : public JandyController, public Capabilities::Restartable, public Capabilities::Screen, public Capabilities::Emulated, public Capabilities::Describable, public Capabilities::DeviceActuator, public Capabilities::SetpointController, public Capabilities::ChlorinatorController, public Capabilities::SpaSwitchConfigurator, public Capabilities::CommandHistory
	{
		inline static const uint8_t ONETOUCH_PAGE_LINES = 12;
		inline static const std::chrono::seconds ONETOUCH_TIMEOUT_DURATION{ std::chrono::seconds(30) };
		inline static const uint32_t ONETOUCH_SCRAPING_STALL_LIMIT{ 10 };
		inline static const uint32_t ONETOUCH_ACTUATION_STEP_LIMIT{ 500 };  // frame backstop so a toggle goal can never wedge NormalOperation (the Navigator's own timeouts normally end it first)
		inline static const uint32_t ONETOUCH_VALUEEDIT_STEP_LIMIT{ 500 };  // frame backstop for a value-edit goal (navigation + the value-step loop)
		inline static const uint32_t ONETOUCH_BOOST_STEP_LIMIT{ 500 };      // frame backstop for a chlorinator-boost goal
		inline static const uint32_t ONETOUCH_SETPOINT_REFRESH_STEP_LIMIT{ 500 };  // frame backstop for a read-only setpoint re-scrape crawl
		inline static const uint32_t ONETOUCH_FAULT_RECOVERY_STATUS_FRAMES{ 3 };   // consecutive recognised-page Status frames required to trust a faulted controller again before recovering to NormalOperation

		enum class OperatingStates
		{
			ColdStart,          // Waiting for initial splash screen from controller
			StartUp,            // Spider engine initialisation
			Scraping,           // Navigator and tasks are running
			NormalOperation,
			ScrapingFaulted,    // Scraping failed unrecoverably, device state unknown
			FaultHasOccurred
		};

		// Health summary of the startup menu crawl: which pages were reached, and which the
		// crawl could not reach -- split into capability-gated pages whose absence is EXPECTED
		// on this model (e.g. the iAqualink or chlorinator pages) versus NOTABLE failures that
		// every panel should have. Surfaced via DescribeDiagnostics.
		struct MenuSurveyResult
		{
			uint32_t PagesReached{ 0 };
			std::vector<std::string> ExpectedAbsent;    // failed but capability-gated (benign)
			std::vector<std::string> NotableFailures;   // failed and expected on every model
			bool EquipmentPageReached{ false };          // the critical Equipment ON/OFF page
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

	public:
		nlohmann::json DescribeDiagnostics() const override;

		// True when the DataHub already carries one or more aux devices with a
		// non-empty label (e.g. seeded passively by a real iAqualink2 on the bus).
		// When so, the emulated OneTouch skips the slow "Label Aux" menu crawl at
		// startup. Exposed for diagnostics and for validating the skip decision.
		bool DataHubHasSeededAuxLabels() const;

		// Configure proactive re-acquisition of the configured chlorinator setpoint by
		// periodically (+ on comms-recovery) scraping the Set AquaPure menu (0 = disabled).
		// Applied post-construction (settings are not available to the constructor); only an
		// actively-emulating device ever navigates. See ChlorinatorSetpointRefresh.
		void EnableChlorinatorSetpointRefresh(std::chrono::seconds interval);

	public:
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
		Capabilities::ActuationResult SetChlorinatorPercentage(uint8_t percentage) override;
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

		// Sanitise a screen row's text for function/label comparison: trim surrounding whitespace
		// and non-printable bytes, yielding the clean displayed text (the controller's inverse-video
		// highlight is a separate Highlight message, never appended to the row Text). Public+static
		// for direct unit testing of the picker compare.
		static std::string SanitiseFunctionText(const std::string& raw);

	protected:
		// Test seam: force the ScrapingFaulted operating state so a test can verify that actuation
		// is refused (NotSupported) rather than falsely accepted while the controller is in a fault,
		// and that the device subsequently recovers when comms resume. Not used in production code.
		void ForceScrapingFaultedForTest() { m_OpState = OperatingStates::ScrapingFaulted; }

		// Test seam: force the FaultHasOccurred operating state (the watchdog-during-startup fault)
		// so the comms-resumed recovery path can be exercised from it too. Not used in production.
		void ForceFaultHasOccurredForTest() { m_OpState = OperatingStates::FaultHasOccurred; }

		// Test seam: true once the device has recovered to NormalOperation (the OperatingStates
		// enum is private, so a test cannot read m_OpState directly). Not used in production.
		bool IsInNormalOperationForTest() const { return m_OpState == OperatingStates::NormalOperation; }

		// Test seam: render text onto a screen line exactly as an incoming MessageLong would, so a
		// test can present a recognised page before driving a Status frame. Not used in production.
		void RenderScreenLineForTest(uint8_t line_id, const std::string& text)
		{
			ScreenMode(Capabilities::ScreenModes::Updating);
			ProcessScreenEvent(Utility::ScreenDataPageUpdaterImpl::evUpdate(line_id, text));
			ProcessScreenUpdates();
		}

		// Test seam: drive one Status-message controller update as if a Status frame arrived for our
		// device id (render the page with RenderScreenLineForTest first). Exercises the real
		// fault-recovery decision in ProcessControllerUpdates without a wire frame. Not used in production.
		void DeliverStatusFrameForTest() { ProcessControllerUpdates(true); }

	private:
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
		void Slot_OneTouch_Ack(const Messages::JandyMessage_Ack& msg);
		void Slot_OneTouch_MessageLong(const Messages::JandyMessage_MessageLong& msg);
		void Slot_OneTouch_Probe(const Messages::JandyMessage_Probe& msg);
		void Slot_OneTouch_Status(const Messages::JandyMessage_Status& msg);
		void Slot_OneTouch_Clear(const Messages::PDAMessage_Clear& msg);
		void Slot_OneTouch_Highlight(const Messages::PDAMessage_Highlight& msg);
		void Slot_OneTouch_HighlightChars(const Messages::PDAMessage_HighlightChars& msg);
		void Slot_OneTouch_ShiftLines(const Messages::PDAMessage_ShiftLines& msg);
		void Slot_OneTouch_DisplayUpdate(const Messages::JandyMessage_DisplayUpdate& msg);
		void Slot_OneTouch_Unknown(const Messages::JandyMessage_Unknown& msg);

	private:
		void PageProcessor_System(const Utility::ScreenDataPage& page);
		void PageProcessor_Service(const Utility::ScreenDataPage& page);
		void PageProcessor_TimeOut(const Utility::ScreenDataPage& page);
		void PageProcessor_OneTouch(const Utility::ScreenDataPage& page);
		void PageProcessor_EquipmentOnOff(const Utility::ScreenDataPage& page);
		void PageProcessor_EquipmentStatus(const Utility::ScreenDataPage& page);
		void PageProcessor_SelectSpeed(const Utility::ScreenDataPage& page);
		void PageProcessor_MenuHelp(const Utility::ScreenDataPage& page);
		void PageProcessor_HelpSubmenu(const Utility::ScreenDataPage& page);
		void PageProcessor_SetTemperature(const Utility::ScreenDataPage& page);
		void PageProcessor_SetTime(const Utility::ScreenDataPage& page);
		void PageProcessor_SystemSetup(const Utility::ScreenDataPage& page);
		void PageProcessor_FreezeProtect(const Utility::ScreenDataPage& page);
		void PageProcessor_Boost(const Utility::ScreenDataPage& page);
		void PageProcessor_SetAquapure(const Utility::ScreenDataPage& page);
		void PageProcessor_Version(const Utility::ScreenDataPage& page);
		void PageProcessor_DiagnosticsSensors(const Utility::ScreenDataPage& page);
		void PageProcessor_DiagnosticsRemotes(const Utility::ScreenDataPage& page);
		void PageProcessor_DiagnosticsErrors(const Utility::ScreenDataPage& page);
		void PageProcessor_LabelAuxList(const Utility::ScreenDataPage& page);
		void PageProcessor_LabelAux(const Utility::ScreenDataPage& page);
		void PageProcessor_MoreOneTouch(const Utility::ScreenDataPage& page);
		void PageProcessor_SetPoolHeat(const Utility::ScreenDataPage& page);
		void PageProcessor_SetSpaHeat(const Utility::ScreenDataPage& page);
		void PageProcessor_Program(const Utility::ScreenDataPage& page);
		void PageProcessor_DisplayLight(const Utility::ScreenDataPage& page);
		void PageProcessor_Lockouts(const Utility::ScreenDataPage& page);
		void PageProcessor_PasswordSettings(const Utility::ScreenDataPage& page);
		void PageProcessor_ProgramGroup(const Utility::ScreenDataPage& page);
		void PageProcessor_GeneralLabels(const Utility::ScreenDataPage& page);
		void PageProcessor_LightLabels(const Utility::ScreenDataPage& page);
		void PageProcessor_WaterfallLabels(const Utility::ScreenDataPage& page);
		void PageProcessor_CustomLabel(const Utility::ScreenDataPage& page);
		void PageProcessor_EnterPassword(const Utility::ScreenDataPage& page);
		void PageProcessor_HelpKeys(const Utility::ScreenDataPage& page);
		void PageProcessor_SpaSwitch(const Utility::ScreenDataPage& page);
		void PageProcessor_StartUp(const Utility::ScreenDataPage& page);

	private:
		static const uint32_t HINT_COUNT{ 2 };
		using HintArrayType = std::array<unsigned char, HINT_COUNT>;

		bool StatusProcessor_ShouldSkipLineProcessing(const HintArrayType& hint_array, const std::string_view line_to_process) const;
		void StatusProcessor_FilterPump(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_PoolHeat(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_SpaHeat(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_SolarHeat(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_HeatPump(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_Chiller(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_AquaPurePercentage(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_SaltLevelPPM(const Utility::ScreenDataPage& page, const uint8_t line_id);
		void StatusProcessor_CheckAquaPure(const Utility::ScreenDataPage& page, const uint8_t line_id);

	private:
		// Navigation-based scraping
		void Scraping_ProcessStep_StartUp();
		void Scraping_ProcessStep();

		// Cross-check the discovered equipment set against the model's expected aux/power-centre
		// layout once scraping ends; records the outcome on the DataHub and logs any anomalies.
		void ValidateDiscoveredEquipment();

		// Summarise the menu crawl once it completes: record reached/failed pages, classify
		// failures as expected-absent (capability-gated) vs notable, and warn if a core page
		// (Equipment ON/OFF) was missed. Result stored in m_MenuSurveyResult for diagnostics.
		void ReportMenuSurvey();

		// On-demand actuation (DeviceActuator): service a single pending toggle goal in
		// NormalOperation by driving the Navigator, and read a device's current on/off
		// state so an explicit On/Off only acts when the state actually differs.
		void Actuation_ProcessStep();
		std::optional<bool> CurrentOnState(const std::shared_ptr<Kernel::AuxillaryDevice>& device) const;

		// On-demand on-screen VALUE EDITOR (SetpointController + chlorinator %): service a
		// single pending value-edit goal in NormalOperation. The Navigator positions the
		// cursor on the goal's value row (no Select); then this device drives the value-step
		// loop directly - Select to begin editing, arrow keys to step the displayed value
		// toward the target, Select to commit. Used for heater setpoints (Set Temperature,
		// 1-degree steps) and chlorinator % (Set AquaPure, 5% steps) alike.
		void ValueEdit_ProcessStep();

		// On-demand SPA-SWITCH ASSIGNMENT edit (SpaSwitchConfigurator): service a single pending
		// assignment goal in NormalOperation. Screen-driven phase machine that walks the Spa
		// Switch config menu and cycles the per-button function picker to the target.
		void SpaSwitchEdit_ProcessStep();

		// On-demand chlorinator BOOST (ChlorinatorController): service a single pending
		// boost start/stop goal in NormalOperation via the Boost Pool page - Select on the
		// "Operate at 100%" page starts it; navigating to the "Stop" item and Select stops it.
		void Boost_ProcessStep();

		// Proactive chlorinator-setpoint re-acquisition (the GET): periodically (and on a
		// chlorinator offline->online edge) drive a READ-ONLY menu visit to the Set AquaPure
		// page so PageProcessor_SetAquapure re-scrapes the configured Pool/Spa %. Runs 5th in
		// NormalOperation; deferred while any user/SET goal is in flight; gated on active
		// emulation. The visit reuses the SpiderEngine with a single-page TargetedVisitPolicy
		// and submits NO value (read-only). m_RefreshInProgress makes it count as a goal so a
		// user command cannot interleave on the single shared Navigator.
		void SetpointRefresh_ProcessStep();

		// True when the DataHub chlorinator is reporting (ChlorinatorStatusTrait not Off/Unknown);
		// the offline->online edge of this drives a one-shot recovery re-scrape.
		bool DataHubChlorinatorOnline() const;

		// Read the currently displayed integer value from a row (e.g. "Pool Heat   90`F" -> 90,
		// "Set Pool to: 45%" -> 45). Returns nullopt when no value is parseable yet (page not
		// rendered, or value blanked mid-edit), so the step loop simply waits.
		std::optional<int> DisplayedValue(uint8_t line_id) const;

		// Read the function name shown on a row, sanitised for comparison (via SanitiseFunctionText):
		// the displayed text with surrounding whitespace/non-printable artifacts trimmed (e.g.
		// "Pool Light" from a Button-Setup row's "S:B  Pool Light"). Empty when the row is
		// blank/not rendered. Used for the picker compare and to read a row's current function.
		std::optional<std::string> DisplayedFunctionOnRow(uint8_t line_id) const;

		// Find the first screen line whose text (trimmed) STARTS WITH 'prefix' (case-insensitive);
		// used to locate the "Spa Switch" menu item and the "S:B" assignment row on screen.
		std::optional<uint8_t> FindLineStartingWith(const std::string& prefix) const;

		// True when any on-demand goal (toggle / value-edit / boost / spa-switch / setpoint
		// refresh) is mid-flight; the single shared Navigator/keypad means goals must never
		// interleave. Including the read-only setpoint refresh here makes a user command arriving
		// mid-refresh be rejected rather than corrupt the in-flight navigation.
		bool GoalInProgress() const;

		// Convert Navigator key command to device KeyCommand
		static KeyCommands ConvertNavKeyCommand(Navigation::NavKeyCommand nav_cmd);

	private:
		// Navigation system
		Navigation::MenuModel m_MenuModel;
		std::unique_ptr<Navigation::Navigator> m_Navigator;
		std::unique_ptr<Navigation::SpiderEngine> m_SpiderEngine;

	private:
		OperatingStates m_OpState{ OperatingStates::ColdStart };
		uint32_t m_ScrapingStallCounter{ 0 };
		uint32_t m_FaultRecoveryStatusCount{ 0 };  // consecutive recognised-page Status frames seen during the current faulted dwell (drives AttemptFaultRecovery's hysteresis)
		uint8_t m_HighlightedLine{ 0 };
		std::optional<MenuSurveyResult> m_MenuSurveyResult;  // populated when the startup crawl completes

	private:
		// On-demand equipment actuation goal (a single toggle at a time). Set by
		// ActuateDevice (DeviceActuator), serviced by Actuation_ProcessStep in
		// NormalOperation; the Navigator drives the keypad to the row and Selects it.
		std::optional<std::string> m_PendingActuationLabel;
		bool m_ActuationInProgress{ false };
		uint32_t m_ActuationStepCount{ 0 };

	private:
		// On-demand on-screen value-edit goal (a single edit at a time). Set by the
		// SetpointController / ChlorinatorController-% methods, serviced by
		// ValueEdit_ProcessStep in NormalOperation. The phase machine is identical for heater
		// setpoints and chlorinator % - only the page/row/target/units differ.
		enum class ValueEditPhase
		{
			Navigating,     // Navigator positioning the cursor on the value row
			BeginEdit,      // Select pressed to enter the in-place value editor
			Stepping,       // arrow keys stepping the displayed value toward the target
			Commit          // Select pressed again to commit the value and leave the editor
		};

		struct ValueEditGoal
		{
			Navigation::PageId page;   // page hosting the value row (Set Temperature / Set AquaPure)
			uint8_t line;              // row index of the value (cursor target + value read-back)
			std::string label;         // row label for content-based cursor positioning
			uint8_t target;            // target value in the on-screen units
			std::string desc;          // human description for logging
		};

		// Value rows (see PageProcessor_SetTemperature / the Set AquaPure page): on Set
		// Temperature, Pool Heat is line 2 / Spa Heat line 3; on Set AquaPure, "Set Pool to:"
		// is line 3 (verified vs onetouch_chlorinator.cap).
		inline static const uint8_t SETTEMP_POOL_HEAT_LINE{ 2 };
		inline static const uint8_t SETTEMP_SPA_HEAT_LINE{ 3 };
		inline static const uint8_t SETAQUAPURE_POOL_LINE{ 3 };
		inline static const uint8_t SETAQUAPURE_SPA_LINE{ 4 };       // Spa % row on Set AquaPure (verified vs onetouch_chlorinator.cap)
		inline static const uint8_t ONETOUCH_CHLORINATOR_STEP{ 5 };  // the OneTouch edits AquaPure % in 5% increments

		std::optional<ValueEditGoal> m_PendingValueEdit;
		ValueEditPhase m_ValueEditPhase{ ValueEditPhase::Navigating };
		bool m_ValueEditInProgress{ false };
		uint32_t m_ValueEditStepCount{ 0 };

		// Shared body of the value-edit capability methods: validate the device can actuate,
		// reject if another goal is mid-flight, then enqueue the goal.
		Capabilities::ActuationResult QueueValueEdit(ValueEditGoal goal);

	private:
		// On-demand chlorinator-boost goal (start/stop, one at a time). Set by
		// SetChlorinatorBoost, serviced by Boost_ProcessStep in NormalOperation.
		enum class BoostPhase
		{
			Navigating,     // Navigator driving to the Boost Pool page
			Acting,         // pressing Select (start) or selecting the Stop item (stop)
			Settle          // waiting for the action to take effect / complete
		};
		std::optional<bool> m_PendingBoost;   // true = start, false = stop
		BoostPhase m_BoostPhase{ BoostPhase::Navigating };
		bool m_BoostInProgress{ false };
		uint32_t m_BoostStepCount{ 0 };

	private:
		// On-demand SPA-SWITCH assignment edit goal (one at a time). Set by SetSpaSwitchAssignment,
		// serviced by SpaSwitchEdit_ProcessStep. Screen-driven: each phase reads the current page and
		// emits one key. Distinct from the value-edit because it crosses several pages (System Setup
		// -> Spa Switch -> number page -> Button Setup -> picker) and must NOT move the cursor on the
		// number-of-switches page (that would change the switch count).
		enum class SpaSwitchEditPhase
		{
			ToSystemSetup,    // Navigator drives to the System Setup menu
			SelectSpaSwitch,  // move cursor to the "Spa Switch" item, then Select -> number page
			PassNumberPage,   // bare Select on the number-of-switches page -> Button Setup list
			ToRow,            // move cursor to the "S:B" row, then Select -> function picker
			CyclePicker,      // LineUp-cycle the picker until it shows the target function
			Commit            // Select to write the function and leave the picker
		};
		struct SpaSwitchEditGoal
		{
			uint8_t switch_number{ 0 };
			uint8_t button_number{ 0 };
			std::string function;   // target function name (as the controller's picker lists it)
			std::string row_tag;    // "<switch>:<button>" e.g. "1:2" -- the Button Setup row label
			std::string desc;
		};
		inline static const uint32_t ONETOUCH_SPASWITCH_STEP_LIMIT{ 800 };  // menu walk + up to a full picker cycle
		std::optional<SpaSwitchEditGoal> m_PendingSpaSwitchEdit;
		SpaSwitchEditPhase m_SpaSwitchEditPhase{ SpaSwitchEditPhase::ToSystemSetup };
		bool m_SpaSwitchEditInProgress{ false };
		uint32_t m_SpaSwitchEditStepCount{ 0 };
		std::optional<std::string> m_PickerFirstSeenFunction;  // wrap detection while cycling the picker
		uint32_t m_SpaSwitchCursorStuck{ 0 };

	private:
		// Proactive chlorinator-setpoint refresh (read-only Set AquaPure re-scrape). m_RefreshState
		// holds the "when to scrape" policy; m_RefreshInProgress tracks an in-flight visit (counts
		// as a goal, so it blocks user commands on the shared Navigator) and is driven step-by-step
		// with m_RefreshStepCount as a frame backstop.
		ChlorinatorSetpointRefresh m_RefreshState;
		bool m_RefreshInProgress{ false };
		uint32_t m_RefreshStepCount{ 0 };

	private:
		// AqualinkD always uses 0x80 (V2_Normal) for ACKs. The controller may ignore
	// 0x00 responses and fail to register the device, so start with V2_Normal.
	Messages::AckTypes m_AckType_ToSend{ Messages::AckTypes::V2_Normal };
		KeyCommands m_KeyCommand_ToSend{ KeyCommands::NoKeyCommand };

	private:
		// Read-only sink for the controller's internal schedules parsed off the per-equipment
		// Program detail pages (the /api/controller/schedules source). Resolved from the
		// HubLocator in the constructor; null on a passive/test rig that registers no store.
		std::shared_ptr<Scheduling::ControllerScheduleStore> m_ControllerScheduleStore;

		// Accumulator for the controller schedules scraped across the per-equipment Program
		// detail pages. The OneTouch, unlike the IAQ's single list page, shows ONE program at a
		// time (one equipment, one "Pgm N of M"), so each detail-page visit adds/updates its
		// entry here and the whole snapshot is republished to the store. Keyed by
		// (target, program-index) so revisiting a page updates in place rather than duplicating,
		// and the map iteration order gives a stable, sorted snapshot.
		std::map<std::pair<std::string, int>, Scheduling::ControllerSchedule> m_ControllerSchedules;

		// The active Program Group, if it has been read off the Program Group page during this
		// crawl (empty otherwise). Stamped onto each schedule and passed to the store so the UI
		// can show which group the snapshot belongs to.
		std::string m_ControllerScheduleGroup;

		// Parse a just-completed Program detail page, fold it into m_ControllerSchedules, and
		// republish the accumulated snapshot to m_ControllerScheduleStore (status Available).
		void PublishControllerSchedules(const Utility::ScreenDataPage& page);

	private:
		Types::ProfilingUnitTypePtr m_ProfilingDomain;
	};

}
// namespace AqualinkAutomate::Devices
