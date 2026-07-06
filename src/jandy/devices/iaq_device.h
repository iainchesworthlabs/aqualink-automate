#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

#include "devices/jandy_controller.h"
#include "devices/jandy_device_types.h"
#include "devices/capabilities/chlorinator_controller.h"
#include "devices/capabilities/command_history.h"
#include "devices/capabilities/controller_schedule_writer.h"
#include "devices/capabilities/describable.h"
#include "devices/capabilities/device_actuator.h"
#include "devices/capabilities/emulated.h"
#include "devices/capabilities/page_navigator.h"
#include "devices/capabilities/restartable.h"
#include "devices/capabilities/screen.h"
#include "devices/capabilities/setpoint_controller.h"
#include "devices/capabilities/spa_switch_configurator.h"
#include "devices/iaq/iaq_command_sink.h"
#include "devices/iaq/iaq_page_model.h"
#include "devices/iaq/iaq_page_registry.h"
#include "devices/iaq/iaq_schedule_parser.h"
#include "devices/iaq/iaq_schedule_writer.h"
#include "devices/iaq/iaq_spaswitch_writer.h"
#include "scheduling/controller_schedule.h"
#include "messages/jandy_message_probe.h"
#include "messages/iaq/iaq_message_aux_status.h"
#include "messages/iaq/iaq_message_command_ready.h"
#include "messages/iaq/iaq_message_control_ready.h"
#include "messages/iaq/iaq_message_heartbeat.h"
#include "messages/iaq/iaq_message_main_status.h"
#include "messages/iaq/iaq_message_message_long.h"
#include "messages/iaq/iaq_message_onetouch_status.h"
#include "messages/iaq/iaq_message_page_button.h"
#include "messages/iaq/iaq_message_page_continue.h"
#include "messages/iaq/iaq_message_page_end.h"
#include "messages/iaq/iaq_message_page_message.h"
#include "messages/iaq/iaq_message_page_start.h"
#include "messages/iaq/iaq_message_poll.h"
#include "messages/iaq/iaq_message_startup.h"
#include "messages/iaq/iaq_message_table_message.h"
#include "messages/iaq/iaq_message_title_message.h"
#include "utility/screen_data_page.h"
#include "utility/screen_data_page_updater.h"
#include "kernel/auxillary_devices/heater_status.h"
#include "kernel/body_of_water_ids.h"
#include "kernel/hub_locator.h"
#include "profiling/profiling.h"

namespace AqualinkAutomate::Devices
{

	class IAQDevice : public JandyController, public Capabilities::Restartable, public Capabilities::Screen, public Capabilities::Emulated, public Capabilities::Describable, public Capabilities::ChlorinatorController, public Capabilities::PageNavigator, public Capabilities::DeviceActuator, public Capabilities::SetpointController, public Capabilities::SpaSwitchConfigurator, public Capabilities::CommandHistory, public Capabilities::ControllerScheduleWriter, public IAQ::ICommandSink
	{
		inline static const uint8_t IAQ_STATUS_PAGE_LINES = 18;
		inline static const uint8_t IAQ_MESSAGE_TABLE_LINES = 18;
		inline static const std::chrono::seconds IAQ_TIMEOUT_DURATION{ std::chrono::seconds(30) };

		// PageStart id of the controller's Schedule list ("Schedule Group A/B"): its
		// TableMessage (0x26) rows are the controller's internal program entries. RE'd
		// from a live capture (docs/iaq_schedule_protocol.md); the row parser rejects
		// non-schedule text, so a same-id page on another model cannot yield garbage.
		inline static const uint8_t IAQ_SCHEDULE_PAGE_ID = 0x28;

		// PageStart id of the schedule editor's device picker (the scrolling list of equipment a new
		// program can drive). Its group-0 TableMessage rows are accumulated during a write.
		inline static const uint8_t IAQ_DEVICE_PICKER_PAGE_ID = 0x38;

		// PageStart id of the time picker (opened from a program's ON/OFF field). PageMessage line 1
		// is "HH:MM", line 2 is "AM"/"PM"; the schedule writer reads line 2 to decide the AM/PM toggle.
		inline static const uint8_t IAQ_TIME_PICKER_PAGE_ID = 0x29;
		inline static const uint8_t IAQ_TIME_PICKER_AMPM_LINE = 2;

		enum class OperatingStates
		{
			StartUp,
			NormalOperation,
			FaultHasOccurred,
			NotPresent          // an id the master never addresses (e.g. an emulated iAqualink2 the panel isn't configured for)
		};

