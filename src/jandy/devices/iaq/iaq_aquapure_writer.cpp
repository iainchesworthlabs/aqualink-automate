#include "devices/iaq/iaq_aquapure_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <magic_enum/magic_enum.hpp>

#include "devices/iaq/iaq_command_sink.h"
#include "devices/iaq/iaq_page_model.h"
#include "devices/jandy_device_types.h"
#include "formatters/jandy_device_formatters.h"
#include "logging/logging.h"
#include "utility/string_manipulation.h"

using namespace AqualinkAutomate::Logging;

namespace AqualinkAutomate::Devices::IAQ
{

	namespace
	{
		// AqualinkTouch (0x33) UI command bytes. These ride the 0x33 poll-ACK channel.
		//
		// 0x02 is the panel's single "Menu / Back" key -- the SAME physical button, whose meaning
		// depends on the screen it is pressed from (menu when at home, back when in a sub-page).
		// That is precisely why every hop below re-reads the page id instead of assuming where a
		// press landed. Cross-checked against AqualinkD's KEY_IAQTCH_MENU (source/iaqtouch.h).
		constexpr uint8_t IAQ_CMD_MENU_OR_BACK{ 0x02 };
		constexpr uint8_t IAQ_CMD_PAGE_BUTTON_BASE{ 0x11 };   // press on-screen PageButton index N -> 0x11 + N
		constexpr uint8_t IAQ_CMD_SUBMIT_VALUE{ 0x80 };       // submit the armed control-data value
		constexpr uint8_t IAQ_CMD_DWELL{ 0x00 };              // send nothing this poll

		// Page ids. HOME/MENU are already validated in-repo (the spa-switch writer walks them) and
		// match AqualinkD's IAQ_PAGE_HOME / IAQ_PAGE_MENU. SET_SWG is AqualinkD's IAQ_PAGE_SET_SWG;
		// no capture here contains it yet, hence the label-driven, fail-safe navigation.
		constexpr uint8_t IAQ_PAGE_HOME{ 0x01 };
		constexpr uint8_t IAQ_PAGE_MENU{ 0x0f };
		constexpr uint8_t IAQ_PAGE_SET_SWG{ 0x30 };

		// The control-data value is sent as ASCII "<field><value>". The field is chosen by the
		// button pressed, NOT by this prefix (verified for the Set Temperature page against
		// iaq_aux_setpoint.cap, and matching AqualinkD, whose control frame carries a literal 0x31).
		constexpr char IAQ_CONTROL_FIELD_PREFIX{ '1' };

		constexpr uint32_t IAQ_AQUAPURE_SETTLE_POLLS{ 4 };    // polls to let the master render after a command
		constexpr uint32_t IAQ_AQUAPURE_MAX_UNWIND{ 6 };      // bounded Menu/Back presses while unwinding
		constexpr uint32_t IAQ_AQUAPURE_POLL_LIMIT{ 200 };    // overall backstop (abandon the goal)

		// Opening AquaPure from the menu. CONFIRMED on a live bus (captures/chlorinator_rs485.cap,
		// an iAqualink module driving a real RS-8 Combo): 0x02 -> PageStart 0x0f, then 0x19 ->
		// PageStart 0x30. AqualinkD hardcodes the same key.
		//
		// This one hop CANNOT be label-driven, because the menu page advertises no buttons at all
		// -- the master sends PageStart(0x0f) immediately followed by PageEnd, with no PageButton
		// frames in between. So the index is pressed blind; what makes that safe is that arrival
		// on page 0x30 is VERIFIED afterwards, and a panel that does not land there is reported as
		// having no route rather than being pressed at repeatedly.
		constexpr uint8_t IAQ_CMD_MENU_TO_AQUAPURE{ 0x19 };
		constexpr uint32_t IAQ_AQUAPURE_MAX_MENU_ATTEMPTS{ 2 };

		// If a panel DOES advertise its menu buttons, prefer the labelled entry over the blind
		// index -- strictly better where it is available, and free where it is not.
		constexpr std::array<std::string_view, 3> AQUAPURE_MENU_LABELS{ "AquaPure", "Aqua Pure", "Chlorinator" };

