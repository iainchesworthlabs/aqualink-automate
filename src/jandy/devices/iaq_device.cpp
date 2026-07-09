#include <bit>
#include <cctype>
#include <format>
#include <functional>

#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>

#include "logging/logging.h"
#include "devices/device_status.h"
#include "devices/iaq_device.h"
#include "formatters/jandy_device_formatters.h"
#include "kernel/auxillary_devices/auxillary_device.h"
#include "kernel/auxillary_traits/auxillary_traits_types.h"
#include "messages/iaq/iaq_message_control_data_response.h"
#include "scheduling/promotion_constraints.h"
#include "utility/spa_switch_assignment.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;
using namespace AqualinkAutomate::Profiling;

namespace AqualinkAutomate::Devices
{

	namespace
	{
		// IAQ (AqualinkTouch 0x33) UI navigation / chlorinator command bytes.  These ride
		// the 0x33 ACK channel as documented in iaq_protocol.md; named here so the queue
		// builders below are self-describing rather than a string of bare hex literals.
		constexpr uint8_t IAQ_CMD_BACK{ 0x02 };                 // navigate back / clean state
		constexpr uint8_t IAQ_CMD_OPEN_AQUAPURE_PAGE{ 0x19 };   // open the AquaPure settings page
		constexpr uint8_t IAQ_CMD_SELECT_POOL{ 0x11 };          // select Pool (button index 0)
		constexpr uint8_t IAQ_CMD_PAGE_BUTTON_BASE{ 0x11 };     // press on-screen PageButton index N -> 0x11 + N
		constexpr uint8_t IAQ_CMD_QUICK_BOOST{ 0x13 };          // Quick Boost (button index 2) / Stop
		constexpr uint8_t IAQ_CMD_BOOST_START{ 0x12 };          // Start boost (button index 1)
		constexpr uint8_t IAQ_CMD_SUBMIT_VALUE{ 0x80 };         // submit the entered value

		// Set Temperature page (verified vs iaq_aux_setpoint.cap): from a clean state, 0x14
		// opens the Set Temperature page; on it Pool Heat is button index 0 (0x11) and Spa
		// Heat is button index 2 (0x13); the absolute value is submitted as control-data.
		constexpr uint8_t IAQ_CMD_OPEN_SETTEMP_PAGE{ 0x14 };    // open the Set Temperature page (button index 3)
		constexpr uint8_t IAQ_CMD_SELECT_POOL_HEAT{ 0x11 };     // Set Temp page: Pool Heat (button index 0)
		constexpr uint8_t IAQ_CMD_SELECT_SPA_HEAT{ 0x13 };      // Set Temp page: Spa Heat (button index 2)

		constexpr char IAQ_BUTTON_INDEX_POOL{ '1' };            // ASCII value-field prefix for control-data values ("1" + value)

		// (The spa-switch and controller-schedule navigation constants + DayCommandFor /
		// ScheduleTimeValue helpers moved with their write state machines to
		// iaq_spaswitch_writer.cpp and iaq_schedule_writer.cpp respectively.)
	}
	// namespace