	public:
		IAQDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated);
		~IAQDevice() override;

	public:
		nlohmann::json DescribeDiagnostics() const override;

		void QueueCommand(uint8_t command);
		void QueueChlorinatorPercentage(uint8_t percentage);
		void QueueChlorinatorBoost(bool enable);

		// Press an on-screen PageButton on the AqualinkTouch (0x33) page UI by its index
		// (the index carried in the master's IAQMessage_PageButton frames). This lets the
		// emulated panel navigate the master's pages -- open sub-pages, toggle equipment --
		// exactly as a physical touch would.
		void SelectPageButton(uint8_t button_index);

		// Capability implementations (ChlorinatorController / PageNavigator): let the
		// capability-routed CommandDispatcher drive the chlorinator output/boost and the
		// page UI through the AqualinkTouch (0x33) panel without knowing IAQ specifics.
		Capabilities::ActuationResult SetChlorinatorPercentage(uint8_t percentage) override;
		Capabilities::ActuationResult SetChlorinatorBoost(bool enable) override;
		Capabilities::ActuationResult ActuatePageButton(uint8_t button_index) override;

		// DeviceActuator: toggle a logical aux on/off by finding the on-screen PageButton
		// whose name matches the device label and pressing it (0x11 + index). Verified vs
		// iaq_aux_setpoint.cap (Pool Light idx9 -> 0x1a, Spillway idx11 -> 0x1c).
		Capabilities::ActuationResult ActuateDevice(const std::shared_ptr<Kernel::AuxillaryDevice>& device, Capabilities::ActuationAction action) override;

		// SetpointController: set the pool/spa heater setpoint via the value-submit protocol -
		// navigate to the Set Temperature page, select the Pool Heat / Spa Heat field and
		// submit the absolute value (NOT stepped). Verified vs iaq_aux_setpoint.cap.
		Capabilities::ActuationResult SetPoolSetpoint(uint8_t temperature) override;
		Capabilities::ActuationResult SetSpaSetpoint(uint8_t temperature) override;