		// Case-insensitive substring search over a button's on-screen name. Button names carry a
		// trailing status suffix ("Pool Light" + "ON"), so an exact match is never safe here.
		bool NameContains(std::string_view name, std::string_view needle)
		{
			if (needle.empty() || (needle.size() > name.size()))
			{
				return false;
			}

			const auto ascii_tolower = [](char c) -> char { return ((c >= 'A') && (c <= 'Z')) ? static_cast<char>(c - 'A' + 'a') : c; };
			const auto it = std::search(name.begin(), name.end(), needle.begin(), needle.end(),
				[&ascii_tolower](char lhs, char rhs) { return ascii_tolower(lhs) == ascii_tolower(rhs); });

			return it != name.end();
		}

		// Index of the first on-screen button whose name contains `needle`.
		std::optional<uint8_t> FindButtonContaining(const PageModel& page, std::string_view needle)
		{
			for (const auto& [index, info] : page.Buttons())
			{
				if (NameContains(Utility::TrimWhitespace(info.name), needle))
				{
					return index;
				}
			}

			return std::nullopt;
		}

		uint8_t PressCommandFor(uint8_t button_index)
		{
			return static_cast<uint8_t>(IAQ_CMD_PAGE_BUTTON_BASE + button_index);
		}
	}
	// unnamed namespace

