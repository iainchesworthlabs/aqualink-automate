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

		// Spa-switch button-assignment WRITE (RE'd + cross-validated from
		// captures/iaq_spaswitch_edit{,2}.cap; see docs/alwin32/spaside-remotes.md). Page ids that
		// IAQ_PageStart carries, and the page-button command bytes for the 4-Function detail edit.
		constexpr uint8_t IAQ_PAGE_HOME{ 0x01 };
		constexpr uint8_t IAQ_PAGE_MENU{ 0x0f };
		constexpr uint8_t IAQ_PAGE_SETUP{ 0x14 };
		constexpr uint8_t IAQ_PAGE_SPA_REMOTES{ 0x3a };
		constexpr uint8_t IAQ_PAGE_SPA_SWITCH_DETAIL{ 0x3b };

		constexpr uint8_t IAQ_CMD_MENU_TO_SETUP{ 0x15 };        // on the menu (0x0f): page-button idx4 -> Setup
		constexpr uint8_t IAQ_CMD_OPEN_SPASWITCH_DETAIL{ 0x16 };// on Spa Remotes (0x3a): idx5 -> 4-Function detail
		constexpr uint8_t IAQ_CMD_SCROLL_PICKER{ 0x15 };        // on the detail (0x3b): idx4 -> page the picker

		// On the 4-Function detail, the selectable cells share one page-button index space:
		//   assignment row (ordinal O = (S-1)*4 + B, 1-based) -> page-button index O + 4
		//   picker row     (visible slot A, 1-based)          -> page-button index A + 11
		// As page-button commands (0x11 + index): row-select = 0x15 + O, commit = 0x1c + A.
		constexpr uint8_t IAQ_SPASWITCH_ROWSELECT_CMD_BASE{ 0x15 };  // + ordinal
		constexpr uint8_t IAQ_SPASWITCH_COMMIT_CMD_BASE{ 0x1c };     // + picker slot
		constexpr uint8_t IAQ_SPASWITCH_MAX_VISIBLE_ROW{ 7 };        // rows 1..7 are on-screen; row 8 (2:4) needs an
		                                                            // assignment-list scroll that is not yet decoded
		constexpr uint32_t IAQ_SPASWITCH_SETTLE_POLLS{ 4 };         // polls to let the master render after a command
		constexpr uint32_t IAQ_SPASWITCH_MAX_SCROLLS{ 10 };        // bound the picker scroll search
		constexpr uint32_t IAQ_SPASWITCH_POLL_LIMIT{ 400 };        // overall backstop (abandon the goal)

		// Controller-schedule WRITE (RE'd from captures/iaq_schedule_{session,clean}.cap; see
		// docs/iaq_schedule_protocol.md, write path). Page ids the Program flow carries and the
		// fixed touch-grid keys (Command = 0x11 + screen position) it uses.
		constexpr uint8_t IAQ_CMD_MENU_TO_SCHEDULE{ 0x11 };   // on the menu (0x0f): pos0 -> Schedule list
		constexpr uint8_t IAQ_CMD_ADD_PROGRAM{ 0x11 };        // on the list (0x28): pos0 -> Add Program -> device picker
		// Device picker (0x38): click a visible row R (1..7) with (base + R); scroll down / confirm.
		// Verified across iaq_schedule_{clean,picker}.cap (row1->0x14, row2->0x15, row3->0x16, row4->0x17).
		constexpr uint8_t IAQ_SCHEDULE_PICK_ROW_BASE{ 0x13 };  // click visible row R -> 0x13 + R
		constexpr uint8_t IAQ_CMD_PICKER_SCROLL{ 0x12 };       // scroll the picker down one page
		constexpr uint8_t IAQ_CMD_PICKER_OK{ 0x13 };           // confirm the highlighted device -> back to list
		// Time fields on the schedule list (0x28) open the time picker (0x29); on the picker, 0x11
		// toggles AM/PM and 0x80 submits (the value rides the control-data response as "1"+HH:MM).
		constexpr uint8_t IAQ_CMD_OPEN_ON_FIELD{ 0x21 };
		constexpr uint8_t IAQ_CMD_OPEN_OFF_FIELD{ 0x22 };
		constexpr uint8_t IAQ_CMD_AMPM_TOGGLE{ 0x11 };
		// Editing an existing program on the list (0x28): click program row R -> 0x22 + R (row1 0x23,
		// row2 0x24, ...); Delete = 0x13 -> confirm dialog; Ok = 0x01 (Cancel = 0x02/Back). Verified
		// against the 0x40 highlight in captures/iaq_editdelete.cap.
		constexpr uint8_t IAQ_SCHEDULE_ROW_BASE{ 0x22 };   // click program row R -> 0x22 + R
		constexpr uint8_t IAQ_CMD_EDIT_PROGRAM{ 0x12 };    // Edit -> enter the highlighted row's edit mode
		constexpr uint8_t IAQ_CMD_DELETE_PROGRAM{ 0x13 };  // Delete -> confirm dialog
		constexpr uint8_t IAQ_CMD_CONFIRM_OK{ 0x01 };      // Ok on the confirm dialog
		// Day keys on the schedule list (0x28): the day-selection touch cells.
		constexpr uint8_t IAQ_CMD_DAY_ALL{ 0x17 };
		constexpr uint8_t IAQ_CMD_DAY_MON{ 0x18 };  // then Tue..Sun are consecutive
		constexpr uint8_t IAQ_CMD_DAY_WKDAYS{ 0x1f };
		constexpr uint8_t IAQ_CMD_DAY_WKENDS{ 0x20 };
		constexpr uint32_t IAQ_SCHEDULE_SETTLE_POLLS{ 4 };    // polls to let the master render after a command
		constexpr uint32_t IAQ_SCHEDULE_MAX_SCROLLS{ 12 };    // bound the device-picker scroll search
		constexpr uint32_t IAQ_SCHEDULE_POLL_LIMIT{ 400 };    // overall backstop (abandon the goal)

		// Map a controller-expressible day-of-week bitmask (bit0=Mon..bit6=Sun) to the schedule
		// list's day touch-cell command. The caller only ever passes a selection the controller can
		// represent (guaranteed by Scheduling::CheckControllerCandidate): all / weekdays / weekends /
		// a single day. Single days are consecutive from Monday (0x18) in Mon..Sun order.
		uint8_t DayCommandFor(uint8_t days_of_week)
		{
			const uint8_t days = days_of_week & 0x7f;
			if (days == 0x7f) { return IAQ_CMD_DAY_ALL; }
			if (days == 0x1f) { return IAQ_CMD_DAY_WKDAYS; }
			if (days == 0x60) { return IAQ_CMD_DAY_WKENDS; }
			if (std::popcount(days) == 1)
			{
				return static_cast<uint8_t>(IAQ_CMD_DAY_MON + std::countr_zero(days));
			}
			return IAQ_CMD_DAY_ALL;   // unreachable for a validated candidate
		}

		// The submit value for a schedule time field: the field-index prefix ('1') + the 12-hour
		// clock time, zero-padded (e.g. 09:00 -> "109:00", 17:00 -> "105:00"). AM/PM is set on the
		// picker by a separate toggle and never rides this value (docs/iaq_schedule_protocol.md).
		std::string ScheduleTimeValue(int hour24, int minute)
		{
			int hour12 = hour24 % 12;
			if (hour12 == 0) { hour12 = 12; }
			return std::format("{}{:02d}:{:02d}", IAQ_BUTTON_INDEX_POOL, hour12, minute);
		}
	}
	// namespace

	IAQDevice::IAQDevice(const std::shared_ptr<Devices::JandyDeviceType>& device_id, Kernel::HubLocator& hub_locator, bool is_emulated) :
		JandyController(device_id, hub_locator),
		Capabilities::Restartable(IAQ_TIMEOUT_DURATION),
		Capabilities::Screen(IAQ_STATUS_PAGE_LINES),
		Capabilities::Emulated(is_emulated),
		m_StatusPage(IAQ_STATUS_PAGE_LINES),
		m_TableInfo(IAQ_MESSAGE_TABLE_LINES),
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
		m_SlotManager.RegisterSlot_FilterByDeviceId<Messages::JandyMessage_Probe>(std::bind(&IAQDevice::Slot_IAQ_Probe, this, std::placeholders::_1), DeviceId().Id());
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
		m_PageSurveyEnabled = true;
		m_PageSurveyRegistry = registry;
		LogInfo(Channel::Devices, [this, &registry]() { return std::format("IAQ ({}): Page survey armed with {} target page(s)", DeviceId(), registry.size()); });
	}

	void IAQDevice::MaybeStartPageSurvey()
	{
		// Source data the pushed home page doesn't carry by visiting a small declarative set of
		// pages -- targeted navigation, not a menu crawl. Only an emulated panel drives the bus,
		// it runs once, and only after the home page is established (so navigation is well-defined).
		if (!IsEmulated() || !m_PageSurveyEnabled || m_PageSurveyDone)
		{
			return;
		}
		m_PageSurveyDone = true;

		auto commands = IAQ::BuildSurveyCommandSequence(m_PageSurveyRegistry);
		LogInfo(Channel::Devices, [this, &commands]() { return std::format("IAQ ({}): Home established -> starting page survey ({} command(s) over {} target page(s))",
			DeviceId(), commands.size(), m_PageSurveyRegistry.size()); });
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

	std::optional<uint8_t> IAQDevice::FindPageButtonByLabel(const std::string& label) const
	{
		const std::string target{ Utility::TrimWhitespace(label) };
		if (target.empty())
		{
			return std::nullopt;
		}

		// Home-page button names carry a trailing status suffix (e.g. "Pool LightON"), so a
		// prefix match against the trimmed label resolves the live button index.
		for (const auto& [index, info] : m_PageButtons)
		{
			if (Utility::TrimWhitespace(info.name).starts_with(target))
			{
				return index;
			}
		}

		return std::nullopt;
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
		auto button_index = FindPageButtonByLabel(target_label);
		if (!button_index.has_value())
		{
			LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): No on-screen button matches '{}' (current page not showing it)", DeviceId(), target_label); });
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
			const auto status = m_PageButtons.at(button_index.value()).status;
			const bool is_on = (status == Messages::ButtonStatuses::On) || (status == Messages::ButtonStatuses::Enabled) || (status == Messages::ButtonStatuses::EnabledStandby);
			if (status != Messages::ButtonStatuses::Unknown && is_on == want_on)
			{
				LogInfo(Channel::Devices, [&]() { return std::format("IAQ ({}): '{}' already {} - no actuation required", DeviceId(), target_label, want_on ? "ON" : "OFF"); });
				return Capabilities::ActuationResult::Accepted;
			}
		}

		LogInfo(Channel::Devices, [&]() { return std::format("IAQ ({}): Toggling '{}' via page button index {}", DeviceId(), target_label, button_index.value()); });
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
		// Program a spa-side switch button's function over the bus by driving the AqualinkTouch (0x33)
		// "4 Function Spa Switch" detail page (RE'd + cross-validated from iaq_spaswitch_edit{,2}.cap;
		// see docs/alwin32/spaside-remotes.md). Only an EMULATED panel transmits, so a passive decoder
		// can't program -- report NotSupported so the controller falls through to another writer.
		if (!IsEmulationActive())
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): Not actively emulating - cannot program spa-switch assignment", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}

		if ((switch_number < 1) || (button_number < 1) || (button_number > 4) || function.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		// The detail page lists assignment rows 1..7 on-screen (ordinal = (S-1)*4 + B). Row 8 (2:4)
		// and any switch >2 need an assignment-list scroll whose protocol is not yet decoded -- and
		// row 8's page-button index would collide with the picker commit range -- so reject those
		// rather than risk writing the wrong cell. The OneTouch writer still covers them.
		const uint32_t ordinal = static_cast<uint32_t>(switch_number - 1) * 4u + button_number;
		if (ordinal > IAQ_SPASWITCH_MAX_VISIBLE_ROW)
		{
			LogWarning(Channel::Devices, [this, switch_number, button_number]() { return std::format("IAQ ({}): spa-switch row {}:{} is not directly selectable on the iAQ detail (needs an undecoded assignment-list scroll) - deferring", DeviceId(), switch_number, button_number); });
			return Capabilities::ActuationResult::NotSupported;
		}

		// One goal at a time on the shared panel UI.
		if (m_PendingSpaSwitchWrite.has_value() || !m_CommandQueue.empty() || m_AwaitingControlReady)
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): Busy - rejecting spa-switch assignment", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}

		SpaSwitchWriteGoal goal;
		goal.switch_number = switch_number;
		goal.button_number = button_number;
		goal.function = function;
		goal.row_tag = std::format("{}:{}", switch_number, button_number);
		goal.desc = std::format("spa-switch {}:{} -> '{}'", switch_number, button_number, function);

		LogInfo(Channel::Devices, [this, &goal]() { return std::format("IAQ ({}): Queued {}", DeviceId(), goal.desc); });
		m_PendingSpaSwitchWrite = std::move(goal);
		m_SpaSwitchWritePhase = SpaSwitchWritePhase::Navigate;
		m_SpaSwitchRowSelected = false;
		m_SpaSwitchWritePollCount = 0;
		m_SpaSwitchScrollCount = 0;
		m_SpaSwitchSettleCount = 0;
		m_SpaSwitchFirstPickerSeen.reset();
		return Capabilities::ActuationResult::Accepted;
	}

	std::vector<std::string> IAQDevice::AvailableFunctions() const
	{
		// The iAQ's live picker (group 0x01) is only present transiently mid-edit; absent a
		// persisted decode, surface the shared canonical list (docs/alwin32/spaside-remotes.md).
		return Utility::KnownSpaSwitchFunctions();
	}

	void IAQDevice::SpaSwitchWrite_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::SpaSwitchWrite_ProcessStep", std::source_location::current());

		if (!m_PendingSpaSwitchWrite.has_value())
		{
			return;
		}
		const SpaSwitchWriteGoal& goal = m_PendingSpaSwitchWrite.value();

		auto eq_ci = [](const std::string& a, const std::string& b)
		{
			if (a.size() != b.size()) { return false; }
			for (std::size_t i = 0; i < a.size(); ++i)
			{
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
			}
			return true;
		};

		// Tear down the goal (reads goal.desc BEFORE resetting -- callers return immediately after).
		auto finish = [&](bool ok)
		{
			if (ok) { LogInfo(Channel::Devices, [&]() { return std::format("IAQ ({}): {} completed", DeviceId(), goal.desc); }); }
			else    { LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): {} abandoned", DeviceId(), goal.desc); }); }
			m_PendingCommand = 0x00;
			m_PendingSpaSwitchWrite.reset();
			m_SpaSwitchWritePhase = SpaSwitchWritePhase::Navigate;
			m_SpaSwitchRowSelected = false;
			m_SpaSwitchScrollCount = 0;
			m_SpaSwitchSettleCount = 0;
			m_SpaSwitchFirstPickerSeen.reset();
		};

		// Overall backstop: never spin forever on a bus that isn't behaving as decoded.
		if (++m_SpaSwitchWritePollCount > IAQ_SPASWITCH_POLL_LIMIT)
		{
			finish(false);
			return;
		}

		// Settle: after issuing a command, dwell a few polls so the master renders the new page /
		// re-pushes the picker before we read state and decide the next step.
		if (m_SpaSwitchSettleCount > 0)
		{
			--m_SpaSwitchSettleCount;
			m_PendingCommand = 0x00;
			return;
		}

		// Issue one command this poll, then settle.
		auto issue = [&](uint8_t cmd)
		{
			m_PendingCommand = cmd;
			m_SpaSwitchSettleCount = IAQ_SPASWITCH_SETTLE_POLLS;
		};

		switch (m_SpaSwitchWritePhase)
		{
		case SpaSwitchWritePhase::Navigate:
		{
			// Page-GATED walk to the 4-Function detail (0x3b). Each hop waits (via settle + page-id
			// re-evaluation) for the master to land on the next page before the following command.
			switch (m_CurrentPageId)
			{
			case IAQ_PAGE_SPA_SWITCH_DETAIL:
				m_SpaSwitchWritePhase = SpaSwitchWritePhase::SelectRow;   // arrived; act next poll
				return;

			case IAQ_PAGE_SPA_REMOTES:
				issue(IAQ_CMD_OPEN_SPASWITCH_DETAIL);                     // 0x16 -> detail
				return;

			case IAQ_PAGE_SETUP:
				if (auto idx = FindPageButtonByLabel("Spa Remotes"); idx.has_value())
				{
					issue(static_cast<uint8_t>(IAQ_CMD_PAGE_BUTTON_BASE + idx.value()));
				}
				else
				{
					m_PendingCommand = 0x00;   // button not rendered yet; dwell one poll
				}
				return;

			case IAQ_PAGE_MENU:
				issue(IAQ_CMD_MENU_TO_SETUP);                            // 0x15 -> Setup
				return;

			case IAQ_PAGE_HOME:
			default:
				issue(IAQ_CMD_BACK);                                     // HOME or unknown: unwind toward menu
				return;
			}
		}

		case SpaSwitchWritePhase::SelectRow:
		{
			if (m_CurrentPageId != IAQ_PAGE_SPA_SWITCH_DETAIL)
			{
				m_SpaSwitchWritePhase = SpaSwitchWritePhase::Navigate;   // lost the page; re-navigate
				return;
			}
			if (!m_SpaSwitchRowSelected)
			{
				const uint32_t ordinal = static_cast<uint32_t>(goal.switch_number - 1) * 4u + goal.button_number;
				issue(static_cast<uint8_t>(IAQ_SPASWITCH_ROWSELECT_CMD_BASE + ordinal));   // 0x15 + ordinal
				m_SpaSwitchRowSelected = true;
			}
			m_SpaSwitchWritePhase = SpaSwitchWritePhase::FindFunction;
			m_SpaSwitchFirstPickerSeen.reset();
			return;
		}

		case SpaSwitchWritePhase::FindFunction:
		{
			if (m_CurrentPageId != IAQ_PAGE_SPA_SWITCH_DETAIL)
			{
				m_SpaSwitchWritePhase = SpaSwitchWritePhase::Navigate;
				return;
			}

			// Target visible in the current picker page? Commit at its slot (0x1c + slot).
			for (const auto& [slot, function] : m_SpaSwitchPickerRows)
			{
				if (eq_ci(function, goal.function))
				{
					issue(static_cast<uint8_t>(IAQ_SPASWITCH_COMMIT_CMD_BASE + slot));
					m_SpaSwitchWritePhase = SpaSwitchWritePhase::Verify;
					return;
				}
			}

			// Not visible: scroll the picker, with wrap-detection (the first row repeating means we
			// have cycled the whole list without finding F) and a hard scroll bound.
			const std::string signature = m_SpaSwitchPickerRows.empty() ? std::string{} : m_SpaSwitchPickerRows.begin()->second;
			if (!m_SpaSwitchFirstPickerSeen.has_value())
			{
				m_SpaSwitchFirstPickerSeen = signature;
			}
			else if (!signature.empty() && (m_SpaSwitchScrollCount > 0) && eq_ci(signature, m_SpaSwitchFirstPickerSeen.value()))
			{
				LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): function '{}' not offered by the picker for {}", DeviceId(), goal.function, goal.row_tag); });
				finish(false);
				return;
			}

			if (++m_SpaSwitchScrollCount > IAQ_SPASWITCH_MAX_SCROLLS)
			{
				LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): exhausted picker scroll for {}", DeviceId(), goal.row_tag); });
				finish(false);
				return;
			}
			issue(IAQ_CMD_SCROLL_PICKER);   // 0x15
			return;
		}

		case SpaSwitchWritePhase::Verify:
		{
			// The commit press IS the save -- the master re-pushes the group-0x00 row, which the read
			// path writes to the DataHub. Confirm it now reads the target function.
			if (nullptr != m_DataHub)
			{
				if (auto live = m_DataHub->SpaSwitchAssignment(goal.switch_number, goal.button_number);
					live.has_value() && eq_ci(live.value(), goal.function))
				{
					finish(true);
					return;
				}
			}
			m_PendingCommand = 0x00;   // dwell until the row re-pushes (or the poll backstop fires)
			return;
		}

		case SpaSwitchWritePhase::Done:
		case SpaSwitchWritePhase::Failed:
		default:
			finish(m_SpaSwitchWritePhase == SpaSwitchWritePhase::Done);
			return;
		}
	}

	Capabilities::ActuationResult IAQDevice::CreateControllerProgram(const Scheduling::ControllerSchedule& program)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::CreateControllerProgram", std::source_location::current());

		// A passive (non-emulated) iAQ never transmits, so it cannot drive the panel.
		if (!IsEmulated())
		{
			LogDebug(Channel::Devices, [this]() { return std::format("IAQ ({}): CreateControllerProgram rejected -- device is passive (not emulated)", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}

		// One goal at a time on the shared panel UI (mirrors the spa-switch writer's convention of
		// reporting a busy panel as NotSupported).
		if (m_PendingScheduleWrite.has_value() || m_PendingSpaSwitchWrite.has_value() || !m_CommandQueue.empty() || m_AwaitingControlReady)
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): busy - rejecting controller-schedule write", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}

		// The controller can only represent a constrained subset -- reject anything it can't.
		const auto feasibility = Scheduling::CheckControllerCandidate(program);
		if (!feasibility.promotable)
		{
			LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): CreateControllerProgram rejected -- program is not controller-representable (target='{}')", DeviceId(), program.target); });
			return Capabilities::ActuationResult::InvalidValue;
		}

		ScheduleWriteGoal goal;
		goal.op = ScheduleWriteOp::Create;
		goal.program = program;
		goal.desc = std::format("create controller program '{}'", program.target);
		QueueScheduleWrite(std::move(goal));
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult IAQDevice::DeleteControllerProgram(const Scheduling::ControllerSchedule& program)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::DeleteControllerProgram", std::source_location::current());

		if (!IsEmulated())
		{
			LogDebug(Channel::Devices, [this]() { return std::format("IAQ ({}): DeleteControllerProgram rejected -- device is passive (not emulated)", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}
		if (m_PendingScheduleWrite.has_value() || m_PendingSpaSwitchWrite.has_value() || !m_CommandQueue.empty() || m_AwaitingControlReady)
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): busy - rejecting controller-schedule delete", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}
		if (program.target.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		ScheduleWriteGoal goal;
		goal.op = ScheduleWriteOp::Delete;
		goal.program = program;
		goal.match = program;   // SelectRow locates the row by matching `match`
		goal.desc = std::format("delete controller program '{}'", program.target);
		QueueScheduleWrite(std::move(goal));
		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult IAQDevice::EditControllerProgram(const Scheduling::ControllerSchedule& existing, const Scheduling::ControllerSchedule& desired)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::EditControllerProgram", std::source_location::current());

		if (!IsEmulated())
		{
			LogDebug(Channel::Devices, [this]() { return std::format("IAQ ({}): EditControllerProgram rejected -- device is passive (not emulated)", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}
		if (m_PendingScheduleWrite.has_value() || m_PendingSpaSwitchWrite.has_value() || !m_CommandQueue.empty() || m_AwaitingControlReady)
		{
			LogWarning(Channel::Devices, [this]() { return std::format("IAQ ({}): busy - rejecting controller-schedule edit", DeviceId()); });
			return Capabilities::ActuationResult::NotSupported;
		}
		if (existing.target.empty())
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		// The desired program must be one the controller can represent -- same feasibility gate as create.
		const auto feasibility = Scheduling::CheckControllerCandidate(desired);
		if (!feasibility.promotable)
		{
			LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): EditControllerProgram rejected -- desired program is not controller-representable (target='{}')", DeviceId(), desired.target); });
			return Capabilities::ActuationResult::InvalidValue;
		}

		ScheduleWriteGoal goal;
		goal.op = ScheduleWriteOp::Edit;
		goal.program = desired;    // the field phases set from goal.program; Verify matches it
		goal.match = existing;     // SelectRow locates the current row by matching `match`
		goal.desc = std::format("edit controller program '{}'", existing.target);
		QueueScheduleWrite(std::move(goal));
		return Capabilities::ActuationResult::Accepted;
	}

	void IAQDevice::QueueScheduleWrite(ScheduleWriteGoal goal)
	{
		m_PendingScheduleWrite = std::move(goal);
		m_ScheduleWritePhase = ScheduleWritePhase::NavigateToList;
		m_ScheduleWritePollCount = 0;
		m_ScheduleWriteSettleCount = 0;
		m_ScheduleWriteScrollCount = 0;
		m_ScheduleProgramAdded = false;
		m_ScheduleDeviceClicked = false;
		m_ScheduleTimeFieldOpened = false;

		LogInfo(Channel::Devices, [&]() { return std::format("IAQ ({}): queued {}", DeviceId(), m_PendingScheduleWrite->desc); });
	}

	void IAQDevice::ControllerScheduleWrite_ProcessStep()
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::ControllerScheduleWrite_ProcessStep", std::source_location::current());

		if (!m_PendingScheduleWrite.has_value())
		{
			return;
		}
		const ScheduleWriteGoal& goal = m_PendingScheduleWrite.value();

		auto finish = [&](bool ok)
		{
			if (ok) { LogInfo(Channel::Devices, [&]() { return std::format("IAQ ({}): {} completed", DeviceId(), goal.desc); }); }
			else    { LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): {} abandoned", DeviceId(), goal.desc); }); }
			m_PendingCommand = 0x00;
			m_PendingScheduleWrite.reset();
			m_ScheduleWritePhase = ScheduleWritePhase::NavigateToList;
			m_ScheduleProgramAdded = false;
			m_ScheduleDeviceClicked = false;
			m_ScheduleTimeFieldOpened = false;
			m_ScheduleWriteScrollCount = 0;
			m_ScheduleWriteSettleCount = 0;
		};

		// Overall backstop.
		if (++m_ScheduleWritePollCount > IAQ_SCHEDULE_POLL_LIMIT)
		{
			finish(false);
			return;
		}

		// Settle: dwell a few polls after a command so the master renders the new page.
		if (m_ScheduleWriteSettleCount > 0)
		{
			--m_ScheduleWriteSettleCount;
			m_PendingCommand = 0x00;
			return;
		}

		auto issue = [&](uint8_t cmd)
		{
			m_PendingCommand = cmd;
			m_ScheduleWriteSettleCount = IAQ_SCHEDULE_SETTLE_POLLS;
		};

		auto eq_ci = [](std::string_view a, std::string_view b)
		{
			if (a.size() != b.size()) { return false; }
			for (std::size_t i = 0; i < a.size(); ++i)
			{
				if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) { return false; }
			}
			return true;
		};

		// Set one time field of the highlighted program: open it (from the list) -> on the time
		// picker, toggle AM/PM to match then submit the value via the control-data handshake. Emits
		// at most one command per poll and advances to `next` once the submit is issued.
		auto set_time = [&](uint8_t open_cmd, int hour, int minute, ScheduleWritePhase next)
		{
			if (!m_ScheduleTimeFieldOpened)
			{
				if (m_CurrentPageId == IAQ_SCHEDULE_PAGE_ID) { issue(open_cmd); m_ScheduleTimeFieldOpened = true; }
				else { m_PendingCommand = 0x00; }   // dwell until the list is up
				return;
			}
			if (m_CurrentPageId != IAQ_TIME_PICKER_PAGE_ID)
			{
				m_PendingCommand = 0x00;   // dwell until the time picker renders
				return;
			}

			// Match AM/PM before submitting (picker line 2 carries the current meridiem).
			const std::string meridiem = Utility::TrimWhitespace(m_StatusPage[IAQ_TIME_PICKER_AMPM_LINE].Text);
			const bool is_am = eq_ci(meridiem, "AM");
			const bool is_pm = eq_ci(meridiem, "PM");
			if (!is_am && !is_pm)
			{
				m_PendingCommand = 0x00;   // picker not fully rendered yet; wait for the meridiem line
				return;
			}
			const bool want_pm = hour >= 12;
			if (is_pm != want_pm)
			{
				issue(IAQ_CMD_AMPM_TOGGLE);   // flip AM<->PM, then re-read next poll
				return;
			}

			// Meridiem matches: submit. The value ("1"+HH:MM) rides the control-data response the
			// master requests with IAQ_ControlReady (Slot_IAQ_ControlReady) after the 0x80 submit.
			m_ControlDataValue = ScheduleTimeValue(hour, minute);
			m_AwaitingControlReady = true;
			issue(IAQ_CMD_SUBMIT_VALUE);
			m_ScheduleTimeFieldOpened = false;
			m_ScheduleWritePhase = next;
		};

		// Does a parsed list row match the program to LOCATE (delete/edit use `match`, the existing
		// program; create is never in a row-locating phase so `match` is unset there)?
		auto matches_program = [&](const Scheduling::ControllerSchedule& row)
		{
			return eq_ci(row.target, goal.match.target)
				&& row.days_of_week == goal.match.days_of_week
				&& row.on_hour == goal.match.on_hour && row.on_minute == goal.match.on_minute
				&& row.off_hour == goal.match.off_hour && row.off_minute == goal.match.off_minute;
		};

		switch (m_ScheduleWritePhase)
		{
		case ScheduleWritePhase::NavigateToList:
		{
			// Page-gated walk to the Schedule list (0x28).
			switch (m_CurrentPageId)
			{
			case IAQ_SCHEDULE_PAGE_ID:
				// Arrived on the list: create adds a program; delete/edit find the target row first.
				m_ScheduleWritePhase = (goal.op == ScheduleWriteOp::Create)
					? ScheduleWritePhase::AddProgram
					: ScheduleWritePhase::SelectRow;
				return;

			case IAQ_PAGE_MENU:
				issue(IAQ_CMD_MENU_TO_SCHEDULE);                          // menu pos0 -> Schedule list
				return;

			case IAQ_PAGE_HOME:
			default:
				issue(IAQ_CMD_BACK);                                      // unwind toward the menu
				return;
			}
		}

		case ScheduleWritePhase::AddProgram:
		{
			if (m_CurrentPageId != IAQ_SCHEDULE_PAGE_ID)
			{
				m_ScheduleWritePhase = ScheduleWritePhase::NavigateToList;   // lost the page; re-navigate
				return;
			}
			if (!m_ScheduleProgramAdded)
			{
				issue(IAQ_CMD_ADD_PROGRAM);            // pos0 on the list -> Add Program -> device picker (0x38)
				m_ScheduleProgramAdded = true;
			}
			m_ScheduleWritePhase = ScheduleWritePhase::SelectDevice;
			return;
		}

		case ScheduleWritePhase::SelectDevice:
		{
			if (m_CurrentPageId != IAQ_DEVICE_PICKER_PAGE_ID)
			{
				m_PendingCommand = 0x00;   // dwell until the picker renders
				return;
			}

			// Two-step select once the target is on-screen: click its visible row (0x13 + row) to
			// highlight it, then confirm with the OK key (0x13) -> returns to the list.
			if (m_ScheduleDeviceClicked)
			{
				issue(IAQ_CMD_PICKER_OK);
				m_ScheduleWritePhase = ScheduleWritePhase::SetOnTime;
				return;
			}

			// Is the target device visible in the current picker page? (Attribute = the visible row.)
			for (const auto& [row, label] : m_DevicePickerRows)
			{
				if (eq_ci(label, goal.program.target))
				{
					issue(static_cast<uint8_t>(IAQ_SCHEDULE_PICK_ROW_BASE + row));   // click the row
					m_ScheduleDeviceClicked = true;
					return;
				}
			}

			// Not visible: scroll down one page, bounded by IAQ_SCHEDULE_MAX_SCROLLS.
			if (++m_ScheduleWriteScrollCount > IAQ_SCHEDULE_MAX_SCROLLS)
			{
				LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): {} -- target device '{}' not found after scrolling the picker", DeviceId(), goal.desc, goal.program.target); });
				finish(false);
				return;
			}
			issue(IAQ_CMD_PICKER_SCROLL);
			return;
		}

		case ScheduleWritePhase::SetOnTime:
			set_time(IAQ_CMD_OPEN_ON_FIELD, goal.program.on_hour, goal.program.on_minute, ScheduleWritePhase::SetOffTime);
			return;

		case ScheduleWritePhase::SetOffTime:
			set_time(IAQ_CMD_OPEN_OFF_FIELD, goal.program.off_hour, goal.program.off_minute, ScheduleWritePhase::SetDay);
			return;

		case ScheduleWritePhase::SetDay:
		{
			if (m_CurrentPageId != IAQ_SCHEDULE_PAGE_ID)
			{
				m_PendingCommand = 0x00;   // dwell until the master renders the list with the new program
				return;
			}
			issue(DayCommandFor(goal.program.days_of_week));
			m_ScheduleWritePhase = ScheduleWritePhase::Verify;
			return;
		}

		case ScheduleWritePhase::Verify:
		{
			// The new program is present once the parsed schedule list carries a row for the target
			// device on the requested day. (Times are set by a later increment; a freshly-created
			// program defaults to 1:00 PM / 1:00 PM until then.)
			for (const auto& [ordinal, text] : m_ScheduleRows)
			{
				if (const auto parsed = IAQ::ParseScheduleRow(text);
					parsed.has_value()
					&& eq_ci(parsed->target, goal.program.target)
					&& parsed->days_of_week == goal.program.days_of_week
					&& parsed->on_hour == goal.program.on_hour && parsed->on_minute == goal.program.on_minute
					&& parsed->off_hour == goal.program.off_hour && parsed->off_minute == goal.program.off_minute)
				{
					finish(true);
					return;
				}
			}
			m_PendingCommand = 0x00;   // dwell until the list re-renders (or the poll backstop fires)
			return;
		}

		case ScheduleWritePhase::SelectRow:
		{
			if (m_CurrentPageId != IAQ_SCHEDULE_PAGE_ID)
			{
				m_PendingCommand = 0x00;   // dwell until the list renders
				return;
			}
			if (m_ScheduleRows.empty())
			{
				m_PendingCommand = 0x00;   // list not populated yet; wait (poll backstop bounds it)
				return;
			}
			// Click the row whose parsed contents match the program to locate, then branch: delete
			// presses Delete on the highlighted row, edit presses Edit to enter its field editor.
			for (const auto& [ordinal, text] : m_ScheduleRows)
			{
				if (const auto parsed = IAQ::ParseScheduleRow(text); parsed.has_value() && matches_program(parsed.value()))
				{
					issue(static_cast<uint8_t>(IAQ_SCHEDULE_ROW_BASE + ordinal));   // highlight the target row
					m_ScheduleWritePhase = (goal.op == ScheduleWriteOp::Edit)
						? ScheduleWritePhase::PressEdit
						: ScheduleWritePhase::PressDelete;
					return;
				}
			}
			LogWarning(Channel::Devices, [&]() { return std::format("IAQ ({}): {} -- no matching program row to {} (target='{}')", DeviceId(), goal.desc, (goal.op == ScheduleWriteOp::Edit) ? "edit" : "delete", goal.match.target); });
			finish(false);
			return;
		}

		case ScheduleWritePhase::PressEdit:
		{
			if (m_CurrentPageId != IAQ_SCHEDULE_PAGE_ID)
			{
				m_PendingCommand = 0x00;   // dwell until the list (with the highlighted row) renders
				return;
			}
			issue(IAQ_CMD_EDIT_PROGRAM);   // Edit -> enter the highlighted row's field-edit mode
			// Re-set the fields from the desired program with the same phases the create flow uses.
			m_ScheduleWritePhase = ScheduleWritePhase::SetOnTime;
			return;
		}

		case ScheduleWritePhase::PressDelete:
		{
			if (m_CurrentPageId != IAQ_SCHEDULE_PAGE_ID)
			{
				m_PendingCommand = 0x00;
				return;
			}
			issue(IAQ_CMD_DELETE_PROGRAM);   // Delete -> the master raises the confirm dialog
			m_ScheduleWritePhase = ScheduleWritePhase::ConfirmDelete;
			return;
		}

		case ScheduleWritePhase::ConfirmDelete:
		{
			issue(IAQ_CMD_CONFIRM_OK);        // Ok on the confirm dialog -> removes the program
			m_ScheduleWritePhase = ScheduleWritePhase::VerifyGone;
			return;
		}

		case ScheduleWritePhase::VerifyGone:
		{
			// Complete once the target program is no longer present in the parsed list.
			for (const auto& [ordinal, text] : m_ScheduleRows)
			{
				if (const auto parsed = IAQ::ParseScheduleRow(text); parsed.has_value() && matches_program(parsed.value()))
				{
					m_PendingCommand = 0x00;   // still listed; dwell until the list re-renders
					return;
				}
			}
			finish(true);
			return;
		}

		case ScheduleWritePhase::Done:
		case ScheduleWritePhase::Failed:
		default:
			finish(m_ScheduleWritePhase == ScheduleWritePhase::Done);
			return;
		}
	}

	void IAQDevice::ProcessControllerUpdates()
	{
		ProcessControllerUpdates(false);
	}

	void IAQDevice::ProcessControllerUpdates(bool is_poll_message)
	{
		auto zone = Factory::ProfilingUnitFactory::Instance().CreateZone("IAQDevice::ProcessControllerUpdates", std::source_location::current());

		LogTrace(Channel::Devices, [&]() { return std::format("IAQ ({}): ProcessControllerUpdates called: state={}, is_poll={}, pending_cmd=0x{:02x}",
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
		if (is_poll_message && m_PendingSpaSwitchWrite.has_value())
		{
			SpaSwitchWrite_ProcessStep();
		}

		// Service an in-flight controller-schedule write goal (create a program) the same way:
		// it inspects the current page + decoded picker rows and sets m_PendingCommand to the next
		// command. Poll only, and mutually exclusive with the spa-switch goal / command queue.
		if (is_poll_message && m_PendingScheduleWrite.has_value())
		{
			ControllerScheduleWrite_ProcessStep();
		}

		// Commands can only be sent in response to IAQ_Poll messages.
		if (is_poll_message && !m_CommandQueue.empty())
		{
			auto cmd = m_CommandQueue.front();
			m_CommandQueue.pop_front();
			LogDebug(Channel::Devices, [&]() { return std::format("IAQ ({}): Sending queued command in Poll ACK: 0x{:02x} ({} remaining)",
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