		// SpaSwitchConfigurator: program a spa-side switch button's function over the bus by driving
		// the AqualinkTouch "4 Function Spa Switch" detail (queues a goal on m_SpaSwitchWriter,
		// serviced per-poll, page-gated on PageId): navigate -> Spa Remotes -> open detail ->
		// select the S:B row -> scroll the device picker to the target function -> commit. RE'd +
		// cross-validated from captures/iaq_spaswitch_edit{,2}.cap. On-screen rows 1-7 are supported;
		// row 8 (2:4) / switch>2 need an undecoded assignment-list scroll, so those return
		// NotSupported and the SpasideRemoteController falls through to the OneTouch. The iAQ is
		// Medium priority, so it takes precedence over the OneTouch for the rows it can program.
		// See docs/alwin32/spaside-remotes.md.
		Capabilities::ActuationResult SetSpaSwitchAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function) override;

		// SpaSwitchConfigurator: the assignable function set. The iAQ scrolls a live picker (group
		// 0x01) during an edit, but that is only present transiently mid-edit; absent a persisted
		// decode we return the shared canonical list (the same set the OneTouch cycles).
		std::vector<std::string> AvailableFunctions() const override;

		// Precedence shared by DeviceActuator + SetpointController + ChlorinatorController
		// (identical signature). The AqualinkTouch effects actions with DIRECT commands
		// (page-button press, value-submit), so it ranks Medium - above the OneTouch (Low),
		// which must crawl menus, and below a Serial Adapter (High). On a combined rig the
		// faster AqualinkTouch path is therefore preferred over the OneTouch.
		Capabilities::ActuationPriority ControllerPriority() const override { return Capabilities::ActuationPriority::Medium; }

		// Arm a start-up PAGE SURVEY: once the home page is established (first MainStatus), an
		// emulated panel walks `registry`'s data pages -- navigating to each, dwelling so it
		// renders+decodes, then back -- to source data the pushed home page does not carry
		// (setpoints, etc.). Targeted navigation instead of menu spidering. Runs once.
		void EnablePageSurvey(const IAQ::PageRegistry& registry);

		// WRITE a new program into the controller's active schedule group by driving the
		// AqualinkTouch Program pages (see docs/iaq_schedule_protocol.md, write path). Queues a
		// goal on m_ScheduleWriter serviced per-poll: navigate to the Schedule
		// list (0x28) -> Add Program (0x11) -> select the target device on the picker (0x38) ->
		// set the ON/OFF times (0x21/0x22 -> time picker -> submit) and day (0x17-0x20).
		// Rejects (InvalidValue) any program the controller cannot represent -- the feasibility is
		// the shared Scheduling::CheckControllerCandidate predicate. NotSupported when passive
		// (a non-emulated iAQ never transmits); Busy if a write is already in flight.
		Capabilities::ActuationResult CreateControllerProgram(const Scheduling::ControllerSchedule& program) override;

		// DELETE an existing controller program: navigate to the Schedule list, click the row whose
		// parsed contents match `program` (target + day + on/off times), press Delete, and confirm.
		// NotSupported when passive / busy; InvalidValue if no matching row is present to remove.
		Capabilities::ActuationResult DeleteControllerProgram(const Scheduling::ControllerSchedule& program) override;

		// EDIT an existing controller program: navigate to the Schedule list, click the row matching
		// `existing`, press Edit (0x12) to enter row-edit mode, then re-set the ON/OFF times and day
		// from `desired` (same field keys as create) and verify the list now shows `desired`.
		// NotSupported when passive / busy; InvalidValue if `desired` is not controller-representable.
		Capabilities::ActuationResult EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired) override;

		// Operating-state queries (also exercised by the device tests).
		bool IsInNormalOperation() const { return m_OpState == OperatingStates::NormalOperation; }
		bool IsFaulted() const { return m_OpState == OperatingStates::FaultHasOccurred; }
		bool IsNotPresent() const { return m_OpState == OperatingStates::NotPresent; }

	protected:
		void ProcessControllerUpdates() override;
		void ProcessControllerUpdates(bool is_poll_message);

		void WatchdogTimeoutOccurred() override;

	private:
		void Slot_IAQ_AuxStatus(const Messages::IAQMessage_AuxStatus& msg);
		void Slot_IAQ_CommandReady(const Messages::IAQMessage_CommandReady& msg);
		void Slot_IAQ_ControlReady(const Messages::IAQMessage_ControlReady& msg);
		void Slot_IAQ_Heartbeat(const Messages::IAQMessage_Heartbeat& msg);
		void Slot_IAQ_MainStatus(const Messages::IAQMessage_MainStatus& msg);
		void Slot_IAQ_MessageLong(const Messages::IAQMessage_MessageLong& msg);
		void Slot_IAQ_OneTouchStatus(const Messages::IAQMessage_OneTouchStatus& msg);
		void Slot_IAQ_PageButton(const Messages::IAQMessage_PageButton& msg);
		void Slot_IAQ_PageContinue(const Messages::IAQMessage_PageContinue& msg);
		void Slot_IAQ_PageEnd(const Messages::IAQMessage_PageEnd& msg);
		void Slot_IAQ_PageMessage(const Messages::IAQMessage_PageMessage& msg);
		void Slot_IAQ_PageStart(const Messages::IAQMessage_PageStart& msg);
		void Slot_IAQ_Poll(const Messages::IAQMessage_Poll& msg);
		void Slot_IAQ_Probe(const Messages::JandyMessage_Probe& msg);
		void Slot_IAQ_StartUp(const Messages::IAQMessage_StartUp& msg);
		void Slot_IAQ_TableMessage(const Messages::IAQMessage_TableMessage& msg);
		void Slot_IAQ_TitleMessage(const Messages::IAQMessage_TitleMessage& msg);

	private:
		void ProcessMainStatus(const Messages::IAQMessage_MainStatus& msg);
		void ProcessAuxStatus(const Messages::IAQMessage_AuxStatus& msg);

		// Create (if missing) and update a heater device in the DataHub, then emit a
		// button-state change.  Factored out of ProcessMainStatus so the three heater
		// updates share one code path.
		void UpdateHeaterDevice(const std::string& label, Kernel::HeaterStatuses status, Kernel::BodyOfWaterIds body_id);

		// Once home is established, queue the page-survey navigation sequence (built from the
		// registry) so it drains one command per poll. Emulated + survey-enabled + not-run only.
		void MaybeStartPageSurvey();

	private:
		// Render the live decoded system status into the device's Screen capability
		// as a fixed "System Status" page so the diagnostics "Actual Devices" card
		// shows real data instead of Page_Unknown.  The IAQ (iAqualink2 cloud
		// interface) has no navigable physical screen, so its screen is a reflection
		// of the status it just decoded from MainStatus (+ DataHub aux state).
		void RenderStatusScreen(const Messages::IAQMessage_MainStatus& msg);

		// Render a fixed "Cloud Link" page for a heartbeat-only IAQ (the iAqualink2
		// cloud interface on 0xA3) which receives ONLY the heartbeat (0x53) and never
		// a MainStatus/AuxStatus or any navigable page.  Without this it would sit on
		// the constructor-default Page_Unknown forever.  Mirrors RenderStatusScreen
		// but the content is the heartbeat liveness, not decoded system status.
		void RenderCloudLinkScreen();

	private:
		Utility::ScreenDataPage m_StatusPage;
		Utility::ScreenDataPage m_TableInfo;

	private:
		Utility::ScreenDataPageUpdater<Utility::ScreenDataPage> m_SM_PageUpdate;
		Utility::ScreenDataPageUpdater<Utility::ScreenDataPage> m_SM_TableUpdate;

	private:
		void Signal_ControlDataResponse(const std::string& ascii_data);

		// Queue the value-submit sequence for a heater setpoint: BACK -> open Set Temperature
		// page -> select the given field button -> submit; the absolute value rides in the
		// control-data response ("1" + value).
		Capabilities::ActuationResult QueueSetpoint(uint8_t select_field_command, uint8_t temperature, const char* body_name);

	private:
		// The decoded live-page UI state: current page id, title, on-screen PageButton table, and
		// the schedule / device-picker / spa-switch-picker row accumulators. Written by the IAQ
		// message slots and read by the actuators + the write state machines. Extracted from this
		// class (SonarCloud S1448/S1820); see docs/iaq_device_decomposition.md.
		IAQ::PageModel m_PageModel;

		// Resolved from the HubLocator: the read-only snapshot of the controller's own
		// internal schedules that the /api/controller/schedules route serves. Null-safe
		// (a passive/test rig may not register one). Populated by PublishSchedulePage().
		std::shared_ptr<Scheduling::ControllerScheduleStore> m_ControllerScheduleStore{ nullptr };

		// Parse the just-completed Schedule list page (m_PageModel's schedule rows + title)
		// into ControllerSchedule spans and swap them into the store, tagged with the
		// active program group. A no-op when the store is absent.
		void PublishSchedulePage() const;

		// The spa-switch button-assignment WRITE state machine (extracted; SonarCloud S1820; see
		// docs/iaq_device_decomposition.md). SetSpaSwitchAssignment arms it via Queue();
		// ProcessControllerUpdates services it one command per poll through this (as ICommandSink).
		IAQ::SpaSwitchWriter m_SpaSwitchWriter;

		// The controller-schedule WRITE state machine (create/delete/edit; extracted; SonarCloud
		// S1820; see docs/iaq_device_decomposition.md). Create/Delete/EditControllerProgram arm it
		// via Queue*(); ProcessControllerUpdates services it one command per poll through this
		// (as ICommandSink), passing m_PageModel + m_StatusPage (for the time picker's AM/PM line).
		IAQ::ScheduleWriter m_ScheduleWriter;

	private:
		// ICommandSink: the write state machines emit their per-poll command through these. IssueCommand
		// sets the single pending poll-ACK byte (no logging -- it fires every poll); ArmControlValue
		// primes the control-data handshake (value + awaiting flag); IsBusy reports the command channel
		// occupied so a new write goal is not armed on top of a draining sequence.
		void IssueCommand(uint8_t command) override { m_PendingCommand = command; }
		void ArmControlValue(std::string value) override { m_ControlDataValue = std::move(value); m_AwaitingControlReady = true; }
		bool IsBusy() const override { return !m_CommandQueue.empty() || m_AwaitingControlReady; }

	private:
		OperatingStates m_OpState{ OperatingStates::StartUp };
		bool m_HasReceivedData{ false };       // has any traffic ever been addressed to this id? (distinguishes "not present" from "went silent")
		bool m_HasReceivedMainStatus{ false }; // has a MainStatus ever decoded? (a 0x33 renders System Status; a heartbeat-only 0xA3 renders Cloud Link)
		uint8_t m_PendingCommand{ 0x00 };
		std::deque<uint8_t> m_CommandQueue;
		bool m_AwaitingControlReady{ false };
		std::string m_ControlDataValue;

		// Start-up page-survey state (extracted; SonarCloud S1820). EnablePageSurvey arms it;
		// MaybeStartPageSurvey (emulated + home established) consumes it once into the command queue.
		IAQ::PageSurvey m_PageSurvey;

	private:
		Types::ProfilingUnitTypePtr m_ProfilingDomain;
	};

}
// namespace AqualinkAutomate::Devices