	Capabilities::ActuationResult AquaPureWriter::Arm(Goal&& goal, bool emulation_active, bool channel_busy,
		const JandyDeviceType& device_id)
	{
		// Only an actively-emulating panel transmits. Accepting while passive (or presence-suppressed
		// because a real AqualinkTouch answered at this address) would strand the goal AND stop the
		// dispatcher trying the next capable controller, so the command would silently do nothing.
		if (!emulation_active)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): Not actively emulating - cannot drive the AquaPure page", device_id); });
			return Capabilities::ActuationResult::NotSupported;
		}

		// One goal at a time on the shared panel UI: a second walk would fight the first for the
		// single command channel and could submit a value on the wrong screen.
		if (m_Pending.has_value() || channel_busy)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): Busy - rejecting AquaPure write", device_id); });
			return Capabilities::ActuationResult::NotSupported;
		}

		// Already positively established that this panel's menu has no AquaPure entry: refuse so the
		// dispatcher falls back to a controller that reaches the setting by menu crawl.
		if (m_RouteUnavailable)
		{
			LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): This panel's menu has no AquaPure entry - deferring to another controller", device_id); });
			return Capabilities::ActuationResult::MappingFailed;
		}

		LogInfo(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): Queued {}", device_id, goal.desc); });

		m_Pending = std::move(goal);
		m_Phase = Phase::Navigate;
		m_PollCount = 0;
		m_SettleCount = 0;
		m_UnwindCount = 0;
		m_MenuAttempts = 0;
		m_FieldSelected = false;

		return Capabilities::ActuationResult::Accepted;
	}

	Capabilities::ActuationResult AquaPureWriter::QueuePercentage(uint8_t percentage, Kernel::BodyOfWaterIds body,
		bool emulation_active, bool channel_busy, const JandyDeviceType& device_id)
	{
		if (percentage > 100)
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		// Only the two bodies the AquaPure page actually has rows for are addressable.
		if ((body != Kernel::BodyOfWaterIds::Pool) && (body != Kernel::BodyOfWaterIds::Spa))
		{
			return Capabilities::ActuationResult::InvalidValue;
		}

		Goal goal;
		goal.is_boost = false;
		goal.percentage = percentage;
		goal.body = body;
		goal.desc = std::format("chlorinator {} output -> {}%", magic_enum::enum_name(body), percentage);

		return Arm(std::move(goal), emulation_active, channel_busy, device_id);
	}

	Capabilities::ActuationResult AquaPureWriter::QueueBoost(bool enable, bool emulation_active, bool channel_busy,
		const JandyDeviceType& device_id)
	{
		Goal goal;
		goal.is_boost = true;
		goal.boost_enable = enable;
		goal.desc = std::format("chlorinator boost {}", enable ? "on" : "off");

		return Arm(std::move(goal), emulation_active, channel_busy, device_id);
	}

	void AquaPureWriter::ObserveMenuPage(const PageModel& page)
	{
		if (IAQ_PAGE_MENU != page.PageId())
		{
			return;
		}

		// The menu renders its full button list, so a miss here is meaningful -- but only once the
		// page actually has buttons (a freshly-started page has none yet).
		if (page.Buttons().empty())
		{
			return;
		}

		for (const auto& label : AQUAPURE_MENU_LABELS)
		{
			if (auto index = FindButtonContaining(page, label); index.has_value())
			{
				m_MenuAquaPureIndex = index;
				m_RouteUnavailable = false;
				return;
			}
		}

		m_MenuAquaPureIndex.reset();
	}

	void AquaPureWriter::IssueAndSettle(ICommandSink& sink, uint8_t cmd)
	{
		sink.IssueCommand(cmd);
		m_SettleCount = IAQ_AQUAPURE_SETTLE_POLLS;
	}

	void AquaPureWriter::FinishGoal(ICommandSink& sink, bool ok, const Goal& goal, const JandyDeviceType& device_id)
	{
		if (ok) { LogInfo(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} completed", device_id, goal.desc); }); }
		else    { LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): {} abandoned", device_id, goal.desc); }); }

		sink.IssueCommand(IAQ_CMD_DWELL);
		m_Pending.reset();
		m_Phase = Phase::Navigate;
		m_SettleCount = 0;
		m_UnwindCount = 0;
		m_MenuAttempts = 0;
		m_FieldSelected = false;
	}

	void AquaPureWriter::StepNavigate(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id)
	{
		switch (page.PageId())
		{
		case IAQ_PAGE_SET_SWG:
			m_Phase = Phase::SelectField;   // arrived; act next poll
			m_UnwindCount = 0;
			return;

		case IAQ_PAGE_MENU:
		{
			// Prefer a labelled menu entry if this panel advertises one; otherwise use the
			// confirmed key. Either way we are about to leave the menu, so bound the attempts:
			// still being here after pressing means the press did not open AquaPure.
			ObserveMenuPage(page);

			if (m_MenuAttempts++ >= IAQ_AQUAPURE_MAX_MENU_ATTEMPTS)
			{
				// Pressed and stayed on the menu: this panel has no reachable AquaPure page.
				// Latch it so every later request refuses up-front and the dispatcher uses the
				// OneTouch menu-crawl instead of walking here again each time.
				LogWarning(Channel::Devices, [&device_id]() { return std::format("IAQ ({}): The panel menu did not open an AquaPure page; the chlorinator cannot be set through the iAQ on this system", device_id); });
				m_RouteUnavailable = true;
				m_Phase = Phase::Failed;
				return;
			}

			const auto command = m_MenuAquaPureIndex.has_value()
				? PressCommandFor(m_MenuAquaPureIndex.value())
				: IAQ_CMD_MENU_TO_AQUAPURE;

			IssueAndSettle(sink, command);
			return;
		}

		case IAQ_PAGE_HOME:
		default:
			// Home (or an unknown sub-page): press Menu/Back to move toward the menu. Bounded,
			// because on some screens this key backs out rather than opening the menu.
			if (m_UnwindCount++ >= IAQ_AQUAPURE_MAX_UNWIND)
			{
				LogWarning(Channel::Devices, [&device_id, &page]() { return std::format("IAQ ({}): Could not reach the panel menu (stuck on page 0x{:02x})", device_id, page.PageId()); });
				m_Phase = Phase::Failed;
				return;
			}

			IssueAndSettle(sink, IAQ_CMD_MENU_OR_BACK);
			return;
		}
	}

	void AquaPureWriter::StepSelectField(const PageModel& page, ICommandSink& sink, const Goal& goal,
		const JandyDeviceType& device_id)
	{
		if (IAQ_PAGE_SET_SWG != page.PageId())
		{
			m_Phase = Phase::Navigate;   // lost the page; re-navigate
			return;
		}

		if (page.Buttons().empty())
		{
			sink.IssueCommand(IAQ_CMD_DWELL);   // still rendering
			return;
		}

		// Resolve the target row BY LABEL on the page that owns it. For an output change that is the
		// body being chlorinated; for boost it is the Quick Boost control (whose single press
		// toggles). Mirrors AqualinkD's iaqtFindButtonByLabel("Pool"/"Spa"/"Quick Boost").
		std::optional<uint8_t> index;
		if (goal.is_boost)
		{
			index = FindButtonContaining(page, "Boost");
		}
		else
		{
			// Rows are labelled with their body AND current value ("Pool 40%" / "Spa 70%"), which
			// is why this matches on CONTAINS. No cross-body fallback: silently writing the pool
			// setpoint when the spa was asked for would be worse than refusing, because the two
			// are independent and the caller would never know it hit the wrong one.
			index = FindButtonContaining(page, (Kernel::BodyOfWaterIds::Spa == goal.body) ? "Spa" : "Pool");
		}

		if (!index.has_value())
		{
			LogWarning(Channel::Devices, [&device_id, &goal]() { return std::format("IAQ ({}): The AquaPure page has no row matching {}; not guessing a button", device_id, goal.desc); });
			m_Phase = Phase::Failed;
			return;
		}

		IssueAndSettle(sink, PressCommandFor(index.value()));
		m_FieldSelected = true;
		m_Phase = goal.is_boost ? Phase::ReturnHome : Phase::Submit;
	}

	void AquaPureWriter::StepSubmit(const PageModel& page, ICommandSink& sink, const Goal& goal)
	{
		if (IAQ_PAGE_SET_SWG != page.PageId())
		{
			m_Phase = Phase::Navigate;   // lost the page before submitting; start over rather than
			m_FieldSelected = false;     // firing a value into whatever is on screen now
			return;
		}

		// Absolute value -- this is what makes the iAQ path fast: the OneTouch equivalent can only
		// step 5% per key press. The value rides the control-data handshake the master opens after
		// the submit command.
		sink.ArmControlValue(std::format("{}{}", IAQ_CONTROL_FIELD_PREFIX, goal.percentage));
		IssueAndSettle(sink, IAQ_CMD_SUBMIT_VALUE);
		m_Phase = Phase::ReturnHome;
	}

	void AquaPureWriter::StepReturnHome(const PageModel& page, ICommandSink& sink, const Goal& goal,
		const JandyDeviceType& device_id)
	{
		// Leave the panel back at home. This is not cosmetic: the live page model is what
		// DeviceActuator resolves aux buttons against, so parking on a settings page would break
		// the next equipment toggle.
		if (IAQ_PAGE_HOME == page.PageId())
		{
			FinishGoal(sink, /*ok=*/true, goal, device_id);
			m_Phase = Phase::Done;
			return;
		}

		if (m_UnwindCount++ >= IAQ_AQUAPURE_MAX_UNWIND)
		{
			// The value was already submitted, so the goal itself succeeded -- only the tidy-up
			// did not. Report success and stop pressing keys.
			FinishGoal(sink, /*ok=*/true, goal, device_id);
			m_Phase = Phase::Done;
			return;
		}

		IssueAndSettle(sink, IAQ_CMD_MENU_OR_BACK);
	}

	void AquaPureWriter::ProcessStep(const PageModel& page, ICommandSink& sink, const JandyDeviceType& device_id)
	{
		if (!m_Pending.has_value())
		{
			return;
		}

		// Overall backstop: never hold the command channel forever.
		if (m_PollCount++ > IAQ_AQUAPURE_POLL_LIMIT)
		{
			const Goal goal = m_Pending.value();
			FinishGoal(sink, /*ok=*/false, goal, device_id);
			return;
		}

		// Let the master finish rendering the page a command just requested.
		if (m_SettleCount > 0)
		{
			--m_SettleCount;
			sink.IssueCommand(IAQ_CMD_DWELL);
			return;
		}

		const Goal goal = m_Pending.value();

		switch (m_Phase)
		{
		case Phase::Navigate:
			StepNavigate(page, sink, device_id);
			break;

		case Phase::SelectField:
			StepSelectField(page, sink, goal, device_id);
			break;

		case Phase::Submit:
			StepSubmit(page, sink, goal);
			break;

		case Phase::ReturnHome:
			StepReturnHome(page, sink, goal, device_id);
			break;

		case Phase::Done:
			FinishGoal(sink, /*ok=*/true, goal, device_id);
			break;

		case Phase::Failed:
		default:
			FinishGoal(sink, /*ok=*/false, goal, device_id);
			break;
		}
	}

}
// namespace AqualinkAutomate::Devices::IAQ