	IAQDevice::IAQDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated) :
		JandyController(device_id, hub_locator),
		Capabilities::Restartable(IAQ_TIMEOUT_DURATION),
		Capabilities::Screen(IAQ_STATUS_PAGE_LINES),
		Capabilities::Emulated(is_emulated),
		m_SM_PageUpdate(m_StatusPage),
		m_SM_TableUpdate(m_TableInfo),
		m_ProfilingDomain(std::move(Factory::ProfilingUnitFactory::Instance().CreateDomain("IAQDevice")))
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::IAQDevice", std::source_location::current());

		LogInfo(Channel::Devices, std::format("Creating IAQDevice: device_id={}, emulated={}, timeout={}s", *device_id, is_emulated, IAQ_TIMEOUT_DURATION.count()));

		m_ProfilingDomain->Start();

		// Read-only sink for the controller's internal schedules parsed off the
		// Schedule list page. Absent on a passive/test rig that registers no store.
		m_ControllerScheduleStore = hub_locator.TryFind<Scheduling::ControllerScheduleStore>();

		m_SM_PageUpdate.initiate();
		m_SM_TableUpdate.initiate();

		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_AuxStatus>(std::bind(&IAQDevice::Slot_IAQ_AuxStatus, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_CommandReady>(std::bind(&IAQDevice::Slot_IAQ_CommandReady, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_ControlReady>(std::bind(&IAQDevice::Slot_IAQ_ControlReady, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_Heartbeat>(std::bind(&IAQDevice::Slot_IAQ_Heartbeat, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_MainStatus>(std::bind(&IAQDevice::Slot_IAQ_MainStatus, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_MessageLong>(std::bind(&IAQDevice::Slot_IAQ_MessageLong, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_OneTouchStatus>(std::bind(&IAQDevice::Slot_IAQ_OneTouchStatus, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_PageButton>(std::bind(&IAQDevice::Slot_IAQ_PageButton, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_PageContinue>(std::bind(&IAQDevice::Slot_IAQ_PageContinue, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_PageEnd>(std::bind(&IAQDevice::Slot_IAQ_PageEnd, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_PageMessage>(std::bind(&IAQDevice::Slot_IAQ_PageMessage, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_PageStart>(std::bind(&IAQDevice::Slot_IAQ_PageStart, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_Poll>(std::bind(&IAQDevice::Slot_IAQ_Poll, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Probe>(std::bind_front(&IAQDevice::Slot_IAQ_Probe, this), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_StartUp>(std::bind(&IAQDevice::Slot_IAQ_StartUp, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_TableMessage>(std::bind(&IAQDevice::Slot_IAQ_TableMessage, this, std::placeholders::_1), DeviceId().Id());
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::IAQMessage_TitleMessage>(std::bind(&IAQDevice::Slot_IAQ_TitleMessage, this, std::placeholders::_1), DeviceId().Id());

		LogInfo(Channel::Devices, std::format("IAQ ({}): IAQDevice construction complete - device ready", DeviceId()));
	}

	IAQDevice::~IAQDevice()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::~IAQDevice", std::source_location::current());

		LogInfo(Channel::Devices, std::format("IAQ ({}): Destroying IAQDevice: final state was {}", DeviceId(), magic_enum::enum_name(m_OpState)));

		m_ProfilingDomain->End();
	}

	void IAQDevice::QueueCommand(uint8_t command)
	{
		LogDebug(Channel::Devices, [this, command]() { return std::format("IAQ ({}): Queuing command: 0x{:02x}", DeviceId(), command); });
		m_PendingCommand = command;
	}

	void IAQDevice::SelectPageButton(uint8_t button_index)
	{
		// On the AqualinkTouch (0x33) page protocol a button is "pressed" by sending the
		// command (0x11 + button_index) in the next IAQ_Poll ACK: 0x11 selects the page's
		// button index 0 (cf. IAQ_CMD_SELECT_POOL), so on-screen button N is 0x11 + N. The
		// command is page-relative -- it presses whatever button currently occupies that
		// index -- which is how the master drives navigation between pages.
		const auto command = static_cast<uint8_t>(IAQ_CMD_PAGE_BUTTON_BASE + button_index);
		LogInfo(Channel::Devices, [this, button_index, command]() { return std::format("IAQ ({}): SelectPageButton(index={}) -> queuing page command 0x{:02x}", DeviceId(), button_index, command); });
		QueueCommand(command);
	}

	void IAQDevice::EnablePageSurvey(const IAQ::PageRegistry& registry)
	{
		m_PageSurvey.Enable(registry);
		LogInfo(Channel::Devices, [this, &registry]() { return std::format("IAQ ({}): Page survey armed with {} target page(s)", DeviceId(), registry.size()); });
	}

	void IAQDevice::MaybeStartPageSurvey()
	{
		// Source data the pushed home page doesn't carry by visiting a small declarative set of
		// pages -- targeted navigation, not a menu crawl. Only an emulated panel drives the bus,
		// it runs once, and only after the home page is established (so navigation is well-defined).
		if (!IsEmulated() || !m_PageSurvey.IsArmed())
		{
			return;
		}

		auto commands = m_PageSurvey.Consume();
		LogInfo(Channel::Devices, [this, &commands]() { return std::format("IAQ ({}): Home established -> starting page survey ({} command(s) over {} target page(s))",
			DeviceId(), commands.size(), m_PageSurvey.TargetCount()); });
		m_CommandQueue.assign(commands.begin(), commands.end());
	}

	void IAQDevice::QueueChlorinatorPercentage(uint8_t percentage)
	{
		LogInfo(Channel::Devices, [this, percentage]() { return std::format("IAQ ({}): QueueChlorinatorPercentage({}%) - queuing command sequence", DeviceId(), percentage); });

		m_CommandQueue.clear();
		m_CommandQueue.push_back(IAQ_CMD_BACK);
		m_CommandQueue.push_back(IAQ_CMD_OPEN_AQUAPURE_PAGE);
		m_CommandQueue.push_back(IAQ_CMD_SELECT_POOL);
		m_CommandQueue.push_back(IAQ_CMD_SUBMIT_VALUE);

		m_AwaitingControlReady = true;
		m_ControlDataValue = std::format("{}{}", IAQ_BUTTON_INDEX_POOL, percentage);
	}

	void IAQDevice::QueueChlorinatorBoost(bool enable)
	{
		LogInfo(Channel::Devices, [this, enable]() { return std::format("IAQ ({}): QueueChlorinatorBoost({}) - queuing command sequence", DeviceId(), enable); });

		m_CommandQueue.clear();
		m_CommandQueue.push_back(IAQ_CMD_BACK);
		m_CommandQueue.push_back(IAQ_CMD_OPEN_AQUAPURE_PAGE);
		m_CommandQueue.push_back(IAQ_CMD_QUICK_BOOST);

		if (enable)
		{
			m_CommandQueue.push_back(IAQ_CMD_BOOST_START);
		}
		else
		{
			m_CommandQueue.push_back(IAQ_CMD_QUICK_BOOST);  // Stop
		}

		m_AwaitingControlReady = false;
		m_ControlDataValue.clear();
	}

	Capabilities::ActuationResult IAQDevice::SetChlorinatorPercentage(uint8_t percentage)
	{
		QueueChlorinatorPercentage(percentage);
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult IAQDevice::SetChlorinatorBoost(bool enable)
	{
		QueueChlorinatorBoost(enable);
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult IAQDevice::ActuatePageButton(uint8_t button_index)
	{
		SelectPageButton(button_index);
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult IAQDevice::ActuateDevice(const std::shared_ptr<Kernel::AuxillaryDevice>& device, Capabilities::ActuationAction action)
	{
		if (nullptr == device)
		{
			return Capabilities::ActuationResult::MappingFailed;
		}

		// A passive IAQ never transmits, so it cannot actuate. This is true both for a
		// non-emulated instance AND for an emulated one that has been presence-suppressed
		// (a real device answered at its address), so gate on IsEmulationActive() rather
		// than IsEmulated(). Report NotSupported so the dispatcher can fall back.
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): Not actively emulating - cannot actuate equipment", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}

		auto label = device->AuxillaryTraits.TryGet(Kernel::AuxillaryTraitsTypes::LabelTrait{});
		if (!label.has_value() || Utility::TrimWhitespace(label.value()).empty())
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): Cannot actuate a device with no label", DeviceId()); });
			return Capabilities::ActuationResult::MappingFailed;
		}
		const std::string target_label{ Utility::TrimWhitespace(label.value()) };

		// LIMITATION (capture-gated follow-up): the IAQ can only actuate a device whose
		// button is on the CURRENTLY-rendered page. If the target isn't visible we report
		// MappingFailed; the CommandDispatcher then falls back to another controller (e.g.
		// an emulated OneTouch, which crawls menus). In an IAQ-only rig the command fails
		// honestly. A full fix -- navigate to the page that hosts the button before
		// pressing -- needs the page-navigation command sequence reverse-engineered from a
		// live capture, so it is deliberately out of scope here.
		auto button_index = m_PageModel.FindButtonByLabel(target_label);
		if (!button_index.has_value())
		{
			LogWarning(Channel::Devices, [this, &target_label]() { return std::format("IAQ ({}): No on-screen button matches '{}' (current page not showing it)", DeviceId(), target_label); });
			return Capabilities::ActuationResult::MappingFailed;
		}

		// Pressing the button is a pure TOGGLE. For an explicit On/Off, only press when the
		// button's current on-screen status differs from the request; otherwise succeed as a
		// no-op rather than toggling it the wrong way.
		if (action != Capabilities::ActuationAction::On && action != Capabilities::ActuationAction::Off)
		{
			// Toggle: always press.
		}
		else
		{
			const bool want_on{ action == Capabilities::ActuationAction::On };
			const auto status = m_PageModel.Buttons().at(button_index.value()).status;
			const bool is_on = (status == Messages::ButtonStatuses::On) || (status == Messages::ButtonStatuses::Enabled) || (status == Messages::ButtonStatuses::EnabledStandby);
			if (status != Messages::ButtonStatuses::Unknown && is_on == want_on)
			{
				LogInfo(Channel::Devices, [this, &target_label, want_on]() { return std::format("IAQ ({}): '{}' already {} - no actuation required", DeviceId(), target_label, want_on ? "ON" : "OFF"); });
				return Capabilities::ActuationResult::Accepted;
			}
		}

		LogInfo(Channel::Devices, [this, &target_label, &button_index]() { return std::format("IAQ ({}): Toggling '{}' via page button index {}", DeviceId(), target_label, button_index.value()); });
		SelectPageButton(button_index.value());
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult IAQDevice::QueueSetpoint(uint8_t select_field_command, uint8_t temperature, const char* body_name)
	{
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, [this, body_name]() { return std::format("IAQ ({}): Not actively emulating - cannot set {} setpoint", DeviceId(), body_name); });
			return Capabilities::ActuationResult::NotSupported;
		}

		LogInfo(Channel::Devices, [this, body_name, temperature]() { return std::format("IAQ ({}): Set {} setpoint -> {} - queuing value-submit sequence", DeviceId(), body_name, static_cast<int>(temperature)); });

		// Verified vs iaq_aux_setpoint.cap: BACK -> open Set Temp page -> select the field
		// button -> submit; the absolute value rides the control-data response as "1" + value
		// (e.g. pool 31 -> "131", spa 39 -> "139") - the field is chosen by the button, not the prefix.
		m_CommandQueue.clear();
		m_CommandQueue.push_back(IAQ_CMD_BACK);
		m_CommandQueue.push_back(IAQ_CMD_OPEN_SETTEMP_PAGE);
		m_CommandQueue.push_back(select_field_command);
		m_CommandQueue.push_back(IAQ_CMD_SUBMIT_VALUE);

		m_AwaitingControlReady = true;
		m_ControlDataValue = std::format("{}{}", IAQ_BUTTON_INDEX_POOL, temperature);

		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult IAQDevice::SetPoolSetpoint(uint8_t temperature)
	{
		return QueueSetpoint(IAQ_CMD_SELECT_POOL_HEAT, temperature, "pool");
	}

	Capabilities::ActuationResult IAQDevice::SetSpaSetpoint(uint8_t temperature)
	{
		return QueueSetpoint(IAQ_CMD_SELECT_SPA_HEAT, temperature, "spa");
	}

	Capabilities::ActuationResult IAQDevice::SetSpaSwitchAssignment(uint8_t switch_number, uint8_t button_number, const std::string& function)
	{
		// Delegate to the extracted spa-switch write state machine. The command channel's busy state
		// (a draining sequence or an in-flight control-data handshake) gates a fresh goal, mirroring
		// the writer's own one-goal-at-a-time rule.
		const bool channel_busy = !m_CommandQueue.empty() || m_AwaitingControlReady;
		return m_SpaSwitchWriter.Queue(switch_number, button_number, function, IsEmulationActive(), channel_busy, DeviceId());
	}

	std::vector<std::string> IAQDevice::AvailableFunctions() const
	{
		// The iAQ's live picker (group 0x01) is only present transiently mid-edit; absent a
		// persisted decode, surface the shared canonical list (docs/alwin32/spaside-remotes.md).
		return Utility::KnownSpaSwitchFunctions();
	}

	Capabilities::ActuationResult IAQDevice::CreateControllerProgram(const Scheduling::ControllerSchedule& program)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::CreateControllerProgram", std::source_location::current());

		// Delegate to the extracted controller-schedule write state machine. Busy = another writer or
		// the command channel is occupied; the writer applies its own passive / feasibility gates.
		const bool channel_busy = m_SpaSwitchWriter.HasPendingGoal() || !m_CommandQueue.empty() || m_AwaitingControlReady;
		return m_ScheduleWriter.QueueCreate(program, IsEmulated(), channel_busy, DeviceId());
	}

	Capabilities::ActuationResult IAQDevice::DeleteControllerProgram(const Scheduling::ControllerSchedule& program)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::DeleteControllerProgram", std::source_location::current());

		const bool channel_busy = m_SpaSwitchWriter.HasPendingGoal() || !m_CommandQueue.empty() || m_AwaitingControlReady;
		return m_ScheduleWriter.QueueDelete(program, IsEmulated(), channel_busy, DeviceId());
	}

	Capabilities::ActuationResult IAQDevice::EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::EditControllerProgram", std::source_location::current());

		const bool channel_busy = m_SpaSwitchWriter.HasPendingGoal() || !m_CommandQueue.empty() || m_AwaitingControlReady;
		return m_ScheduleWriter.QueueEdit(existing, desired, IsEmulated(), channel_busy, DeviceId());
	}

	void IAQDevice::ProcessControllerUpdates()
	{
		ProcessControllerUpdates(false);
	}

	void IAQDevice::ProcessControllerUpdates(bool is_poll_message)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::ProcessControllerUpdates", std::source_location::current());

		LogTrace(Channel::Devices, [this, is_poll_message]() { return std::format("IAQ ({}): ProcessControllerUpdates called: state={}, is_poll={}, pending_cmd=0x{:02x}",
			DeviceId(), magic_enum::enum_name(m_OpState), is_poll_message, m_PendingCommand); });

		// This id has been addressed on the bus (poll/status), so any later watchdog
		// timeout is a genuine drop-out rather than "never present".
		m_HasReceivedData = true;

		// Non-emulated devices skip straight to NormalOperation on the first update.
		if (!IsEmulated() && m_OpState == OperatingStates::StartUp)
		{
			LogInfo(Channel::Devices, [this]() { return std::format("IAQ ({}): Non-emulated device detected - entering NormalOperation", DeviceId()); });
			m_OpState = OperatingStates::NormalOperation;
			Status(Devices::DeviceStatus_Normal{});
		}

		switch (m_OpState)
		{
		case OperatingStates::StartUp:
		{
			LogDebug(Channel::Devices, [this]() { return std::format("IAQ ({}): Processing StartUp state - waiting for MainStatus/AuxStatus", DeviceId()); });
			Status(Devices::DeviceStatus_Initializing{});
			break;
		}

		case OperatingStates::NormalOperation:
		{
			LogTrace(Channel::Devices, [this]() { return std::format("IAQ ({}): Processing NormalOperation state", DeviceId()); });
			break;
		}

		case OperatingStates::FaultHasOccurred:
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): Processing FaultHasOccurred state", DeviceId()); });
			break;
		}

		case OperatingStates::NotPresent:
		{
			// Device not present yet; nothing to process for this update.
			break;
		}
		}

		// Service an in-flight spa-switch write goal: it inspects the current page + decoded picker
		// rows and sets m_PendingCommand to the single next command (or leaves it 0 to dwell). Poll
		// only -- commands ride the poll ACK. The command queue is empty while such a goal is active
		// (enforced at queue time), so the send block below carries m_PendingCommand on the wire.
		if (is_poll_message && m_SpaSwitchWriter.HasPendingGoal())
		{
			m_SpaSwitchWriter.ProcessStep(m_PageModel, *this, m_DataHub.get(), DeviceId());
		}

		// Service an in-flight controller-schedule write goal (create a program) the same way:
		// it inspects the current page + decoded picker rows and sets m_PendingCommand to the next
		// command. Poll only, and mutually exclusive with the spa-switch goal / command queue.
		if (is_poll_message && m_ScheduleWriter.HasPendingGoal())
		{
			m_ScheduleWriter.ProcessStep(m_PageModel, m_StatusPage, *this, DeviceId());
		}

		// Commands can only be sent in response to IAQ_Poll messages.
		if (is_poll_message && !m_CommandQueue.empty())
		{
			auto cmd = m_CommandQueue.front();
			m_CommandQueue.pop_front();
			LogDebug(Channel::Devices, [this, cmd]() { return std::format("IAQ ({}): Sending queued command in Poll ACK: 0x{:02x} ({} remaining)",
				DeviceId(), cmd, m_CommandQueue.size()); });
			Signal_AckMessage(static_cast<uint8_t>(0x00), cmd);
		}
		else if (is_poll_message && m_PendingCommand != 0x00)
		{
			LogDebug(Channel::Devices, [this]() { return std::format("IAQ ({}): Sending command in Poll ACK: 0x{:02x}", DeviceId(), m_PendingCommand); });
			Signal_AckMessage(static_cast<uint8_t>(0x00), m_PendingCommand);
			m_PendingCommand = 0x00;
		}
		else
		{
			Signal_AckMessage(static_cast<uint8_t>(0x00), static_cast<uint8_t>(0x00));
		}
	}

	void IAQDevice::WatchdogTimeoutOccurred()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::WatchdogTimeoutOccurred", std::source_location::current());

		if (m_OpState == OperatingStates::NotPresent)
		{
			// Already determined this id is not present on the bus -- stay quiet.
			return;
		}

		LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): Watchdog timeout occurred: state={}, timeout_duration={}s", DeviceId(), magic_enum::enum_name(m_OpState), IAQ_TIMEOUT_DURATION.count()); });

		if (m_OpState == OperatingStates::StartUp)
		{
			if (m_HasReceivedData)
			{
				// Traffic addressed to this id was being received and then stopped -- a
				// genuine fault (the device was present but went silent).
				LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): No valid data received during StartUp -> entering FaultHasOccurred", DeviceId()); });
				m_OpState = OperatingStates::FaultHasOccurred;
				Status(Devices::DeviceStatus_FaultOccurred{});
			}
			else
			{
				// This id was never addressed on the bus: the master is not polling an
				// iAqualink2 here (e.g. an emulated id the panel isn't configured for).
				// That is "not present", not a fault -- settle quietly rather than alarm.
				LogInfo(Channel::Devices, [this]() { return std::format("IAQ ({}): No traffic ever addressed to this id -> marking NotPresent (master is not polling an iAqualink2 here)", DeviceId()); });
				m_OpState = OperatingStates::NotPresent;
				Status(Devices::DeviceStatus_Initializing{});
			}
		}
	}

	void IAQDevice::Signal_ControlDataResponse(const std::string& ascii_data)
	{
		if (!IsEmulated())
		{
			return;
		}

		auto data_msg = std::make_shared<Messages::IAQMessage_ControlDataResponse>(ascii_data);
		if (data_msg)
		{
			LogDebug(Channel::Devices, [this, &ascii_data]() { return std::format("IAQ ({}): Signalling control data response: '{}'", DeviceId(), ascii_data); });
			data_msg->Signal_MessageToSend();
		}
	}

	nlohmann::json IAQDevice::DescribeDiagnostics() const
	{
		nlohmann::json j;

		j["device_type"] = "IAQ";
		j["device_id"] = std::format("0x{:02x}", DeviceId().Id()());
		j["operating_state"] = std::string(magic_enum::enum_name(m_OpState));

		j["screen"] = DescribeScreen();

		j["pending_command"] = std::format("0x{:02x}", m_PendingCommand);
		j["command_queue_depth"] = static_cast<uint32_t>(m_CommandQueue.size());
		j["awaiting_control_ready"] = m_AwaitingControlReady;
		j["control_data_value"] = m_ControlDataValue;
		j["is_emulated"] = IsEmulated();
		j["emulation_suppressed"] = IsEmulationSuppressed();
		j["recent_commands"] = DescribeRecentCommands();
		j["is_running"] = IsRunning();

		return j;
	}

}
// namespace AqualinkAutomate::Devices
