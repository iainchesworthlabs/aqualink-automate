#include <functional>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "logging/logging.h"
#include "auxillaries/jandy_auxillary_traits_types.h"
#include "devices/device_status.h"
#include "devices/onetouch_device.h"
#include "devices/onetouch/onetouch_goals.h"
#include "devices/onetouch/onetouch_schedule_parser.h"
#include "devices/onetouch/onetouch_scraper.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "formatters/jandy_device_formatters.h"
#include "kernel/body_of_water.h"
#include "kernel/body_of_water_ids.h"
#include "navigation/onetouch_menu_model.h"
#include "navigation/visit_policies.h"
#include "scheduling/promotion_constraints.h"
#include "utility/jandy_equipment_validator.h"
#include "utility/jandy_pool_configuration_decoder.h"
#include "utility/screen_data_page_processor.h"
#include "utility/spa_switch_assignment.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Messages;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Devices
{

	OneTouchDevice::OneTouchDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated) :
		JandyController(device_id, hub_locator),
		Capabilities::Restartable(ONETOUCH_TIMEOUT_DURATION),
		Capabilities::Screen(ONETOUCH_PAGE_LINES),
		Capabilities::Emulated(is_emulated),
		m_MenuModel(Navigation::CreateOneTouchMenuModel()),
		m_Navigator(std::make_unique<Navigation::Navigator>(m_MenuModel)),
		m_SpiderEngine(std::make_unique<Navigation::SpiderEngine>(m_MenuModel, *m_Navigator)),
		m_ProfilingDomain(std::move(Factory::ProfilingUnitFactory::Instance().CreateDomain("OneTouchDevice")))
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::OneTouchDevice", std::source_location::current());

		LogInfo(Channel::Devices, std::format("Creating OneTouchDevice: device_id={}, emulated={}, timeout={}s", *device_id, is_emulated, ONETOUCH_TIMEOUT_DURATION.count()));

		m_ProfilingDomain->Start();

		// The read path (page + status processors, panel-config decode, controller-schedule
		// accumulation) lives in the OneTouchScraper collaborator. Build it with the shared DataHub
		// and the (optional) controller-schedule store, then register its processors into the Screen
		// capability - each processor closure is bound to the scraper.
		m_Scraper = std::make_unique<OneTouchScraper>(device_id, m_DataHub, hub_locator.TryFind<Scheduling::ControllerScheduleStore>());
		PageProcessors(m_Scraper->MakeProcessors());
		LogTrace(Channel::Devices, std::format("OneTouch ({}): Registered {} page processors for OneTouchDevice", DeviceId(), PageProcessors().size()));

		LogDebug(Channel::Devices, std::format("OneTouch ({}): Registering OneTouchDevice message slot handlers", DeviceId()));
		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_MessageLong>(std::bind(&OneTouchDevice::Slot_OneTouch_MessageLong, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Probe>(std::bind(&OneTouchDevice::Slot_OneTouch_Probe, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Status>(std::bind(&OneTouchDevice::Slot_OneTouch_Status, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<PDAMessage_Clear>(std::bind(&OneTouchDevice::Slot_OneTouch_Clear, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<PDAMessage_Highlight>(std::bind(&OneTouchDevice::Slot_OneTouch_Highlight, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<PDAMessage_HighlightChars>(std::bind(&OneTouchDevice::Slot_OneTouch_HighlightChars, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<PDAMessage_ShiftLines>(std::bind(&OneTouchDevice::Slot_OneTouch_ShiftLines, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_DisplayUpdate>(std::bind(&OneTouchDevice::Slot_OneTouch_DisplayUpdate, this, std::placeholders::_1), (*device_id)());
		m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Unknown>(std::bind(&OneTouchDevice::Slot_OneTouch_Unknown, this, std::placeholders::_1), (*device_id)());

		if (!IsEmulated())
		{
			LogTrace(Channel::Devices, std::format("OneTouch ({}): Registering ACK handler for non-emulated device", DeviceId()));
			m_SlotManager.RegisterSlot_FilterByDeviceId<JandyMessage_Ack>(std::bind(&OneTouchDevice::Slot_OneTouch_Ack, this, std::placeholders::_1), (*device_id)());
		}

		LogInfo(Channel::Devices, std::format("OneTouch ({}): OneTouchDevice construction complete - device ready", DeviceId()));
	}

	OneTouchDevice::~OneTouchDevice()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::~OneTouchDevice", std::source_location::current());

		LogInfo(Channel::Devices, std::format("OneTouch ({}): Destroying OneTouchDevice: final state was {}", DeviceId(), magic_enum::enum_name(m_OpState)));

		m_ProfilingDomain->End();

		LogTrace(Channel::Devices, std::format("OneTouch ({}): OneTouchDevice destruction complete", DeviceId()));
	}

	void OneTouchDevice::ProcessControllerUpdates()
	{
		// Non-Status message variant - don't send key commands
		ProcessControllerUpdates(false);
	}

	void OneTouchDevice::ProcessControllerUpdates(bool is_status_message)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ProcessControllerUpdates", std::source_location::current());

		LogTrace(Channel::Devices, std::format("OneTouch ({}): ProcessControllerUpdates called: state={}, is_status={}, pending_cmd={}",
			DeviceId(), magic_enum::enum_name(m_OpState), is_status_message, magic_enum::enum_name(m_KeyCommand_ToSend)));

		// NOTE: Do NOT reset m_KeyCommand_ToSend here. Commands can only be sent in
		// response to Status messages. If a command is set during a non-Status message
		// (e.g., MessageLong/Highlight), it must persist until the next Status message.

		// Non-emulated devices should not run the scraping state machine; they
		// passively observe screens driven by the physical device.  Skip straight
		// to NormalOperation on the first update.
		if (!IsEmulated() && (m_OpState == OperatingStates::ColdStart || m_OpState == OperatingStates::StartUp))
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): Non-emulated device detected - skipping scraping, entering NormalOperation", DeviceId()));
			m_OpState = OperatingStates::NormalOperation;
			Status(Devices::DeviceStatus_Normal{});
		}

		switch (m_OpState)
		{
		case OperatingStates::ColdStart:
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ProcessControllerUpdates -> cold_start", std::source_location::current());
			auto detected = m_MenuModel.DetectPage(DisplayedPage());
			if (detected == Navigation::PageId::StartUp)
			{
				// Splash is still showing - stay in ColdStart and wait.
				// Page processor has already extracted model/type/revision.
				LogDebug(Channel::Devices, std::format("OneTouch ({}): Cold start splash active - waiting for controller to transition", DeviceId()));
			}
			else if (detected != Navigation::PageId::Unknown)
			{
				// Controller has moved past splash to a real page - start spider
				LogInfo(Channel::Devices, std::format("OneTouch ({}): Controller ready - proceeding to StartUp", DeviceId()));
				m_OpState = OperatingStates::StartUp;
				Scraping_ProcessStep_StartUp();
			}
			// else: Unknown page - stay in ColdStart, waiting for recognisable screen
			break;
		}

		case OperatingStates::StartUp:
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ProcessControllerUpdates -> start_up", std::source_location::current());
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Processing StartUp state", DeviceId()));
			Scraping_ProcessStep_StartUp();
			break;
		}

		case OperatingStates::Scraping:
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ProcessControllerUpdates -> scraping", std::source_location::current());
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Processing Scraping state", DeviceId()));
			Scraping_ProcessStep();
			break;
		}

		case OperatingStates::NormalOperation:
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ProcessControllerUpdates -> normal_operation", std::source_location::current());
			LogTrace(Channel::Devices, std::format("OneTouch ({}): Processing NormalOperation state", DeviceId()));
			ServiceActiveGoal();
			ValueEdit_ProcessStep();
			Boost_ProcessStep();
			SpaSwitchEdit_ProcessStep();
			ControllerScheduleWrite_ProcessStep();
			SetpointRefresh_ProcessStep();
			break;
		}

		case OperatingStates::ScrapingFaulted:
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ProcessControllerUpdates -> scraping_faulted", std::source_location::current());
			LogWarning(Channel::Scraping, std::format("OneTouch ({}): ScrapingFaulted state - device in unknown state, no commands will be sent", DeviceId()));
			// Do not send any commands - device state is unknown. But if the controller has
			// resumed coherent comms, this attempts to recover us back to NormalOperation.
			AttemptFaultRecovery(is_status_message);
			break;
		}

		case OperatingStates::FaultHasOccurred:
		{
			auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ProcessControllerUpdates -> fault", std::source_location::current());
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Processing FaultHasOccurred state", DeviceId()));
			// As for ScrapingFaulted: try to recover once the controller is talking coherently again.
			AttemptFaultRecovery(is_status_message);
			break;
		}
		}

		// Key commands can ONLY be sent in response to Status messages.
		// The controller ignores key commands in ACKs for other message types.
		if (is_status_message && m_KeyCommand_ToSend != KeyCommands::NoKeyCommand)
		{
			LogTrace(Channel::Devices, std::format("OneTouch({}) : Sending key command in Status ACK: {}", DeviceId(), magic_enum::enum_name(m_KeyCommand_ToSend)));
			// Key commands must be sent with V1_Normal ACK type - the controller ignores
			// key commands in V2_Normal (0x80) ACKs.
			Signal_AckMessage(AckTypes::V1_Normal, m_KeyCommand_ToSend);
			// Clear the command after sending to prevent repeated sends
			m_KeyCommand_ToSend = KeyCommands::NoKeyCommand;
		}
		else
		{
			if (m_KeyCommand_ToSend != KeyCommands::NoKeyCommand)
			{
				LogDebug(Channel::Devices, std::format("OneTouch({}) : Key command {} pending - will send on next Status message", DeviceId(), magic_enum::enum_name(m_KeyCommand_ToSend)));
			}
			Signal_AckMessage(m_AckType_ToSend, KeyCommands::NoKeyCommand);
		}
	}

	void OneTouchDevice::WatchdogTimeoutOccurred()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::WatchdogTimeoutOccurred", std::source_location::current());

		LogWarning(Channel::Devices, std::format("OneTouch({}) : Watchdog timeout occurred: state={}, timeout_duration={}s", DeviceId(), magic_enum::enum_name(m_OpState), ONETOUCH_TIMEOUT_DURATION.count()));

		if (m_OpState == OperatingStates::Scraping)
		{
			// Scraping was in progress when the watchdog fired.  Abandon the
			// scraping sequence and fall through to normal (passive) operation
			// so the device is at least partially functional.
			LogWarning(Channel::Devices, std::format("OneTouch({}) : Abandoning scraping due to watchdog timeout -> entering NormalOperation", DeviceId()));
			ValidateDiscoveredEquipment();
			// Startup discovery is over (degraded): allow periodic refresh, but do NOT seed the
			// timer - the timed-out crawl may never have reached Set AquaPure, so let the first
			// periodic re-scrape happen promptly to recover the configured setpoint.
			m_RefreshState.MarkStartupComplete();
			m_OpState = OperatingStates::NormalOperation;
			m_ScrapingStallCounter = 0;
			Status(Devices::DeviceStatus_Normal{});
		}
		else if (m_OpState == OperatingStates::StartUp || m_OpState == OperatingStates::ColdStart)
		{
			// Never received a recognisable page during start-up.
			LogWarning(Channel::Devices, std::format("OneTouch({}) : No valid page received during startup -> entering FaultHasOccurred", DeviceId()));
			m_OpState = OperatingStates::FaultHasOccurred;
			Status(Devices::DeviceStatus_FaultOccurred{});
		}
	}

	void OneTouchDevice::AttemptFaultRecovery(bool is_status_message)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::AttemptFaultRecovery", std::source_location::current());

		// Only a Status frame marks a fully-rendered, coherent page (the per-line MessageLong /
		// Highlight frames arrive mid-update). Counting only Status frames makes the hysteresis a
		// genuine "N coherent screen refreshes" confidence measure rather than per-byte noise.
		if (!is_status_message)
		{
			return;
		}

		// A RECOGNISED page is the evidence that the controller is talking coherently to our device
		// id again. An Unknown page (line noise, a partial/garbled screen, or a page this model does
		// not expose) breaks the streak, so recovery needs CONSECUTIVE good frames. This is the
		// backoff: a permanently-broken controller that never sustains a recognised page never
		// crosses the threshold and stays safely faulted (honestly reporting NotSupported) instead
		// of thrashing in and out of NormalOperation.
		const auto detected = m_MenuModel.DetectPage(DisplayedPage());
		if (Navigation::PageId::Unknown == detected)
		{
			if (0 != m_FaultRecoveryStatusCount)
			{
				LogDebug(Channel::Devices, std::format("OneTouch ({}): fault-recovery streak reset (no recognised page while {})", DeviceId(), magic_enum::enum_name(m_OpState)));
			}
			m_FaultRecoveryStatusCount = 0;
			return;
		}

		if (++m_FaultRecoveryStatusCount < ONETOUCH_FAULT_RECOVERY_STATUS_FRAMES)
		{
			LogDebug(Channel::Devices, std::format("OneTouch ({}): controller responding while {} (page '{}', {}/{} good frames)",
				DeviceId(), magic_enum::enum_name(m_OpState), magic_enum::enum_name(detected), m_FaultRecoveryStatusCount, ONETOUCH_FAULT_RECOVERY_STATUS_FRAMES));
			return;
		}

		// Threshold reached: the controller is coherent again. Degrade straight to NormalOperation,
		// mirroring the watchdog Scraping->NormalOperation recovery. We deliberately do NOT re-run
		// the full menu crawl: re-scraping would drive the keypad through the whole menu tree from a
		// controller whose responsiveness is still suspect, whereas NormalOperation restores actuation
		// immediately and re-acquires state passively + via the periodic setpoint refresh. Re-validate
		// the discovered equipment, allow the refresh to run, and reset the shared Navigator so the
		// first on-demand goal starts clean.
		LogWarning(Channel::Devices, std::format("OneTouch ({}): controller resumed comms (page '{}') - recovering from {} to NormalOperation",
			DeviceId(), magic_enum::enum_name(detected), magic_enum::enum_name(m_OpState)));

		ValidateDiscoveredEquipment();
		m_RefreshState.MarkStartupComplete();
		if (m_Navigator)
		{
			m_Navigator->Reset();
		}
		m_OpState = OperatingStates::NormalOperation;
		m_ScrapingStallCounter = 0;
		m_FaultRecoveryStatusCount = 0;
		Status(Devices::DeviceStatus_Normal{});
	}

	OneTouchDevice::KeyCommands OneTouchDevice::ConvertNavKeyCommand(Navigation::NavKeyCommand nav_cmd)
	{
		switch (nav_cmd)
		{
		case Navigation::NavKeyCommand::NoCommand:
			return KeyCommands::NoKeyCommand;
		case Navigation::NavKeyCommand::PageDown_Or_Select1:
			return KeyCommands::PageDown_Or_Select1;
		case Navigation::NavKeyCommand::Back:
			return KeyCommands::Back_Or_Select2;
		case Navigation::NavKeyCommand::PageUp_Or_Select3:
			return KeyCommands::PageUp_Or_Select3;
		case Navigation::NavKeyCommand::Select:
			return KeyCommands::Select;
		case Navigation::NavKeyCommand::LineDown:
			return KeyCommands::LineDown;
		case Navigation::NavKeyCommand::LineUp:
			return KeyCommands::LineUp;
		default:
			return KeyCommands::NoKeyCommand;
		}
	}

	bool OneTouchDevice::DataHubHasSeededAuxLabels() const
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::DataHubHasSeededAuxLabels", std::source_location::current());

		if (nullptr == JandyController::m_DataHub)
		{
			return false;
		}

		// A real iAqualink2 (AqualinkTouch 0x33) decodes aux NAMES from its AuxStatus
		// (0x72) frames and sets LabelTrait on the matching DataHub aux devices
		// passively. We treat any aux device carrying a non-empty label as evidence
		// the labels are already known, so the emulated OneTouch can skip scraping
		// them. Devices are keyed by JandyAuxillaryId so only Jandy auxes are
		// considered (matching what the Label Aux crawl would have populated).
		for (const auto& device : JandyController::m_DataHub->Devices.FindByTrait(Auxillaries::JandyAuxillaryId{}))
		{
			if (nullptr == device)
			{
				continue;
			}

			if (auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
				label.has_value() && !Utility::TrimWhitespace(label.value()).empty())
			{
				return true;
			}
		}

		return false;
	}

	Capabilities::ActuationResult OneTouchDevice::ActuateDevice(const std::shared_ptr<Kernel::AuxillaryDevice>& device, Capabilities::ActuationAction action)
	{
		if (nullptr == device)
		{
			return Capabilities::ActuationResult::MappingFailed;
		}

		// Passive/suppressed (cannot transmit) or in a dead-end fault state (a queued goal would
		// be stranded): refuse honestly so the dispatcher can fall back. Transient startup states
		// are NOT blocked - the goal is serviced once scraping completes.
		if (auto reason = ReasonCannotActuate("actuate equipment"); reason.has_value())
		{
			return reason.value();
		}

		auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
		if (!label.has_value() || Utility::TrimWhitespace(label.value()).empty())
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Cannot actuate a device with no label", DeviceId()));
			return Capabilities::ActuationResult::MappingFailed;
		}
		const std::string target_label{ Utility::TrimWhitespace(label.value()) };

		// The keypad Select is a pure in-place TOGGLE. For an explicit On/Off, only act
		// when the device's current state differs; if it already matches the request,
		// succeed as a no-op rather than toggling it the wrong way.
		if (action != Capabilities::ActuationAction::Toggle)
		{
			const bool want_on{ action == Capabilities::ActuationAction::On };
			if (auto current = CurrentOnState(device); current.has_value() && (current.value() == want_on))
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): '{}' already {} - no actuation required", DeviceId(), target_label, want_on ? "ON" : "OFF"));
				return Capabilities::ActuationResult::Accepted;
			}
		}

		// One goal at a time: reject a new request while any toggle/setpoint goal is
		// mid-navigation so two cursor walks never interleave on the single shared Navigator.
		if (GoalInProgress())
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Busy actuating; rejecting request for '{}'", DeviceId(), target_label));
			return Capabilities::ActuationResult::NotSupported;
		}

		m_Runner.TryStart(std::make_unique<OneTouch::ToggleGoal>(target_label));
		LogInfo(Channel::Devices, std::format("OneTouch ({}): Queued toggle of '{}'", DeviceId(), target_label));
		return Capabilities::ActuationResult::Accepted;
	}

	std::optional<bool> OneTouchDevice::CurrentOnState(const std::shared_ptr<Kernel::AuxillaryDevice>& device) const
	{
		namespace ATT = Kernel::AuxillaryTraitsTypes;

		if (!device->AuxillaryTraits.Has(ATT::AuxillaryTypeTrait{}))
		{
			return std::nullopt;
		}

		if (*(device->AuxillaryTraits[ATT::AuxillaryTypeTrait{}]) == ATT::AuxillaryTypes::Pump)
		{
			if (auto s = device->AuxillaryTraits.TryGet(ATT::PumpStatusTrait{}); s.has_value())
			{
				return (s.value() == Kernel::PumpStatuses::Running);
			}
			return std::nullopt;
		}

		if (auto s = device->AuxillaryTraits.TryGet(ATT::AuxillaryStatusTrait{}); s.has_value())
		{
			return (s.value() == Kernel::AuxillaryStatuses::On);
		}
		return std::nullopt;
	}

	void OneTouchDevice::ServiceActiveGoal()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ServiceActiveGoal", std::source_location::current());

		if (!m_Navigator || !m_Runner.HasActiveGoal())
		{
			return;
		}

		// A per-cycle view of the single shared keypad: the current screen, the cursor line and the
		// shared Navigator. The goal drives it and emits at most one key, which we translate into the
		// wire KeyCommands (actually sent on the next Status message by ProcessControllerUpdates).
		OneTouch::KeypadContext ctx{ DeviceId(), DisplayedPage(), m_HighlightedLine, *m_Navigator };
		m_Runner.Service(ctx);
		if (ctx.emitted_key.has_value())
		{
			m_KeyCommand_ToSend = ConvertNavKeyCommand(ctx.emitted_key.value());
		}
	}

	bool OneTouchDevice::GoalInProgress() const
	{
		return m_Runner.HasActiveGoal()
			|| m_ValueEditInProgress
			|| m_PendingValueEdit.has_value()
			|| m_BoostInProgress
			|| m_PendingBoost.has_value()
			|| m_SpaSwitchEditInProgress
			|| m_PendingSpaSwitchEdit.has_value()
			|| m_ScheduleWriteInProgress
			|| m_PendingScheduleWrite.has_value()
			|| m_RefreshInProgress;
	}

	bool OneTouchDevice::MoveCursorToward(uint8_t target_line)
	{
		if (m_HighlightedLine == target_line) { return true; }
		if (m_HighlightedLine == Navigation::Navigator::CURSOR_LINE_NONE)
		{
			m_KeyCommand_ToSend = KeyCommands::LineDown;   // establish a cursor first
			return false;
		}
		m_KeyCommand_ToSend = (m_HighlightedLine < target_line) ? KeyCommands::LineDown : KeyCommands::LineUp;
		return false;
	}

	std::optional<Capabilities::ActuationResult> OneTouchDevice::ReasonCannotActuate(std::string_view what) const
	{
		// A passive OneTouch never transmits key commands (non-emulated or presence-suppressed),
		// so it cannot actuate; gate on IsEmulationActive() rather than IsEmulated().
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Not actively emulating - cannot {}", DeviceId(), what));
			return Capabilities::ActuationResult::NotSupported;
		}

		// Dead-end fault states (ScrapingFaulted / FaultHasOccurred) never run the per-frame
		// service step that drains a queued goal (NormalOperation only), so a goal queued here
		// would be stranded while the caller is told it succeeded. Refuse honestly with
		// NotSupported so the dispatcher can fall back. Transient startup states are NOT blocked.
		if (OperatingStates::ScrapingFaulted == m_OpState || OperatingStates::FaultHasOccurred == m_OpState)
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): controller is in fault state {} - cannot {}", DeviceId(), magic_enum::enum_name(m_OpState), what));
			return Capabilities::ActuationResult::NotSupported;
		}

		return std::nullopt;
	}

	void OneTouchDevice::EnableChlorinatorSetpointRefresh(std::chrono::seconds interval)
	{
		m_RefreshState.Configure(interval);
		LogInfo(Channel::Devices, std::format("OneTouch ({}): Chlorinator setpoint refresh {} (interval {}s)",
			DeviceId(), m_RefreshState.IsEnabled() ? "enabled" : "disabled", interval.count()));
	}

	bool OneTouchDevice::DataHubChlorinatorOnline() const
	{
		if (!JandyController::m_DataHub)
		{
			return false;
		}

		using namespace Kernel::AuxillaryTraitsTypes;

		auto chlorinators = JandyController::m_DataHub->Chlorinators();
		if (chlorinators.empty())
		{
			return false;
		}

		const auto& device = chlorinators.front();
		if (!device->AuxillaryTraits.Has(ChlorinatorStatusTrait{}))
		{
			return false;
		}

		const auto status = *(device->AuxillaryTraits[ChlorinatorStatusTrait{}]);
		return (status != Kernel::ChlorinatorStatuses::Off) && (status != Kernel::ChlorinatorStatuses::Unknown);
	}

	void OneTouchDevice::SetpointRefresh_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::SetpointRefresh_ProcessStep", std::source_location::current());

		if (!m_SpiderEngine || !m_Navigator)
		{
			return;
		}

		// Drive an in-flight read-only refresh crawl (visit Set AquaPure so its page processor
		// re-scrapes the configured %, then let the controller time out back to its default screen).
		if (m_RefreshInProgress)
		{
			if (++m_RefreshStepCount > ONETOUCH_SETPOINT_REFRESH_STEP_LIMIT)
			{
				LogDebug(Channel::Scraping, std::format("OneTouch ({}): setpoint refresh exceeded step limit - abandoning", DeviceId()));
				m_Navigator->Reset();
				m_RefreshInProgress = false;
				m_RefreshState.NotifyScrapeFinished(std::chrono::steady_clock::now());
				return;
			}

			const auto nav_cmd = m_SpiderEngine->ProcessStep(DisplayedPage(), m_HighlightedLine);
			if (const auto state = m_SpiderEngine->GetState(); state == Navigation::SpiderEngine::State::Complete || state == Navigation::SpiderEngine::State::Failed)
			{
				LogDebug(Channel::Scraping, std::format("OneTouch ({}): setpoint refresh crawl {} ({} pages visited)",
					DeviceId(),
					(state == Navigation::SpiderEngine::State::Complete) ? "complete" : "failed",
					m_SpiderEngine->GetVisitedPages().size()));
				m_Navigator->Reset();
				m_RefreshInProgress = false;
				m_RefreshState.NotifyScrapeFinished(std::chrono::steady_clock::now());
				return;
			}

			if (nav_cmd.has_value())
			{
				m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
			}
			return;
		}

		// Not in progress: decide whether to start a refresh this tick. The decision struct
		// applies all the gating (enabled / configured / actively emulating / menu free / startup
		// finished / interval-or-recovery), so a passive or busy device never navigates.
		if (m_RefreshState.Evaluate(IsEmulationActive(), GoalInProgress(), DataHubChlorinatorOnline(), std::chrono::steady_clock::now()) != ChlorinatorSetpointRefresh::Action::Scrape)
		{
			return;
		}

		LogInfo(Channel::Scraping, std::format("OneTouch ({}): starting read-only Set AquaPure setpoint refresh", DeviceId()));

		auto policy = std::make_unique<Navigation::TargetedVisitPolicy>(
			std::set<Navigation::PageId>{ Navigation::PageId::SetAquapure });
		m_SpiderEngine->StartCrawl(std::move(policy));
		m_RefreshInProgress = true;
		m_RefreshStepCount = 0;
		m_RefreshState.NotifyScrapeStarted(std::chrono::steady_clock::now());

		// Drive the first step immediately (mirrors Scraping_ProcessStep_StartUp).
		const auto nav_cmd = m_SpiderEngine->ProcessStep(DisplayedPage(), m_HighlightedLine);
		if (nav_cmd.has_value())
		{
			m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
		}
	}

	Capabilities::ActuationResult OneTouchDevice::SetPoolSetpoint(uint8_t temperature)
	{
		return QueueValueEdit({ Navigation::PageId::SetTemperature, SETTEMP_POOL_HEAT_LINE, "Pool Heat", temperature, "pool setpoint" });
	}

	Capabilities::ActuationResult OneTouchDevice::SetSpaSetpoint(uint8_t temperature)
	{
		return QueueValueEdit({ Navigation::PageId::SetTemperature, SETTEMP_SPA_HEAT_LINE, "Spa Heat", temperature, "spa setpoint" });
	}

	Capabilities::ActuationResult OneTouchDevice::SetChlorinatorPercentage(uint8_t percentage)
	{
		// The OneTouch edits AquaPure % in 5% steps, so the target must be a multiple of 5
		// for the value-step loop to land exactly. Round to the nearest 5 and clamp to 100.
		const uint8_t clamped = (percentage > 100) ? 100 : percentage;
		const auto rounded = static_cast<uint8_t>(((clamped + (ONETOUCH_CHLORINATOR_STEP / 2)) / ONETOUCH_CHLORINATOR_STEP) * ONETOUCH_CHLORINATOR_STEP);
		// Drives the POOL chlorination row ("Set Pool to: NN%") to match the IAQ's single-%
		// behaviour. Verified vs onetouch_chlorinator.cap (Pool % = Set AquaPure line 3).
		return QueueValueEdit({ Navigation::PageId::SetAquapure, OneTouchScraper::SETAQUAPURE_POOL_LINE, "Set Pool", rounded, "chlorinator %" });
	}

	Capabilities::ActuationResult OneTouchDevice::QueueValueEdit(ValueEditGoal goal)
	{
		// Passive/suppressed or in a dead-end fault state: refuse honestly (a queued edit would
		// be stranded) so the dispatcher can fall back.
		if (auto reason = ReasonCannotActuate(std::format("edit {}", goal.desc)); reason.has_value())
		{
			return reason.value();
		}

		// One goal at a time: reject while any goal is mid-navigation so two cursor walks
		// never interleave on the single shared Navigator.
		if (GoalInProgress())
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Busy actuating; rejecting {} request", DeviceId(), goal.desc));
			return Capabilities::ActuationResult::NotSupported;
		}

		LogInfo(Channel::Devices, std::format("OneTouch ({}): Queued {} -> {}", DeviceId(), goal.desc, static_cast<int>(goal.target)));
		m_PendingValueEdit = std::move(goal);
		m_ValueEditPhase = ValueEditPhase::Navigating;
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult OneTouchDevice::SetChlorinatorBoost(bool enable)
	{
		if (auto reason = ReasonCannotActuate(std::format("{} boost", enable ? "start" : "stop")); reason.has_value())
		{
			return reason.value();
		}

		if (GoalInProgress())
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Busy actuating; rejecting boost {} request", DeviceId(), enable ? "start" : "stop"));
			return Capabilities::ActuationResult::NotSupported;
		}

		m_PendingBoost = enable;
		m_BoostPhase = BoostPhase::Navigating;
		LogInfo(Channel::Devices, std::format("OneTouch ({}): Queued chlorinator boost {}", DeviceId(), enable ? "start" : "stop"));
		return Capabilities::ActuationResult::Accepted;
	}

	void OneTouchDevice::ValueEdit_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ValueEdit_ProcessStep", std::source_location::current());

		if (!m_PendingValueEdit.has_value() || !m_Navigator)
		{
			return;
		}

		const ValueEditGoal& goal = m_PendingValueEdit.value();
		const uint8_t row_line = goal.line;
		const std::string& row_label = goal.label;
		const std::string& desc = goal.desc;
		const auto target = static_cast<int>(goal.target);

		auto finish = [&](bool ok)
		{
			if (ok)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): {} edit completed", DeviceId(), desc));
			}
			else
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): {} edit abandoned", DeviceId(), desc));
			}
			m_Navigator->Reset();
			m_PendingValueEdit.reset();
			m_ValueEditInProgress = false;
			m_ValueEditPhase = ValueEditPhase::Navigating;
		};

		// Frame backstop so a mis-detected page can never wedge NormalOperation (the
		// Navigator's own timeouts normally drive it to Failed first).
		if (m_ValueEditInProgress)
		{
			if (++m_ValueEditStepCount > ONETOUCH_VALUEEDIT_STEP_LIMIT)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): {} edit exceeded {} steps - abandoning", DeviceId(), desc, ONETOUCH_VALUEEDIT_STEP_LIMIT));
				finish(false);
				return;
			}
		}

		switch (m_ValueEditPhase)
		{
		case ValueEditPhase::Navigating:
		{
			// Kick off navigation once: drive to the goal's page and position the cursor on
			// the value row. select_target is left Unknown so the Navigator stops AT the row
			// (cursor positioned) instead of pressing Select - the in-place value editor is
			// driven by this device, not the Navigator.
			if (!m_ValueEditInProgress)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to '{}' row for {}", DeviceId(), row_label, desc));
				m_Navigator->NavigateToItem(goal.page, row_line, row_label, Navigation::PageId::Unknown);
				m_ValueEditInProgress = true;
				m_ValueEditStepCount = 0;
			}

			if (auto nav_cmd = m_Navigator->OnPageUpdate(DisplayedPage(), m_HighlightedLine); nav_cmd.has_value())
			{
				m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
			}

			if (m_Navigator->IsComplete())
			{
				if (m_Navigator->IsSuccess())
				{
					LogInfo(Channel::Devices, std::format("OneTouch ({}): Cursor on '{}' row - entering value editor", DeviceId(), row_label));
					m_ValueEditPhase = ValueEditPhase::BeginEdit;
				}
				else
				{
					finish(false);
				}
			}
			break;
		}

		case ValueEditPhase::BeginEdit:
		{
			// Skip the edit entirely if the row already shows the target value (avoids a
			// pointless enter/exit-edit toggle). Wait if the value isn't readable yet.
			if (auto current = OneTouch::DisplayedValue(DisplayedPage(), row_line); current.has_value() && current.value() == target)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): '{}' already at target {} - no edit required", DeviceId(), row_label, target));
				finish(true);
				break;
			}

			// Select on the highlighted row ENTERS the in-place value editor (verified vs
			// hardware: Select then arrows change the value).
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Select to begin editing '{}'", DeviceId(), row_label));
			m_KeyCommand_ToSend = KeyCommands::Select;
			m_ValueEditPhase = ValueEditPhase::Stepping;
			break;
		}

		case ValueEditPhase::Stepping:
		{
			// In edit mode, step toward the target per status cycle: LineUp increments,
			// LineDown decrements (the device applies its own increment - 1 degree for
			// setpoints, 5% for chlorinator - so the target must be reachable by it). If the
			// value isn't parseable yet (page mid-render), wait for the next update.
			auto current = OneTouch::DisplayedValue(DisplayedPage(), row_line);
			if (!current.has_value())
			{
				LogTrace(Channel::Devices, std::format("OneTouch ({}): '{}' value not yet readable - waiting", DeviceId(), row_label));
				break;
			}

			if (current.value() == target)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): '{}' reached target {} - committing", DeviceId(), row_label, target));
				m_ValueEditPhase = ValueEditPhase::Commit;
				break;
			}

			m_KeyCommand_ToSend = (current.value() < target) ? KeyCommands::LineUp : KeyCommands::LineDown;
			LogTrace(Channel::Devices, std::format("OneTouch ({}): Stepping '{}' {} -> {} ({})", DeviceId(), row_label, current.value(), target, magic_enum::enum_name(m_KeyCommand_ToSend)));
			break;
		}

		case ValueEditPhase::Commit:
		{
			// Press Select once to COMMIT the edited value and leave the editor (verified vs
			// hardware: each edit is bracketed Select...Select, NOT Back - Back would navigate
			// away from the page instead of committing the value in place).
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Select to commit '{}'", DeviceId(), row_label));
			m_KeyCommand_ToSend = KeyCommands::Select;
			finish(true);
			break;
		}
		}
	}

	void OneTouchDevice::Boost_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Boost_ProcessStep", std::source_location::current());

		if (!m_PendingBoost.has_value() || !m_Navigator)
		{
			return;
		}

		const bool want_start = m_PendingBoost.value();

		auto finish = [&](bool ok)
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): chlorinator boost {} {}", DeviceId(), want_start ? "start" : "stop", ok ? "completed" : "abandoned"));
			m_Navigator->Reset();
			m_PendingBoost.reset();
			m_BoostInProgress = false;
			m_BoostPhase = BoostPhase::Navigating;
		};

		if (m_BoostInProgress)
		{
			if (++m_BoostStepCount > ONETOUCH_BOOST_STEP_LIMIT)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): boost {} exceeded {} steps - abandoning", DeviceId(), want_start ? "start" : "stop", ONETOUCH_BOOST_STEP_LIMIT));
				finish(false);
				return;
			}
		}

		// The Boost Pool page shows "Time Remaining" while a boost is running and "Operate ...
		// at 100%" when idle - used to decide whether an action is actually needed.
		auto boost_is_running = [this]()
		{
			const auto& page = DisplayedPage();
			for (std::size_t i = 0; i < page.Size(); ++i)
			{
				if (page[i].Text.contains("Time Remaining"))
				{
					return true;
				}
			}
			return false;
		};

		switch (m_BoostPhase)
		{
		case BoostPhase::Navigating:
		{
			// Drive to the Boost Pool page (no in-place Select yet - we decide the action from
			// the page state once there).
			if (!m_BoostInProgress)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to Boost Pool to {} boost", DeviceId(), want_start ? "start" : "stop"));
				m_Navigator->NavigateTo(Navigation::PageId::Boost);
				m_BoostInProgress = true;
				m_BoostStepCount = 0;
			}

			if (auto nav_cmd = m_Navigator->OnPageUpdate(DisplayedPage(), m_HighlightedLine); nav_cmd.has_value())
			{
				m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
			}

			if (m_Navigator->IsComplete())
			{
				if (!m_Navigator->IsSuccess())
				{
					finish(false);
					break;
				}

				const bool running = boost_is_running();
				if (want_start && running)
				{
					LogInfo(Channel::Devices, std::format("OneTouch ({}): boost already running - nothing to do", DeviceId()));
					finish(true);
				}
				else if (!want_start && !running)
				{
					LogInfo(Channel::Devices, std::format("OneTouch ({}): boost already stopped - nothing to do", DeviceId()));
					finish(true);
				}
				else if (want_start)
				{
					// Idle page ("Operate the chlorinator at 100%"): a single Select starts boost
					// (verified vs onetouch_chlorinator.cap).
					LogDebug(Channel::Devices, std::format("OneTouch ({}): Select to start boost", DeviceId()));
					m_KeyCommand_ToSend = KeyCommands::Select;
					m_BoostPhase = BoostPhase::Settle;
				}
				else
				{
					// Running page: navigate to the "Stop" submenu item and Select it in place
					// (verified vs onetouch_chlorinator.cap - user confirmed the pump stopped).
					LogDebug(Channel::Devices, std::format("OneTouch ({}): Navigating to 'Stop' to stop boost", DeviceId()));
					m_Navigator->NavigateToItem(Navigation::PageId::Boost, 0, "Stop", Navigation::PageId::Boost);
					m_BoostPhase = BoostPhase::Acting;
				}
			}
			break;
		}

		case BoostPhase::Acting:
		{
			// Stop path: let the Navigator walk the cursor to the "Stop" item and Select it.
			if (auto nav_cmd = m_Navigator->OnPageUpdate(DisplayedPage(), m_HighlightedLine); nav_cmd.has_value())
			{
				m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
			}
			if (m_Navigator->IsComplete())
			{
				finish(m_Navigator->IsSuccess());
			}
			break;
		}

		case BoostPhase::Settle:
		{
			// Start path: the Select has been queued; the action is one-shot, so we are done.
			finish(true);
			break;
		}
		}
	}

	std::string OneTouchDevice::SanitiseFunctionText(const std::string& raw)
	{
		// Thin forwarder to the pure implementation so existing callers (and the direct unit
		// tests that reference OneTouchDevice::SanitiseFunctionText) keep working while the logic
		// lives in one place - devices/onetouch/onetouch_screen_reader.h.
		return OneTouch::SanitiseFunctionText(raw);
	}

	Capabilities::ActuationResult OneTouchDevice::SetSpaSwitchAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function)
	{
		// Passive/suppressed (cannot transmit) or in a dead-end fault state: refuse honestly. The
		// screen-driven spa-switch service step runs ONLY in NormalOperation, so a goal queued while
		// faulted would be stranded until comms recover - the same gate the other capability methods
		// apply (this path previously checked only emulation, letting a faulted panel accept it).
		if (auto reason = ReasonCannotActuate("program spa-switch assignment"); reason.has_value())
		{
			return reason.value();
		}

		if ((switch_number < 1) || (button_number < 1) || function.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		// One goal at a time on the single shared keypad.
		if (GoalInProgress())
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Busy actuating; rejecting spa-switch assignment {}:{}", DeviceId(), switch_number, button_number));
			return Capabilities::ActuationResult::NotSupported;
		}

		SpaSwitchEditGoal goal;
		goal.switch_number = switch_number;
		goal.button_number = button_number;
		goal.function = function;
		goal.row_tag = std::format("{}:{}", switch_number, button_number);
		goal.desc = std::format("spa-switch {}:{} -> '{}'", switch_number, button_number, function);

		LogInfo(Channel::Devices, std::format("OneTouch ({}): Queued {}", DeviceId(), goal.desc));
		m_PendingSpaSwitchEdit = std::move(goal);
		m_SpaSwitchEditPhase = SpaSwitchEditPhase::ToSystemSetup;
		return Capabilities::ActuationResult::Accepted;
	}

	std::vector<std::string> OneTouchDevice::AvailableFunctions() const
	{
		// The OneTouch picker cycles the shared canonical list (docs/alwin32/spaside-remotes.md).
		return Utility::KnownSpaSwitchFunctions();
	}

	void OneTouchDevice::SpaSwitchEdit_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::SpaSwitchEdit_ProcessStep", std::source_location::current());

		if (!m_PendingSpaSwitchEdit.has_value() || !m_Navigator)
		{
			return;
		}

		const SpaSwitchEditGoal& goal = m_PendingSpaSwitchEdit.value();

		// The picker shows the currently-selected function on line 3 (verified vs
		// spaside_setup_nav.cap: line 3 cycles through the function list as 0x06 is pressed).
		static constexpr uint8_t PICKER_FUNCTION_LINE{ 3 };
		// Bound the per-phase scroll so a missing item (e.g. the menu differs on this model) ends
		// the goal cleanly rather than scrolling forever (the step backstop also covers it).
		static constexpr uint32_t MAX_SCROLL{ 40 };

		auto finish = [&](bool ok)
		{
			if (ok) { LogInfo(Channel::Devices, std::format("OneTouch ({}): {} completed", DeviceId(), goal.desc)); }
			else    { LogWarning(Channel::Devices, std::format("OneTouch ({}): {} abandoned", DeviceId(), goal.desc)); }
			m_Navigator->Reset();
			m_PendingSpaSwitchEdit.reset();
			m_SpaSwitchEditInProgress = false;
			m_SpaSwitchEditPhase = SpaSwitchEditPhase::ToSystemSetup;
			m_PickerFirstSeenFunction.reset();
			m_SpaSwitchCursorStuck = 0;
		};

		if (m_SpaSwitchEditInProgress)
		{
			if (++m_SpaSwitchEditStepCount > ONETOUCH_SPASWITCH_STEP_LIMIT)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): {} exceeded {} steps - abandoning", DeviceId(), goal.desc, ONETOUCH_SPASWITCH_STEP_LIMIT));
				finish(false);
				return;
			}
		}

		const auto& page = DisplayedPage();
		// Row-text read, case-insensitive compare and cursor stepping are shared helpers now:
		// OneTouch::LineText / OneTouch::EqualsCaseInsensitive (onetouch_screen_reader.h) and the
		// MoveCursorToward member.

		switch (m_SpaSwitchEditPhase)
		{
		case SpaSwitchEditPhase::ToSystemSetup:
		{
			// Reuse the proven navigator to reach System Setup, then hand off to the screen-driven
			// walk (the Spa Switch sub-pages -- especially the number-of-switches page -- need bare
			// Selects without cursor moves, which the navigator's edge model does not express).
			if (!m_SpaSwitchEditInProgress)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to System Setup for {}", DeviceId(), goal.desc));
				m_Navigator->NavigateTo(Navigation::PageId::SystemSetup);
				m_SpaSwitchEditInProgress = true;
				m_SpaSwitchEditStepCount = 0;
			}

			if (auto nav_cmd = m_Navigator->OnPageUpdate(page, m_HighlightedLine); nav_cmd.has_value())
			{
				m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
			}

			if (m_Navigator->IsComplete())
			{
				if (m_Navigator->IsSuccess())
				{
					m_Navigator->Reset();   // navigation done; the rest is screen-driven
					m_SpaSwitchCursorStuck = 0;
					m_SpaSwitchEditPhase = SpaSwitchEditPhase::SelectSpaSwitch;
				}
				else
				{
					finish(false);
				}
			}
			break;
		}

		case SpaSwitchEditPhase::SelectSpaSwitch:
		{
			// On the System Setup menu: find the "Spa Switch" item (scrolling if below the fold),
			// move the cursor onto it, then Select to open the Spa Switch number page.
			if (auto line = OneTouch::FindLineStartingWith(page, "Spa Switch"); line.has_value())
			{
				m_SpaSwitchCursorStuck = 0;
				if (MoveCursorToward(line.value()))
				{
					m_KeyCommand_ToSend = KeyCommands::Select;
					m_SpaSwitchEditPhase = SpaSwitchEditPhase::PassNumberPage;
				}
			}
			else
			{
				m_KeyCommand_ToSend = KeyCommands::LineDown;   // scroll the list to reveal it
				if (++m_SpaSwitchCursorStuck > MAX_SCROLL)
				{
					LogWarning(Channel::Devices, std::format("OneTouch ({}): 'Spa Switch' menu item not found", DeviceId()));
					finish(false);
				}
			}
			break;
		}

		case SpaSwitchEditPhase::PassNumberPage:
		{
			// The "Spa Switch / Setup" number-of-switches page (line 1 == "Setup"). Press a BARE
			// Select to advance to the Button Setup list WITHOUT moving the cursor -- moving it
			// would change the configured switch count.
			if (OneTouch::LineText(page,1) == "Setup")
			{
				m_KeyCommand_ToSend = KeyCommands::Select;
				m_SpaSwitchEditPhase = SpaSwitchEditPhase::ToRow;
				m_SpaSwitchCursorStuck = 0;
			}
			break;   // else: still transitioning -- wait for the page
		}

		case SpaSwitchEditPhase::ToRow:
		{
			// The "Button Setup" list (line 1 contains "Button Setup"). Find the "S:B" row, move
			// the cursor onto it, Select to open that button's function picker.
			if (OneTouch::LineText(page,1).contains("Button Setup"))
			{
				if (auto line = OneTouch::FindLineStartingWith(page, goal.row_tag); line.has_value())
				{
					m_SpaSwitchCursorStuck = 0;
					if (MoveCursorToward(line.value()))
					{
						m_KeyCommand_ToSend = KeyCommands::Select;
						m_PickerFirstSeenFunction.reset();
						m_SpaSwitchEditPhase = SpaSwitchEditPhase::CyclePicker;
					}
				}
				else
				{
					m_KeyCommand_ToSend = KeyCommands::LineDown;   // scroll to reveal the row
					if (++m_SpaSwitchCursorStuck > MAX_SCROLL)
					{
						LogWarning(Channel::Devices, std::format("OneTouch ({}): button row '{}' not found", DeviceId(), goal.row_tag));
						finish(false);
					}
				}
			}
			break;   // else: still transitioning -- wait
		}

		case SpaSwitchEditPhase::CyclePicker:
		{
			// The per-button picker (line 1 == "Button <S:B>"). Cycle (LineUp) until the selected
			// function (line 3) matches the target, then commit. Wrap-detect to bail if the target
			// is not offered by this controller.
			if (OneTouch::LineText(page,1).contains("Button") && OneTouch::LineText(page,1).contains(goal.row_tag))
			{
				auto current = OneTouch::DisplayedFunctionOnRow(page, PICKER_FUNCTION_LINE);
				if (!current.has_value())
				{
					break;   // not rendered yet -- wait
				}

				if (OneTouch::EqualsCaseInsensitive(current.value(), goal.function))
				{
					m_SpaSwitchEditPhase = SpaSwitchEditPhase::Commit;
					break;
				}

				if (!m_PickerFirstSeenFunction.has_value())
				{
					m_PickerFirstSeenFunction = current;
				}
				else if (OneTouch::EqualsCaseInsensitive(current.value(), m_PickerFirstSeenFunction.value()))
				{
					LogWarning(Channel::Devices, std::format("OneTouch ({}): function '{}' not offered by the picker for {}", DeviceId(), goal.function, goal.row_tag));
					finish(false);
					break;
				}

				m_KeyCommand_ToSend = KeyCommands::LineUp;   // cycle to the next function
			}
			break;   // else: not on the picker yet -- wait
		}

		case SpaSwitchEditPhase::Commit:
		{
			// Select writes the chosen function and leaves the picker (back to the Button Setup
			// list, which then shows "S:B  <function>").
			m_KeyCommand_ToSend = KeyCommands::Select;
			finish(true);
			break;
		}
		}
	}

	//=========================================================================
	// Controller-schedule WRITE (ControllerScheduleWriter). Drives the emulated
	// keypad through the Program menu to create / edit / delete one of the
	// controller's own internal Program timers. RE'd from
	// captures/onetouch_program.cap; see docs/onetouch_schedule_protocol.md.
	//=========================================================================

	std::optional<std::pair<int, int>> OneTouchDevice::DisplayedTime(uint8_t line_id) const
	{
		const auto& page = DisplayedPage();
		if (line_id >= page.Size())
		{
			return std::nullopt;
		}

		// The read-path parser already knows how to turn an "ON  11:00 AM" / "OFF  2:00 PM" row into a
		// 24-hour (hour, minute). Reuse it by handing the line to ParseProgramDetailLines shaped as a
		// minimal detail page (target on 0, ON on 3, OFF on 4, All Days on 5) and reading back the
		// field we asked for. This keeps ONE 12h->24h decode (no duplicate meridiem logic here).
		std::vector<std::string> lines(6, std::string{});
		lines[0] = "X";              // non-empty target so the parse is not rejected
		lines[3] = "ON  1:00 AM";    // placeholder for the row we are NOT reading
		lines[4] = "OFF 1:00 AM";
		lines[5] = "All Days";
		const uint8_t slot = (line_id == ONETOUCH_SCHEDULE_OFF_LINE) ? 4 : 3;
		lines[slot] = page[line_id].Text;

		const auto parsed = OneTouch::ParseProgramDetailLines(lines);
		if (!parsed.has_value())
		{
			return std::nullopt;
		}
		return (slot == 4)
			? std::pair<int, int>{ parsed->off_hour, parsed->off_minute }
			: std::pair<int, int>{ parsed->on_hour, parsed->on_minute };
	}

	std::optional<uint8_t> OneTouchDevice::DisplayedDays(uint8_t line_id) const
	{
		const auto& page = DisplayedPage();
		if (line_id >= page.Size())
		{
			return std::nullopt;
		}
		return OneTouch::ParseDaysRow(page[line_id].Text);
	}

	Capabilities::ActuationResult OneTouchDevice::QueueScheduleWrite(ScheduleWriteGoal goal)
	{
		// Passive/suppressed or in a dead-end fault state: refuse honestly (a queued goal would be
		// stranded) so the dispatcher can fall back to another writer.
		if (auto reason = ReasonCannotActuate(goal.desc); reason.has_value())
		{
			return reason.value();
		}

		// One goal at a time on the single shared keypad.
		if (GoalInProgress())
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): Busy actuating; rejecting {}", DeviceId(), goal.desc));
			return Capabilities::ActuationResult::NotSupported;
		}

		LogInfo(Channel::Devices, std::format("OneTouch ({}): Queued {}", DeviceId(), goal.desc));
		m_PendingScheduleWrite = std::move(goal);
		m_ScheduleWritePhase = ScheduleWritePhase::ToProgramMenu;
		m_ScheduleWriteInProgress = false;
		m_ScheduleWriteStepCount = 0;
		m_ScheduleWriteFieldStep = 0;
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult OneTouchDevice::CreateControllerProgram(const Scheduling::ControllerSchedule& program)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::CreateControllerProgram", std::source_location::current());

		// The controller can only represent a constrained subset (single equipment on/off span on an
		// all/weekdays/weekends/single-day selection) - reject anything it can't BEFORE the panel walk.
		if (!IsEmulationActive())
		{
			LogDebug(Channel::Devices, std::format("OneTouch ({}): CreateControllerProgram rejected -- device is passive", DeviceId()));
			return Capabilities::ActuationResult::NotSupported;
		}
		if (const auto feasibility = Scheduling::CheckControllerCandidate(program); !feasibility.promotable)
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): CreateControllerProgram rejected -- not controller-representable (target='{}')", DeviceId(), program.target));
			return Capabilities::ActuationResult::InvalidValue;
		}

		ScheduleWriteGoal goal;
		goal.op = ScheduleWriteOp::Create;
		goal.program = program;
		goal.desc = std::format("create controller program '{}'", program.target);
		return QueueScheduleWrite(std::move(goal));
	}

	Capabilities::ActuationResult OneTouchDevice::DeleteControllerProgram(const Scheduling::ControllerSchedule& program)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::DeleteControllerProgram", std::source_location::current());

		if (!IsEmulationActive())
		{
			LogDebug(Channel::Devices, std::format("OneTouch ({}): DeleteControllerProgram rejected -- device is passive", DeviceId()));
			return Capabilities::ActuationResult::NotSupported;
		}
		if (program.target.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		ScheduleWriteGoal goal;
		goal.op = ScheduleWriteOp::Delete;
		goal.program = program;
		goal.match = program;   // SelectEquipment locates the equipment by target
		goal.desc = std::format("delete controller program '{}'", program.target);
		return QueueScheduleWrite(std::move(goal));
	}

	Capabilities::ActuationResult OneTouchDevice::EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::EditControllerProgram", std::source_location::current());

		if (!IsEmulationActive())
		{
			LogDebug(Channel::Devices, std::format("OneTouch ({}): EditControllerProgram rejected -- device is passive", DeviceId()));
			return Capabilities::ActuationResult::NotSupported;
		}
		if (existing.target.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}
		// The desired program must be one the controller can represent -- same gate as create.
		if (const auto feasibility = Scheduling::CheckControllerCandidate(desired); !feasibility.promotable)
		{
			LogWarning(Channel::Devices, std::format("OneTouch ({}): EditControllerProgram rejected -- desired not controller-representable (target='{}')", DeviceId(), desired.target));
			return Capabilities::ActuationResult::InvalidValue;
		}

		ScheduleWriteGoal goal;
		goal.op = ScheduleWriteOp::Edit;
		goal.program = desired;    // the field phases set from goal.program; Verify matches it
		goal.match = existing;     // SelectEquipment locates the equipment by target
		goal.desc = std::format("edit controller program '{}'", existing.target);
		return QueueScheduleWrite(std::move(goal));
	}

	void OneTouchDevice::ControllerScheduleWrite_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::ControllerScheduleWrite_ProcessStep", std::source_location::current());

		if (!m_PendingScheduleWrite.has_value() || !m_Navigator)
		{
			return;
		}
		const ScheduleWriteGoal& goal = m_PendingScheduleWrite.value();

		auto finish = [&](bool ok)
		{
			if (ok) { LogInfo(Channel::Devices, std::format("OneTouch ({}): {} completed", DeviceId(), goal.desc)); }
			else    { LogWarning(Channel::Devices, std::format("OneTouch ({}): {} abandoned", DeviceId(), goal.desc)); }
			m_Navigator->Reset();
			m_PendingScheduleWrite.reset();
			m_ScheduleWritePhase = ScheduleWritePhase::ToProgramMenu;
			m_ScheduleWriteInProgress = false;
			m_ScheduleWriteFieldStep = 0;
		};

		// Frame backstop so a mis-detected page can never wedge NormalOperation.
		if (m_ScheduleWriteInProgress)
		{
			if (++m_ScheduleWriteStepCount > ONETOUCH_SCHEDULE_STEP_LIMIT)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): {} exceeded {} steps - abandoning", DeviceId(), goal.desc, ONETOUCH_SCHEDULE_STEP_LIMIT));
				finish(false);
				return;
			}
		}

		const auto& page = DisplayedPage();
		// Row-text read, case-insensitive compare and cursor stepping are shared helpers now:
		// OneTouch::LineText / OneTouch::EqualsCaseInsensitive (onetouch_screen_reader.h) and the
		// MoveCursorToward member.
		// True when the current page is the per-equipment Program detail page rather than the Program
		// equipment LIST. The LIST's line 0 is the "Program" title; the detail page's line 0 is the
		// equipment name and it carries either "Pgm N of M" (line 2) or "No Programs" (line 4).
		auto on_detail_page = [&]()
		{
			if (OneTouch::LineText(page,0).contains("Program")) { return false; }   // the LIST title
			return OneTouch::LineText(page,2).contains("Pgm ")
				|| OneTouch::LineText(page,4).contains("No Programs")
				|| OneTouch::LineText(page,ONETOUCH_SCHEDULE_CHANGE_ROW).contains("Change");
		};
		// True when the current page is the Add/Change editor (title line 1 + the arrow-keys prompt).
		auto on_editor_page = [&]()
		{
			const std::string title = OneTouch::LineText(page,ONETOUCH_SCHEDULE_TITLE_LINE);
			return title.contains("New Program") || title.contains("Change Program");
		};

		// Step a 12h+meridiem hour wheel (24 positions) toward `target_hour` closed-loop: read the
		// echoed value, emit ONE key the shorter way round, Select once matched. Advances to `next`.
		auto step_hour = [&](uint8_t line, int target_hour, int target_minute, ScheduleWritePhase next)
		{
			if (!on_editor_page()) { return; }   // page mid-transition; wait
			auto cur = DisplayedTime(line);
			if (!cur.has_value()) { return; }    // value blanked mid-render; wait
			if (cur->first == target_hour)
			{
				m_KeyCommand_ToSend = KeyCommands::Select;   // commit the hour, advance to the minute
				m_ScheduleWritePhase = next;
				m_ScheduleWriteFieldStep = 0;
				(void)target_minute;
				return;
			}
			if (++m_ScheduleWriteFieldStep > ONETOUCH_SCHEDULE_MAX_STEP) { finish(false); return; }
			const int forward = ((target_hour - cur->first) + 24) % 24;   // steps if we go LineUp (+1/step)
			m_KeyCommand_ToSend = (forward <= 12) ? KeyCommands::LineUp : KeyCommands::LineDown;
		};
		// Step a 0-59 minute wheel toward `target_minute` closed-loop, then Select to advance.
		auto step_minute = [&](uint8_t line, int target_minute, ScheduleWritePhase next)
		{
			if (!on_editor_page()) { return; }
			auto cur = DisplayedTime(line);
			if (!cur.has_value()) { return; }
			if (cur->second == target_minute)
			{
				m_KeyCommand_ToSend = KeyCommands::Select;
				m_ScheduleWritePhase = next;
				m_ScheduleWriteFieldStep = 0;
				return;
			}
			if (++m_ScheduleWriteFieldStep > ONETOUCH_SCHEDULE_MAX_STEP) { finish(false); return; }
			const int forward = ((target_minute - cur->second) + 60) % 60;
			m_KeyCommand_ToSend = (forward <= 30) ? KeyCommands::LineUp : KeyCommands::LineDown;
		};

		switch (m_ScheduleWritePhase)
		{
		case ScheduleWritePhase::ToProgramMenu:
		{
			// Reuse the proven navigator to reach the Program equipment-list page (Menu/Help ->
			// Program), then hand off to the screen-driven walk (the list scroll + detail + editor
			// need bare content-driven cursoring the navigator's edge model does not express).
			if (!m_ScheduleWriteInProgress)
			{
				LogInfo(Channel::Devices, std::format("OneTouch ({}): Navigating to Program menu for {}", DeviceId(), goal.desc));
				m_Navigator->NavigateTo(Navigation::PageId::Program);
				m_ScheduleWriteInProgress = true;
				m_ScheduleWriteStepCount = 0;
			}

			if (auto nav_cmd = m_Navigator->OnPageUpdate(page, m_HighlightedLine); nav_cmd.has_value())
			{
				m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
			}

			if (m_Navigator->IsComplete())
			{
				if (m_Navigator->IsSuccess())
				{
					m_Navigator->Reset();   // navigation done; the rest is screen-driven
					m_ScheduleWriteFieldStep = 0;
					m_ScheduleWritePhase = ScheduleWritePhase::SelectEquipment;
				}
				else
				{
					finish(false);
				}
			}
			break;
		}

		case ScheduleWritePhase::SelectEquipment:
		{
			// On the Program equipment LIST (line 0 == "Program"): find the target equipment row
			// (scrolling if below the fold), move the cursor onto it, Select -> its detail page.
			// Guard against acting once the detail page has already rendered (a fast transition).
			if (on_detail_page())
			{
				m_ScheduleWriteFieldStep = 0;
				m_ScheduleWritePhase = ScheduleWritePhase::ChooseAction;
				return;
			}
			if (auto line = OneTouch::FindLineStartingWith(page, goal.program.target); line.has_value() && line.value() != 0)
			{
				m_ScheduleWriteFieldStep = 0;
				if (MoveCursorToward(line.value()))
				{
					m_KeyCommand_ToSend = KeyCommands::Select;
					m_ScheduleWritePhase = ScheduleWritePhase::ChooseAction;
				}
			}
			else
			{
				m_KeyCommand_ToSend = KeyCommands::LineDown;   // scroll the list to reveal the equipment
				if (++m_ScheduleWriteFieldStep > ONETOUCH_SCHEDULE_MAX_STEP)
				{
					LogWarning(Channel::Devices, std::format("OneTouch ({}): equipment '{}' not found in the Program list", DeviceId(), goal.program.target));
					finish(false);
				}
			}
			break;
		}

		case ScheduleWritePhase::ChooseAction:
		{
			// On the per-equipment detail page. Delete: cursor to the Delete row (10) and Select ->
			// immediate removal (NO confirm dialog). Create: cursor to Add (9); Edit: cursor to Change
			// (11) -> the editor. An equipment with no program shows only Add, so Edit/Delete of a
			// missing program simply can't find its row and the step backstop ends the goal cleanly.
			if (!on_detail_page()) { break; }   // still transitioning -- wait

			if (goal.op == ScheduleWriteOp::Delete)
			{
				// "No Programs" already? Nothing to delete -- treat as done.
				if (OneTouch::LineText(page,4).contains("No Programs"))
				{
					finish(true);
					break;
				}
				if (MoveCursorToward(ONETOUCH_SCHEDULE_DELETE_ROW))
				{
					m_KeyCommand_ToSend = KeyCommands::Select;   // immediate delete, no confirm
					m_ScheduleWritePhase = ScheduleWritePhase::VerifyGone;
				}
				break;
			}

			if (const uint8_t action_row = (goal.op == ScheduleWriteOp::Edit) ? ONETOUCH_SCHEDULE_CHANGE_ROW : ONETOUCH_SCHEDULE_ADD_ROW; MoveCursorToward(action_row))
			{
				m_KeyCommand_ToSend = KeyCommands::Select;   // -> the editor
				m_ScheduleWriteFieldStep = 0;
				m_ScheduleWritePhase = ScheduleWritePhase::EnterEditor;
			}
			break;
		}

		case ScheduleWritePhase::EnterEditor:
		{
			// Wait for the Add/Change editor to render, then begin field entry at ON-hour. The panel
			// reports NO field highlight in the editor, so the active field is tracked purely by the
			// phase progression (each field's Select advances the phase).
			if (on_editor_page())
			{
				m_ScheduleWriteFieldStep = 0;
				m_ScheduleWritePhase = ScheduleWritePhase::SetOnHour;
			}
			break;
		}

		case ScheduleWritePhase::SetOnHour:
			step_hour(ONETOUCH_SCHEDULE_ON_LINE, goal.program.on_hour, goal.program.on_minute, ScheduleWritePhase::SetOnMinute);
			break;

		case ScheduleWritePhase::SetOnMinute:
			step_minute(ONETOUCH_SCHEDULE_ON_LINE, goal.program.on_minute, ScheduleWritePhase::SetOffHour);
			break;

		case ScheduleWritePhase::SetOffHour:
			step_hour(ONETOUCH_SCHEDULE_OFF_LINE, goal.program.off_hour, goal.program.off_minute, ScheduleWritePhase::SetOffMinute);
			break;

		case ScheduleWritePhase::SetOffMinute:
			step_minute(ONETOUCH_SCHEDULE_OFF_LINE, goal.program.off_minute, ScheduleWritePhase::SetDays);
			break;

		case ScheduleWritePhase::SetDays:
		{
			// Step the days wheel to the target selection, then Select -> the program SAVES and the
			// panel returns to the detail page (there is no separate save opcode). Closed-loop on the
			// echoed days row; the wheel is the controller-allowed set only, so a validated candidate
			// (guaranteed by CheckControllerCandidate) is always reachable.
			if (!on_editor_page()) { break; }
			auto cur = DisplayedDays(ONETOUCH_SCHEDULE_DAYS_LINE);
			if (!cur.has_value()) { break; }   // days row blanked mid-render; wait
			if (cur.value() == (goal.program.days_of_week & OneTouch::DayMask::AllDays))
			{
				m_KeyCommand_ToSend = KeyCommands::Select;   // commit days -> SAVE -> detail page
				m_ScheduleWritePhase = ScheduleWritePhase::Verify;   // Create/Edit only reach SetDays
				m_ScheduleWriteFieldStep = 0;
				break;
			}
			if (++m_ScheduleWriteFieldStep > ONETOUCH_SCHEDULE_MAX_STEP) { finish(false); break; }
			m_KeyCommand_ToSend = KeyCommands::LineUp;   // cycle the days wheel (bounded)
			break;
		}

		case ScheduleWritePhase::Verify:
		{
			// The program saved and the panel returned to the detail page. Re-parse it and confirm it
			// now carries the target program (target + on/off + days). Dwell until it renders.
			if (!on_detail_page()) { break; }
			int idx = 0;
			int count = 0;
			if (const auto parsed = OneTouch::ParseProgramDetailPage(page, &idx, &count);
				parsed.has_value()
				&& OneTouch::EqualsCaseInsensitive(parsed->target, goal.program.target)
				&& parsed->days_of_week == (goal.program.days_of_week & OneTouch::DayMask::AllDays)
				&& parsed->on_hour == goal.program.on_hour && parsed->on_minute == goal.program.on_minute
				&& parsed->off_hour == goal.program.off_hour && parsed->off_minute == goal.program.off_minute)
			{
				finish(true);
			}
			break;
		}

		case ScheduleWritePhase::VerifyGone:
		{
			// Delete complete once the detail page shows "No Programs" (or no longer parses as a
			// program-detail with the deleted program). Dwell until the panel re-renders.
			if (!on_detail_page()) { break; }
			if (OneTouch::LineText(page,4).contains("No Programs"))
			{
				finish(true);
				break;
			}
			if (const auto parsed = OneTouch::ParseProgramDetailPage(page); !parsed.has_value())
			{
				finish(true);
			}
			break;
		}
		}
	}

	void OneTouchDevice::Scraping_ProcessStep_StartUp()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Scraping_ProcessStep_StartUp", std::source_location::current());

		LogDebug(Channel::Devices, std::format("OneTouch ({}): Processing StartUp scraping step", DeviceId()));

		Status(Devices::DeviceStatus_Initializing{});

		// Start the SpiderEngine crawl if not already active
		if (m_SpiderEngine->GetState() == Navigation::SpiderEngine::State::Idle)
		{
			// If a real iAqualink2 has already seeded aux labels onto the DataHub,
			// skip the slow "Label Aux" crawl (~36 pages under a 30s watchdog) and
			// reuse those labels. Otherwise fall back to the full scrape so non-IAQ
			// systems still discover their aux labels.
			const bool skip_label_pages = DataHubHasSeededAuxLabels();
			if (skip_label_pages)
			{
				LogInfo(Channel::Scraping, std::format("OneTouch ({}): IAQ-seeded aux labels present on DataHub - skipping Label Aux scrape", DeviceId()));
			}
			else
			{
				LogDebug(Channel::Scraping, std::format("OneTouch ({}): No seeded aux labels found - performing full Label Aux scrape", DeviceId()));
			}

			// Use FullDiscoveryVisitPolicy for startup - visits all navigable pages
			auto policy = std::make_unique<Navigation::FullDiscoveryVisitPolicy>(
				[this](Navigation::PageId page, const Utility::ScreenDataPage& content)
				{
					LogDebug(Channel::Scraping, std::format("OneTouch ({}): SpiderEngine visited page {}",
						DeviceId(), static_cast<uint32_t>(page)));
				},
				[this]()
				{
					LogInfo(Channel::Scraping, std::format("OneTouch ({}): SpiderEngine startup crawl complete", DeviceId()));
				},
				skip_label_pages
			);
			m_SpiderEngine->StartCrawl(std::move(policy));
		}

		// Transition to Scraping state - the SpiderEngine handles sync internally
		m_OpState = OperatingStates::Scraping;
		m_ScrapingStallCounter = 0;

		// Process the first step immediately
		Scraping_ProcessStep();
	}

	void OneTouchDevice::Scraping_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("OneTouchDevice::Scraping_ProcessStep", std::source_location::current());

		if (!m_SpiderEngine || !m_Navigator)
		{
			LogWarning(Channel::Scraping, std::format("OneTouch ({}): No active spider engine or navigator", DeviceId()));
			m_OpState = OperatingStates::NormalOperation;
			Status(Devices::DeviceStatus_Normal{});
			return;
		}

		// Delegate to SpiderEngine
		auto nav_cmd = m_SpiderEngine->ProcessStep(DisplayedPage(), m_HighlightedLine);

		// Check engine state
		if (m_SpiderEngine->GetState() == Navigation::SpiderEngine::State::Complete)
		{
			LogInfo(Channel::Scraping, std::format("OneTouch ({}): Startup scrape complete ({} pages visited) - entering NormalOperation",
				DeviceId(), m_SpiderEngine->GetVisitedPages().size()));
			ReportMenuSurvey();
			ValidateDiscoveredEquipment();

			// The startup crawl already visited Set AquaPure (its page processor scraped the
			// configured %), so mark startup complete and seed the refresh timer from now - the
			// first periodic re-scrape is then a full interval away rather than immediate. If the
			// crawl could not reach Set AquaPure, there is no chlorinator on this panel, so disable
			// periodic refresh entirely (it would only ever fail).
			if (m_SpiderEngine->GetFailedPages().contains(Navigation::PageId::SetAquapure))
			{
				m_RefreshState.Disable();
			}
			m_RefreshState.MarkStartupComplete();
			m_RefreshState.NotifyScrapeFinished(std::chrono::steady_clock::now());

			m_Navigator->Reset();
			m_OpState = OperatingStates::NormalOperation;
			Status(Devices::DeviceStatus_Normal{});
			return;
		}

		if (m_SpiderEngine->GetState() == Navigation::SpiderEngine::State::Failed)
		{
			LogError(Channel::Scraping, std::format("OneTouch ({}): SpiderEngine failed - entering ScrapingFaulted", DeviceId()));
			m_OpState = OperatingStates::ScrapingFaulted;
			Status(Devices::DeviceStatus_FaultOccurred{});
			return;
		}

		// If we got a command, convert and queue it
		if (nav_cmd.has_value())
		{
			m_KeyCommand_ToSend = ConvertNavKeyCommand(nav_cmd.value());
			m_ScrapingStallCounter = 0;
			LogDebug(Channel::Scraping, std::format("OneTouch ({}): Sending navigation command: {}",
				DeviceId(), magic_enum::enum_name(m_KeyCommand_ToSend)));
		}
		else
		{
			// No command - check if we're on a transient page (no edges = controller auto-transitions)
			auto detected = m_MenuModel.DetectPage(DisplayedPage());
			const auto* page_info = (detected != Navigation::PageId::Unknown)
				? m_MenuModel.GetPage(detected) : nullptr;

			if (page_info && page_info->edges.empty())
			{
				// On a transient page - don't count as stall, controller will auto-transition
				LogDebug(Channel::Scraping, std::format("OneTouch ({}): On transient page '{}' - waiting for controller to transition",
					DeviceId(), page_info->name));
				m_ScrapingStallCounter = 0;
			}
			else if (m_Navigator && (m_Navigator->GetState() == Navigation::Navigator::State::WaitingForPage
				|| m_Navigator->GetState() == Navigation::Navigator::State::MovingCursor))
			{
				// Navigator is actively waiting for a Status message to decrement the
				// pending counter — non-Status messages (MessageLong, Highlight, etc.)
				// trigger ProcessStep but that's not a stall.
				m_ScrapingStallCounter = 0;
			}
			else
			{
				m_ScrapingStallCounter++;

				if (m_ScrapingStallCounter >= ONETOUCH_SCRAPING_STALL_LIMIT)
				{
					LogWarning(Channel::Scraping, std::format("OneTouch ({}): Scraping stalled for {} iterations",
						DeviceId(), m_ScrapingStallCounter));
					// Reset stall counter and let the spider engine handle recovery
					m_ScrapingStallCounter = 0;
				}
			}
		}
	}

	void OneTouchDevice::ValidateDiscoveredEquipment()
	{
		if (!m_DataHub)
		{
			return;
		}

		// Gather the Jandy ids of every numbered auxillary that was discovered.
		std::vector<Auxillaries::JandyAuxillaryIds> discovered_aux_ids;
		for (const auto& aux : m_DataHub->Auxillaries())
		{
			if (aux && aux->AuxillaryTraits.Has(Auxillaries::JandyAuxillaryId{}))
			{
				discovered_aux_ids.push_back(aux->AuxillaryTraits[Auxillaries::JandyAuxillaryId{}]);
			}
		}

		// Equipment occupying an aux relay that is NOT a numbered aux because an IO-board DIP
		// switch repurposed the relay (cleaner / spillover / sprinkler). Counted toward the
		// relay total so a DIP-repurposed panel still validates against the model's aux count.
		const auto reconfigured_aux_relays = static_cast<uint8_t>(
			m_DataHub->CountOfType(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Cleaner)
			+ m_DataHub->CountOfType(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Spillover)
			+ m_DataHub->CountOfType(Kernel::AuxillaryTraitsTypes::AuxillaryTypes::Sprinkler));

		auto result = Utility::ValidateDiscoveredEquipment(
			m_DataHub->ExpectedAuxillaryCount,
			m_DataHub->ExpectedPowerCenterCount,
			discovered_aux_ids,
			reconfigured_aux_relays);

		if (result.ExpectedAuxillaries == 0)
		{
			// The version page was never scraped (no model decoded) - nothing to validate against.
			LogDebug(Channel::Devices, std::format("OneTouch ({}): Skipping equipment validation - model not yet decoded", DeviceId()));
		}
		else if (result.Passed())
		{
			LogInfo(Channel::Devices, std::format("OneTouch ({}): Equipment validated - {} aux relay(s) across {} power center(s) match the model",
				DeviceId(), result.DiscoveredAuxillaries, result.DiscoveredPowerCenters));
		}
		else
		{
			for (const auto& anomaly : result.Anomalies)
			{
				LogWarning(Channel::Devices, std::format("OneTouch ({}): Equipment validation anomaly - {}", DeviceId(), anomaly));
			}
		}

		m_DataHub->EquipmentValidationResult = std::move(result);
	}

	void OneTouchDevice::ReportMenuSurvey()
	{
		if (!m_SpiderEngine)
		{
			return;
		}

		const auto& visited = m_SpiderEngine->GetVisitedPages();
		const auto& failed = m_SpiderEngine->GetFailedPages();

		MenuSurveyResult survey;
		survey.PagesReached = static_cast<uint32_t>(visited.size() - failed.size());
		survey.EquipmentPageReached = visited.contains(Navigation::PageId::EquipmentOnOff)
			&& !failed.contains(Navigation::PageId::EquipmentOnOff);

		for (const auto page : failed)
		{
			const auto* page_info = m_MenuModel.GetPage(page);
			const std::string name = page_info ? page_info->name : std::format("page {}", std::to_underlying(page));

			if (auto requirement = Navigation::OneTouchPageCapabilityRequirement(page); requirement.has_value())
			{
				survey.ExpectedAbsent.push_back(std::format("{} ({})", name, requirement.value()));
			}
			else
			{
				survey.NotableFailures.push_back(name);
			}
		}

		LogInfo(Channel::Scraping, std::format("OneTouch ({}): Menu survey - {} page(s) reached, {} expected-absent, {} notable failure(s)",
			DeviceId(), survey.PagesReached, survey.ExpectedAbsent.size(), survey.NotableFailures.size()));

		if (!survey.EquipmentPageReached)
		{
			LogWarning(Channel::Scraping, std::format("OneTouch ({}): Menu survey - the Equipment ON/OFF page was not reached; the discovered equipment set may be incomplete", DeviceId()));
		}

		for (const auto& notable : survey.NotableFailures)
		{
			LogWarning(Channel::Scraping, std::format("OneTouch ({}): Menu survey - unexpected failure to reach '{}'", DeviceId(), notable));
		}

		for (const auto& expected : survey.ExpectedAbsent)
		{
			LogDebug(Channel::Scraping, std::format("OneTouch ({}): Menu survey - expected-absent page skipped: {}", DeviceId(), expected));
		}

		m_MenuSurveyResult = std::move(survey);
	}

	nlohmann::json OneTouchDevice::DescribeDiagnostics() const
	{
		nlohmann::json j;

		j["device_type"] = "OneTouch";
		j["device_id"] = std::format("0x{:02x}", DeviceId().Id()());
		j["operating_state"] = std::string(magic_enum::enum_name(m_OpState));

		j["screen"] = DescribeScreen();

		// Navigator state
		if (m_Navigator)
		{
			nlohmann::json nav;
			nav["state"] = std::string(magic_enum::enum_name(m_Navigator->GetState()));
			nav["current_page"] = std::string(magic_enum::enum_name(m_Navigator->GetCurrentPage()));
			nav["target_page"] = std::string(magic_enum::enum_name(m_Navigator->GetTargetPage()));
			nav["cursor_line"] = m_Navigator->GetCursorLine();
			nav["synced"] = m_Navigator->IsSynced();
			j["navigator"] = nav;
		}

		// Spider engine state
		if (m_SpiderEngine)
		{
			nlohmann::json spider;
			spider["state"] = std::string(magic_enum::enum_name(m_SpiderEngine->GetState()));
			spider["visited_count"] = static_cast<uint32_t>(m_SpiderEngine->GetVisitedPages().size());
			spider["current_target"] = std::string(magic_enum::enum_name(m_SpiderEngine->GetCurrentTarget()));
			spider["label_scrape_skipped"] = DataHubHasSeededAuxLabels();
			j["spider_engine"] = spider;
		}

		// Menu survey health (populated once the startup crawl completes): which pages were
		// reached and which could not be, split into expected-absent (capability-gated, e.g.
		// the iAqualink / chlorinator pages) vs notable failures every panel should have.
		if (m_MenuSurveyResult.has_value())
		{
			const auto& survey = m_MenuSurveyResult.value();
			nlohmann::json menu_survey;
			menu_survey["pages_reached"] = survey.PagesReached;
			menu_survey["equipment_page_reached"] = survey.EquipmentPageReached;
			menu_survey["expected_absent"] = survey.ExpectedAbsent;
			menu_survey["notable_failures"] = survey.NotableFailures;
			j["menu_survey"] = std::move(menu_survey);
		}

		j["scraping_stall_counter"] = m_ScrapingStallCounter;
		j["highlighted_line"] = m_HighlightedLine;
		j["pending_key_command"] = std::string(magic_enum::enum_name(m_KeyCommand_ToSend));
		j["ack_type"] = std::string(magic_enum::enum_name(m_AckType_ToSend));
		j["is_emulated"] = IsEmulated();
		j["emulation_suppressed"] = IsEmulationSuppressed();
		j["recent_commands"] = DescribeRecentCommands();
		j["is_running"] = IsRunning();

		return j;
	}

}
// namespace AqualinkAutomate::Devices
