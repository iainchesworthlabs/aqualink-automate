#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "devices/jandy_device_types.h"
#include "navigation/navigator.h"
#include "utility/screen_data_page.h"

namespace AqualinkAutomate::Devices::OneTouch
{

	// The single physical keypad the OneTouch exposes: one cursor, one screen, one key emitted
	// per Status cycle, one shared Navigator. Every on-demand actuation goal drives it, and they
	// are mutually exclusive by that physical necessity. KeypadContext is the per-tick view of
	// that resource handed to the active goal's Step(): it reads the current screen + cursor,
	// borrows the Navigator, and collects the ONE key the goal wants to emit this cycle. The
	// owning OneTouchDevice builds a fresh context each Status cycle and translates emitted_key
	// (a Navigation::NavKeyCommand) into its wire KeyCommands afterwards.
	struct KeypadContext
	{
		const Devices::JandyDeviceType& device_id;   // for "OneTouch ({})" log context
		const Utility::ScreenDataPage& page;          // the currently displayed screen
		uint8_t highlighted_line;                     // cursor line (Navigator::CURSOR_LINE_NONE = none)
		Navigation::Navigator& navigator;             // the shared menu navigator

		std::optional<Navigation::NavKeyCommand> emitted_key;   // the goal's output this cycle (if any)

		const Devices::JandyDeviceType& DeviceId() const { return device_id; }

		// Queue the single key the goal wants sent this Status cycle.
		void Emit(Navigation::NavKeyCommand key) { emitted_key = key; }

		// Step the on-screen cursor one line toward target_line (establishing a cursor first if
		// none is highlighted); returns true once the cursor already sits on target_line (no key
		// emitted). Shared by the screen-driven goals that walk lists by content.
		bool MoveCursorToward(uint8_t target_line);
	};

	// Outcome of a single goal Step().
	enum class GoalStatus
	{
		Running,   // still working; keep servicing next cycle
		Done,      // completed successfully
		Failed     // abandoned (timed out, page not found, target unreachable, ...)
	};

	// One on-demand actuation goal (toggle / value-edit / boost / spa-switch / schedule-write).
	// Serviced one at a time by OneTouchGoalRunner; each Step() advances the goal's own phase
	// machine against the shared keypad and returns whether it is still running.
	class IKeypadGoal
	{
	public:
		virtual ~IKeypadGoal() = default;

		virtual GoalStatus Step(KeypadContext& ctx) = 0;

		// Short human description for logging (e.g. "toggle 'Pool Light'").
		virtual std::string_view Description() const = 0;
	};

	// Serialises the on-demand goals onto the single keypad: holds at most one active goal, drives
	// it each Status cycle, and clears it (resetting the Navigator) when it finishes. Replaces the
	// per-goal m_PendingX / m_XInProgress / m_XStepCount fields and the OR-of-booleans that used to
	// enforce "one goal at a time" on OneTouchDevice.
	class OneTouchGoalRunner
	{
	public:
		bool HasActiveGoal() const { return nullptr != m_ActiveGoal; }

		// Accept a goal only when the keypad is free. Returns false (goal discarded) if one is
		// already in flight - the caller has already applied its capability preconditions.
		bool TryStart(std::unique_ptr<IKeypadGoal> goal);

		// Drive the active goal one cycle. On Done/Failed, resets the Navigator and clears the
		// goal so the keypad is free again. No-op when idle.
		void Service(KeypadContext& ctx);

	private:
		std::unique_ptr<IKeypadGoal> m_ActiveGoal;
	};

}
// namespace AqualinkAutomate::Devices::OneTouch
